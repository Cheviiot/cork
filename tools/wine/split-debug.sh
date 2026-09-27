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
# аварийный дамп, а собрать Wine заново ради этого — десятки минут.
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

for tool in llvm-objcopy llvm-strip; do
    command -v "$tool" >/dev/null || { echo "split-debug: $tool is not installed" >&2; exit 2; }
done

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
    # Отсутствие отладочных секций — не ошибка: часть файлов уже без них, и
    # пустой результат просто убирается.
    if llvm-objcopy --only-keep-debug "$file" "$debug_out/$rel.debug" 2>/dev/null &&
       [ -s "$debug_out/$rel.debug" ]; then
        extracted=$((extracted + 1))
    else
        rm -f "$debug_out/$rel.debug"
    fi
    llvm-strip --strip-debug "$file" 2>/dev/null || true
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
