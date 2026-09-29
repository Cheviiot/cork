#!/usr/bin/env bash
# Сборка cork-helper.exe — PE-половины обёртки инструментов.
#
# Собирается wineg++ из дерева Wine, которое поставляется вместе с Cork, а не
# MSVC во время install. Компиляция хелпера только что установленным cl.exe —
# самый хрупкий шаг, какой может быть в установке: он требует работающего
# Wine, работающего компилятора и префикса, то есть всего того, ради чего
# хелпер и нужен.
#
# Результат вшивается в бинарник cork, поэтому обычная сборка проекта
# хелпер не пересобирает и Wine ей не нужен.
#
# Использование:
#   tools/wine/build-helper.sh [--wine DIR] [--out FILE]

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
wine_root="$repo_root/build/wine-staging/opt/cork-wine"
out_file="$repo_root/build/helper/cork-helper.exe"

while [ $# -gt 0 ]; do
    case "$1" in
        --wine) wine_root="$2"; shift 2 ;;
        --out) out_file="$2"; shift 2 ;;
        -h|--help) sed -n '2,16p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

wineg="$wine_root/bin/wineg++"
if [ ! -x "$wineg" ]; then
    echo "wineg++ not found at $wineg; build Wine first: tools/wine/build.sh" >&2
    exit 1
fi

mkdir -p "$(dirname "$out_file")"

echo "==> building $(basename "$out_file") with $wineg"
"$wineg" \
    -b x86_64-windows \
    -mconsole \
    -O2 \
    -std=c++17 \
    -Wall -Wextra \
    -I "$repo_root/src" \
    -o "$out_file" \
    "$repo_root/helper/helper.cpp" \
    "$repo_root/src/proto/protocol.cpp"

# Подопытная программа для интеграционных проверок. Не поставляется, живёт
# только в build/ и собирается тем же тулчейном, что и хелпер, — иначе проверка
# шла бы против другого компилятора, чем продукт.
child_out="$(dirname "$out_file")/cork-test-child.exe"
echo "==> building $(basename "$child_out")"
"$wineg" -b x86_64-windows -mconsole -O2 -std=c++17 -Wall -Wextra \
    -o "$child_out" "$repo_root/tests/wine/child.cpp"

# Отладочная информация хелперу не нужна: он вшивается в бинарник cork, и
# каждый килобайт здесь едет к пользователю.
if command -v llvm-strip >/dev/null 2>&1; then
    llvm-strip --strip-debug "$out_file" 2>/dev/null || true
    llvm-strip --strip-debug "$child_out" 2>/dev/null || true
fi

echo "built: $out_file"
ls -la "$out_file"
file "$out_file" 2>/dev/null || true
