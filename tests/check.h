#pragma once

// Тесты без фреймворка — конвенция, принятая в проектах на этой машине.
// Каждый тест это отдельный main(), регистрируемый через add_test. Никакой
// зависимости, никакого discovery, падение видно по коду возврата и строке с
// номером.

#include <cstdio>
#include <string>

namespace cork::test {

inline int g_failures = 0;

inline void report(bool ok, const char *expr, const char *file, int line, const std::string &extra) {
    if (ok) {
        return;
    }
    ++g_failures;
    std::fprintf(stderr, "%s:%d: FAIL %s", file, line, expr);
    if (!extra.empty()) {
        std::fprintf(stderr, "\n  %s", extra.c_str());
    }
    std::fputc('\n', stderr);
}

inline int finish(const char *name) {
    if (g_failures == 0) {
        std::fprintf(stderr, "%s: ok\n", name);
        return 0;
    }
    std::fprintf(stderr, "%s: %d check(s) failed\n", name, g_failures);
    return 1;
}

} // namespace cork::test

#define CHECK(expr) ::cork::test::report((expr), #expr, __FILE__, __LINE__, {})

#define CHECK_EQ(a, b)                                                                     \
    do {                                                                                   \
        const auto &check_a = (a);                                                         \
        const auto &check_b = (b);                                                         \
        ::cork::test::report(check_a == check_b, #a " == " #b, __FILE__, __LINE__,       \
                                ::cork::test::describe(check_a, check_b));               \
    } while (false)

namespace cork::test {

// Универсального способа напечатать произвольный тип нет, поэтому поддержаны
// только те, что реально сравниваются в тестах. Остальное печатается без
// подробностей — этого хватает, чтобы найти строку.
template <class A, class B>
inline std::string describe(const A &, const B &) {
    return {};
}

inline std::string describe(const std::string &a, const std::string &b) {
    return "got: " + a + "\n  want: " + b;
}

inline std::string describe(const std::string &a, const char *b) {
    return "got: " + a + "\n  want: " + std::string(b);
}

} // namespace cork::test
