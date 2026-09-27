#pragma once

// Подкоманды cork. Всё, что печатает пользователю, живёт здесь: слои ниже
// возвращают значения и не знают ни про stdout, ни про язык интерфейса.

#include <string>
#include <vector>

namespace cork::cli {

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
