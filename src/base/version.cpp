#include "base/version.hpp"

#include <algorithm>
#include <cstdlib>

// В fmt 12 fmt::format живёт в format.h, а core.h оставлен только для базовой
// части (print и вывод в поток).
#include <fmt/format.h>

namespace cork {

std::string format_version(std::string_view version, std::string_view revision, bool dirty) {
    std::string out = fmt::format("cork {}", version);
    if (revision.empty()) {
        return out;
    }
    // Двенадцати знаков хватает, чтобы коммит был однозначен, и при этом строка
    // остаётся читаемой в отчёте об ошибке.
    std::string_view shortened = revision.substr(0, std::min<std::size_t>(revision.size(), 12));
    out += fmt::format(" ({}{})", shortened, dirty ? "-dirty" : "");
    return out;
}

std::string version_string() { return format_version(kVersion, kGitRevision, kGitDirty); }

std::string wine_runtime_id() {
    const char *v = std::getenv("CORK_WINE_ID");
    return v != nullptr && *v != '\0' ? std::string(v) : std::string(kDefaultWineId);
}

} // namespace cork
