#include "setup/prefix.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <system_error>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fmt/format.h>

#include "base/clone.hpp"
#include "base/fs.hpp"

namespace cork::setup {
namespace {

namespace stdfs = std::filesystem;

// Файлы реестра Wine. Все три лежат в корне префикса, и все три способны
// хранить пути в домашний каталог создателя.
constexpr const char *kRegistryFiles[] = {"user.reg", "system.reg", "userdef.reg"};

std::string home_dir() {
    const char *h = std::getenv("HOME");
    return h != nullptr ? h : std::string();
}

std::string user_name() {
    for (const char *var : {"USER", "LOGNAME", "USERNAME"}) {
        if (const char *v = std::getenv(var); v != nullptr && *v != '\0') {
            return v;
        }
    }
    return {};
}

// Каталог профиля Windows внутри префикса: drive_c/users/<кто-то>.
// Их может быть несколько, но wineboot создаёт один — по имени того, кто его
// запустил.
std::vector<stdfs::path> user_profiles(const stdfs::path &prefix) {
    std::vector<stdfs::path> out;
    std::error_code ec;
    const stdfs::path users = prefix / "drive_c" / "users";
    for (const auto &entry : stdfs::directory_iterator(users, ec)) {
        if (ec) {
            break;
        }
        const std::string name = entry.path().filename().string();
        // Public — общий профиль, он не привязан к человеку.
        if (entry.is_directory(ec) && name != "Public") {
            out.push_back(entry.path());
        }
    }
    return out;
}

// DOS-форма unix-пути, как её пишет Wine в реестр: диск Z и обратные слэши,
// причём в .reg-файле каждый слэш ещё и удвоен.
std::string dos_escaped(std::string_view unix_path) {
    std::string out = "Z:";
    for (const char c : unix_path) {
        if (c == '/') {
            out += "\\\\";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

} // namespace

Result<DepersonaliseStats> depersonalise_prefix(const stdfs::path &prefix) {
    DepersonaliseStats stats;
    std::error_code ec;
    if (!stdfs::is_directory(prefix, ec)) {
        return err_not_found(fmt::format("{} is not a prefix", prefix.string()));
    }

    // 1. Символьные ссылки профиля. wineboot делает Desktop, Documents,
    // Downloads и прочие ссылками в домашний каталог — и туда же будет писать
    // всё, что запущено в этом префиксе, у кого бы он ни оказался.
    //
    // Заодно запоминаем, куда ссылка вела: этим же путём она записана в
    // реестре, и починить его без этой карты пришлось бы угадыванием.
    //
    // Обход рекурсивный, а не только по верхнему уровню профиля: такие ссылки
    // есть и глубже — например, AppData/Roaming/Microsoft/Windows/Templates
    // указывает в домашний каталог ровно так же, как Desktop.
    std::map<std::string, std::string> link_targets;  // unix-путь -> имя папки
    const std::string home = home_dir();
    for (const auto &profile : user_profiles(prefix)) {
        std::vector<stdfs::path> links;
        for (stdfs::recursive_directory_iterator it(profile,
                                                    stdfs::directory_options::skip_permission_denied,
                                                    ec);
             it != stdfs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) {
                break;
            }
            if (it->is_symlink(ec)) {
                links.push_back(it->path());
                // Внутрь ссылки не спускаемся: она ведёт в домашний каталог,
                // и обход ушёл бы гулять по нему целиком.
                it.disable_recursion_pending();
            }
        }
        for (const auto &link : links) {
            auto target = stdfs::read_symlink(link, ec);
            if (ec) {
                continue;
            }
            // Ссылки внутрь самого префикса законны и трогать их незачем;
            // убираем только те, что уводят наружу, в чужой домашний каталог.
            const std::string target_str = target.string();
            if (!home.empty() && target_str.find(home) != 0) {
                continue;
            }
            link_targets.emplace(target_str, link.filename().string());
            stdfs::remove(link, ec);
            stdfs::create_directory(link, ec);
            if (ec) {
                return err_io(fmt::format("replacing {}: {}", link.string(), ec.message()));
            }
            ++stats.links_replaced;
        }
    }

    // 2. Последовательные и параллельные порты. Это ссылки на устройства
    // конкретной машины: /dev/ttyS0 у одного есть, у другого нет, а у третьего
    // это вообще другое устройство.
    const stdfs::path dosdevices = prefix / "dosdevices";
    for (const auto &entry : stdfs::directory_iterator(dosdevices, ec)) {
        if (ec) {
            break;
        }
        const std::string name = entry.path().filename().string();
        const bool is_port = (name.rfind("com", 0) == 0 || name.rfind("lpt", 0) == 0) &&
                             name.size() >= 4 &&
                             std::isdigit(static_cast<unsigned char>(name[3])) != 0;
        if (!is_port) {
            continue;
        }
        stdfs::remove(entry.path(), ec);
        ++stats.devices_removed;
    }

    // 3. Реестр. Пути хранятся там в DOS-форме через диск Z, то есть ссылаются
    // на файловую систему хоста напрямую. Заменяем их на путь внутри профиля —
    // тот самый каталог, который только что появился вместо ссылки.
    const std::string user = user_name();
    for (const char *name : kRegistryFiles) {
        const stdfs::path reg = prefix / name;
        if (!fs::is_regular_file(reg)) {
            continue;
        }
        auto content = fs::read_file(reg);
        if (!content) {
            return std::unexpected(std::move(content).error());
        }
        std::string text = *content;
        bool touched = false;

        for (const auto &[unix_path, folder] : link_targets) {
            const std::string needle = dos_escaped(unix_path);
            const std::string replacement =
                fmt::format("C:\\\\users\\\\{}\\\\{}", user, folder);
            for (std::size_t at = text.find(needle); at != std::string::npos;
                 at = text.find(needle, at + replacement.size())) {
                text.replace(at, needle.size(), replacement);
                touched = true;
                ++stats.registry_edits;
            }
        }
        // Остаётся то, что ссылками профиля не объясняется: следы установки
        // — откуда ставили Wine Mono, где лежал дистрибутив. Работе они не
        // нужны, а машину выдают. Заменяются на корень диска C, потому что
        // удалить строку целиком значит испортить формат .reg.
        if (!home.empty()) {
            const std::string host = dos_escaped(home);
            for (std::size_t at = text.find(host); at != std::string::npos;
                 at = text.find(host, at)) {
                std::size_t end = at + host.size();
                // Съедаем и хвост пути до закрывающей кавычки: без этого
                // остался бы обрубок вида "C:\\Project\\Tools\\cork".
                while (end < text.size() && text[end] != '"') {
                    ++end;
                }
                text.replace(at, end - at, "C:\\\\");
                touched = true;
                ++stats.registry_edits;
            }
        }

        // Имя профиля в путях реестра заменяется на нейтральное — тем же
        // именем назван и сам каталог после переименования ниже.
        if (!user.empty()) {
            const std::string from = fmt::format("\\\\users\\\\{}\\\\", user);
            const std::string to = "\\\\users\\\\default\\\\";
            for (std::size_t at = text.find(from); at != std::string::npos;
                 at = text.find(from, at + to.size())) {
                text.replace(at, from.size(), to);
                touched = true;
                ++stats.registry_edits;
            }
        }

        if (touched) {
            if (auto r = fs::write_atomic(reg, text); !r) {
                return std::unexpected(std::move(r).error());
            }
        }
    }

    // 4. Имя каталога профиля. Wine называет его по тому, кто запускал
    // wineboot, и у другого пользователя такой профиль остался бы лежать
    // мёртвым грузом, а TEMP указывал бы в несуществующий каталог.
    // Переименовываем в нейтральное; обратно, в имя текущего пользователя,
    // его вернёт instantiate_prefix при создании сессии.
    for (const auto &profile : user_profiles(prefix)) {
        if (profile.filename().string() == "default") {
            continue;
        }
        stdfs::rename(profile, profile.parent_path() / "default", ec);
        if (ec) {
            return err_io(fmt::format("renaming {}: {}", profile.string(), ec.message()));
        }
    }
    return stats;
}

Result<void> instantiate_prefix(const stdfs::path &prefix, const std::string &user) {
    if (user.empty() || user == "default") {
        return {};
    }
    std::error_code ec;
    const stdfs::path from = prefix / "drive_c" / "users" / "default";
    const stdfs::path to = prefix / "drive_c" / "users" / user;
    if (!stdfs::is_directory(from, ec) || stdfs::exists(to, ec)) {
        return {};
    }
    stdfs::rename(from, to, ec);
    if (ec) {
        return err_io(fmt::format("renaming the profile to {}: {}", user, ec.message()));
    }

    // Реестр знает профиль по имени, и без правки TEMP, Desktop и прочее
    // указывали бы в каталог, которого больше нет.
    for (const char *name : kRegistryFiles) {
        const stdfs::path reg = prefix / name;
        if (!fs::is_regular_file(reg)) {
            continue;
        }
        auto content = fs::read_file(reg);
        if (!content) {
            return std::unexpected(std::move(content).error());
        }
        std::string text = *content;
        const std::string needle = "\\\\users\\\\default\\\\";
        const std::string replacement = fmt::format("\\\\users\\\\{}\\\\", user);
        bool touched = false;
        for (std::size_t at = text.find(needle); at != std::string::npos;
             at = text.find(needle, at + replacement.size())) {
            text.replace(at, needle.size(), replacement);
            touched = true;
        }
        if (touched) {
            if (auto r = fs::write_atomic(reg, text); !r) {
                return std::unexpected(std::move(r).error());
            }
        }
    }
    return {};
}

Result<std::vector<PortabilityViolation>> check_prefix_portable(const stdfs::path &prefix,
                                                                const std::string &home,
                                                                const std::string &user) {
    std::vector<PortabilityViolation> found;
    std::error_code ec;
    if (!stdfs::is_directory(prefix, ec)) {
        return err_not_found(fmt::format("{} is not a prefix", prefix.string()));
    }

    // Что именно ищем. Домашний путь в обеих формах — unix и DOS через Z: — и
    // отдельно имя пользователя, потому что оно встречается там, где полного
    // пути нет.
    std::vector<std::string> needles;
    if (!home.empty()) {
        needles.push_back(home);
        needles.push_back(dos_escaped(home));
    }

    for (stdfs::recursive_directory_iterator it(prefix,
                                                stdfs::directory_options::skip_permission_denied,
                                                ec);
         it != stdfs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            return err_io(fmt::format("walking {}: {}", prefix.string(), ec.message()));
        }

        // Цель символьной ссылки — самая частая привязка к машине и при этом
        // невидимая для поиска по содержимому файлов.
        if (it->is_symlink(ec)) {
            const auto target = stdfs::read_symlink(it->path(), ec).string();
            for (const auto &needle : needles) {
                if (target.find(needle) != std::string::npos) {
                    found.push_back({it->path(), 0, fmt::format("symlink points at {}", target)});
                    break;
                }
            }
            continue;
        }

        // Содержимое проверяется только у реестра. Остальное — это DLL и
        // прочие двоичные файлы Wine, одинаковые у всех, и читать гигабайт
        // ради поиска строки, которой там быть не может, незачем.
        const std::string name = it->path().filename().string();
        const bool is_registry =
            std::any_of(std::begin(kRegistryFiles), std::end(kRegistryFiles),
                        [&name](const char *r) { return name == r; });
        if (!is_registry || !it->is_regular_file(ec)) {
            continue;
        }

        auto content = fs::read_file(it->path());
        if (!content) {
            return std::unexpected(std::move(content).error());
        }
        std::uint64_t line_no = 0;
        std::size_t start = 0;
        while (start < content->size()) {
            std::size_t end = content->find('\n', start);
            if (end == std::string::npos) {
                end = content->size();
            }
            ++line_no;
            const std::string_view line = std::string_view(*content).substr(start, end - start);
            for (const auto &needle : needles) {
                if (line.find(needle) != std::string_view::npos) {
                    found.push_back({it->path(), line_no, std::string(line)});
                    break;
                }
            }
            start = end + 1;
        }
    }

    // Имя пользователя в имени каталога профиля — тоже привязка. После
    // деперсонализации профиль называется default, и настоящее имя ему
    // возвращает instantiate_prefix уже на машине пользователя.
    if (!user.empty()) {
        for (const auto &profile : user_profiles(prefix)) {
            if (profile.filename().string() == user) {
                found.push_back({profile, 0, "profile directory is named after the builder"});
            }
        }
    }
    return found;
}

bool prefix_matches_wine(const stdfs::path &prefix, const stdfs::path &wine_runtime) {
    const stdfs::path stamp = prefix / ".update-timestamp";
    const stdfs::path inf = wine_runtime / "share" / "wine" / "wine.inf";
    if (!fs::is_regular_file(stamp) || !fs::is_regular_file(inf)) {
        return false;
    }
    std::ifstream in(stamp);
    std::int64_t recorded = 0;
    in >> recorded;
    std::error_code ec;
    const auto inf_time = stdfs::last_write_time(inf, ec);
    if (ec) {
        return false;
    }
    const auto inf_epoch = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::clock_cast<std::chrono::system_clock>(inf_time)
                                   .time_since_epoch())
                               .count();
    return recorded >= inf_epoch;
}

// Разделяет с runtime те файлы префикса, которые ему побайтово равны.
//
// wineboot раскладывает встроенные DLL в system32 и syswow64 копированием из
// runtime, и копии выходят точные: 1406 файлов при нуле отличающихся.
// Копированием настоящим, а не разделением экстентов, — проверено на btrfs:
// свежий префикс занимает 1,2 ГБ, из них 561,77 МиБ в одном только system32
// эксклюзивные. После reflink освобождается 1,1 ГБ, и префикс занимает
// 124 МиБ.
//
// Считать по `du` тут нельзя: он приписывает разделённый экстент каждому
// файлу отдельно и показывает 1,2 ГБ и до, и после. Нужен либо
// `btrfs filesystem du`, либо разница свободного места.
//
// Reflink, а не жёсткая ссылка, и это принципиально: Wine перезаписывает
// встроенные DLL в префиксе на месте (dlls/setupapi/fakedll.c,
// create_dest_file). Reflink это переживает — запись копирует экстент и
// разводит файлы обратно, — а жёсткая ссылка испортила бы общий runtime.
//
// Отсутствие reflink не отказ: на ext4 префикс просто останется как был.
void share_with_runtime(const stdfs::path &prefix, const stdfs::path &wine_runtime,
                        DepersonaliseStats &stats) {
    const std::pair<const char *, const char *> pairs[] = {
        {"drive_c/windows/system32", "lib/wine/x86_64-windows"},
        {"drive_c/windows/syswow64", "lib/wine/i386-windows"},
    };
    std::error_code ec;
    for (const auto &[in_prefix, in_runtime] : pairs) {
        const stdfs::path dir = prefix / in_prefix;
        const stdfs::path src_dir = wine_runtime / in_runtime;
        if (!fs::is_dir(dir) || !fs::is_dir(src_dir)) {
            continue;
        }
        for (const auto &entry : stdfs::directory_iterator(dir, ec)) {
            if (ec) {
                return;
            }
            if (!entry.is_regular_file(ec) || entry.is_symlink(ec)) {
                continue;
            }
            const stdfs::path src = src_dir / entry.path().filename();
            if (!fs::is_regular_file(src) || !fs::files_identical(src, entry.path())) {
                continue;
            }
            const auto size = entry.file_size(ec);
            if (ec) {
                continue;
            }
            if (!fs::reflink_replace(src, entry.path()).has_value()) {
                // Первый отказ означает, что файловая система так не умеет;
                // остальные тысячи файлов ответят тем же.
                return;
            }
            ++stats.shared_files;
            stats.shared_bytes += size;
        }
    }
}

Result<void> boot_prefix(const stdfs::path &wine_runtime, const stdfs::path &prefix) {
    const stdfs::path wine = wine_runtime / "bin" / "wine";
    if (!fs::is_regular_file(wine)) {
        return err_not_found("no wine at " + wine.string());
    }
    if (auto r = fs::mkdir_p(prefix); !r.has_value()) {
        return r;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        return err_io("fork for wineboot failed");
    }
    if (pid == 0) {
        // stdio в /dev/null: службы Wine, поднятые при бутстрапе, наследуют
        // дескрипторы и держат их открытыми дольше самого wineboot.
        const int null_fd = ::open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            ::dup2(null_fd, STDIN_FILENO);
            ::dup2(null_fd, STDOUT_FILENO);
            ::dup2(null_fd, STDERR_FILENO);
        }
        ::setsid();
        ::setenv("WINEPREFIX", prefix.c_str(), 1);
        ::setenv("WINEDEBUG", "-all", 1);
        ::execl(wine.c_str(), wine.c_str(), "wineboot", "--init", nullptr);
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return err_io("waiting for wineboot failed");
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return err_io("wineboot did not finish cleanly");
    }

    // Дождаться, пока wineserver закончит и сбросит реестр на диск.
    //
    // Реестр пишет не wineboot, а wineserver при завершении, и завершается он
    // не сразу — по умолчанию висит ещё несколько секунд. Без ожидания
    // префикс выглядит готовым, а system.reg, user.reg и .update-timestamp в
    // нём ещё не появились: шаблон, снятый в этот момент, получается вообще
    // без реестра, и cl в склонированном из него префиксе отвечает
    // «D8037: cannot create temporary il file», потому что TEMP взять неоткуда.
    //
    // Проверяется это дороже, чем кажется: каталог есть, drive_c на месте, всё
    // выглядит правильно, и не хватает ровно четырёх файлов в корне.
    const stdfs::path server = wine_runtime / "bin" / "wineserver";
    if (fs::is_regular_file(server)) {
        const pid_t wait_pid = ::fork();
        if (wait_pid == 0) {
            const int null_fd = ::open("/dev/null", O_RDWR);
            if (null_fd >= 0) {
                ::dup2(null_fd, STDIN_FILENO);
                ::dup2(null_fd, STDOUT_FILENO);
                ::dup2(null_fd, STDERR_FILENO);
            }
            ::setenv("WINEPREFIX", prefix.c_str(), 1);
            ::execl(server.c_str(), server.c_str(), "-w", nullptr);
            ::_exit(127);
        }
        if (wait_pid > 0) {
            int wait_status = 0;
            while (::waitpid(wait_pid, &wait_status, 0) < 0 && errno == EINTR) {
            }
        }
    }

    if (!fs::is_regular_file(prefix / "system.reg")) {
        return err_verification("wineboot left no registry in " + prefix.string());
    }

    // Сразу после загрузки, пока префиксом никто не пользовался: именно здесь
    // лежат нетронутые копии встроенных DLL, и именно здесь их дешевле всего
    // вернуть в общие экстенты. Молча — это внутренний шаг, а не результат,
    // о котором стоит докладывать.
    DepersonaliseStats ignored;
    share_with_runtime(prefix, wine_runtime, ignored);
    return {};
}

Result<DepersonaliseStats> build_prefix_template(const stdfs::path &prefix,
                                                 const stdfs::path &destination,
                                                 const stdfs::path &wine_runtime) {
    std::error_code ec;
    if (stdfs::exists(destination, ec)) {
        return err_conflict(fmt::format("{} already exists", destination.string()));
    }
    auto copied = fs::clone_tree(prefix, destination);
    if (!copied) {
        return std::unexpected(std::move(copied).error().at("copying the prefix"));
    }

    auto stats = depersonalise_prefix(destination);
    if (!stats) {
        stdfs::remove_all(destination, ec);
        return stats;
    }

    // Каталоги, которых в свежем префиксе нет, а сборке они нужны.
    //
    // wineboot --init их не создаёт: Wine заводит %TEMP% при первом
    // обращении. Префикс, которым уже пользовались, их поэтому имеет, а
    // только что поднятый — нет, и шаблон из него выглядит исправным ровно до
    // первого запуска компилятора: cl отвечает «D8037: cannot create
    // temporary il file» и больше ничего.
    //
    // Прежний шаблон работал по случайности — его делали из префикса, в
    // котором уже собирали. Стоило собрать шаблон честно, с нуля, и он
    // перестал годиться.
    for (const char *needed : {"users/default/Temp", "users/default/AppData/Local/Temp",
                               "windows/temp"}) {
        if (auto r = fs::mkdir_p(destination / "drive_c" / needed); !r.has_value()) {
            stdfs::remove_all(destination, ec);
            return std::unexpected(std::move(r).error().at("preparing the template"));
        }
    }

    if (!wine_runtime.empty()) {
        share_with_runtime(destination, wine_runtime, *stats);
    }

    // Гейт переносимости. Он здесь не для порядка: это единственное место, где
    // непереносимый шаблон можно остановить до того, как он попадёт к людям.
    // Поэтому найденное — отказ, а недоделанный шаблон убирается.
    auto violations = check_prefix_portable(destination, home_dir(), user_name());
    if (!violations) {
        stdfs::remove_all(destination, ec);
        return std::unexpected(std::move(violations).error());
    }
    if (!violations->empty()) {
        std::string detail;
        for (std::size_t i = 0; i < violations->size() && i < 10; ++i) {
            const auto &v = (*violations)[i];
            detail += fmt::format("\n  {}{}: {}", v.file.string(),
                                  v.line != 0 ? fmt::format(":{}", v.line) : "", v.detail);
        }
        if (violations->size() > 10) {
            detail += fmt::format("\n  ... and {} more", violations->size() - 10);
        }
        stdfs::remove_all(destination, ec);
        return err_verification(
            fmt::format("the prefix template still refers to this machine:{}", detail));
    }
    return stats;
}

} // namespace cork::setup
