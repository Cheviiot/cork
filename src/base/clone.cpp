#include "base/clone.hpp"

#include <cerrno>
#include <cstring>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fmt/format.h>

#include "base/fs.hpp"

namespace cork::fs {
namespace {

namespace stdfs = std::filesystem;

constexpr std::size_t kChunk = 1 << 20;

class Fd {
public:
    explicit Fd(int fd) : fd_(fd) {}
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    ~Fd() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    [[nodiscard]] int get() const { return fd_; }
    [[nodiscard]] bool ok() const { return fd_ >= 0; }
    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_;
};

// Копирование содержимого одним из трёх способов, от дешёвого к дорогому.
// Способ, сработавший первым, запоминается и дальше пробуется сразу: перебор
// на каждом из тысяч файлов префикса обошёлся бы дороже самого копирования.
Result<void> copy_contents(int in, int out, std::uint64_t size, CloneMethod &method) {
    if (method == CloneMethod::Reflink) {
        if (::ioctl(out, FICLONE, in) == 0) {
            return {};
        }
        // EOPNOTSUPP и EXDEV означают «эта файловая система так не умеет» или
        // «файлы на разных». Оба — не ошибка, а повод перейти к следующему
        // способу и больше к этому не возвращаться.
        method = CloneMethod::CopyFileRange;
    }

    if (method == CloneMethod::CopyFileRange) {
        off_t in_off = 0;
        off_t out_off = 0;
        std::uint64_t left = size;
        bool worked = true;
        while (left > 0) {
            const ssize_t n = ::copy_file_range(in, &in_off, out, &out_off,
                                                static_cast<std::size_t>(left), 0);
            if (n <= 0) {
                worked = false;
                break;
            }
            left -= static_cast<std::uint64_t>(n);
        }
        if (worked) {
            return {};
        }
        method = CloneMethod::ReadWrite;
        if (::lseek(in, 0, SEEK_SET) < 0 || ::lseek(out, 0, SEEK_SET) < 0) {
            return err_errno("rewinding after a failed copy_file_range", errno);
        }
        if (::ftruncate(out, 0) != 0) {
            return err_errno("truncating after a failed copy_file_range", errno);
        }
    }

    std::vector<char> buf(kChunk);
    for (;;) {
        const ssize_t n = ::read(in, buf.data(), buf.size());
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return err_errno("reading while copying", errno);
        }
        ssize_t written = 0;
        while (written < n) {
            const ssize_t w = ::write(out, buf.data() + written, static_cast<std::size_t>(n - written));
            if (w < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return err_errno("writing while copying", errno);
            }
            written += w;
        }
    }
    return {};
}

Result<void> clone_file(const stdfs::path &from, const stdfs::path &to, const struct stat &st,
                        CloneMethod &method) {
    Fd in(::open(from.c_str(), O_RDONLY | O_CLOEXEC));
    if (!in.ok()) {
        return err_errno(fmt::format("opening {}", from.string()), errno);
    }
    Fd out(::open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                  st.st_mode & 07777));
    if (!out.ok()) {
        return err_errno(fmt::format("creating {}", to.string()), errno);
    }
    if (auto r = copy_contents(in.get(), out.get(), static_cast<std::uint64_t>(st.st_size),
                               method);
        !r) {
        return r;
    }
    // Права ставятся явно: umask мог срезать биты при создании, а в префиксе
    // Wine исполняемость файлов значима.
    if (::fchmod(out.get(), st.st_mode & 07777) != 0) {
        return err_errno(fmt::format("setting permissions on {}", to.string()), errno);
    }
    // Время изменения переносится, чтобы Wine не решил, что префикс обновился
    // и не запустил update_wineprefix на каждую сборку.
    struct timespec times[2];
    times[0] = st.st_atim;
    times[1] = st.st_mtim;
    if (::futimens(out.get(), times) != 0) {
        return err_errno(fmt::format("setting times on {}", to.string()), errno);
    }
    return {};
}

} // namespace

const char *clone_method_name(CloneMethod m) {
    switch (m) {
    case CloneMethod::Reflink: return "reflink";
    case CloneMethod::CopyFileRange: return "copy_file_range";
    case CloneMethod::ReadWrite: return "read/write";
    }
    return "unknown";
}

bool supports_reflink(const stdfs::path &dir) {
    std::error_code ec;
    stdfs::create_directories(dir, ec);
    const stdfs::path src = dir / ".cork-reflink-probe-src";
    const stdfs::path dst = dir / ".cork-reflink-probe-dst";

    bool ok = false;
    {
        Fd in(::open(src.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
        if (in.ok()) {
            const char byte = 'x';
            (void)!::write(in.get(), &byte, 1);
            Fd out(::open(dst.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
            // Проверяется настоящей попыткой, а не именем файловой системы:
            // btrfs без reflink и ext4 с ним одинаково возможны, и гадать по
            // имени значит однажды угадать неверно.
            ok = out.ok() && ::ioctl(out.get(), FICLONE, in.get()) == 0;
        }
    }
    stdfs::remove(src, ec);
    stdfs::remove(dst, ec);
    return ok;
}

Result<CloneStats> clone_tree(const stdfs::path &from, const stdfs::path &to) {
    std::error_code ec;
    if (!stdfs::is_directory(from, ec)) {
        return err_not_found(fmt::format("{} is not a directory", from.string()));
    }
    if (stdfs::exists(to, ec) && !stdfs::is_empty(to, ec)) {
        // Слияние двух деревьев даёт результат, который потом невозможно
        // объяснить: часть файлов из одного, часть из другого, и никакой
        // записи о том, какая именно.
        return err_conflict(fmt::format("{} already exists and is not empty", to.string()));
    }
    if (auto r = mkdir_p(to); !r) {
        return std::unexpected(std::move(r).error());
    }

    CloneStats stats;
    // Каталоги создаются до того, как в них что-то кладётся, поэтому обход
    // идёт сверху вниз, а времена на каталогах ставятся вторым проходом: иначе
    // создание файла внутри сдвинуло бы только что выставленное время.
    std::vector<std::pair<stdfs::path, struct stat>> dirs;

    stdfs::recursive_directory_iterator it(from, stdfs::directory_options::none, ec);
    if (ec) {
        return err_io(fmt::format("walking {}: {}", from.string(), ec.message()));
    }
    for (; it != stdfs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            return err_io(fmt::format("walking {}: {}", from.string(), ec.message()));
        }
        const stdfs::path rel = it->path().lexically_relative(from);
        const stdfs::path dst = to / rel;

        struct stat st{};
        if (::lstat(it->path().c_str(), &st) != 0) {
            return err_errno(fmt::format("stat {}", it->path().string()), errno);
        }

        if (S_ISLNK(st.st_mode)) {
            auto target = stdfs::read_symlink(it->path(), ec);
            if (ec) {
                return err_io(fmt::format("reading link {}: {}", it->path().string(),
                                          ec.message()));
            }
            stdfs::create_symlink(target, dst, ec);
            if (ec) {
                return err_io(fmt::format("creating link {}: {}", dst.string(), ec.message()));
            }
            ++stats.symlinks;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            stdfs::create_directory(dst, ec);
            if (ec) {
                return err_io(fmt::format("creating {}: {}", dst.string(), ec.message()));
            }
            stdfs::permissions(dst, static_cast<stdfs::perms>(st.st_mode & 07777), ec);
            dirs.emplace_back(dst, st);
            ++stats.directories;
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            // Сокеты, устройства и каналы в префиксе Wine не появляются, а
            // если появились — это не то, что стоит копировать молча.
            return err_unsupported(
                fmt::format("{} is neither a file, a directory nor a symlink",
                            it->path().string()));
        }

        if (auto r = clone_file(it->path(), dst, st, stats.method); !r) {
            return std::unexpected(std::move(r).error());
        }
        ++stats.files;
        stats.bytes += static_cast<std::uint64_t>(st.st_size);
    }

    // Времена каталогов — последними и в обратном порядке, чтобы вложенные не
    // сбивали время родителей.
    for (auto d = dirs.rbegin(); d != dirs.rend(); ++d) {
        struct timespec times[2];
        times[0] = d->second.st_atim;
        times[1] = d->second.st_mtim;
        if (::utimensat(AT_FDCWD, d->first.c_str(), times, AT_SYMLINK_NOFOLLOW) != 0) {
            return err_errno(fmt::format("setting times on {}", d->first.string()), errno);
        }
    }
    return stats;
}

bool files_identical(const stdfs::path &a, const stdfs::path &b) {
    struct stat sa {};
    struct stat sb {};
    if (::stat(a.c_str(), &sa) != 0 || ::stat(b.c_str(), &sb) != 0) {
        return false;
    }
    if (!S_ISREG(sa.st_mode) || !S_ISREG(sb.st_mode) || sa.st_size != sb.st_size) {
        return false;
    }
    // Один и тот же файл — уже одно и то же место на диске, делить нечего.
    if (sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino) {
        return false;
    }
    Fd fa(::open(a.c_str(), O_RDONLY | O_CLOEXEC));
    Fd fb(::open(b.c_str(), O_RDONLY | O_CLOEXEC));
    if (!fa.ok() || !fb.ok()) {
        return false;
    }
    std::vector<char> ba(kChunk);
    std::vector<char> bb(kChunk);
    for (;;) {
        const ssize_t ra = ::read(fa.get(), ba.data(), ba.size());
        const ssize_t rb = ::read(fb.get(), bb.data(), bb.size());
        if (ra < 0 || rb < 0 || ra != rb) {
            return false;
        }
        if (ra == 0) {
            return true;
        }
        if (std::memcmp(ba.data(), bb.data(), static_cast<std::size_t>(ra)) != 0) {
            return false;
        }
    }
}

Result<void> reflink_replace(const stdfs::path &from, const stdfs::path &to) {
    struct stat st {};
    if (::stat(to.c_str(), &st) != 0) {
        return err_errno(fmt::format("stat {}", to.string()), errno);
    }
    Fd in(::open(from.c_str(), O_RDONLY | O_CLOEXEC));
    if (!in.ok()) {
        return err_errno(fmt::format("opening {}", from.string()), errno);
    }
    // Через временный файл рядом и rename: прерывание на середине не должно
    // оставить в префиксе обрезанную DLL.
    const stdfs::path tmp = to.parent_path() / (to.filename().string() + ".reflink.tmp");
    Fd out(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 07777));
    if (!out.ok()) {
        return err_errno(fmt::format("creating {}", tmp.string()), errno);
    }
    if (::ioctl(out.get(), FICLONE, in.get()) != 0) {
        const int saved = errno;
        out.close();
        std::error_code ec;
        stdfs::remove(tmp, ec);
        return err_errno(fmt::format("reflinking {} onto {}", from.string(), to.string()), saved);
    }
    if (::fchmod(out.get(), st.st_mode & 07777) != 0) {
        return err_errno(fmt::format("setting permissions on {}", tmp.string()), errno);
    }
    // Время изменения — как у заменяемого файла: по mtime wine.inf Wine
    // решает, обновлять ли префикс, и сбив его, мы получили бы
    // update_wineprefix на каждую сборку.
    struct timespec times[2];
    times[0] = st.st_atim;
    times[1] = st.st_mtim;
    if (::futimens(out.get(), times) != 0) {
        return err_errno(fmt::format("setting times on {}", tmp.string()), errno);
    }
    out.close();
    std::error_code ec;
    stdfs::rename(tmp, to, ec);
    if (ec) {
        stdfs::remove(tmp, ec);
        return err_io(fmt::format("replacing {}: {}", to.string(), ec.message()));
    }
    return {};
}

} // namespace cork::fs
