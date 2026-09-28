#include "exec/runner.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fmt/format.h>

#include "base/clone.hpp"
#include "base/fs.hpp"
#include "exec/clpost.hpp"
#include "exec/filters.hpp"
#include "exec/msbuild.hpp"
#include "exec/response.hpp"
#include "exec/session.hpp"
#include "exec/tools.hpp"
#include "proto/protocol.hpp"
#include "setup/generation.hpp"
#include "setup/layout.hpp"

namespace cork::exec {
namespace {

namespace stdfs = std::filesystem;

constexpr int kExitTimeout = 124;
constexpr int kExitSpawnFailed = 127;
constexpr int kExitSignalled = 130;
constexpr int kExitInternal = 70;

// Сколько ждать вывода после того, как процесс wine уже завершился. Нужно
// потому, что фоновые службы Wine наследуют наши конвейеры и EOF может не
// прийти вовсе. Правильное решение — поднимать службы заранее со stdio в
// /dev/null; это делает bootstrap_prefix ниже, а запас остаётся страховкой.
constexpr int kDrainGraceMs = 300;

// Столько ждём после мягкого сигнала, прежде чем убить группу.
constexpr int kKillGraceMs = 5000;

std::string env_or(const char *name, std::string fallback) {
    const char *v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? std::string(v) : std::move(fallback);
}

int timeout_ms_from_env() {
    const char *v = std::getenv("CORK_TIMEOUT");
    if (v == nullptr || *v == '\0') {
        return 0;
    }
    // Принимается тот же вид, что и раньше: 30m, 2h, 45s, просто число секунд.
    char *end = nullptr;
    const double value = std::strtod(v, &end);
    if (end == v || value <= 0) {
        return 0;
    }
    double multiplier = 1.0;
    if (*end == 'm') {
        multiplier = 60.0;
    } else if (*end == 'h') {
        multiplier = 3600.0;
    } else if (*end == 's' || *end == '\0') {
        multiplier = 1.0;
    } else {
        return 0;
    }
    return static_cast<int>(value * multiplier * 1000.0);
}

// Раскладка корня описана в одном месте — setup::Root, — и обёртка обязана
// пользоваться тем же описанием, что install и doctor. Иначе CORK_HOME
// учитывался бы установкой и игнорировался запуском, а расходятся такие вещи
// молча.
Result<setup::Root> cork_root() {
    setup::Root root = setup::Root::from_environment();
    if (root.base.empty()) {
        return err_config("neither CORK_HOME nor HOME is set, cannot locate the runtime");
    }
    return root;
}

Result<stdfs::path> resolve_wine(const setup::Config &cfg) {
    // Явный обход остаётся, но без гарантий: собранный нами хелпер и патчи
    // автономности рассчитаны на свою сборку.
    if (const char *override_path = std::getenv("CORK_WINE");
        override_path != nullptr && *override_path != '\0') {
        fmt::print(stderr,
                   "cork: using CORK_WINE={}; this is unsupported and skips the "
                   "runtime version check\n",
                   override_path);
        return stdfs::path(override_path);
    }

    auto root = cork_root();
    if (!root.has_value()) {
        return std::unexpected(std::move(root.error()));
    }
    if (cfg.wine_id.empty()) {
        return err_config("the installation records no Wine runtime; re-run `cork install`");
    }
    const stdfs::path wine = root->runtime(cfg.wine_id) / "bin" / "wine";
    if (!fs::is_regular_file(wine)) {
        return err_not_found(fmt::format(
            "the cork Wine runtime {} is not installed at {}\n"
            "Run `cork download` to fetch it. A system Wine is not supported.",
            cfg.wine_id, wine.string()));
    }
    return wine;
}

// Первый запуск в префиксе поднимает службы Wine, и они наследуют stdio того
// процесса, который их породил. Если это окажется настоящая сборка, службы
// удержат её конвейеры открытыми, и EOF не придёт вовсе. Лечить это запасом
// по времени на дочитывание бессмысленно: конец потока так и не наступит.
// Поэтому службы поднимаются заранее, со stdio в /dev/null.
// Предупреждение о том, что префикс копируется по-настоящему.
//
// На btrfs и xfs клонирование почти бесплатно, и говорить не о чем. На ext4
// каждая новая сборка стоит настоящие полгигабайта, и узнать об этом лучше от
// нас, чем по забитому диску. Сказать надо один раз: повторять на каждый вызов
// компилятора значит утопить в этом сообщении вывод сборки.
void warn_about_slow_prefixes(const setup::Root &root, const Session &session) {
    if (!session.created || prefix_mode_from_env() == PrefixMode::Shared) {
        return;
    }
    const stdfs::path marker = root.base / ".slow-prefix-warned";
    if (fs::exists_no_follow(marker)) {
        return;
    }
    if (fs::supports_reflink(root.base / "sessions")) {
        return;
    }
    fmt::print(stderr,
               "cork: this filesystem has no reflink support, so every build session copies\n"
               "      the Wine prefix in full. Set CORK_PREFIX_MODE=shared to use one prefix\n"
               "      for all builds instead (parallel builds will then share it).\n");
    (void)fs::write_atomic(marker, std::string_view("1"));
}

// Запуск со stdio в /dev/null, с ожиданием завершения. Отдельной функцией,
// потому что бутстрап префикса — это несколько таких запусков подряд, и
// разница между ними только в аргументах.
void run_quiet(const stdfs::path &prefix, const stdfs::path &program,
               std::initializer_list<const char *> args, const char *wine_path = nullptr) {
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
        }
        ::setsid();
        ::setenv("WINEPREFIX", prefix.c_str(), 1);
        ::setenv("WINEDEBUG", "-all", 1);
        if (wine_path != nullptr) {
            ::setenv("WINEPATH", wine_path, 1);
        }
        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(program.c_str()));
        for (const char *a : args) {
            argv.push_back(const_cast<char *>(a));
        }
        argv.push_back(nullptr);
        ::execv(program.c_str(), argv.data());
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
}

// Запуск сервера PDB. Отсоединённо, потому что `-start -spawn` не
// завершается: он и есть сам сервер. Двойной fork — чтобы не оставить зомби,
// ждать этого ребёнка некому.
void start_pdb_server(const stdfs::path &prefix, const stdfs::path &wine,
                      const stdfs::path &mspdbsrv, const char *wine_path) {
    const pid_t pid = ::fork();
    if (pid < 0) {
        return;
    }
    if (pid == 0) {
        if (::fork() == 0) {
            const int null_fd = ::open("/dev/null", O_RDWR);
            if (null_fd >= 0) {
                ::dup2(null_fd, STDIN_FILENO);
                ::dup2(null_fd, STDOUT_FILENO);
                ::dup2(null_fd, STDERR_FILENO);
            }
            ::setsid();
            ::setenv("WINEPREFIX", prefix.c_str(), 1);
            ::setenv("WINEDEBUG", "-all", 1);
            if (wine_path != nullptr) {
                ::setenv("WINEPATH", wine_path, 1);
            }
            ::execl(wine.c_str(), wine.c_str(), mspdbsrv.c_str(), "-start", "-spawn", nullptr);
            ::_exit(127);
        }
        ::_exit(0);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
}

void bootstrap_prefix(const stdfs::path &wine, const stdfs::path &prefix) {
    const stdfs::path marker = prefix / ".cork-bootstrapped";
    if (fs::exists_no_follow(marker)) {
        return;
    }
    (void)fs::mkdir_p(prefix);
    const stdfs::path wineserver = wine.parent_path() / "wineserver";

    // wineboot — только если префикс и правда не загружен. Сессионный
    // префикс клонируется из шаблона, который загружен целиком, и повторный
    // wineboot ему не нужен. Хуже того: он вреден.
    //
    // Измерено, шесть свежих клонов на вариант, засчитывается первая попытка:
    // без прогрева `cl /Zi /FS` проходит 6 раз из 6, а после
    // `wineboot --init` — 4 из 6, с `C1902: Program database manager
    // mismatch`. wineboot перезапускает службы, и первая компиляция с /FS
    // попадает в гонку с их запуском: `/FS` работает через отдельный процесс
    // mspdbsrv.exe, которому службы нужны. Сообщение об ошибке не упоминает
    // ни префикса, ни служб, поэтому сказано здесь.
    if (!fs::is_regular_file(prefix / "system.reg")) {
        run_quiet(prefix, wine, {"wineboot", "--init"});
        // Реестр пишет wineserver при выходе, а не wineboot. Без ожидания
        // префикс остаётся без служб, и первая же /FS отказывает.
        if (fs::is_regular_file(wineserver)) {
            run_quiet(prefix, wineserver, {"-w"});
        }
    }

    // Службы поднимаются от процесса со stdio в /dev/null. Иначе их поднимет
    // первый настоящий вызов инструмента — и они унаследуют его конвейеры,
    // после чего EOF на них не придёт никогда.
    //
    // `wineserver -p` здесь не нужен, хотя и напрашивается: сервер и так не
    // выходит, пока в префиксе жив хоть один процесс Windows, а сервер PDB
    // ниже как раз такой. Зато с `-p` он не выходит и когда не жив никто, то
    // есть каждая законченная сборка оставляла бы по паре процессов до
    // следующей сборки мусора — на приёмочном прогоне это девять и девять.
    run_quiet(prefix, wine, {"winepath", "-w", "C:\\"});

    (void)fs::write_atomic(marker, std::string_view("1"));
}

// Нужен ли этому вызову сервер PDB. /FS — единственный ключ, который его
// включает; /Zi без /FS пишет PDB сам. Проверяется и содержимое
// response-файлов: MSBuild передаёт ключи через них.
bool wants_pdb_server(const std::vector<std::string> &args) {
    return std::any_of(args.begin(), args.end(), [](const std::string &a) {
        return a == "/FS" || a == "-FS";
    });
}

// Поднимает сервер PDB в префиксе, один раз.
//
// Не при заводе сессии, а при первом вызове с /FS, и это не экономия
// запуска: сервер не выходит сам, пока жив префикс сессии, — значит сессия,
// которая никогда не собирала с /FS, иначе держала бы его до сборки мусора
// ни за чем.
//
// Почему вообще мы, а не сам `cl`. Сервер один на всю сборку, а job — свой у
// каждого вызова инструмента. Подними его первый же `cl`, он оказался бы в
// job ЭТОГО вызова и умер вместе с ним по KILL_ON_JOB_CLOSE, а остальные
// `cl`, пишущие в тот же PDB, получили бы
// `C1090: PDB API call failed, error code '23'` — это 0x6BA,
// RPC_S_SERVER_UNAVAILABLE. На сборке из девяти файлов через ninja это
// происходит регулярно; на одном файле не видно вовсе, потому что терять
// сервер некому.
void ensure_pdb_server(const stdfs::path &wine, const stdfs::path &prefix,
                       const setup::ToolEnvironment &tool_env) {
    const stdfs::path marker = prefix / ".cork-pdb-server";
    if (fs::exists_no_follow(marker)) {
        return;
    }
    const stdfs::path mspdbsrv = tool_env.host_bin / "mspdbsrv.exe";
    if (!fs::is_regular_file(mspdbsrv)) {
        return;
    }
    // Маркер ставится до запуска, а не после: два параллельных `cl` иначе
    // поднимут по серверу каждый.
    (void)fs::write_atomic(marker, std::string_view("1"));
    start_pdb_server(prefix, wine, mspdbsrv, tool_env.wine_path.c_str());
}

struct Pipe {
    int read_fd = -1;
    int write_fd = -1;

    bool open() {
        int fds[2];
        if (::pipe2(fds, O_CLOEXEC) != 0) {
            return false;
        }
        read_fd = fds[0];
        write_fd = fds[1];
        return true;
    }
    void close_write() {
        if (write_fd >= 0) {
            ::close(write_fd);
            write_fd = -1;
        }
    }
    void close_read() {
        if (read_fd >= 0) {
            ::close(read_fd);
            read_fd = -1;
        }
    }
};

// Построчная выдача с применением фильтра. Остаток без перевода строки
// держится в буфере до следующего куска.
class LineWriter {
public:
    LineWriter(int out_fd, LineFilter filter, const FilterConfig &cfg)
        : out_fd_(out_fd), filter_(filter), cfg_(cfg) {}

    void feed(std::string_view chunk) {
        buffer_ += chunk;
        std::size_t start = 0;
        for (;;) {
            const std::size_t nl = buffer_.find('\n', start);
            if (nl == std::string::npos) {
                break;
            }
            emit(std::string_view(buffer_).substr(start, nl - start));
            start = nl + 1;
        }
        buffer_.erase(0, start);
    }

    // Хвост без перевода строки тоже надо отдать: компиляторы иногда
    // заканчивают вывод без него, и потерять последнюю строку нельзя.
    void flush() {
        if (!buffer_.empty()) {
            emit(buffer_);
            buffer_.clear();
        }
    }

private:
    void emit(std::string_view line) {
        std::string text = strip_cr(line);
        if (filter_ != nullptr) {
            text = filter_(text, cfg_);
        }
        text += '\n';
        std::size_t written = 0;
        while (written < text.size()) {
            const ssize_t n = ::write(out_fd_, text.data() + written, text.size() - written);
            if (n <= 0) {
                if (errno == EINTR) {
                    continue;
                }
                return;  // потребитель закрыл поток; молча прекращаем
            }
            written += static_cast<std::size_t>(n);
        }
    }

    int out_fd_;
    LineFilter filter_;
    FilterConfig cfg_;
    std::string buffer_;
};

} // namespace

int map_exit_code(std::string_view tool, std::uint32_t full_code) {
    // mt.exe: «манифест не изменился». CMake проверяет именно 0xbb, а Unix
    // всё равно отдаст родителю только байт, поэтому отобразить надо здесь.
    constexpr std::uint32_t kManifestUnchanged = 0x41020001u;
    constexpr int kManifestUnchangedForCMake = 0xbb;
    if (tool == "mt" && full_code == kManifestUnchanged) {
        return kManifestUnchangedForCMake;
    }
    return static_cast<int>(full_code & 0xffu);
}

int run_tool(std::string_view tool, const std::vector<std::string> &args,
             const RunOptions &options) {
    // Собранная программа проходит тем же путём, что инструмент из таблицы,
    // и отличается двумя вещами: её .exe задан прямо, а вывод не фильтруется
    // — фильтры знают про формат сообщений cl и dumpbin, и чужой программе
    // они бы только испортили вывод.
    const ToolSpec produced{"", "", ToolDir::MsvcBin, true};
    const bool run_produced = !options.program.empty();
    const ToolSpec *spec = run_produced ? &produced : find_tool(tool);
    if (spec == nullptr) {
        fmt::print(stderr, "cork: unknown tool \"{}\"\n", tool);
        return kExitSpawnFailed;
    }

    // --- конфигурация и раскладка ---
    stdfs::path root = options.generation_root;
    std::string arch = options.target_arch;
    if (root.empty()) {
        std::error_code ec;
        const stdfs::path self = stdfs::read_symlink("/proc/self/exe", ec);
        if (ec) {
            fmt::print(stderr, "cork: cannot determine own path: {}\n", ec.message());
            return kExitInternal;
        }
        if (arch.empty()) {
            arch = self.parent_path().filename().string();
        }
        // Обёртка лежит внутри поколения, и путь от себя — единственно
        // верный ответ: он привязывает вызов к тому поколению, каталог
        // которого стоит в PATH, а не к тому, что сейчас считается текущим.
        auto found = setup::find_generation_root(self.parent_path());
        if (found.has_value()) {
            root = *found;
        } else {
            // А сам cork лежит где угодно — хоть в /usr/local/bin, — и для
            // «cork cl x.c» путь от себя не значит ничего. Тогда работает
            // current. Без этого человек, поставивший один бинарник в PATH,
            // получает «installation is incomplete» при исправной установке.
            auto current = setup::Root::from_environment().resolve_current();
            if (!current.has_value()) {
                fmt::print(stderr, "cork: {}\n", found.error().to_string());
                return kExitInternal;
            }
            root = *current;
            // Архитектура, угаданная по имени каталога, здесь бессмысленна:
            // рядом с /usr/local/bin/cork это «bin». Но заданную вызывающим
            // трогать нельзя — она не угадана, а выбрана.
            if (options.target_arch.empty()) {
                arch.clear();
            }
        }
    }

    auto cfg = setup::Config::load(root);
    if (!cfg.has_value()) {
        fmt::print(stderr, "cork: {}\n", cfg.error().to_string());
        return kExitInternal;
    }
    if (arch.empty()) {
        // Архитектура по умолчанию — та же, что у хоста. CORK_TARGET нужен
        // тем, кто зовёт «cork cl» напрямую и целится не туда.
        if (const char *t = std::getenv("CORK_TARGET"); t != nullptr && *t != '\0') {
            arch = t;
        } else {
            arch = cfg->host_arch;
        }
    }
    if (cfg->targets.count(arch) == 0) {
        fmt::print(stderr, "cork: no target architecture \"{}\" in this installation\n", arch);
        return kExitInternal;
    }

    auto wine = resolve_wine(*cfg);
    if (!wine.has_value()) {
        fmt::print(stderr, "cork: {}\n", wine.error().to_string());
        return kExitSpawnFailed;
    }

    stdfs::path exe;
    if (run_produced) {
        std::error_code abs_ec;
        exe = stdfs::absolute(options.program, abs_ec);
        if (abs_ec) {
            exe = options.program;
        }
    } else {
        const setup::TargetPaths &target = cfg->targets.at(arch);
        stdfs::path exe_dir;
        switch (spec->dir) {
            case ToolDir::MsvcBin: exe_dir = root / target.bin; break;
            case ToolDir::SdkBin: exe_dir = root / target.sdk_bin; break;
            case ToolDir::MsBuild: exe_dir = root / target.msbuild_bin; break;
        }
        exe = exe_dir / std::string(spec->exe);
    }
    const std::string exe_name = exe.filename().string();
    if (!fs::is_regular_file(exe)) {
        if (run_produced) {
            fmt::print(stderr, "cork: {} does not exist\n", exe.string());
        } else {
            fmt::print(stderr, "cork: {} is not installed at {}\n", spec->exe, exe.string());
        }
        return kExitSpawnFailed;
    }

    const stdfs::path helper = root / (cfg->helper_path.empty()
                                           ? std::string("bin/cork-helper.exe")
                                           : cfg->helper_path);
    if (!fs::is_regular_file(helper)) {
        fmt::print(stderr, "cork: the PE helper is missing at {}; re-run `cork install`\n",
                   helper.string());
        return kExitInternal;
    }

    auto home = cork_root();
    if (!home.has_value()) {
        fmt::print(stderr, "cork: {}\n", home.error().to_string());
        return kExitInternal;
    }
    // Префикс сессии, а не один на всех. Префикс — это изменяемое состояние
    // (реестр, system32, службы), и две параллельные сборки в нём мешают друг
    // другу, а брошенный процесс Wine предыдущей сборки достаётся следующей.
    //
    // Явный WINEPREFIX перекрывает всё: он означает «я знаю, что делаю».
    Lock session_hold;
    stdfs::path prefix;
    if (const char *explicit_prefix = std::getenv("WINEPREFIX");
        explicit_prefix != nullptr && *explicit_prefix != '\0') {
        prefix = explicit_prefix;
    } else {
        std::error_code cwd_ec;
        const std::string key = session_key(stdfs::current_path(cwd_ec));
        auto session = open_session(*home, key, wine->parent_path().parent_path(), cfg->wine_id,
                                    session_hold);
        if (!session.has_value()) {
            fmt::print(stderr, "cork: {}\n", session.error().to_string());
            return kExitInternal;
        }
        prefix = session->prefix;
        warn_about_slow_prefixes(*home, *session);
    }
    bootstrap_prefix(*wine, prefix);

    const setup::ToolEnvironment tool_env = setup::derive_environment(*cfg, root, arch);

    // --- запрос для хелпера ---
    const stdfs::path run_dir = home->base / "run" / std::to_string(::getpid());
    if (auto r = fs::mkdir_p(run_dir); !r.has_value()) {
        fmt::print(stderr, "cork: {}\n", r.error().to_string());
        return kExitInternal;
    }
    // Каталог запуска убирается при любом выходе: временных файлов после
    // сборки оставаться не должно. Переписанные .rsp накапливаются по одному
    // на вызов компилятора, то есть тысячами за одну большую сборку.
    struct RunDirCleanup {
        stdfs::path dir;
        bool keep;
        ~RunDirCleanup() {
            if (keep) {
                fmt::print(stderr, "cork: kept {} (CORK_KEEP_RUN)\n", dir.string());
                return;
            }
            std::error_code ec;
            stdfs::remove_all(dir, ec);
        }
    } run_dir_cleanup{run_dir, std::getenv("CORK_KEEP_RUN") != nullptr};

    proto::Request request;
    request.exe = exe.string();
    request.status_path = (run_dir / "status").string();
    request.args = args;
    const PathMode path_mode = path_mode_from_env();
    request.path_refs = path_argument_refs(spec->name, args, path_mode);
    request.flags = proto::kTranslatePaths;
    // Лазейка на случай инструмента, которому job мешает. Она снимает
    // гарантию удержания — процессы, ушедшие через setsid(), переживут
    // сборку, — поэтому включается только руками и никогда сама.
    if (const char *job = std::getenv("CORK_JOB");
        job != nullptr && std::string_view(job) == "off") {
        request.flags |= proto::kNoJob;
    }

    // Response-файлы: «@путь» со списком аргументов внутри. Ими пользуется всё,
    // что генерирует длинные командные строки, поэтому без разбора здесь пути
    // внутри них остались бы unix-овыми и компилятор не нашёл бы ни одного
    // заголовка.
    //
    // Нечитаемый файл — не повод молчать: инструмент всё равно упадёт, но уже
    // со своим сообщением, из которого не видно, что виноват именно он.
    for (const auto &rsp : find_response_args(args)) {
        auto content = read_response_file(rsp.path);
        if (!content.has_value()) {
            fmt::print(stderr, "cork: {}\n", content.error().to_string());
            return kExitInternal;
        }
        proto::ResponseFile rf;
        rf.arg_index = static_cast<std::uint32_t>(rsp.index);
        rf.args = std::move(*content);
        rf.path_refs = path_argument_refs(spec->name, rf.args, path_mode);
        request.response_files.push_back(std::move(rf));
    }
    if (!request.response_files.empty()) {
        request.flags |= proto::kTranslateResponseFiles;
    }

    // После разбора response-файлов, а не до: MSBuild и всё, что порождает
    // длинные командные строки, кладут ключи внутрь @rsp, и /FS там же.
    if (wants_pdb_server(request.args) ||
        std::any_of(request.response_files.begin(), request.response_files.end(),
                    [](const proto::ResponseFile &rf) { return wants_pdb_server(rf.args); })) {
        ensure_pdb_server(*wine, prefix, tool_env);
    }

    // --- окружение ---
    std::vector<std::string> env_entries = {
        "INCLUDE=" + tool_env.include,
        "LIB=" + tool_env.lib,
        "LIBPATH=" + tool_env.lib_path,
        "WINEPATH=" + tool_env.wine_path,
        "WINEPREFIX=" + prefix.string(),
        "WINEDEBUG=" + env_or("WINEDEBUG", "-all"),
    };
    // MSBuild ищет компилятор и SDK через реестр, которого под Wine нет.
    // Всё, что ниже, переводит этот поиск на переменные окружения — см.
    // exec/msbuild.hpp, там у каждой записан отказ, который она гасит.
    if (spec->name == "msbuild") {
        for (const auto &[key, value] : msbuild_env(*cfg, root, arch)) {
            env_entries.push_back(key + "=" + value);
        }
        // Свойства и ключи, которые надо навязать, если вызывающий не задал
        // их сам. Ставятся первыми: MSBuild берёт последнее значение, и
        // явный ключ пользователя обязан победить.
        auto forced = msbuild_forced_args(*cfg, args);
        if (!forced.empty()) {
            std::vector<std::string> merged = std::move(forced);
            merged.insert(merged.end(), request.args.begin(), request.args.end());
            request.args = std::move(merged);
            // Индексы путей считались по исходному списку и теперь съехали.
            request.path_refs = path_argument_refs(spec->name, request.args, path_mode);
        }
    }

    // WINEDLLOVERRIDES выставляется только потомку: «vcruntime140=n» означает
    // «только native», и наш хелпер, собранный wineg++ против встроенного
    // рантайма Wine, с такой переменной не загружается вовсе — проверено, wine
    // выходит с кодом 53 ещё до запуска инструмента.
    request.child_env.push_back("WINEDLLOVERRIDES=" + tool_env.wine_dll_overrides);

    const stdfs::path request_file = run_dir / "request";
    {
        const auto bytes = proto::encode(request);
        if (auto r = fs::write_atomic(
                request_file, std::span<const std::byte>(
                                  reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()));
            !r.has_value()) {
            fmt::print(stderr, "cork: {}\n", r.error().to_string());
            return kExitInternal;
        }
    }

    // --- конвейеры и сигналы ---
    const bool raw = spec->raw_output;
    Pipe out_pipe;
    Pipe err_pipe;
    if (!raw && (!out_pipe.open() || !err_pipe.open())) {
        fmt::print(stderr, "cork: cannot create pipes: {}\n", std::strerror(errno));
        return kExitInternal;
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGCHLD);
    // Сигналы блокируются заранее и читаются из дескриптора: обработчиков
    // нет, значит нет и требований к async-signal-safety.
    sigprocmask(SIG_BLOCK, &mask, nullptr);
    // SFD_NONBLOCK обязателен: ниже дескриптор вычитывается циклом «пока
    // читается», и на блокирующем дескрипторе этот цикл повисает навсегда,
    // как только кончатся ожидающие сигналы.
    const int sig_fd = ::signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (sig_fd < 0) {
        fmt::print(stderr, "cork: signalfd failed: {}\n", std::strerror(errno));
        return kExitInternal;
    }

    const int timeout_ms = timeout_ms_from_env();
    int timer_fd = -1;
    if (timeout_ms > 0) {
        timer_fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
        itimerspec spec_time{};
        spec_time.it_value.tv_sec = timeout_ms / 1000;
        spec_time.it_value.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1000000L;
        ::timerfd_settime(timer_fd, 0, &spec_time, nullptr);
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        fmt::print(stderr, "cork: fork failed: {}\n", std::strerror(errno));
        return kExitInternal;
    }
    if (pid == 0) {
        // Своя группа процессов: сигнал по одному pid снаружи не должен
        // оставлять дерево Wine сиротой.
        ::setpgid(0, 0);
        sigprocmask(SIG_UNBLOCK, &mask, nullptr);
        if (!raw) {
            ::dup2(out_pipe.write_fd, STDOUT_FILENO);
            ::dup2(err_pipe.write_fd, STDERR_FILENO);
        }
        for (const auto &kv : env_entries) {
            ::putenv(const_cast<char *>(kv.c_str()));
        }
        const std::string wine_str = wine->string();
        const std::string helper_str = helper.string();
        const std::string req_str = request_file.string();
        ::execl(wine_str.c_str(), wine_str.c_str(), helper_str.c_str(), req_str.c_str(), nullptr);
        ::_exit(kExitSpawnFailed);
    }

    out_pipe.close_write();
    err_pipe.close_write();

    const FilterConfig filter_cfg;
    LineWriter out_writer(STDOUT_FILENO, filter_for(spec->name, false), filter_cfg);
    LineWriter err_writer(STDERR_FILENO, filter_for(spec->name, true), filter_cfg);

    bool child_exited = false;
    int child_status = 0;
    bool timed_out = false;
    bool signalled = false;
    std::chrono::steady_clock::time_point drain_deadline{};
    std::chrono::steady_clock::time_point kill_deadline{};

    auto now = [] { return std::chrono::steady_clock::now(); };

    while (true) {
        pollfd fds[4];
        int n = 0;
        const int out_slot = out_pipe.read_fd >= 0 ? n : -1;
        if (out_pipe.read_fd >= 0) {
            fds[n++] = {out_pipe.read_fd, POLLIN, 0};
        }
        const int err_slot = err_pipe.read_fd >= 0 ? n : -1;
        if (err_pipe.read_fd >= 0) {
            fds[n++] = {err_pipe.read_fd, POLLIN, 0};
        }
        const int sig_slot = n;
        fds[n++] = {sig_fd, POLLIN, 0};
        const int timer_slot = timer_fd >= 0 ? n : -1;
        if (timer_fd >= 0) {
            fds[n++] = {timer_fd, POLLIN, 0};
        }

        int wait_ms = -1;
        if (child_exited && drain_deadline.time_since_epoch().count() != 0) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  drain_deadline - now())
                                  .count();
            wait_ms = static_cast<int>(left > 0 ? left : 0);
        }
        if (kill_deadline.time_since_epoch().count() != 0) {
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(kill_deadline - now())
                    .count();
            const int left_ms = static_cast<int>(left > 0 ? left : 0);
            wait_ms = wait_ms < 0 ? left_ms : std::min(wait_ms, left_ms);
        }

        // Всё дочитано и процесс завершился — выходим.
        if (child_exited && out_pipe.read_fd < 0 && err_pipe.read_fd < 0) {
            break;
        }
        if (child_exited && drain_deadline.time_since_epoch().count() != 0 &&
            now() >= drain_deadline) {
            break;
        }
        if (kill_deadline.time_since_epoch().count() != 0 && now() >= kill_deadline) {
            ::kill(-pid, SIGKILL);
            kill_deadline = {};
        }

        const int ready = ::poll(fds, static_cast<nfds_t>(n), wait_ms);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        const auto drain = [&](int slot, Pipe &pipe, LineWriter &writer) {
            if (slot < 0 || (fds[slot].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
                return;
            }
            char buf[65536];
            for (;;) {
                const ssize_t got = ::read(pipe.read_fd, buf, sizeof buf);
                if (got > 0) {
                    writer.feed(std::string_view(buf, static_cast<std::size_t>(got)));
                    continue;
                }
                if (got == 0) {
                    writer.flush();
                    pipe.close_read();
                    return;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;
                }
                if (errno == EINTR) {
                    continue;
                }
                writer.flush();
                pipe.close_read();
                return;
            }
        };

        drain(out_slot, out_pipe, out_writer);
        drain(err_slot, err_pipe, err_writer);

        if ((fds[sig_slot].revents & POLLIN) != 0) {
            signalfd_siginfo info{};
            while (::read(sig_fd, &info, sizeof info) == sizeof info) {
                if (info.ssi_signo == SIGCHLD) {
                    int status = 0;
                    if (::waitpid(pid, &status, WNOHANG) == pid) {
                        child_exited = true;
                        child_status = status;
                        // Службы Wine могут держать наши конвейеры открытыми,
                        // поэтому после выхода процесса ждём вывод ограниченно.
                        drain_deadline = now() + std::chrono::milliseconds(kDrainGraceMs);
                    }
                } else {
                    // Мягкий сигнал уходит всей группе: Wine превратит его в
                    // консольное событие для Windows-дерева.
                    signalled = true;
                    ::kill(-pid, static_cast<int>(info.ssi_signo));
                    kill_deadline = now() + std::chrono::milliseconds(kKillGraceMs);
                }
            }
        }

        if (timer_slot >= 0 && (fds[timer_slot].revents & POLLIN) != 0) {
            std::uint64_t ticks = 0;
            [[maybe_unused]] const ssize_t got = ::read(timer_fd, &ticks, sizeof ticks);
            timed_out = true;
            ::kill(-pid, SIGKILL);
        }
    }

    out_writer.flush();
    err_writer.flush();
    out_pipe.close_read();
    err_pipe.close_read();
    if (!child_exited) {
        ::waitpid(pid, &child_status, 0);
    }
    if (sig_fd >= 0) {
        ::close(sig_fd);
    }
    if (timer_fd >= 0) {
        ::close(timer_fd);
    }
    sigprocmask(SIG_UNBLOCK, &mask, nullptr);

    if (timed_out) {
        fmt::print(stderr, "cork: {}: timed out after {} (CORK_TIMEOUT), killed\n",
                   exe_name, env_or("CORK_TIMEOUT", "?"));
        return kExitTimeout;
    }
    if (signalled) {
        return kExitSignalled;
    }

    // --- разбор статуса ---
    auto raw_status = fs::read_file(run_dir / "status");
    if (!raw_status.has_value() || raw_status->empty()) {
        // Отсутствие статуса при нулевом коде wine — это не успех, а
        // «хелпер не запустился». Молча принять это за успех значит отдать
        // сборочной системе ноль там, где ничего не собиралось.
        // Код возврата самого wine здесь единственная зацепка, и без него
        // сообщение бесполезно: «не отчитался» не говорит, кто именно не
        // справился — wine, хелпер или запуск инструмента.
        const int wine_code =
            WIFEXITED(child_status) ? WEXITSTATUS(child_status) : -WTERMSIG(child_status);
        fmt::print(stderr,
                   "cork: the helper did not report a status (wine exited {}); "
                   "the tool may not have run at all.\n"
                   "Set CORK_KEEP_RUN=1 to keep {} for inspection.\n",
                   wine_code, run_dir.string());
        return kExitInternal;
    }

    proto::Status status;
    if (proto::decode(reinterpret_cast<const unsigned char *>(raw_status->data()),
                      raw_status->size(), status) != 0) {
        fmt::print(stderr, "cork: the helper wrote an unreadable status file\n");
        return kExitInternal;
    }

    switch (status.kind) {
        case proto::StatusKind::ChildExited: {
            const int code = map_exit_code(spec->name, status.code);
            // Препроцессированный вывод /P уходит не в поток, а в файл, и
            // фильтр потоков до него не достаёт: DOS-пути остаются внутри, в
            // каждой директиве #line. Чинится только после того, как
            // компилятор закончил, и только при успехе — на отказе файл
            // неполон, и трогать его незачем.
            if (code == 0) {
                for (const auto &out : preprocessed_outputs(args, stdfs::current_path())) {
                    if (auto r = rewrite_preprocessed_file(out, filter_cfg); !r.has_value()) {
                        fmt::print(stderr, "cork: {}\n", r.error().to_string());
                        return kExitInternal;
                    }
                }
            }
            return code;
        }
        case proto::StatusKind::SpawnFailed:
            fmt::print(stderr, "cork: could not start {}: Windows error {}\n", exe_name,
                       status.code);
            return kExitSpawnFailed;
        case proto::StatusKind::BadRequest:
            fmt::print(stderr, "cork: the helper rejected the request (reason {})\n",
                       status.code);
            return kExitInternal;
    }
    return kExitInternal;
}

} // namespace cork::exec
