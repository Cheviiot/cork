#include "setup/runtime.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

#include <sys/utsname.h>

#include <fmt/format.h>

#include "archive/zip.hpp"
#include "base/fs.hpp"

#include "cork_embedded_runtime_pins.h"

namespace cork::setup {
namespace {

namespace stdfs = std::filesystem;

bool looks_like_sha256(std::string_view s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
               return std::isxdigit(c) != 0;
           });
}

} // namespace

Result<std::vector<RuntimePin>> parse_runtime_pins(std::string_view text) {
    std::vector<RuntimePin> out;
    std::istringstream in{std::string(text)};
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        if (const auto hash = line.find('#'); hash != std::string::npos) {
            line.erase(hash);
        }
        std::istringstream fields{line};
        RuntimePin pin;
        std::string extra;
        if (!(fields >> pin.wine_id >> pin.host_arch >> pin.sha256 >> pin.url)) {
            // Пустая строка после снятия комментария — не ошибка, а всё
            // остальное неполное — ошибка.
            if (pin.wine_id.empty()) {
                continue;
            }
            return err_config(
                fmt::format("runtime pin on line {} needs four fields: id arch sha256 url",
                            number));
        }
        if (fields >> extra) {
            return err_config(
                fmt::format("runtime pin on line {} has more than four fields", number));
        }
        if (!looks_like_sha256(pin.sha256)) {
            return err_config(
                fmt::format("runtime pin on line {} has no sha256 where one belongs", number));
        }
        out.push_back(std::move(pin));
    }
    return out;
}

Result<std::vector<RuntimePin>> builtin_runtime_pins() {
    const auto bytes = assets::runtime_pins();
    return parse_runtime_pins(
        std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
}

std::string_view host_arch() {
    // Те же слова, что в именах артефактов, а не то, что печатает uname:
    // список закреплений читают и правят люди, и «x86_64» против «amd64» в
    // соседних строках — лишний повод ошибиться.
    struct utsname u {};
    if (::uname(&u) != 0) {
        return "unknown";
    }
    const std::string_view machine{u.machine};
    if (machine == "x86_64" || machine == "amd64") {
        return "amd64";
    }
    if (machine == "aarch64" || machine == "arm64") {
        return "arm64";
    }
    return "unknown";
}

Result<RuntimeInstall> install_runtime(const Root &root, std::string_view wine_id,
                                       const std::vector<RuntimePin> &pins,
                                       store::ArtifactStore &store, store::Progress &progress) {
    RuntimeInstall result;
    result.path = root.runtime(wine_id);

    if (fs::is_regular_file(result.path / "bin" / "wine")) {
        result.already_present = true;
        return result;
    }

    const std::string_view arch = host_arch();
    const auto pin =
        std::find_if(pins.begin(), pins.end(), [&](const RuntimePin &p) {
            return p.wine_id == wine_id && p.host_arch == arch;
        });
    if (pin == pins.end()) {
        // Отдельный текст на случай, когда закреплений нет вовсе: это не
        // «версия не та», а «публиковать ещё нечего», и человеку важно
        // различать эти два.
        if (pins.empty()) {
            return err_not_found(fmt::format(
                "no Wine runtime has been published yet, so {} cannot be fetched.\n"
                "Build it with tools/wine/build.sh and point {} at the result.",
                wine_id, result.path.string()));
        }
        return err_not_found(
            fmt::format("no published Wine runtime for {} on {}", wine_id, arch));
    }

    store::FetchRequest request;
    request.id.sha256 = pin->sha256;
    request.url = pin->url;
    request.label = fmt::format("Wine runtime {}", wine_id);

    auto blob = store.acquire(request, progress);
    if (!blob.has_value()) {
        return std::unexpected(std::move(blob).error().at("fetching the Wine runtime"));
    }

    // В сторону и переименованием, а не прямо на место. Прерывание посреди
    // распаковки оставило бы дерево, у которого есть bin/wine и нет половины
    // DLL, — то есть выглядящее установленным и неработающее.
    const stdfs::path staging = result.path.parent_path() / (std::string(wine_id) + ".unpacking");
    std::error_code ec;
    stdfs::remove_all(staging, ec);
    if (auto r = fs::mkdir_p(staging); !r.has_value()) {
        return std::unexpected(std::move(r).error());
    }

    auto stats = archive::extract_zip(*blob, staging);
    if (!stats.has_value()) {
        stdfs::remove_all(staging, ec);
        return std::unexpected(std::move(stats).error().at("unpacking the Wine runtime"));
    }

    if (!fs::is_regular_file(staging / "bin" / "wine")) {
        stdfs::remove_all(staging, ec);
        return err_verification(
            fmt::format("the runtime archive for {} has no bin/wine in it", wine_id));
    }

    stdfs::rename(staging, result.path, ec);
    if (ec) {
        stdfs::remove_all(staging, ec);
        return err_io(fmt::format("publishing the runtime at {}: {}", result.path.string(),
                                  ec.message()));
    }

    result.files = stats->files;
    result.symlinks = stats->symlinks;
    return result;
}

} // namespace cork::setup
