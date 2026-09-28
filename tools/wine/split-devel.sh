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
while IFS= read -r line; do
    line="${line%%#*}"
    # shellcheck disable=SC2086
    set -- $line
    [ $# -eq 2 ] || continue
    case "$1" in
        runtime|sdk|drop) classes+=("$1"); patterns+=("$2") ;;
        *) echo "split-devel: unknown class '$1' in $rules" >&2; exit 2 ;;
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
moved=0
dropped=0

while IFS= read -r -d '' file; do
    rel="${file#"$tree"/}"
    class="$(classify "$rel")" || { unmatched+=("$rel"); continue; }
    case "$class" in
        runtime) ;;
        sdk)
            moved=$((moved + 1))
            [ "$dry_run" = 1 ] && continue
            mkdir -p "$sdk_out/$(dirname "$rel")"
            mv "$file" "$sdk_out/$rel"
            ;;
        drop)
            dropped=$((dropped + 1))
            [ "$dry_run" = 1 ] && continue
            rm -f "$file"
            ;;
    esac
done < <(find "$tree" \( -type f -o -type l \) -print0)

# Непокрытый файл — отказ, а не предупреждение. Предупреждение здесь никто не
# прочитает, а состав артефакта разойдётся с тем, что написано в правилах.
if [ "${#unmatched[@]}" -gt 0 ]; then
    echo "split-devel: ${#unmatched[@]} file(s) match no rule in $rules:" >&2
    printf '  %s\n' "${unmatched[@]:0:20}" >&2
    [ "${#unmatched[@]}" -gt 20 ] && echo "  ... and $(( ${#unmatched[@]} - 20 )) more" >&2
    echo "Add them to the rules, on purpose, one class each." >&2
    exit 1
fi

if [ "$dry_run" = 1 ]; then
    printf 'would move %d file(s) to sdk, drop %d\n' "$moved" "$dropped"
    exit 0
fi

find "$tree" -type d -empty -delete 2>/dev/null

after=$(du -sb "$tree" | cut -f1)
sdk_size=$(du -sb "$sdk_out" 2>/dev/null | cut -f1 || echo 0)
human() { numfmt --to=iec --suffix=B "$1" 2>/dev/null || echo "$1"; }

printf 'split:     %d file(s) to sdk, %d dropped\n' "$moved" "$dropped"
printf 'runtime:   %s -> %s\n' "$(human "$before")" "$(human "$after")"
printf 'sdk:       %s in %s\n' "$(human "$sdk_size")" "$sdk_out"
