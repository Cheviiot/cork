#pragma once

// Версии и диапазоны версий из манифеста установщика Visual Studio.
//
// Диапазоны нужны потому, что зависимости в манифесте ограничивают версию, и
// ограничение это обязано доехать до выбора пакета. Стоит его потерять — и
// запрос «нужна 2.0» молча получает 1.0, а расхождение всплывает
// потом, где-нибудь в середине сборки.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cork::manifest {

// До четырёх числовых частей, как принято у Microsoft (14.44.35207.0).
// Отсутствующая часть считается нулём, поэтому 14.44 и 14.44.0.0 равны.
struct Version {
    std::uint32_t parts[4] = {0, 0, 0, 0};

    // Суффикс после числовой части (например "-preview"). В сравнении не
    // участвует: в манифестах он встречается как пометка канала, а не как
    // младший разряд, и упорядочивать по нему было бы домыслом.
    std::string suffix;

    static Version parse(std::string_view);
    [[nodiscard]] std::string to_string() const;

    friend bool operator==(const Version &a, const Version &b);
    friend bool operator<(const Version &a, const Version &b);
    friend bool operator<=(const Version &a, const Version &b) { return a < b || a == b; }
    friend bool operator>(const Version &a, const Version &b) { return b < a; }
    friend bool operator>=(const Version &a, const Version &b) { return b <= a; }
    friend bool operator!=(const Version &a, const Version &b) { return !(a == b); }
};

// Нотация NuGet, в которой манифесты записывают зависимости:
//   "1.0"        — не ниже 1.0
//   "[1.0]"      — ровно 1.0
//   "[1.0,2.0)"  — от 1.0 включительно до 2.0 исключительно
//   "(1.0,2.0]"  — от 1.0 исключительно до 2.0 включительно
//   "[1.0,)"     — не ниже 1.0
//   "(,2.0]"     — не выше 2.0
//   ""           — любая
class VersionRange {
public:
    static VersionRange any() { return {}; }
    static VersionRange parse(std::string_view);

    [[nodiscard]] bool contains(const Version &) const;
    [[nodiscard]] bool is_any() const { return !has_min_ && !has_max_; }
    [[nodiscard]] const std::string &text() const { return text_; }

private:
    std::string text_;
    Version min_;
    Version max_;
    bool has_min_ = false;
    bool has_max_ = false;
    bool min_inclusive_ = true;
    bool max_inclusive_ = true;
};

} // namespace cork::manifest
