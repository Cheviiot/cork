#pragma once

// Межпроцессная блокировка через flock(2).
//
// Блокировка берётся на именованный общий артефакт, а не на каталог
// назначения. Разница принципиальная: Wine, кэш и префикс общие для всех
// установок сразу, и блокировка на «свой» каталог не мешает двум процессам
// одновременно полезть в общий кэш. Здесь блокировка берётся на
// именованный общий артефакт (блоб, поколение, runtime Wine, префикс), и имя —
// часть вызова, чтобы нельзя было случайно взять «не ту» блокировку.
//
// flock выбран потому, что ядро снимает его само при закрытии дескриптора: после
// падения или SIGKILL держателя чистить нечего. Файл-маркер с проверкой на
// существование такого свойства не имеет.

#include <filesystem>
#include <string>
#include <string_view>

#include "base/error.hpp"

namespace cork {

class Lock {
public:
    enum class Mode { Shared, Exclusive };

    Lock() = default;
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;
    Lock(Lock &&) noexcept;
    Lock &operator=(Lock &&) noexcept;
    ~Lock();

    // Берёт блокировку на dir/<name>.lock. wait_ms == 0 — не ждать вовсе и
    // сразу вернуть Conflict с pid держателя; отрицательное — ждать без
    // ограничения.
    [[nodiscard]] static Result<Lock> acquire(const std::filesystem::path &dir,
                                              std::string_view name, Mode mode,
                                              int wait_ms = 0);

    void release();
    [[nodiscard]] bool held() const { return fd_ >= 0; }

private:
    int fd_ = -1;
    std::filesystem::path path_;
};

} // namespace cork
