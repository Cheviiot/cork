#pragma once

// Раскладка установленного дерева и производные от неё пути окружения.
//
// Один источник для install (он записывает пути в конфигурацию) и для
// обёрток (они собирают из них INCLUDE, LIB и WINEPATH). Вычислять это в двух
// местах значило бы рано или поздно разойтись.

#include <filesystem>
#include <string>

#include "setup/config.hpp"

namespace cork::setup {

// Пути внутри поколения, относительные — такими они и лежат в конфигурации,
// чтобы дерево оставалось переносимым.
TargetPaths derive_target_paths(std::string_view msvc_version, std::string_view sdk_version,
                                std::string_view host_arch, std::string_view target_arch,
                                std::string_view dotnet_host);

// Переменные окружения, которые нужны Wine-хостируемым инструментам. Пути
// здесь уже в DOS-нотации: INCLUDE и LIB читает сам компилятор, и unix-путей
// он не понимает.
struct ToolEnvironment {
    std::string include;
    std::string lib;
    std::string lib_path;
    std::string wine_path;
    std::string wine_dll_overrides;
};

ToolEnvironment derive_environment(const Config &, const std::filesystem::path &generation_root,
                                   std::string_view target_arch);

// Unix-путь в DOS-нотацию через диск Z:. Используется только там, где нельзя
// спросить Wine: в переменных окружения, которые выставляются до запуска
// процесса. Внутри хелпера для этого есть wine_get_dos_file_name, который
// учитывает настоящую таблицу дисков префикса.
std::string to_wine_path(const std::filesystem::path &);

} // namespace cork::setup
