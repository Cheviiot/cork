// Точка входа. Бинарник многоликий, как busybox: под именем cl, link или
// msbuild он становится соответствующим инструментом MSVC, а под собственным
// именем — управляющей утилитой.

#include <filesystem>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "base/version.hpp"
#include "cli/commands.hpp"
#include "cli/spec.hpp"
#include "exec/runner.hpp"
#include "exec/shims.hpp"
#include "exec/tools.hpp"

namespace {

std::string invoked_name(const char *argv0) {
    if (argv0 == nullptr) {
        return {};
    }
    return std::filesystem::path(argv0).filename().string();
}

} // namespace

namespace cork::cli {

int cmd_version() {
    fmt::print("{}\n", version_string());
    // Вторая строка — не украшение: при разборе «Wine не тот» первым делом
    // нужно знать, какой рантайм этот бинарник вообще ищет, и гадать об этом
    // по исходникам не должен никто, включая CI.
    fmt::print("wine runtime {}\n", wine_runtime_id());
    // Хеш берётся у install.cpp, а не считается здесь: сырой ассет читают
    // ровно два файла, и список -Wno-c++26-extensions не должен расти от
    // того, что кому-то понадобилось его имя.
    fmt::print("{}\n", format_helper_line(embedded_helper_sha256()));
    return 0;
}

int cmd_help(int exit_code) {
    auto &out = exit_code == 0 ? stdout : stderr;
    fmt::print(out, "{}", render_help());
    return exit_code;
}

} // namespace cork::cli

int main(int argc, char **argv) {
    const std::string self = invoked_name(argc > 0 ? argv[0] : nullptr);
    std::vector<std::string> args(argv + (argc > 0 ? 1 : 0), argv + argc);

    // Вызов под именем инструмента: обёртка. Проверяется раньше подкоманд,
    // потому что именно так её и зовут сборочные системы.
    if (cork::exec::find_tool(self) != nullptr) {
        return cork::exec::run_tool(self, args, {});
    }
    if (cork::exec::is_native_shim(self)) {
        return cork::exec::run_shim(cork::exec::shim_name(self), args);
    }

    if (args.empty()) {
        return cork::cli::cmd_help(1);
    }

    const std::string command = args.front();
    const std::vector<std::string> rest(args.begin() + 1, args.end());

    // «cork cl ...» — тот же инструмент, но без нужды класть каталог в PATH.
    if (cork::exec::find_tool(command) != nullptr) {
        return cork::exec::run_tool(command, rest, {});
    }

    if (command == "setup") {
        return cork::cli::cmd_setup(rest);
    }
    if (command == "download" || command == "dl") {
        return cork::cli::cmd_download(rest);
    }
    if (command == "install" || command == "i") {
        return cork::cli::cmd_install(rest);
    }
    if (command == "doctor") {
        return cork::cli::cmd_doctor(rest);
    }
    if (command == "gc") {
        return cork::cli::cmd_gc(rest);
    }
    if (command == "run") {
        return cork::cli::cmd_run(rest);
    }
    if (command == "session") {
        return cork::cli::cmd_session(rest);
    }
    if (command == "template") {
        return cork::cli::cmd_template(rest);
    }
    if (command == "env") {
        return cork::cli::cmd_env(rest);
    }
    if (command == "completion") {
        return cork::cli::cmd_completion(rest);
    }
    if (command == "version" || command == "v" || command == "--version") {
        return cork::cli::cmd_version();
    }
    if (command == "help" || command == "h" || command == "-h" || command == "--help") {
        // `cork help <команда>` печатает её ключи. Без этого справка либо
        // разрастается до нечитаемого полотна, либо умалчивает о ключах.
        if (!rest.empty()) {
            if (const auto *spec = cork::cli::find_command(rest.front())) {
                fmt::print("{}", cork::cli::render_command_help(*spec));
                return 0;
            }
            fmt::print(stderr, "cork: no command named \"{}\"\n\n", rest.front());
            return cork::cli::cmd_help(1);
        }
        return cork::cli::cmd_help(0);
    }

    fmt::print(stderr, "cork: unknown command \"{}\"\n\n", command);
    return cork::cli::cmd_help(1);
}
