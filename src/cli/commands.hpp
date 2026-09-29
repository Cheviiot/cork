#pragma once

// Подкоманды cork. Всё, что печатает пользователю, живёт здесь: слои ниже
// возвращают значения и не знают ни про stdout, ни про язык интерфейса.

#include <filesystem>
#include <string>
#include <vector>

#include "setup/generation.hpp"

namespace cork::cli {

// Доводит установку до готовой к сборке: строит шаблон префикса, если его
// нет или он отстал от этой сборки Wine. Зовётся из install, чтобы человеку
// не оставалось ручного шага между «установлено» и «можно собирать».
int ensure_prefix_template(setup::Root &);

// Путь к своей сборке Wine у текущего поколения. Живёт здесь, а не в слое
// setup, потому что нужен только командам и только чтобы гасить wineserver
// сессии её же сервером.
std::filesystem::path wine_runtime_of(const setup::Root &);

int cmd_setup(const std::vector<std::string> &args);
int cmd_download(const std::vector<std::string> &args);
int cmd_install(const std::vector<std::string> &args);
int cmd_doctor(const std::vector<std::string> &args);
int cmd_gc(const std::vector<std::string> &args);
int cmd_run(const std::vector<std::string> &args);
int cmd_session(const std::vector<std::string> &args);
int cmd_template(const std::vector<std::string> &args);
int cmd_env(const std::vector<std::string> &args);
int cmd_completion(const std::vector<std::string> &args);
int cmd_version();
int cmd_help(int exit_code);

} // namespace cork::cli
