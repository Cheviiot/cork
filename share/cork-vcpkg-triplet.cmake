# Оверлей-триплет vcpkg для сборки зависимостей под Windows через cork.
#
#   eval "$(cork env)"
#   vcpkg install zlib \
#       --overlay-triplets="$(dirname "$CORK_TOOLCHAIN_FILE")" \
#       --triplet x64-windows
#
# Имя файла должно совпадать с именем триплета, поэтому в поколение он
# кладётся как <arch>-windows.cmake, а не под этим именем.
#
# Зачем вообще: без триплета vcpkg собирает зависимости хостовым
# компилятором, и несовпадение всплывает на линковке чужой библиотеки, где
# разбираться дороже всего.

set(VCPKG_TARGET_ARCHITECTURE @CORK_VCPKG_ARCH@)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

set(VCPKG_CMAKE_SYSTEM_NAME Windows)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "@CORK_TOOLCHAIN_FILE@")

# Цель передаётся в toolchain-файл: без неё он возьмёт умолчание x64, и
# триплет x86-windows молча собрал бы 64-битное.
set(VCPKG_PLATFORM_TOOLSET_VERSION "")
list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS "-DCORK_TARGET=@CORK_TARGET@")
