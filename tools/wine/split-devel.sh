#!/usr/bin/env bash
# Разделяет собранное дерево Wine на то, что едет пользователю, и то, что
# нужно только нам, по правилам из wine/split-rules.txt.
#
# Зачем. Заголовки, импорт-библиотеки и winegcc — это около 148 МБ, и на
# машине пользователя они не нужны ни разу: PE-хелпер собирается в CI и едет
# вшитым в бинарник cork. Но главное не мегабайты. Сейчас devel попадает в
# артефакт побочным эффектом `make install`, и его состав нигде не закреплён:
# всякая правка в дереве Wine молча меняет то, что уезжает. Здесь состав —
# список, а файл, не подошедший ни под одно правило, останавливает сборку.
#
# Отдельным скриптом, а не куском build.sh: шаг, который можно запустить на
# готовом дереве, можно и проверить, не пересобирая Wine.
#
# Использование:
#   tools/wine/split-devel.sh <дерево> [--sdk-out DIR] [--rules FILE] [--dry-run]

set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

tree=""
sdk_out=""
rules="$repo_root/wine/split-rules.txt"
dry_run=0

while [ $# -gt 0 ]; do
    case "$1" in
        --sdk-out) sdk_out="$2"; shift 2 ;;
        --rules) rules="$2"; shift 2 ;;
        --dry-run) dry_run=1; shift ;;
        -h|--help) sed -n '2,17p' "${BASH_SOURCE[0]}"; exit 0 ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) tree="$1"; shift ;;
    esac
done

if [ -z "$tree" ] || [ ! -d "$tree" ]; then
    echo "split-devel: give the directory of a built Wine tree" >&2
    exit 2
fi
tree="$(cd "$tree" && pwd)"
[ -n "$sdk_out" ] || sdk_out="$tree-sdk"
[ -f "$rules" ] || { echo "split-devel: no rules file at $rules" >&2; exit 2; }

classes=()
patterns=()
# read -r с двумя полями, а не `set -- $line`: там образец попадает под
# раскрытие имён файлов, и `include/**` превращается в список того, что лежит
# в текущем каталоге. build.sh к этому моменту стоит в каталоге сборки Wine,
# где есть include/, — правило рассыпалось на полторы тысячи слов и молча
# выпало, а вместе с ним и всё, что оно покрывало.
while read -r class pattern extra; do
    case "$class" in
        ''|\#*) continue ;;
    esac
    if [ -z "$pattern" ] || [ -n "$extra" ]; then
        echo "split-devel: expected 'class pattern' in $rules, got: $class $pattern $extra" >&2
        exit 2
    fi
    case "$class" in
        runtime|sdk|drop) classes+=("$class"); patterns+=("$pattern") ;;
        *) echo "split-devel: unknown class '$class' in $rules" >&2; exit 2 ;;
    esac
done < "$rules"

[ "${#classes[@]}" -gt 0 ] || { echo "split-devel: $rules has no rules" >&2; exit 2; }

# Первое совпадение побеждает. Сверка — сопоставлением образцов bash, в
# котором * переходит через «/»; для этих правил так и нужно.
classify() {
    local path=$1 i
    for ((i = 0; i < ${#patterns[@]}; ++i)); do
        # shellcheck disable=SC2053
        if [[ $path == ${patterns[i]} ]]; then
            printf '%s' "${classes[i]}"
            return 0
        fi
    done
    return 1
}

before=$(du -sb "$tree" | cut -f1)
unmatched=()
to_move=()
to_drop=()

# Сначала классифицируется всё дерево и только потом что-либо трогается.
#
# Раньше перенос шёл по ходу, а о непокрытых файлах скрипт сообщал в конце, —
# и упавший прогон оставлял дерево разрезанным пополам: часть в sdk, часть на
# месте, и понять, где что, уже неоткуда. Отказ обязан не менять ничего.
while IFS= read -r -d '' file; do
    rel="${file#"$tree"/}"
    class="$(classify "$rel")" || { unmatched+=("$rel"); continue; }
    case "$class" in
        runtime) ;;
        sdk) to_move+=("$rel") ;;
        drop) to_drop+=("$rel") ;;
    esac
done < <(find "$tree" \( -type f -o -type l \) -print0)

# Непокрытый файл — отказ, а не предупреждение. Предупреждение здесь никто не
# прочитает, а состав артефакта разойдётся с тем, что написано в правилах.
if [ "${#unmatched[@]}" -gt 0 ]; then
    echo "split-devel: ${#unmatched[@]} file(s) match no rule in $rules:" >&2
    printf '  %s\n' "${unmatched[@]:0:20}" >&2
    [ "${#unmatched[@]}" -gt 20 ] && echo "  ... and $(( ${#unmatched[@]} - 20 )) more" >&2
    echo "Add them to the rules, on purpose, one class each." >&2
    echo "Nothing was moved." >&2
    exit 1
fi

moved=${#to_move[@]}
dropped=${#to_drop[@]}

if [ "$dry_run" = 1 ]; then
    printf 'would move %d file(s) to sdk, drop %d\n' "$moved" "$dropped"
    exit 0
fi

for rel in ${to_move[@]+"${to_move[@]}"}; do
    mkdir -p "$sdk_out/$(dirname "$rel")"
    mv "$tree/$rel" "$sdk_out/$rel" || exit 1
done
for rel in ${to_drop[@]+"${to_drop[@]}"}; do
    rm -f "$tree/$rel" || exit 1
done

find "$tree" -type d -empty -delete 2>/dev/null

after=$(du -sb "$tree" | cut -f1)
sdk_size=$(du -sb "$sdk_out" 2>/dev/null | cut -f1 || echo 0)
human() { numfmt --to=iec --suffix=B "$1" 2>/dev/null || echo "$1"; }

printf 'split:     %d file(s) to sdk, %d dropped\n' "$moved" "$dropped"
printf 'runtime:   %s -> %s\n' "$(human "$before")" "$(human "$after")"
printf 'sdk:       %s in %s\n' "$(human "$sdk_size")" "$sdk_out"
