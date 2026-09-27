#pragma once

// Версия приходит из project(VERSION) через CMake — единый источник правды,
// чтобы `cork version` и метаданные пакета не могли разойтись.
//
// Коммит и время сборки прокидываются явно из CMake: в C++ нет ничего вроде
// встроенных метаданных сборки, откуда их можно было бы взять. По отчёту об
// ошибке нужно знать точный коммит, а не только тег — теги двигают, а из
// грязного дерева собирают.

#include <string>
#include <string_view>

namespace cork {

inline constexpr const char *kVersion = CORK_VERSION;
inline constexpr const char *kGitRevision = CORK_GIT_REVISION;
inline constexpr bool kGitDirty = CORK_GIT_DIRTY;

// Сборка Wine, под которую собран вшитый PE-хелпер. Печатается `cork version`,
// потому что это первое, что нужно знать при разборе «почему не запускается»:
// какой именно рантайм этот бинарник ищет.
inline constexpr const char *kDefaultWineId = CORK_WINE_ID;

// Чистая функция, отделённая от сбора данных, чтобы её можно было проверить
// тестом на всех сочетаниях: без ревизии, с ревизией, с грязным деревом.
[[nodiscard]] std::string format_version(std::string_view version, std::string_view revision,
                                         bool dirty);

[[nodiscard]] std::string version_string();

// Id рантайма Wine для этого запуска: CORK_WINE_ID из окружения, иначе тот, с
// которым собран хелпер.
[[nodiscard]] std::string wine_runtime_id();

} // namespace cork
