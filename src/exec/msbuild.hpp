#pragma once

// Окружение и аргументы для MSBuild.
//
// Почему этого не хватает того, что нужно cl и link. Они берут пути из
// INCLUDE и LIB, и этого достаточно. MSBuild ищет и компилятор, и Windows SDK
// совсем иначе — через реестр Windows, которого под Wine нет. Свойство
// DisableRegistryUse переводит этот поиск на переменные окружения, и дальше
// приходится выставить всё, что он ожидал найти в реестре.
//
// Всё, что здесь перечислено, добыто отладкой конкретных отказов, а не выведено
// из документации: у каждой переменной есть код ошибки, который она гасит, и
// файл целей, который её читает. Поэтому каждая снабжена ссылкой на причину —
// иначе через полгода набор будет выглядеть случайным, и его начнут «чистить».

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "setup/config.hpp"
#include "setup/layout.hpp"

namespace cork::exec {

// Переменные окружения сверх обычных INCLUDE/LIB/WINEPATH.
[[nodiscard]] std::map<std::string, std::string> msbuild_env(
    const setup::Config &, const std::filesystem::path &generation_root,
    const std::string &target_arch);

// Глобальные свойства, которые надо навязать через командную строку, если
// вызывающий не задал их сам.
[[nodiscard]] std::vector<std::string> msbuild_forced_args(
    const setup::Config &, const std::vector<std::string> &args);

// Имя платформы MSBuild для нашей архитектуры: x86 там зовётся Win32.
[[nodiscard]] std::string msbuild_platform(const std::string &arch);

// Все числовые имена наборов инструментов, которые может спросить MSBuild.
[[nodiscard]] std::vector<std::string> toolset_suffixes(
    const std::filesystem::path &generation_root);

} // namespace cork::exec
