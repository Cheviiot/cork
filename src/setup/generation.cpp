#include "setup/generation.hpp"

#include <cerrno>
#include <cstdlib>
#include <random>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

#include <fmt/format.h>

#include "base/fs.hpp"
#include "base/lock.hpp"

namespace cork::setup {
namespace {

namespace stdfs = std::filesystem;

// Имя каталога сборки. Достаточно, чтобы два процесса на одной машине не
// столкнулись; уникальность на весь мир здесь не нужна и не проверяется.
std::string unique_token() {
    static std::mt19937_64 rng{std::random_device{}()};
    return fmt::format("{:08x}{:016x}", static_cast<unsigned>(::getpid()), rng());
}

// Первые двенадцать шестнадцатеричных знаков дайджеста. Этого хватает, чтобы
// два разных дерева не получили одно имя, и мало, чтобы имя осталось
// читаемым.
std::string digest_suffix(std::string_view digest) {
    const auto colon = digest.find(':');
    std::string_view hex = colon == std::string_view::npos ? digest : digest.substr(colon + 1);
    return std::string(hex.substr(0, std::min<std::size_t>(hex.size(), 12)));
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

// Замена символической ссылки без промежуточного состояния: новая ссылка
// создаётся под временным именем в том же каталоге и переименовывается поверх
// старой. rename(2) поверх существующего пути атомарен, поэтому момента, в
// который current не существует, не бывает.
Result<void> replace_symlink(const stdfs::path &link, const stdfs::path &target) {
    const stdfs::path tmp = link.parent_path() / fmt::format(".{}.{}", link.filename().string(),
                                                             unique_token());
    std::error_code ec;
    stdfs::create_directory_symlink(target, tmp, ec);
    if (ec) {
        return err_io(fmt::format("creating {}: {}", tmp.string(), ec.message()));
    }
    if (::rename(tmp.c_str(), link.c_str()) != 0) {
        const int saved = errno;
        stdfs::remove(tmp, ec);
        return err_errno(fmt::format("replacing {}", link.string()), saved);
    }
    return {};
}

} // namespace

Root Root::from_environment() {
    Root r;
    if (const char *home = std::getenv("CORK_HOME"); home && *home) {
        r.base = home;
        return r;
    }
    const char *h = std::getenv("HOME");
    r.base = stdfs::path(h ? h : ".") / ".cork";
    return r;
}

Result<void> Root::ensure() const {
    for (const auto &dir : {store(), staging(), toolchains(), locks()}) {
        if (auto r = fs::mkdir_p(dir); !r) {
            return r;
        }
    }
    return {};
}

Result<stdfs::path> Root::resolve_current() const {
    std::error_code ec;
    const stdfs::path link = current();
    if (!stdfs::is_symlink(link, ec)) {
        return err_not_found(fmt::format("{} is not a symlink; nothing is installed yet",
                                         link.string()));
    }
    auto target = stdfs::read_symlink(link, ec);
    if (ec) {
        return err_io(fmt::format("reading {}: {}", link.string(), ec.message()));
    }
    stdfs::path resolved = target.is_absolute() ? target : link.parent_path() / target;
    if (!stdfs::is_directory(resolved, ec)) {
        // Единственный способ здесь оказаться — удалить каталог поколения
        // руками: публикация такого состояния не создаёт.
        return err_not_found(
            fmt::format("{} points at {}, which is not a directory", link.string(),
                        resolved.string()));
    }
    return resolved;
}

std::string generation_name(std::string_view name, std::string_view tree_digest) {
    return fmt::format("{}-{}", name, digest_suffix(tree_digest));
}

Staging::Staging(Staging &&other) noexcept
    : root_(std::move(other.root_)), path_(std::move(other.path_)), keep_(other.keep_) {
    other.path_.clear();
}

Staging &Staging::operator=(Staging &&other) noexcept {
    if (this != &other) {
        root_ = std::move(other.root_);
        path_ = std::move(other.path_);
        keep_ = other.keep_;
        other.path_.clear();
    }
    return *this;
}

Staging::~Staging() {
    if (path_.empty() || keep_) {
        return;
    }
    std::error_code ec;
    stdfs::remove_all(path_, ec);
}

Result<Staging> Staging::create(const Root &root) {
    if (auto r = root.ensure(); !r) {
        return std::unexpected(std::move(r).error());
    }
    Staging s;
    s.root_ = root;
    s.path_ = root.staging() / unique_token();
    if (auto r = fs::mkdir_p(s.path_); !r) {
        return std::unexpected(std::move(r).error());
    }
    return s;
}

Result<Staging> Staging::adopt(const Root &root, const stdfs::path &path) {
    if (!fs::is_dir(path)) {
        return err_not_found(fmt::format("{} is not a staging directory", path.string()));
    }
    Staging s;
    s.root_ = root;
    s.path_ = path;
    // Подхваченный каталог по умолчанию остаётся на диске: его создал не этот
    // объект, и удалять чужую работу при выходе из области видимости он не
    // вправе. Удалит его publish или сборка мусора.
    s.keep_ = true;
    return s;
}

Result<stdfs::path> Staging::publish(std::string_view name, std::string_view tree_digest) {
    if (path_.empty()) {
        return err_internal("publishing a staging directory that was moved away");
    }
    if (tree_digest.empty()) {
        return err_internal("publishing without a tree digest");
    }

    // Блокировка на каталог поколений, а не на dest: current и список
    // поколений общие для всех установок, и одновременное переключение
    // испортило бы обоим.
    auto lock = Lock::acquire(root_.locks(), "toolchains", Lock::Mode::Exclusive, -1);
    if (!lock) {
        return std::unexpected(std::move(lock).error().at("publishing a generation"));
    }

    const stdfs::path target = root_.toolchains() / generation_name(name, tree_digest);
    std::error_code ec;

    if (stdfs::exists(target, ec)) {
        // Это же самое дерево уже опубликовано: в имя входит его дайджест, так
        // что совпадение имени означает совпадение содержимого. Переписывать
        // проверенное нечем и незачем — остаётся переключить current.
        stdfs::remove_all(path_, ec);
        path_.clear();
    } else {
        if (::rename(path_.c_str(), target.c_str()) != 0) {
            const int saved = errno;
            if (saved == EXDEV) {
                // staging и toolchains обязаны лежать на одной файловой
                // системе — иначе публикация перестаёт быть атомарной и
                // становится долгим копированием, прерывание которого
                // оставляет полудерево. Это не то, что можно обойти молча.
                return err_io(fmt::format(
                    "{} and {} are on different filesystems, so publishing cannot be atomic",
                    root_.staging().string(), root_.toolchains().string()));
            }
            return err_errno(fmt::format("publishing {}", target.string()), saved);
        }
        path_.clear();
    }

    if (auto r = replace_symlink(root_.current(), target); !r) {
        return std::unexpected(std::move(r).error().at("switching current"));
    }
    return target;
}

Result<GcStats> collect_staging(const Root &root, std::chrono::seconds age) {
    GcStats stats;
    std::error_code ec;
    if (!stdfs::is_directory(root.staging(), ec)) {
        return stats;
    }

    const auto now = stdfs::file_time_type::clock::now();
    for (const auto &entry : stdfs::directory_iterator(root.staging(), ec)) {
        if (ec) {
            return err_io(fmt::format("listing {}: {}", root.staging().string(), ec.message()));
        }
        if (!entry.is_directory(ec)) {
            continue;
        }
        const auto written = entry.last_write_time(ec);
        if (ec) {
            continue;
        }
        if (now - written < age) {
            continue;
        }

        // Возраст сам по себе ничего не доказывает: распаковка Windows SDK
        // идёт минутами, и её каталог тоже «старый». Занятость определяется
        // блокировкой, которую ядро снимает само при падении держателя.
        auto busy = Lock::acquire(root.locks(), "staging-" + entry.path().filename().string(),
                                  Lock::Mode::Exclusive, 0);
        if (!busy) {
            continue;
        }

        const std::uint64_t size = tree_size(entry.path());
        const stdfs::path lock_file =
            root.locks() / ("staging-" + entry.path().filename().string() + ".lock");
        stdfs::remove_all(entry.path(), ec);
        if (ec) {
            return err_io(
                fmt::format("removing {}: {}", entry.path().string(), ec.message()));
        }
        // Файл блокировки убирается вместе с каталогом, иначе в locks/
        // накапливается по пустому файлу на каждую сборку за всю жизнь машины.
        // Гонки здесь нет: блокировка на него сейчас наша, а претендовать на
        // неё может только тот, кто пришёл за этим каталогом, которого уже нет.
        stdfs::remove(lock_file, ec);
        ec.clear();
        ++stats.staging_removed;
        stats.bytes_freed += size;
    }
    return stats;
}

Result<GcStats> collect_generations(const Root &root, std::size_t keep) {
    GcStats stats;
    auto generations = list_generations(root);
    if (!generations.has_value()) {
        return std::unexpected(std::move(generations).error());
    }

    const auto current = root.resolve_current();
    std::error_code ec;
    std::size_t kept = 0;
    for (const auto &gen : *generations) {
        // Текущее поколение не удаляется, даже если оно старее порога: на
        // него показывает вся установка, и его исчезновение — не
        // освобождение места, а поломка.
        const bool is_current = current.has_value() && *current == gen;
        if (kept < keep || is_current) {
            ++kept;
            continue;
        }

        const std::uint64_t size = tree_size(gen);
        stdfs::remove_all(gen, ec);
        if (ec) {
            return err_io(fmt::format("removing {}: {}", gen.string(), ec.message()));
        }
        ++stats.generations_removed;
        stats.bytes_freed += size;
    }
    return stats;
}

Result<std::vector<stdfs::path>> list_generations(const Root &root) {
    std::vector<stdfs::path> out;
    std::error_code ec;
    if (!stdfs::is_directory(root.toolchains(), ec)) {
        return out;
    }
    std::vector<std::pair<stdfs::file_time_type, stdfs::path>> found;
    for (const auto &entry : stdfs::directory_iterator(root.toolchains(), ec)) {
        if (ec) {
            return err_io(fmt::format("listing {}: {}", root.toolchains().string(), ec.message()));
        }
        // current — символическая ссылка на одно из поколений, а не поколение.
        if (entry.is_symlink(ec) || !entry.is_directory(ec)) {
            continue;
        }
        found.emplace_back(entry.last_write_time(ec), entry.path());
    }
    // Равные mtime разводятся именем: три поколения, опубликованные подряд,
    // на файловой системе с грубым временем получают одно и то же, а порядок
    // здесь решает, что удалит сборка мусора.
    std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) {
        return a.first != b.first ? a.first > b.first : a.second > b.second;
    });
    out.reserve(found.size());
    for (auto &[when, path] : found) {
        out.push_back(std::move(path));
    }
    return out;
}

} // namespace cork::setup
