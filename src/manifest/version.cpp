#include "manifest/version.hpp"

#include <algorithm>
#include <cctype>

namespace cork::manifest {
namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.front())) != 0)) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.back())) != 0)) {
        s.remove_suffix(1);
    }
    return s;
}

} // namespace

Version Version::parse(std::string_view text) {
    Version v;
    text = trim(text);

    std::size_t i = 0;
    for (int part = 0; part < 4 && i < text.size(); ++part) {
        std::uint64_t value = 0;
        const std::size_t start = i;
        while (i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) != 0)) {
            // Насыщение вместо переполнения: манифест — внешние данные, и
            // абсурдно длинное число не должно тихо превращаться в маленькое.
            value = std::min<std::uint64_t>(value * 10 + static_cast<std::uint64_t>(text[i] - '0'),
                                            0xffffffffull);
            ++i;
        }
        if (i == start) {
            break;  // не цифра там, где ожидалось число
        }
        v.parts[part] = static_cast<std::uint32_t>(value);
        if (i < text.size() && text[i] == '.') {
            ++i;
        } else {
            break;
        }
    }

    if (i < text.size()) {
        v.suffix = std::string(text.substr(i));
    }
    return v;
}

std::string Version::to_string() const {
    std::string out;
    for (int i = 0; i < 4; ++i) {
        if (i > 0) {
            out += '.';
        }
        out += std::to_string(parts[i]);
    }
    out += suffix;
    return out;
}

bool operator==(const Version &a, const Version &b) {
    return a.parts[0] == b.parts[0] && a.parts[1] == b.parts[1] && a.parts[2] == b.parts[2] &&
           a.parts[3] == b.parts[3];
}

bool operator<(const Version &a, const Version &b) {
    for (int i = 0; i < 4; ++i) {
        if (a.parts[i] != b.parts[i]) {
            return a.parts[i] < b.parts[i];
        }
    }
    return false;
}

VersionRange VersionRange::parse(std::string_view text) {
    VersionRange r;
    text = trim(text);
    r.text_ = std::string(text);

    if (text.empty()) {
        return r;
    }

    const bool bracketed = (text.front() == '[' || text.front() == '(') &&
                           (text.back() == ']' || text.back() == ')');
    if (!bracketed) {
        // Голая версия в нотации NuGet означает «не ниже».
        r.min_ = Version::parse(text);
        r.has_min_ = true;
        r.min_inclusive_ = true;
        return r;
    }

    r.min_inclusive_ = text.front() == '[';
    r.max_inclusive_ = text.back() == ']';
    const std::string_view inner = text.substr(1, text.size() - 2);

    const std::size_t comma = inner.find(',');
    if (comma == std::string_view::npos) {
        // "[1.0]" — ровно эта версия. Для "(1.0)" такого смысла нет, но и
        // вреда тоже: диапазон получится пустым, и ни один вариант в него не
        // попадёт, что честнее молчаливого «сойдёт любая».
        const Version exact = Version::parse(inner);
        r.min_ = exact;
        r.max_ = exact;
        r.has_min_ = true;
        r.has_max_ = true;
        return r;
    }

    const std::string_view lo = trim(inner.substr(0, comma));
    const std::string_view hi = trim(inner.substr(comma + 1));
    if (!lo.empty()) {
        r.min_ = Version::parse(lo);
        r.has_min_ = true;
    }
    if (!hi.empty()) {
        r.max_ = Version::parse(hi);
        r.has_max_ = true;
    }
    return r;
}

bool VersionRange::contains(const Version &v) const {
    if (has_min_) {
        if (min_inclusive_ ? (v < min_) : (v <= min_)) {
            return false;
        }
    }
    if (has_max_) {
        if (max_inclusive_ ? (max_ < v) : (max_ <= v)) {
            return false;
        }
    }
    return true;
}

} // namespace cork::manifest
