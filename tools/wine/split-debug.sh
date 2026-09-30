#!/usr/bin/env bash
# Вынимает отладочную информацию из собранного дерева Wine в отдельное дерево.
#
# Зачем. Шестьдесят один процент собранного Wine — это DWARF: 664 МиБ из
# 1082 МиБ PE-файлов (замерено на 11.18 с i386 и x86_64; у ELF-части доля ещё
# выше, ntdll.so худеет вчетверо). Пользователю она не нужна ни разу: отладчик
# Wine на его машине не запускается, а отчёт об ошибке приходит с адресами,
# которые разрешаются по нашей копии.
#
# Почему вынимается, а не выбрасывается. Без неё нельзя прочитать ни один
# аварийный дамп, а собрать Wine заново ради этого — десятки минут. Поэтому
# файл, у которого отладочные секции есть, а вынуть их не удалось,
# останавливает скрипт: «не смог и потому срезал» — худший из возможных
# исходов, и однажды он уже случился.
#
# Запускать надо из контейнера cork-dev. На хосте llvm-objcopy не умеет PE.
#
# Почему --strip-debug, а не --strip-all. В PE x86_64 раскрутка стека живёт в
# .pdata и .xdata, и без них SEH перестаёт работать. Дымовой проверкой это не
# ловится: ломается только настоящее исключение в настоящей сборке.
#
# Отдельным скриптом, а не куском build.sh: шаг, который можно запустить на
# готовом дереве, можно и проверить, не пересобирая Wine.
#
# Использование:
#   tools/wine/split-debug.sh <дерево> [--debug-out DIR] [--dry-run]

set -uo pipefail

tree=""
debug_out=""
dry_run=0

while [ $# -gt 0 ]; do
    case "$1" in
        --debug-out) debug_out="$2"; shift 2 ;;
        --dry-run) dry_run=1; shift ;;
        -h|--help) sed -n '2,25p' "${BASH_SOURCE[0]}"; exit 0 ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) tree="$1"; shift ;;
    esac
done

if [ -z "$tree" ] || [ ! -d "$tree" ]; then
    echo "split-debug: give the directory of a built Wine tree" >&2
    exit 2
fi
tree="$(cd "$tree" && pwd)"
[ -n "$debug_out" ] || debug_out="$tree-debug"

for tool in llvm-objcopy llvm-strip llvm-objdump; do
    command -v "$tool" >/dev/null || { echo "split-debug: $tool is not installed" >&2; exit 2; }
done

# Версия проверяется здесь, а не выясняется на первом же файле.
#
# llvm-objcopy до 20.1 не умеет --only-keep-debug на PE, который clang собрал
# не в mingw-режиме: каталог отладки линковщик кладёт в .rdata, эта секция
# срезается, и patchDebugDirectory отвечает «debug directory not found».
# Починено в llvm/llvm-project#121653.
#
# Сорок минут сборки Wine, чтобы узнать это на d3dx9_33.dll, уже потрачены
# один раз. Отказ на входе стоит секунду и называет причину.
llvm_major="$(llvm-objcopy --version | sed -n 's/.*LLVM version \([0-9]*\).*/\1/p' | head -1)"
if [ -z "$llvm_major" ]; then
    echo "split-debug: could not read the version of llvm-objcopy" >&2
    exit 2
fi
if [ "$llvm_major" -lt 20 ]; then
    echo "split-debug: llvm-objcopy $llvm_major cannot extract debug info from these PE files." >&2
    echo "  --only-keep-debug on a PE built by clang outside mingw mode needs LLVM 20.1" >&2
    echo "  or newer; older versions fail with \"debug directory not found\"." >&2
    echo "  See llvm/llvm-project#121653. Install a newer LLVM and put it first in PATH." >&2
    exit 2
fi

# Ищем по содержимому, а не по расширению: в дереве Wine исполняемое лежит и
# как .dll, и как .exe, и как .drv, .sys, .acm, .ocx, .cpl, и вовсе без
# расширения.
is_binary() {
    local magic
    magic="$(head -c4 "$1" 2>/dev/null | od -An -tx1 | tr -d ' \n')"
    case "$magic" in
        4d5a*) return 0 ;;    # MZ — PE
        7f454c46) return 0 ;; # \x7fELF
        *) return 1 ;;
    esac
}

before=$(du -sb "$tree" | cut -f1)
processed=0
extracted=0

while IFS= read -r -d '' file; do
    is_binary "$file" || continue
    processed=$((processed + 1))
    [ "$dry_run" = 1 ] && continue

    rel="${file#"$tree"/}"
    mkdir -p "$debug_out/$(dirname "$rel")"

    # Есть ли что выносить, решается по самому файлу, а не по тому, получилось
    # ли у objcopy.
    #
    # Это не перестраховка. Раньше здесь стояло «не вышло — значит секций и
    # нет», и на хосте, где llvm-objcopy не умеет PE («debug directory not
    # found»), скрипт счёл так про 4102 файла из 4231 и всё равно их срезал.
    # Получилось не разделение, а потеря 700 МБ отладочной информации, о
    # которой скрипт бодро отчитался как об успехе.
    if ! llvm-objdump -h "$file" 2>/dev/null | grep -q '\.debug'; then
        continue
    fi
    if ! llvm-objcopy --only-keep-debug "$file" "$debug_out/$rel.debug" 2>"$debug_out/.err" ||
       [ ! -s "$debug_out/$rel.debug" ]; then
        echo "split-debug: $rel has debug sections but they could not be extracted:" >&2
        sed -n '1,3p' "$debug_out/.err" >&2
        echo "Nothing was stripped. Run this inside the cork-dev container:" >&2
        echo "  distrobox enter cork-dev -- $0 $tree" >&2
        rm -f "$debug_out/$rel.debug" "$debug_out/.err"
        exit 1
    fi
    rm -f "$debug_out/.err"
    extracted=$((extracted + 1))
    if ! llvm-strip --strip-debug "$file" 2>/dev/null; then
        echo "split-debug: failed to strip $rel" >&2
        exit 1
    fi
done < <(find "$tree" -type f -print0)

if [ "$dry_run" = 1 ]; then
    printf 'would process %d binaries in %s\n' "$processed" "$tree"
    exit 0
fi

after=$(du -sb "$tree" | cut -f1)
debug_size=$(du -sb "$debug_out" 2>/dev/null | cut -f1 || echo 0)
human() { numfmt --to=iec --suffix=B "$1" 2>/dev/null || echo "$1"; }

printf 'binaries:  %d processed, %d had debug info\n' "$processed" "$extracted"
printf 'tree:      %s -> %s\n' "$(human "$before")" "$(human "$after")"
printf 'debug:     %s in %s\n' "$(human "$debug_size")" "$debug_out"
