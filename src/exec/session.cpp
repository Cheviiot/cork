#include "exec/session.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <system_error>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fmt/format.h>

#include "base/clone.hpp"
#include "base/fs.hpp"
#include "base/sha256.hpp"
#include "setup/prefix.hpp"

namespace cork::exec {
namespace {

namespace stdfs = std::filesystem;

// Признаки корня сборочного дерева, от самого надёжного к самому общему.
// build.ninja и CMakeCache.txt означают именно каталог сборки; .sln — корень
// решения; .git — корень репозитория, и это уже догадка, зато работающая
// тогда, когда сборка идёт прямо в дереве исходников.
// Путь, с которого начинать поиск корня сборки.
//
// CMake проверяет компилятор и каждую возможность языка отдельным крошечным
// проектом со своим CMakeCache.txt: определение компилятора уходит в
// <сборка>/CMakeFiles/<версия>/CompilerIdC, try_compile — в
// <сборка>/CMakeFiles/CMakeScratch/TryCompile-XXXXXX. По внешнему признаку это
// настоящие корни сборки, и каждый такой каталог заводил собственную сессию,
// то есть собственный префикс Wine со своим wineserver.
//
// Измерено на Ogre 14.5.2: configure успел завести 32 сессии и не дошёл даже
// до половины проверок. Виновата была не компиляция, а тридцать два wineboot.
//
// Правило одно на все случаи: всё, что лежит под CMakeFiles, принадлежит той
// сборке, которой этот CMakeFiles принадлежит. Проверять имена проб по
// отдельности мало — на момент определения компилятора CMakeCache.txt ещё не
// записан, и подъём из пробы всё равно не нашёл бы корня.
stdfs::path search_from(const stdfs::path &cwd) {
    for (stdfs::path dir = cwd; !dir.empty(); dir = dir.parent_path()) {
        if (dir.filename() == "CMakeFiles") {
            return dir.parent_path();
        }
        if (dir.parent_path() == dir) {
            break;
        }
    }
    return cwd;
}

bool looks_like_build_root(const stdfs::path &dir) {
    std::error_code ec;
    for (const char *name : {"build.ninja", "CMakeCache.txt", "Makefile", "build.gradle"}) {
        if (stdfs::exists(dir / name, ec)) {
            return true;
        }
    }
    for (const auto &entry : stdfs::directory_iterator(dir, ec)) {
        if (ec) {
            break;
        }
        if (entry.path().extension() == ".sln") {
            return true;
        }
    }
    return stdfs::exists(dir / ".git", ec);
}

// Имя каталога сессии: читаемая часть плюс хеш полного пути. Без хеша два
// проекта с одинаковым именем каталога делили бы префикс; без читаемой части
// `cork session list` выдавал бы столбец шестнадцатеричных чисел.
std::string key_for_path(const stdfs::path &root) {
    std::string name = root.filename().string();
    if (name.empty() || name == "." || name == "/") {
        name = "root";
    }
    for (char &c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) == 0 && c != '-' && c != '_' && c != '.') {
            c = '-';
        }
    }
    if (name.size() > 32) {
        name.resize(32);
    }
    const std::string digest = Sha256::hex_of(root.string());
    return fmt::format("{}-{}", name, digest.substr(0, 10));
}

std::uint64_t tree_size(const stdfs::path &p) {
    std::error_code ec;
    std::uint64_t total = 0;
    for (stdfs::recursive_directory_iterator it(p, stdfs::directory_options::skip_permission_denied,
                                                ec);
         it != stdfs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            break;
        }
        if (it->is_regular_file(ec)) {
            total += it->file_size(ec);
        }
    }
    return total;
}

// Гасит wineserver этого префикса. Ядерный вариант, и он безопасен именно
// потому, что префикс у сессии свой: чужие сборки к нему отношения не имеют.
void kill_prefix(const stdfs::path &wine_runtime, const stdfs::path &prefix) {
    const stdfs::path server = wine_runtime / "bin" / "wineserver";
    if (!fs::is_regular_file(server)) {
        return;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        return;
    }
    if (pid == 0) {
        const int null_fd = ::open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            ::dup2(null_fd, STDIN_FILENO);
            ::dup2(null_fd, STDOUT_FILENO);
            ::dup2(null_fd, STDERR_FILENO);
            ::close(null_fd);
        }
        ::setenv("WINEPREFIX", prefix.c_str(), 1);
        ::execl(server.c_str(), server.c_str(), "-k", nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
}

} // namespace

PrefixMode prefix_mode_from_env() {
    const char *v = std::getenv("CORK_PREFIX_MODE");
    if (v != nullptr && std::string_view(v) == "shared") {
        return PrefixMode::Shared;
    }
    return PrefixMode::PerSession;
}

std::string session_key(const stdfs::path &cwd) {
    if (const char *v = std::getenv("CORK_SESSION"); v != nullptr && *v != '\0') {
        return v;
    }
    std::error_code ec;
    const stdfs::path start = search_from(stdfs::absolute(cwd, ec));
    stdfs::path dir = start;
    // Вверх до корня, но не бесконечно: символьная ссылка, указывающая вверх,
    // способна сделать подъём бесконечным, а ошибаться здесь незачем.
    for (int depth = 0; depth < 64; ++depth) {
        if (looks_like_build_root(dir)) {
            return key_for_path(dir);
        }
        const stdfs::path parent = dir.parent_path();
        if (parent == dir) {
            break;
        }
        dir = parent;
    }
    // Ничего похожего на корень сборки не нашлось — ключом становится сам
    // рабочий каталог. Так разовый вызов компилятора получает свою сессию и
    // не подмешивается к чужой сборке.
    return key_for_path(start);
}

Result<Session> open_session(const setup::Root &root, const std::string &key,
                             const stdfs::path &wine_runtime, const std::string &wine_id,
                             Lock &hold) {
    Session s;
    s.key = key;
    s.dir = root.base / "sessions" / key;

    if (prefix_mode_from_env() == PrefixMode::Shared) {
        // Общий префикс: сессии как таковой нет, и блокировка не нужна.
        s.prefix = root.prefix(wine_id);
        return s;
    }

    if (auto r = fs::mkdir_p(s.dir); !r) {
        return std::unexpected(std::move(r).error());
    }
    s.prefix = s.dir / "prefix";

    // Разделяемая блокировка: параллельные вызовы компилятора одной сборки
    // держат её одновременно, а `session stop` и сборка мусора берут
    // исключительную и потому ждут или отступают.
    auto lock = Lock::acquire(root.locks(), "session-" + key, Lock::Mode::Shared, -1);
    if (!lock) {
        return std::unexpected(std::move(lock).error().at("opening a session"));
    }
    hold = std::move(*lock);

    if (fs::is_dir(s.prefix)) {
        return s;
    }

    // Префикса ещё нет. Создаётся он из шаблона, если тот есть, — это
    // единственный способ не платить за wineboot на каждой новой сборке.
    const stdfs::path template_dir = root.base / "template" / wine_id;
    if (fs::is_dir(template_dir)) {
        auto stats = fs::clone_tree(template_dir, s.prefix);
        if (!stats) {
            return std::unexpected(std::move(stats).error().at("instantiating the prefix"));
        }
        // В шаблоне профиль назван нейтрально; здесь ему возвращается имя
        // того, кто на самом деле собирает. Без этого TEMP указывал бы в
        // каталог, которого в префиксе нет.
        const char *user = std::getenv("USER");
        if (user == nullptr || *user == '\0') {
            user = std::getenv("LOGNAME");
        }
        if (user != nullptr && *user != '\0') {
            if (auto r = setup::instantiate_prefix(s.prefix, user); !r) {
                return std::unexpected(std::move(r).error());
            }
        }
        s.created = true;
        return s;
    }

    // Шаблона нет — префикс создаст wineboot при первом запуске. Медленно, но
    // работает; шаблон появляется в поставке, а при сборке из исходников его
    // может не быть.
    if (auto r = fs::mkdir_p(s.prefix); !r) {
        return std::unexpected(std::move(r).error());
    }
    (void)wine_runtime;
    s.created = true;
    return s;
}

Result<void> close_session(const setup::Root &root, const std::string &key,
                           const stdfs::path &wine_runtime, bool force) {
    const stdfs::path dir = root.base / "sessions" / key;
    if (!fs::is_dir(dir)) {
        return err_not_found(fmt::format("no session named '{}'", key));
    }

    auto lock = Lock::acquire(root.locks(), "session-" + key, Lock::Mode::Exclusive, 0);
    if (!lock) {
        if (!force) {
            return err_conflict(fmt::format(
                "session '{}' is in use; finish the build or pass --force to kill it", key));
        }
        // Занятость означает, что в префиксе ещё живут процессы Wine. Снести
        // каталог под ними значит оставить их работать на пустом месте,
        // поэтому сначала гасим их, и только потом убираем.
        kill_prefix(wine_runtime, dir / "prefix");
        auto again = Lock::acquire(root.locks(), "session-" + key, Lock::Mode::Exclusive, 5000);
        if (!again) {
            return err_conflict(
                fmt::format("session '{}' is still in use after killing its Wine server", key));
        }
        lock = std::move(again);
    } else {
        // Гасим всегда, а не только при --force. В префиксе сессии может
        // жить сервер PDB, поднятый для `cl /FS`, а сам он не выйдет
        // никогда. Снести каталог, не погасив его, значит оставить его
        // работать на пустом месте: каждая сборка в Debug добавляла бы по
        // такому процессу.
        kill_prefix(wine_runtime, dir / "prefix");
    }

    std::error_code ec;
    stdfs::remove_all(dir, ec);
    if (ec) {
        return err_io(fmt::format("removing {}: {}", dir.string(), ec.message()));
    }
    lock->release();
    stdfs::remove(root.locks() / ("session-" + key + ".lock"), ec);
    return {};
}

Result<std::vector<SessionInfo>> list_sessions(const setup::Root &root) {
    std::vector<SessionInfo> out;
    std::error_code ec;
    const stdfs::path dir = root.base / "sessions";
    if (!stdfs::is_directory(dir, ec)) {
        return out;
    }
    for (const auto &entry : stdfs::directory_iterator(dir, ec)) {
        if (ec) {
            return err_io(fmt::format("listing {}: {}", dir.string(), ec.message()));
        }
        if (!entry.is_directory(ec)) {
            continue;
        }
        SessionInfo info;
        info.key = entry.path().filename().string();
        info.prefix = entry.path() / "prefix";
        info.bytes = tree_size(entry.path());

        const auto written = entry.last_write_time(ec);
        if (!ec) {
            info.last_used = std::chrono::clock_cast<std::chrono::system_clock>(written);
        }
        // Занятость определяется попыткой взять исключительную блокировку:
        // held означает, что сборка идёт прямо сейчас.
        auto probe = Lock::acquire(root.locks(), "session-" + info.key, Lock::Mode::Exclusive, 0);
        info.busy = !probe.has_value();
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(),
              [](const SessionInfo &a, const SessionInfo &b) { return a.key < b.key; });
    return out;
}

Result<std::uint64_t> collect_sessions(const setup::Root &root, const stdfs::path &wine_runtime,
                                       std::chrono::seconds age) {
    auto sessions = list_sessions(root);
    if (!sessions) {
        return std::unexpected(std::move(sessions).error());
    }
    const auto now = std::chrono::system_clock::now();
    std::uint64_t removed = 0;
    for (const auto &s : *sessions) {
        if (s.busy) {
            continue;
        }
        if (now - s.last_used < age) {
            continue;
        }
        if (auto r = close_session(root, s.key, wine_runtime, false); r) {
            ++removed;
        }
    }
    return removed;
}

} // namespace cork::exec
