# shellcheck shell=sh
# Окружение для сборки cork. Подключается точкой, не запускается:
#
#     . tools/toolchain_env.sh
#
# VCPKG_ROOT на этой машине намеренно не экспортируется глобально — каждый
# проект задаёт его сам в своей сборочной сессии. Здесь он указывает на общий
# клон в .workspace, тот же, которым пользуется AeonLC: так порты собираются
# один раз на всю машину.

_cork_vcpkg_default="/home/cheviiot/Project/.workspace/vcpkg"

if [ -z "${VCPKG_ROOT:-}" ]; then
    if [ -d "$_cork_vcpkg_default" ]; then
        export VCPKG_ROOT="$_cork_vcpkg_default"
    else
        echo "toolchain_env.sh: не найден vcpkg в $_cork_vcpkg_default;" >&2
        echo "  задайте VCPKG_ROOT вручную перед подключением этого файла." >&2
    fi
fi

# Телеметрия vcpkg не нужна и в офлайне только мешает.
export VCPKG_DISABLE_METRICS=1

unset _cork_vcpkg_default

if [ -n "${VCPKG_ROOT:-}" ]; then
    echo "VCPKG_ROOT=$VCPKG_ROOT"
fi
