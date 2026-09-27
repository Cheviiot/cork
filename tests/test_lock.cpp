#include <string>

#include "base/fs.hpp"
#include "base/lock.hpp"
#include "check.h"

namespace fs = cork::fs;
using cork::Lock;

int main() {
    const auto dir = fs::stdfs::temp_directory_path() / "cork-test-lock";
    fs::stdfs::remove_all(dir);

    // Эксклюзивная берётся.
    auto a = Lock::acquire(dir, "toolchain", Lock::Mode::Exclusive);
    CHECK(a.has_value());
    CHECK(a && a->held());

    // Вторая эксклюзивная на то же имя в этом же процессе не возьмётся: flock
    // привязан к описанию открытого файла, а не к процессу, поэтому отдельный
    // open даёт отдельную блокировку и она конфликтует.
    {
        auto b = Lock::acquire(dir, "toolchain", Lock::Mode::Exclusive);
        CHECK(!b.has_value());
        if (!b) {
            CHECK_EQ(std::to_string(static_cast<int>(b.error().code)),
                     std::to_string(static_cast<int>(cork::Error::Code::Conflict)));
        }
    }

    // Другое имя не конфликтует — блокировки именованные, а не одна на всё.
    {
        auto other = Lock::acquire(dir, "wine", Lock::Mode::Exclusive);
        CHECK(other.has_value());
    }

    // После освобождения имя снова доступно.
    a->release();
    CHECK(!a->held());
    {
        auto c = Lock::acquire(dir, "toolchain", Lock::Mode::Exclusive);
        CHECK(c.has_value());
    }

    // Две разделяемые уживаются.
    {
        auto s1 = Lock::acquire(dir, "store", Lock::Mode::Shared);
        auto s2 = Lock::acquire(dir, "store", Lock::Mode::Shared);
        CHECK(s1.has_value());
        CHECK(s2.has_value());
    }

    // Перемещение передаёт владение и не освобождает блокировку дважды.
    {
        auto m1 = Lock::acquire(dir, "moved", Lock::Mode::Exclusive);
        CHECK(m1.has_value());
        Lock m2 = std::move(*m1);
        CHECK(m2.held());
        CHECK(!m1->held());
    }

    fs::stdfs::remove_all(dir);
    return cork::test::finish("test_lock");
}
