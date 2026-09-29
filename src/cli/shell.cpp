// Команды, обслуживающие оболочку: env и completion.
//
// Обе выводят текст, который пользователь скармливает своему shell, и обе
// строятся из одного источника — таблицы команд. Скрипт автодополнения,
// собранный вручную, расходится с настоящим разборщиком молча: человек видит,
// что существующий ключ не дополняется, и решает, что его нет.

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "cli/commands.hpp"
#include "cli/spec.hpp"
#include "i18n/messages.hpp"
#include "setup/config.hpp"
#include "setup/generation.hpp"
#include "setup/layout.hpp"

namespace cork::cli {
namespace {

namespace stdfs = std::filesystem;

// Кавычки для POSIX-оболочки: строка в одинарных кавычках, а сама одинарная
// кавычка закрывает их, вставляется экранированной и открывает снова.
std::string shell_quote(std::string_view s) {
    std::string out = "'";
    for (const char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out += "'";
    return out;
}

// zsh читает описание как часть спецификации, а не как обычную строку: в
// _describe двоеточие отделяет имя от описания, в _arguments описание
// заканчивается закрывающей скобкой. Апостроф в «Microsoft's» ломал весь
// скрипт молча — оболочка просто не дополняла ничего.
std::string zsh_escape(std::string_view s, char special) {
    std::string out;
    for (const char c : s) {
        if (c == special || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

std::string detect_shell() {
    if (const char *s = std::getenv("SHELL"); s != nullptr) {
        const std::string_view path(s);
        for (const char *known : {"fish", "zsh", "bash"}) {
            if (path.find(known) != std::string_view::npos) {
                return known;
            }
        }
    }
    return "bash";
}

} // namespace

// Версия, которую понимает -fms-compatibility-version, из версии набора
// инструментов. 14.51.36231 -> 19.51: первое число компилятора на единицу
// больше десятки набора, второе совпадает. Соотношение держится у Microsoft
// с 2015 года и записано здесь, потому что вывести его из чего-то ещё
// нельзя, а ошибка в нём тихо меняет значение _MSC_VER в редакторе.
std::string msc_version_of(std::string_view msvc_version) {
    const auto dot = msvc_version.find('.');
    if (dot == std::string_view::npos) {
        return {};
    }
    const std::string_view major = msvc_version.substr(0, dot);
    const auto second = msvc_version.find('.', dot + 1);
    const std::string_view minor = msvc_version.substr(
        dot + 1, second == std::string_view::npos ? std::string_view::npos : second - dot - 1);
    if (major != "14" || minor.empty()) {
        return {};
    }
    return "19." + std::string(minor);
}

int cmd_env(const std::vector<std::string> &args) {
    setup::Root root = setup::Root::from_environment();
    std::string arch;
    std::string shell = detect_shell();
    bool clangd = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--root" && i + 1 < args.size()) {
            root.base = stdfs::absolute(args[++i]);
        } else if (args[i] == "--arch" && i + 1 < args.size()) {
            arch = args[++i];
        } else if (args[i] == "--clangd") {
            clangd = true;
        } else if (args[i] == "--shell" && i + 1 < args.size()) {
            shell = args[++i];
        } else if (args[i] == "-h" || args[i] == "--help") {
            fmt::print("{}", render_command_help(*find_command("env")));
            return 0;
        } else {
            fmt::print(stderr, "cork env: unknown option {}\n", args[i]);
            return 2;
        }
    }

    auto current = root.resolve_current();
    if (!current.has_value()) {
        fmt::print(stderr, "cork env: {}\n", current.error().to_string());
        return 1;
    }
    auto cfg = setup::Config::load(*current);
    if (!cfg.has_value()) {
        fmt::print(stderr, "cork env: {}\n", cfg.error().to_string());
        return 1;
    }
    if (arch.empty()) {
        arch = cfg->host_arch;
    }
    // Написание приводится к нашему: человек, пришедший из clang или rust,
    // наберёт «x86_64-pc-windows-msvc», и отвечать ему «нет такой цели» было
    // бы неправдой — цель есть, не совпало написание.
    if (std::string canonical = setup::normalise_target(arch); !canonical.empty()) {
        arch = std::move(canonical);
    }
    if (cfg->targets.find(arch) == cfg->targets.end()) {
        fmt::print(stderr, "cork env: no target '{}' in this installation\n", arch);
        return 1;
    }

    // Путь ведёт через current, а не в конкретное поколение: оболочку
    // настраивают один раз, а поколение меняется при каждой установке, и
    // записанный намертво путь устареет молча. Проверка выше нужна ровно для
    // того, чтобы не выдать ссылку, за которой ничего нет.
    const stdfs::path bin = root.current() / "bin" / arch;
    // INCLUDE, LIB и прочее обёртки выставляют себе сами при каждом запуске,
    // и вынести их в окружение оболочки значило бы завести второй источник
    // истины, который разойдётся с первым после следующей установки.
    // CORK_TOOLCHAIN_FILE — исключение из правила выше, и оно осознанное.
    // Его читают не обёртки, а cmake пользователя, и второго источника истины
    // тут не возникает: путь тот же самый current, что и у PATH.
    const stdfs::path toolchain = root.current() / "share" / "cork-toolchain.cmake";

    // Настройка для clangd — то, чего не хватает редактору.
    //
    // clangd опознаёт обёртку `cl` и сам ставит режим MSVC, но заголовки не
    // находит ни одного: их расположение компилятор Microsoft берёт из
    // INCLUDE, а обёртка выставляет его только дочернему процессу и только
    // на время компиляции. В редакторе INCLUDE взяться неоткуда, и человек
    // видит `'stdio.h' file not found` в файле, который прекрасно
    // собирается.
    //
    // Лечится тем же /winsysroot, которым пользуется clang-cl: clangd — это
    // clang, и раскладку он понимает. Версия MSVC задаётся явно, иначе clang
    // подставит своё умолчание (19.33 против нашей 19.51) и разойдётся с
    // настоящим компилятором на проверках _MSC_VER.
    if (clangd) {
        fmt::print("# cork env --clangd > .clangd\n");
        fmt::print("CompileFlags:\n");
        fmt::print("  Add:\n");
        fmt::print("    - /winsysroot\n");
        fmt::print("    - {}\n", root.current().string());
        const std::string ms_version = msc_version_of(cfg->msvc_version);
        if (!ms_version.empty()) {
            fmt::print("    - -fms-compatibility-version={}\n", ms_version);
        }
        return 0;
    }

    if (shell == "fish") {
        fmt::print("set -gx PATH {} $PATH\n", shell_quote(bin.string()));
        fmt::print("set -gx CORK_TOOLCHAIN_FILE {}\n", shell_quote(toolchain.string()));
    } else {
        fmt::print("export PATH={}:$PATH\n", shell_quote(bin.string()));
        fmt::print("export CORK_TOOLCHAIN_FILE={}\n", shell_quote(toolchain.string()));
    }
    return 0;
}

int cmd_completion(const std::vector<std::string> &args) {
    std::string shell;
    for (const auto &a : args) {
        if (a == "-h" || a == "--help") {
            fmt::print("{}", render_command_help(*find_command("completion")));
            return 0;
        }
        shell = a;
    }
    if (shell.empty()) {
        shell = detect_shell();
    }

    // Список команд и ключей берётся из таблицы, а не пишется здесь заново.
    std::string commands;
    for (const auto &c : command_table()) {
        if (!commands.empty()) {
            commands += " ";
        }
        commands += c.name;
    }

    if (shell == "bash") {
        fmt::print("# cork completion for bash. Add to ~/.bashrc:\n");
        fmt::print("#   eval \"$(cork completion bash)\"\n");
        fmt::print("_cork() {{\n");
        fmt::print("    local cur prev cmd\n");
        fmt::print("    cur=\"${{COMP_WORDS[COMP_CWORD]}}\"\n");
        fmt::print("    prev=\"${{COMP_WORDS[COMP_CWORD-1]}}\"\n");
        fmt::print("    cmd=\"${{COMP_WORDS[1]}}\"\n");
        fmt::print("    if [ \"$COMP_CWORD\" -eq 1 ]; then\n");
        fmt::print("        COMPREPLY=($(compgen -W \"{}\" -- \"$cur\"))\n", commands);
        fmt::print("        return\n");
        fmt::print("    fi\n");
        // Ключи, которым нужен каталог или файл, дополняются штатными
        // средствами оболочки: своего списка путей у нас нет и быть не может.
        fmt::print("    case \"$prev\" in\n");
        fmt::print("        --root|--store|--generation|--manifest)\n");
        fmt::print("            COMPREPLY=($(compgen -d -- \"$cur\")); return ;;\n");
        fmt::print("        --architecture|--arch)\n");
        fmt::print("            COMPREPLY=($(compgen -W \"x64 x86 arm64\" -- \"$cur\")); return ;;\n");
        fmt::print("        --shell)\n");
        fmt::print("            COMPREPLY=($(compgen -W \"bash zsh fish\" -- \"$cur\")); return ;;\n");
        fmt::print("    esac\n");
        fmt::print("    case \"$cmd\" in\n");
        for (const auto &c : command_table()) {
            std::string options;
            for (const auto &o : c.options) {
                if (!options.empty()) {
                    options += " ";
                }
                options += o.name;
            }
            fmt::print("        {}) COMPREPLY=($(compgen -W \"{}\" -- \"$cur\")) ;;\n", c.name,
                       options);
        }
        fmt::print("    esac\n");
        fmt::print("}}\n");
        fmt::print("complete -F _cork cork\n");
        return 0;
    }

    if (shell == "zsh") {
        fmt::print("# cork completion for zsh. Add to ~/.zshrc:\n");
        fmt::print("#   eval \"$(cork completion zsh)\"\n");
        fmt::print("_cork() {{\n");
        fmt::print("    local -a commands\n");
        fmt::print("    commands=(\n");
        for (const auto &c : command_table()) {
            fmt::print("        {}\n",
                       shell_quote(fmt::format("{}:{}", c.name,
                                               zsh_escape(i18n::tr(c.summary), ':'))));
        }
        fmt::print("    )\n");
        fmt::print("    if (( CURRENT == 2 )); then\n");
        fmt::print("        _describe 'command' commands\n");
        fmt::print("        return\n");
        fmt::print("    fi\n");
        fmt::print("    case \"${{words[2]}}\" in\n");
        for (const auto &c : command_table()) {
            std::string options;
            for (const auto &o : c.options) {
                if (!options.empty()) {
                    options += " ";
                }
                options += shell_quote(
                    fmt::format("{}[{}]", o.name, zsh_escape(i18n::tr(o.help), ']')));
            }
            fmt::print("        {}) _arguments {} ;;\n", c.name, options);
        }
        fmt::print("    esac\n");
        fmt::print("}}\n");
        fmt::print("compdef _cork cork\n");
        return 0;
    }

    if (shell == "fish") {
        fmt::print("# cork completion for fish. Save as ~/.config/fish/completions/cork.fish:\n");
        fmt::print("#   cork completion fish > ~/.config/fish/completions/cork.fish\n");
        for (const auto &c : command_table()) {
            fmt::print("complete -c cork -n __fish_use_subcommand -a {} -d {}\n", c.name,
                       shell_quote(i18n::tr(c.summary)));
        }
        for (const auto &c : command_table()) {
            for (const auto &o : c.options) {
                fmt::print("complete -c cork -n '__fish_seen_subcommand_from {}' -l {} -d {}\n",
                           c.name, o.name.substr(2), shell_quote(i18n::tr(o.help)));
            }
        }
        return 0;
    }

    fmt::print(stderr, "cork completion: unknown shell '{}'; try bash, zsh or fish\n", shell);
    return 2;
}

} // namespace cork::cli
