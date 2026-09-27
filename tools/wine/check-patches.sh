#!/usr/bin/env bash
# Проверяет целостность серии патчей к Wine, ничего не меняя на диске.
#
# Дешёвый страж при бампе Wine: подмодуль двигают одной командой, и без этой
# проверки расхождение всплывёт только через полчаса сборки. Подмодуль нужен,
# собранный Wine — нет.
#
# Дерево после tools/wine/build.sh остаётся пропатченным, поэтому проверка
# понимает оба состояния: на чистом дереве патчи проверяются прямым
# применением, на пропатченном — обратным. Второе заодно доказывает, что на
# диске лежит ровно эта серия, а не что-то похожее.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
src_dir="$repo_root/wine/src"
patch_dir="$repo_root/wine/patches"
stamp="$src_dir/.cork-patches-applied"

if [ ! -f "$src_dir/configure" ]; then
    echo "wine/src is empty; run: git submodule update --init --depth 1 wine/src" >&2
    exit 1
fi

series_hash="$(cat "$patch_dir/series" "$patch_dir"/*.patch | sha256sum | cut -d' ' -f1)"

direction="forward"
if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$series_hash" ]; then
    direction="reverse"
elif [ -n "$(git -C "$src_dir" status --porcelain --untracked-files=no)" ]; then
    echo "wine/src is modified but not by this exact series." >&2
    echo "Reset it first: git -C wine/src checkout -- . && rm -f wine/src/.cork-patches-applied" >&2
    exit 1
fi

echo "series hash: $series_hash"
echo "tree state:  $([ "$direction" = reverse ] && echo "patched" || echo "pristine")"

check_args=(apply --check)
[ "$direction" = "reverse" ] && check_args+=(--reverse)

failed=0
while read -r name; do
    [ -z "$name" ] && continue
    case "$name" in \#*) continue ;; esac
    if [ ! -f "$patch_dir/$name" ]; then
        echo "MISSING $name (listed in series but not present)" >&2
        failed=1
        continue
    fi
    # Патчи серии трогают разные файлы — это её сознательное свойство, и оно
    # позволяет проверять каждый независимо, не выстраивая их поверх друг друга.
    if git -C "$src_dir" "${check_args[@]}" "$patch_dir/$name"; then
        echo "ok      $name"
    else
        echo "FAILED  $name" >&2
        failed=1
    fi
done < "$patch_dir/series"

# Забытый в каталоге патч молча не применяется, и собранный Wine отличается от
# ожидаемого.
for f in "$patch_dir"/*.patch; do
    base="$(basename "$f")"
    if ! grep -qxF "$base" "$patch_dir/series"; then
        echo "ORPHAN  $base (present but not listed in series)" >&2
        failed=1
    fi
done

exit "$failed"
