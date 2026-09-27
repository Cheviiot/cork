#include "base/lock.hpp"

#include <cerrno>
#include <chrono>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include "base/fs.hpp"

namespace cork {
namespace {

// Имя держателя пишется в сам файл блокировки, чтобы в сообщении об отказе было
// видно, кого ждать. Это подсказка, а не источник истины: держатель мог умереть,
// и тогда flock уже снят, а текст остался.
std::string holder_hint(const std::filesystem::path &path) {
    auto content = fs::read_file(path);
    if (!content.has_value() || content->empty()) {
        return "another cork process";
    }
    return "pid " + *content;
}

} // namespace

Lock::Lock(Lock &&other) noexcept : fd_(other.fd_), path_(std::move(other.path_)) {
    other.fd_ = -1;
}

Lock &Lock::operator=(Lock &&other) noexcept {
    if (this != &other) {
        release();
        fd_ = other.fd_;
        path_ = std::move(other.path_);
        other.fd_ = -1;
    }
    return *this;
}

Lock::~Lock() { release(); }

Result<Lock> Lock::acquire(const std::filesystem::path &dir, std::string_view name, Mode mode,
                           int wait_ms) {
    if (auto r = fs::mkdir_p(dir); !r.has_value()) {
        return std::unexpected(std::move(r.error()).at("preparing lock directory"));
    }

    const std::filesystem::path path = dir / (std::string(name) + ".lock");

    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        return err_errno("opening lock file " + path.string(), errno);
    }

    const int op = (mode == Mode::Exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB;

    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(wait_ms < 0 ? 0 : wait_ms);

    for (;;) {
        if (::flock(fd, op) == 0) {
            break;
        }
        if (errno != EWOULDBLOCK) {
            const int e = errno;
            ::close(fd);
            return err_errno("locking " + path.string(), e);
        }
        // Ждём, только если попросили. По умолчанию второй запуск против того же
        // артефакта отказывает сразу: молча встать в очередь — значит скрыть от
        // пользователя, что он запустил две одинаковые операции.
        if (wait_ms == 0 || (wait_ms > 0 && clock::now() >= deadline)) {
            const std::string who = holder_hint(path);
            ::close(fd);
            return err_conflict("another cork process is already working on \"" +
                                std::string(name) + "\" (" + who + ")");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Записываем pid уже под блокировкой — иначе два процесса могли бы
    // перетереть подсказку друг друга.
    const std::string pid = std::to_string(::getpid());
    if (::ftruncate(fd, 0) == 0) {
        [[maybe_unused]] const ssize_t written = ::pwrite(fd, pid.data(), pid.size(), 0);
    }

    Lock lock;
    lock.fd_ = fd;
    lock.path_ = path;
    return lock;
}

void Lock::release() {
    if (fd_ < 0) {
        return;
    }
    // Явный LOCK_UN не нужен — close снимает блокировку, — но он делает намерение
    // видимым и снимает её на миг раньше.
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
    fd_ = -1;
}

} // namespace cork
