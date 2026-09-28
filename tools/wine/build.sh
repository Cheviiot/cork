#!/usr/bin/env bash
# Сборка Wine для cork: применение серии патчей, configure, make, установка
# в staging и докладка Wine Mono нужной версии.
#
# Запускать из контейнера cork-dev (см. docs/development.md). Полная сборка
# занимает десятки минут, поэтому она намеренно отделена от обычного
# cmake --build: Wine не является частью CMake-проекта и пересобирается только
# по явной команде.
#
# Использование:
#   tools/wine/build.sh [--jobs N] [--out DIR] [--clean] [--keep-debug]
#                       [--keep-devel]
#
# По умолчанию результат оказывается в build/wine-staging/opt/cork-wine.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
src_dir="$repo_root/wine/src"
patch_dir="$repo_root/wine/patches"
flags_file="$repo_root/wine/configure-flags.txt"

jobs="$(nproc)"
out_dir="$repo_root/build/wine-staging"
build_dir="$repo_root/build/wine-build"
install_prefix="/opt/cork-wine"
clean_build=0
strip_debug=1
split_devel=1

while [ $# -gt 0 ]; do
    case "$1" in
        --jobs) jobs="$2"; shift 2 ;;
        # DESTDIR разрешается относительно каталога сборки, а не того, где
        # стоял человек, поэтому относительный --out тихо уезжал внутрь
        # build/wine-build. Приводим к абсолютному сразу.
        --out) out_dir="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"; shift 2 ;;
        --clean) clean_build=1; shift ;;
        --keep-debug) strip_debug=0; shift ;;
        --keep-devel) split_devel=0; shift ;;
        -h|--help) sed -n '2,20p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [ ! -f "$src_dir/configure" ]; then
    echo "wine/src is empty; run: git submodule update --init --depth 1 wine/src" >&2
    exit 1
fi

# --- PE-компилятор -----------------------------------------------------------
# Wine собирает PE-часть отдельным компилятором, и configure ищет его по списку
# имён, последнее из которых — просто "clang". Прежний CI из-за этого
# перезаписывал /usr/bin/clang симлинком на нужную версию: стоковый clang 18.1.3
# падал с SIGSEGV на dlls/vcruntime140/typeinfo.c, а CC=... до этого поиска не
# доставал. Оказалось, что configure принимает переменную x86_64_CC напрямую
# ("Let the user override the test"), и makedep берёт именно её — так что порча
# системных путей не нужна. Задайте CORK_PE_CC, если системный clang не
# подходит.
pe_cc="${CORK_PE_CC:-}"

# --- версия Wine Mono --------------------------------------------------------
# Берётся из самих исходников, а не из константы рядом: Wine требует конкретную
# версию, и при бампе тега она меняется молча. На 11.14 было 11.2.0, на 11.18
# уже 11.3.0 — ровно та ошибка, которую здесь нельзя допустить.
mono_version="$(sed -n 's/^#define MONO_VERSION "\(.*\)"/\1/p' "$src_dir/dlls/appwiz.cpl/addons.c")"
if [ -z "$mono_version" ]; then
    echo "could not read MONO_VERSION from dlls/appwiz.cpl/addons.c" >&2
    exit 1
fi

# --- применение патчей -------------------------------------------------------
# Идемпотентно: штамп хранит хеш серии вместе с содержимым патчей. Если хеш
# совпал, дерево уже пропатчено этой самой серией и трогать его не надо; если
# нет — дерево возвращается к чистому состоянию и патчи накатываются заново.
series_hash="$(cat "$patch_dir/series" "$patch_dir"/*.patch | sha256sum | cut -d' ' -f1)"
stamp="$src_dir/.cork-patches-applied"

if [ ! -f "$stamp" ] || [ "$(cat "$stamp")" != "$series_hash" ]; then
    echo "==> applying patch series"
    git -C "$src_dir" checkout -- .
    git -C "$src_dir" clean -fdq -e .cork-patches-applied
    while read -r name; do
        [ -z "$name" ] && continue
        case "$name" in \#*) continue ;; esac
        echo "    $name"
        git -C "$src_dir" apply "$patch_dir/$name"
    done < "$patch_dir/series"
    printf '%s' "$series_hash" > "$stamp"
else
    echo "==> patch series already applied"
fi

# --- configure ---------------------------------------------------------------
mapfile -t configure_flags < <(grep -v '^\s*#' "$flags_file" | grep -v '^\s*$')

# Дерево сборки сохраняется между запусками: повторная сборка Wine после правки
# одного патча занимает минуту вместо получаса. --clean нужен, когда меняются
# флаги configure или тег подмодуля.
if [ "$clean_build" -eq 1 ]; then
    rm -rf "$build_dir"
fi
mkdir -p "$build_dir"
cd "$build_dir"

if [ ! -f Makefile ] || [ "$src_dir/configure" -nt Makefile ] || [ "$flags_file" -nt Makefile ]; then
    echo "==> configure (${#configure_flags[@]} flags, prefix $install_prefix)"
    configure_env=()
    [ -n "$pe_cc" ] && configure_env+=("x86_64_CC=$pe_cc")
    env "${configure_env[@]}" "$src_dir/configure" \
        "${configure_flags[@]}" \
        --prefix="$install_prefix"
else
    echo "==> configure up to date"
fi

echo "==> make -j$jobs"
make -j"$jobs"

echo "==> install into $out_dir"
rm -rf "$out_dir"
make install DESTDIR="$out_dir"

staged="$out_dir$install_prefix"

# --- Wine Mono ---------------------------------------------------------------
# Без Mono MSBuild.exe не стартует вообще: mscoree.dll перехватывает любой PE с
# заголовком CLR на уровне загрузчика и требует совпадающей версии Wine Mono
# прежде, чем отдать управление настоящему рантайму .NET, который и так едет
# вместе с MSBuild.
mono_archive="$repo_root/build/wine-mono-$mono_version-x86.tar.xz"
if [ ! -f "$mono_archive" ]; then
    echo "==> fetching Wine Mono $mono_version"
    curl -fsSL -o "$mono_archive.part" \
        "https://dl.winehq.org/wine/wine-mono/$mono_version/wine-mono-$mono_version-x86.tar.xz"
    mv "$mono_archive.part" "$mono_archive"
fi
mkdir -p "$staged/share/wine/mono"
tar -xf "$mono_archive" -C "$staged/share/wine/mono"

# --- отладочная информация ---------------------------------------------------
# Вынимается отдельным скриптом, чтобы её можно было вынуть и на уже собранном
# дереве, не пересобирая Wine. Обоснование — там же.
if [ "$strip_debug" = 1 ]; then
    "$repo_root/tools/wine/split-debug.sh" "$staged" --debug-out "$out_dir-debug$install_prefix"
fi

# --- devel-часть -------------------------------------------------------------
# Заголовки, импорт-библиотеки и winegcc уезжают в отдельное дерево: на машине
# пользователя они не нужны ни разу, PE-хелпер собирается здесь. Состав задан
# списком в wine/split-rules.txt, и файл, не подошедший ни под одно правило,
# останавливает сборку — иначе состав артефакта определялся бы тем, что
# `make install` положил сегодня.
if [ "$split_devel" = 1 ]; then
    "$repo_root/tools/wine/split-devel.sh" "$staged" --sdk-out "$out_dir-sdk$install_prefix"
fi

# --- дымовая проверка --------------------------------------------------------
# После strip и разделения, а не до: проверять надо то, что поедет
# пользователю.
echo "==> smoke test"
"$staged/bin/wine" --version

echo
echo "Wine staged at: $staged"
echo "  wine:      $("$staged/bin/wine" --version)"
echo "  mono:      $mono_version"
echo "  patches:   $series_hash"
