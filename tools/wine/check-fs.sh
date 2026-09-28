#!/bin/sh
# Проверка того, что cl /Zi /FS работает: свежие префиксы, только первые
# попытки, счёт успехов.
#
# Почему так, а не одним запуском. Префикс, в котором /FS уже отработал,
# держит выживший mspdbsrv и проходит всегда — замер в нём показывает успех
# независимо от того, что проверяли. И отказ здесь не детерминирован: в
# худшем из измеренных вариантов он случается два раза из шести, а такую
# разницу одиночный опыт показывает как чистую победу. Шесть неверных
# объяснений этого дефекта выросли ровно из этих двух ошибок.
#
# Проверяются оба случая, потому что ведут они себя по-разному:
#   * пустой префикс — тот, что загружается с нуля;
#   * клон шаблона — то, из чего делается каждая сборочная сессия.
#
# Использование:
#   check-fs.sh <каталог сборки Wine> [-n ПОВТОРОВ] [-t КАТАЛОГ_ИНСТРУМЕНТОВ]
set -eu

wine_root=${1:?укажите каталог с bin/wine}
shift
runs=6
tools=

while [ $# -gt 0 ]; do
    case $1 in
        -n) runs=$2; shift 2 ;;
        -t) tools=$2; shift 2 ;;
        *) echo "неизвестный аргумент: $1" >&2; exit 2 ;;
    esac
done

# По умолчанию — настоящие инструменты текущего поколения, а не обёртки из
# bin/x64: обёртка завела бы свою сессию и свой префикс, то есть проверяла бы
# не то, ради чего эта проверка написана.
generation=$HOME/.cork/toolchains/current
if [ -z "$tools" ]; then
    tools=$generation/$(sed -n 's/.*"bin": "\(vc\/tools\/msvc\/[^"]*x64\)".*/\1/p' \
        "$generation/cork.json" | head -1)
fi
[ -f "$tools/cl.exe" ] || { echo "нет cl.exe в '$tools'" >&2; exit 2; }

wine_bin=$wine_root/bin/wine
[ -x "$wine_bin" ] || { echo "нет $wine_bin" >&2; exit 2; }

template=$(ls -d "$HOME"/.cork/prefix/* 2>/dev/null | head -1 || true)

work=$HOME/.cork/tmp/check-fs.$$
trap 'rm -rf "$work"' EXIT INT TERM
mkdir -p "$work"
printf 'int main(void){return 0;}\n' > "$work/a.c"

# Одна попытка: свежий префикс, одна компиляция, префикс за собой убирается.
# Код возврата и есть результат.
attempt() {
    dir=$work/p.$1.$2
    mkdir -p "$dir"
    if [ "$1" = clone ]; then
        cp -a --reflink=auto "$template" "$dir/prefix"
    else
        WINEPREFIX=$dir/prefix WINEDEBUG=-all "$wine_root/bin/wineboot" --init >/dev/null 2>&1
        WINEPREFIX=$dir/prefix "$wine_root/bin/wineserver" -w
    fi
    cp "$work/a.c" "$dir/a.c"
    cd "$dir"
    rc=0
    WINEPREFIX=$dir/prefix WINEDEBUG=-all WINEPATH=$tools \
        WINEDLLOVERRIDES='vcruntime140=n;vcruntime140_1=n' \
        "$wine_bin" "$tools/cl.exe" /nologo /Zi /FS /c a.c > out.txt 2>&1 || rc=$?
    [ -f a.obj ] || rc=${rc:-1}
    WINEPREFIX=$dir/prefix "$wine_root/bin/wineserver" -k >/dev/null 2>&1 || true
    cd "$work"
    rm -rf "$dir"
    return "$rc"
}

status=0
for kind in empty clone; do
    if [ "$kind" = clone ] && [ -z "$template" ]; then
        echo "клон шаблона  : пропущен, шаблона нет"
        continue
    fi
    ok=0
    i=1
    while [ "$i" -le "$runs" ]; do
        if attempt "$kind" "$i"; then ok=$((ok + 1)); fi
        i=$((i + 1))
    done
    case $kind in
        empty) name="пустой префикс" ;;
        clone) name="клон шаблона  " ;;
    esac
    echo "$name: успехов $ok из $runs"
    [ "$ok" -eq "$runs" ] || status=1
done

exit "$status"
