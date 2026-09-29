# cork — кросс-компиляция настоящим MSVC на Linux через Wine.
#
# Планка 3.28, а не 4.4.3 как у AeonLC: в контейнере cork-dev (Fedora 44)
# стоит cmake 4.3.0, на хосте 3.31.7, на раннерах GitHub — около 3.31. Проект не
# должен требовать предварительной установки пиннутого cmake, иначе «собрать
# одной командой» перестаёт быть правдой.
cmake_minimum_required(VERSION 3.28)

project(cork
    VERSION 0.1.0
    DESCRIPTION "Cross compile with the real MSVC toolchain on Linux via Wine"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Модулей у нас нет, а сканирование на них стоит времени на каждой сборке.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/Warnings.cmake)
include(cmake/EmbedAssets.cmake)

option(CORK_SANITIZE "Список санитайзеров для -fsanitize, пусто — выключено" "")

find_package(fmt CONFIG REQUIRED)
find_package(simdjson CONFIG REQUIRED)
find_package(libzip CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)

# libmspack не поставляет из vcpkg ни CMake-конфига, ни файла pkg-config: порт
# кладёт только заголовок и liblibmspack.a. Поэтому цель собирается вручную —
# иначе пришлось бы либо править порт, либо линковать библиотеку по голому пути
# и молча потерять её при смене триплета.
find_library(CORK_MSPACK_LIBRARY NAMES libmspack mspack REQUIRED)
find_path(CORK_MSPACK_INCLUDE_DIR NAMES mspack.h REQUIRED)
add_library(cork::mspack UNKNOWN IMPORTED)
set_target_properties(cork::mspack PROPERTIES
    IMPORTED_LOCATION "${CORK_MSPACK_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${CORK_MSPACK_INCLUDE_DIR}")

# Сборка Wine, под которую собран вшитый хелпер. Задаётся здесь, а не в коде:
# это свойство сборки, и разъехавшись с настоящим артефактом, оно даст отказ
# на установке, а не тихо неверный запуск.
set(CORK_WINE_ID "11.18-mono11.3.0" CACHE STRING "Wine runtime this build expects")

# Путь к собранному PE-хелперу; пусто — искать в build/helper. Отсутствие не
# ошибка: получается бинарник, который честно об этом сообщает.
set(CORK_HELPER "" CACHE FILEPATH "Built cork-helper.exe to embed")
cork_generate_embedded_helper("${CMAKE_BINARY_DIR}/generated/cork_embedded_helper.h")
cork_generate_embedded_toolchain("${CMAKE_BINARY_DIR}/generated/cork_embedded_toolchain.h")
cork_generate_embedded_runtime_pins("${CMAKE_BINARY_DIR}/generated/cork_embedded_runtime_pins.h")

# Ревизия снимается на этапе конфигурации, а не сборки. Это значит, что после
# коммита без переконфигурации строка версии останется прежней — сознательный
# размен: гонять git на каждой сборке ради этого дороже, а в CI и в релизе
# конфигурация всегда свежая.
find_package(Git QUIET)
set(CORK_GIT_REVISION "")
set(CORK_GIT_DIRTY false)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE CORK_GIT_REVISION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE cork_git_status
        ERROR_QUIET)
    if(NOT cork_git_status STREQUAL "")
        set(CORK_GIT_DIRTY true)
    endif()
endif()

# Вся логика живёт в статической библиотеке, а исполняемый файл — только точка
# входа. Так тесты линкуются с той же самой сборкой кода, что и продукт, а не с
# отдельно скомпилированными копиями.
add_library(cork_core STATIC
    src/archive/cab.cpp
    src/archive/cfbf.cpp
    src/archive/msi.cpp
    src/archive/msi_extract.cpp
    src/archive/zip.cpp
    src/i18n/messages.cpp
    src/exec/clpost.cpp
    src/exec/filters.cpp
    src/exec/msbuild.cpp
    src/exec/response.cpp
    src/exec/session.cpp
    src/exec/shims.cpp
    src/exec/runner.cpp
    src/exec/tools.cpp
    src/base/error.cpp
    src/base/clone.cpp
    src/base/fs.cpp
    src/base/json.cpp
    src/base/lock.cpp
    src/base/md5.cpp
    src/base/sha256.cpp
    src/base/version.cpp
    src/net/http.cpp
    src/cli/doctor.cpp
    src/cli/download.cpp
    src/cli/install.cpp
    src/cli/run.cpp
    src/cli/shell.cpp
    src/cli/spec.cpp
    src/setup/config.cpp
    src/setup/install.cpp
    src/setup/relocate.cpp
    src/setup/digest.cpp
    src/setup/doctor.cpp
    src/setup/generation.cpp
    src/setup/layout.cpp
    src/setup/prefix.cpp
    src/setup/receipt.cpp
    src/setup/runtime.cpp
    src/store/store.cpp
    src/manifest/index.cpp
    src/manifest/model.cpp
    src/manifest/parse.cpp
    src/manifest/version.cpp
    src/proto/protocol.cpp
    src/resolve/resolve.cpp
)
target_include_directories(cork_core PUBLIC src ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(cork_core PUBLIC fmt::fmt PRIVATE simdjson::simdjson libzip::zip CURL::libcurl cork::mspack)
target_compile_definitions(cork_core PUBLIC
    CORK_VERSION="${PROJECT_VERSION}"
    CORK_GIT_REVISION="${CORK_GIT_REVISION}"
    CORK_GIT_DIRTY=$<IF:$<BOOL:${CORK_GIT_DIRTY}>,true,false>
    CORK_WINE_ID="${CORK_WINE_ID}"
)
# -Wno-c++26-extensions: #embed пришёл из C23 и попадёт в C++ только в C++26,
# так что gcc считает его расширением. Снять, когда проект перейдёт на C++26;
# признак — предупреждение перестанет появляться без флага. Стоит на файлах, а
# не на цели: их два, и оба — те, что читают вшитые ресурсы.
set_source_files_properties(src/cli/install.cpp src/setup/runtime.cpp PROPERTIES
    COMPILE_OPTIONS -Wno-c++26-extensions)

cork_set_warnings(cork_core)

add_executable(cork src/main.cpp)
target_link_libraries(cork PRIVATE cork_core)
cork_set_warnings(cork)

if(CORK_SANITIZE)
    foreach(target cork_core cork)
        target_compile_options(${target} PUBLIC -fsanitize=${CORK_SANITIZE} -fno-omit-frame-pointer)
        target_link_options(${target} PUBLIC -fsanitize=${CORK_SANITIZE})
    endforeach()
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
