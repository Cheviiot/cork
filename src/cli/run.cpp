#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include <fmt/format.h>

#include "base/fs.hpp"
#include "base/lock.hpp"
#include "cli/commands.hpp"
#include "exec/runner.hpp"
#include "exec/session.hpp"
#include "i18n/messages.hpp"
#include "setup/config.hpp"
#include "setup/generation.hpp"
#include "setup/prefix.hpp"

namespace cork::cli {
namespace {

namespace stdfs = std::filesystem;

std::string humanize_bytes(std::uint64_t n) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(n);
    std::size_t u = 0;
    while (v >= 1024.0 && u + 1 < std::size(units)) {
        v /= 1024.0;
        ++u;
    }
    return u == 0 ? fmt::format("{} {}", n, units[u]) : fmt::format("{:.1f} {}", v, units[u]);
}

// Путь к своей сборке Wine у текущего поколения. Нужен, чтобы погасить
// wineserver сессии её же сервером, а не чужим.
stdfs::path wine_runtime_of(const setup::Root &root) {
    auto current = root.resolve_current();
    if (!current.has_value()) {
        return {};
    }
    auto cfg = setup::Config::load(*current);
    if (!cfg.has_value() || cfg->wine_id.empty()) {
        return {};
    }
    return root.runtime(cfg->wine_id);
}

// Программа, собранная этой установкой, а не нативная команда. Отличается
// расширением, и этого достаточно: ELF с именем на .exe — случай, которого в
// сборочном дереве под Windows не бывает.
bool looks_like_windows_program(const std::string &program) {
    if (program.size() < 4) {
        return false;
    }
    std::string tail = program.substr(program.size() - 4);
    for (char &c : tail) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return tail == ".exe";
}

setup::Root root_from(const std::vector<std::string> &args, std::size_t &i) {
    setup::Root root = setup::Root::from_environment();
    if (args[i] == "--root" && i + 1 < args.size()) {
        root.base = stdfs::absolute(args[++i]);
    }
    return root;
}


} // namespace

int cmd_run(const std::vector<std::string> &args) {
    std::vector<std::string> command;
    setup::Root root = setup::Root::from_environment();
    std::string key;
    bool keep = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--") {
            command.assign(args.begin() + static_cast<std::ptrdiff_t>(i) + 1, args.end());
            break;
        }
        if (args[i] == "--root" && i + 1 < args.size()) {
            root = root_from(args, i);
        } else if (args[i] == "--session" && i + 1 < args.size()) {
            key = args[++i];
        } else if (args[i] == "--keep") {
            keep = true;
        } else if (args[i] == "-h" || args[i] == "--help") {
            return cmd_help(0);
        } else {
            fmt::print(stderr, "cork run: unexpected argument '{}'; use -- before the command\n",
                       args[i]);
            return 2;
        }
    }

    if (command.empty()) {
        fmt::print(stderr, "cork run: nothing to run.\nUsage: cork run [options] -- <command>\n");
        return 2;
    }

    std::error_code ec;
    // Сессия уже задана снаружи — значит, её кто-то завёл до нас: либо
    // объемлющий `cork run`, либо человек, выставивший CORK_SESSION руками. В
    // обоих случаях убирать её не наше дело. Без этой проверки вложенный
    // вызов сносил бы префикс из-под того, кто его создал, и сборка, внутри
    // которой есть свой `cork run`, разваливалась бы на середине.
    const bool inherited = key.empty() && std::getenv("CORK_SESSION") != nullptr;
    if (inherited) {
        key = std::getenv("CORK_SESSION");
    }
    if (key.empty()) {
        key = exec::session_key(stdfs::current_path(ec));
    }
    // Ключ передаётся дочерним процессам, чтобы все вызовы компилятора внутри
    // этой команды попали в ту же сессию, каким бы ни был их рабочий каталог.
    // Без этого сборка, уходящая в подкаталоги, завела бы по сессии на каждый.
    ::setenv("CORK_SESSION", key.c_str(), 1);

    const stdfs::path wine = wine_runtime_of(root);

    // Сборка мусора до начала, а не после: брошенные сессии копятся именно
    // тогда, когда сборку прервали, а следующий запуск — первый удобный
    // момент их заметить.
    if (auto swept = exec::collect_sessions(root, wine, std::chrono::hours(24));
        swept.has_value() && *swept > 0) {
        fmt::print(stderr, "cork: removed {} abandoned build session(s)\n", *swept);
    }

    // Ctrl-C в терминале приходит всей группе процессов сразу — и ребёнку, и
    // нам. Ребёнку он и нужен, а вот нам умереть нельзя: убирать сессию после
    // этого будет некому, то есть прерванная сборка оставит полгигабайта
    // мусора ровно в том случае, ради которого эта команда написана.
    //
    // Поэтому родитель игнорирует SIGINT и SIGTERM и просто ждёт. Ребёнок
    // получает их своим чередом и умирает; мы это видим по статусу.
    struct sigaction ignore{};
    struct sigaction old_int{};
    struct sigaction old_term{};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    ::sigaction(SIGINT, &ignore, &old_int);
    ::sigaction(SIGTERM, &ignore, &old_term);

    const pid_t pid = ::fork();
    if (pid < 0) {
        fmt::print(stderr, "cork run: fork failed\n");
        return 1;
    }
    if (pid == 0) {
        // Ребёнку сигналы возвращаются: игнорирование наследуется через
        // fork и exec, и без этого Ctrl-C не остановил бы саму сборку.
        ::sigaction(SIGINT, &old_int, nullptr);
        ::sigaction(SIGTERM, &old_term, nullptr);
        // Собранная программа запускается через Wine в префиксе этой же
        // сессии. Без этого кросс-компиляция проверяема только наполовину:
        // результат собрался, а запускается ли он — неизвестно.
        if (looks_like_windows_program(command[0])) {
            exec::RunOptions opts;
            opts.program = command[0];
            // Поколение берётся от current, а не от собственного пути: сам
            // cork лежит где угодно, а обёртки — внутри поколения, и только
            // им путь от себя что-то говорит.
            if (auto current = root.resolve_current(); current.has_value()) {
                opts.generation_root = *current;
                if (auto cfg = setup::Config::load(*current); cfg.has_value()) {
                    opts.target_arch = cfg->host_arch;
                }
            }
            const std::vector<std::string> rest(command.begin() + 1, command.end());
            ::_exit(exec::run_tool("", rest, opts));
        }
        std::vector<char *> argv;
        std::vector<std::string> copies = command;
        argv.reserve(copies.size() + 1);
        for (auto &a : copies) {
            argv.push_back(a.data());
        }
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        fmt::print(stderr, "cork run: cannot run '{}'\n", command[0]);
        ::_exit(127);
    }

    // EINTR здесь всё равно возможен — от сигнала, который мы не глушили, —
    // и ожидание после него продолжается: выйти отсюда раньше ребёнка значит
    // не убрать за ним.
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            break;
        }
    }

    ::sigaction(SIGINT, &old_int, nullptr);
    ::sigaction(SIGTERM, &old_term, nullptr);

    if (!keep && !inherited) {
        if (auto r = exec::close_session(root, key, wine, true);
            !r.has_value() && r.error().code != Error::Code::NotFound) {
            // «Сессии нет» — не отказ: команда могла и не запускать ни одного
            // инструмента, и тогда префикс не создавался.
            fmt::print(stderr, "cork run: {}\n", r.error().to_string());
        }
    } else if (keep) {
        fmt::print(stderr, "cork: session '{}' kept\n", key);
    }

    if (WIFSIGNALED(status)) {
        // Код возврата по соглашению shell: 128 плюс номер сигнала. Так
        // сборочная система отличит «убили» от обычного ненулевого кода.
        return 128 + WTERMSIG(status);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

int cmd_template(const std::vector<std::string> &args) {
    setup::Root root = setup::Root::from_environment();
    stdfs::path source;
    bool force = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--root" && i + 1 < args.size()) {
            root = root_from(args, i);
        } else if (args[i] == "--force") {
            force = true;
        } else if (args[i] == "-h" || args[i] == "--help") {
            return cmd_help(0);
        } else {
            source = args[i];
        }
    }

    auto current = root.resolve_current();
    if (!current.has_value()) {
        fmt::print(stderr, "cork template: {}\n", current.error().to_string());
        return 1;
    }
    auto cfg = setup::Config::load(*current);
    if (!cfg.has_value()) {
        fmt::print(stderr, "cork template: {}\n", cfg.error().to_string());
        return 1;
    }

    const stdfs::path runtime = root.runtime(cfg->wine_id);
    const bool explicit_source = !source.empty();
    if (!explicit_source) {
        source = root.prefix(cfg->wine_id);
    }

    // Префикс, отставший от этой сборки Wine, поднимается заново.
    //
    // Без этого команда молча делала шаблон из того, что лежало, и закрепляла
    // задержку навсегда: каждая сессия, склонированная с такого шаблона,
    // гоняла wine.inf заново. Диагностика при этом советовала выполнить
    // `cork template`, а он ничего не менял — совет, который не помогает,
    // хуже молчания.
    //
    // Указанный руками префикс не трогаем: раз его назвали, значит знают, что
    // берут.
    if (!explicit_source && !setup::prefix_matches_wine(source, runtime)) {
        if (fs::is_dir(source)) {
            fmt::print(stderr, "cork: the prefix at {} predates this Wine; rebuilding it\n",
                       source.string());
            std::error_code rm_ec;
            stdfs::remove_all(source, rm_ec);
        }
        if (auto r = setup::boot_prefix(runtime, source); !r.has_value()) {
            fmt::print(stderr, "cork template: {}\n", r.error().to_string());
            return 1;
        }
    }
    if (!fs::is_dir(source)) {
        fmt::print(stderr,
                   "cork template: no prefix at {}.\n"
                   "Run any tool once to have Wine create one, then try again.\n",
                   source.string());
        return 1;
    }

    const stdfs::path destination = root.base / "template" / cfg->wine_id;
    std::error_code ec;
    if (force) {
        stdfs::remove_all(destination, ec);
    }

    i18n::say(i18n::Msg::BuildingTemplate, source.string());
    auto stats = setup::build_prefix_template(source, destination);
    if (!stats.has_value()) {
        fmt::print(stderr, "cork template: {}\n", stats.error().to_string());
        return 1;
    }
    i18n::say(i18n::Msg::TemplateAt, destination.string());
    i18n::say(i18n::Msg::TemplateStats, stats->links_replaced, stats->devices_removed,
              stats->registry_edits);
    i18n::say(i18n::Msg::TemplateNote);
    return 0;
}

int cmd_session(const std::vector<std::string> &args) {
    setup::Root root = setup::Root::from_environment();
    std::string action = "list";
    std::string key;
    bool force = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--root" && i + 1 < args.size()) {
            root = root_from(args, i);
        } else if (args[i] == "--force") {
            force = true;
        } else if (args[i] == "-h" || args[i] == "--help") {
            return cmd_help(0);
        } else if (args[i] == "list" || args[i] == "stop" || args[i] == "kill") {
            action = args[i];
        } else {
            key = args[i];
        }
    }

    const stdfs::path wine = wine_runtime_of(root);

    if (action == "list") {
        auto sessions = exec::list_sessions(root);
        if (!sessions.has_value()) {
            fmt::print(stderr, "cork session: {}\n", sessions.error().to_string());
            return 1;
        }
        if (sessions->empty()) {
            i18n::say(i18n::Msg::NoSessions);
            return 0;
        }
        for (const auto &s : *sessions) {
            fmt::print("{:<44} {:>10}{}\n", s.key, humanize_bytes(s.bytes),
                       s.busy ? fmt::format("  ({})", i18n::tr(i18n::Msg::SessionInUse))
                              : std::string());
        }
        return 0;
    }

    // stop и kill различаются одним: kill сначала гасит процессы Wine этого
    // префикса, stop отказывается трогать занятую сессию.
    const bool hard = action == "kill" || force;
    if (key.empty()) {
        std::error_code ec;
        key = exec::session_key(stdfs::current_path(ec));
        i18n::say(i18n::Msg::SessionForDirectory, key);
    }
    if (auto r = exec::close_session(root, key, wine, hard); !r.has_value()) {
        fmt::print(stderr, "cork session: {}\n", r.error().to_string());
        return 1;
    }
    i18n::say(i18n::Msg::RemovedSession, key);
    return 0;
}

} // namespace cork::cli
