# Вшивание PE-хелпера в бинарник cork.
#
# Хелпер собирается отдельно (tools/wine/build-helper.sh) тем же wineg++, что
# и Wine, и обычная сборка проекта его не производит. Поэтому путь к нему —
# параметр, а отсутствие — не ошибка сборки: получается бинарник, который
# честно сообщает, что хелпер не вшит, вместо того чтобы молча не собираться.
#
# Используется #embed: та же схема, что и в других проектах на этой машине.
# Массив в виде текста на 160 КБ компилятор жуёт заметно дольше.

function(cork_generate_embedded_helper output_file)
    set(helper "${CORK_HELPER}")
    if(helper STREQUAL "")
        set(helper "${CMAKE_SOURCE_DIR}/build/helper/cork-helper.exe")
    endif()

    if(EXISTS "${helper}")
        message(STATUS "cork: embedding PE helper from ${helper}")
        set(body "#embed \"${helper}\"")
        set(present "true")
    else()
        message(STATUS "cork: no PE helper at ${helper}; building without it "
                       "(run tools/wine/build-helper.sh, then re-configure)")
        # Пустой массив недопустим, поэтому один байт-заглушка; признак
        # отсутствия несёт отдельный флаг.
        set(body "0")
        set(present "false")
    endif()

    file(WRITE "${output_file}"
"// Создаётся CMake, не редактировать.
#pragma once

#include <cstddef>
#include <span>

namespace cork::assets {

inline constexpr bool kHelperEmbedded = ${present};

inline constexpr unsigned char kHelperBytes[] = {
${body}
};

inline std::span<const std::byte> helper() {
    if constexpr (!kHelperEmbedded) {
        return {};
    }
    return std::span<const std::byte>(reinterpret_cast<const std::byte *>(kHelperBytes),
                                      sizeof kHelperBytes);
}

} // namespace cork::assets
")

    # Переконфигурация при смене хелпера: без этого пересборка после
    # tools/wine/build-helper.sh вшила бы старый.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${helper}")
endfunction()

# Файл тулчейна CMake выкладывается в поколение той же дорогой, что и хелпер.
# Он текстовый и маленький, но лежать должен рядом с инструментами, к которым
# относится: поколений может быть несколько, и один файл на всех указывал бы
# не туда после следующей установки.
function(cork_generate_embedded_toolchain output_file)
    set(source "${CMAKE_SOURCE_DIR}/share/cork-toolchain.cmake")
    file(WRITE "${output_file}"
"// Создаётся CMake, не редактировать.
#pragma once

#include <cstddef>
#include <span>

namespace cork::assets {

inline constexpr unsigned char kCMakeToolchainBytes[] = {
#embed \"${source}\"
};

inline std::span<const std::byte> cmake_toolchain() {
    return std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(kCMakeToolchainBytes),
        sizeof kCMakeToolchainBytes);
}

} // namespace cork::assets
")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
endfunction()

# Список закреплённых артефактов рантайма. Вшивается по той же причине, что и
# каталоги локализации: без него первая установка невозможна, а отдельный файл
# рядом с программой теряется при копировании.
function(cork_generate_embedded_runtime_pins output_file)
    set(source "${CMAKE_SOURCE_DIR}/wine/runtime-pins.txt")
    file(WRITE "${output_file}"
"// Создаётся CMake, не редактировать.
#pragma once

#include <cstddef>
#include <span>

namespace cork::assets {

inline constexpr unsigned char kRuntimePinsBytes[] = {
#embed \"${source}\"
};

inline std::span<const std::byte> runtime_pins() {
    return std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(kRuntimePinsBytes),
        sizeof kRuntimePinsBytes);
}

} // namespace cork::assets
")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
endfunction()
