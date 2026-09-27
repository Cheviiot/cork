#include "base/fs.hpp"

#include <cerrno>
#include <cstdio>
#include <system_error>
#include <vector>

#include <unistd.h>

namespace cork::fs {
namespace {

// Лексическая нормализация без обращения к диску: нам надо уметь проверять пути
// записей архива до того, как что-либо создано. weakly_canonical здесь не
// годится — он резолвит существующие символьные ссылки, а на несуществующих
// путях поведение зависит от реализации.
stdfs::path normalize(const stdfs::path &p) { return p.lexically_normal(); }

bool starts_with_path(const stdfs::path &whole, const stdfs::path &prefix) {
    auto w = whole.begin();
    auto p = prefix.begin();
    for (; p != prefix.end(); ++p, ++w) {
        if (w == whole.end() || *w != *p) {
            return false;
        }
    }
    return true;
}

} // namespace

Result<stdfs::path> safe_join(const stdfs::path &base, std::string_view relative) {
    const stdfs::path rel(relative);

    if (rel.is_absolute() || rel.has_root_name() || rel.has_root_directory()) {
        return err_verification(
            "archive entry \"" + std::string(relative) + "\" is an absolute path");
    }

    const stdfs::path base_norm = normalize(base);
    const stdfs::path target = normalize(base_norm / rel);

    if (!starts_with_path(target, base_norm)) {
        return err_verification("archive entry \"" + std::string(relative) +
                                "\" escapes the extraction directory " + base_norm.string());
    }
    return target;
}

bool symlink_target_inside(const stdfs::path &base, const stdfs::path &link_path,
                           std::string_view target) {
    const stdfs::path t(target);
    if (t.is_absolute() || t.has_root_name() || t.has_root_directory()) {
        return false;
    }
    const stdfs::path base_norm = normalize(base);
    // Цель разрешается от каталога, в котором лежит сама ссылка — так её потом
    // разрешит ядро.
    const stdfs::path resolved = normalize(link_path.parent_path() / t);
    return starts_with_path(resolved, base_norm);
}

Result<void> mkdir_p(const stdfs::path &p) {
    std::error_code ec;
    stdfs::create_directories(p, ec);
    // create_directories возвращает false и без ошибки, если каталог уже был —
    // это не отказ.
    if (ec) {
        return err_io("creating directory " + p.string() + ": " + ec.message());
    }
    return {};
}

Result<void> write_atomic(const stdfs::path &path, std::span<const std::byte> data,
                          stdfs::perms perms) {
    const stdfs::path dir = path.parent_path().empty() ? stdfs::path(".") : path.parent_path();
    stdfs::path tmp = dir / (path.filename().string() + ".tmp-" + std::to_string(::getpid()));

    std::FILE *f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr) {
        return err_errno("creating " + tmp.string(), errno);
    }
    if (!data.empty() && std::fwrite(data.data(), 1, data.size(), f) != data.size()) {
        const int e = errno;
        std::fclose(f);
        std::remove(tmp.c_str());
        return err_errno("writing " + tmp.string(), e);
    }
    if (std::fclose(f) != 0) {
        const int e = errno;
        std::remove(tmp.c_str());
        return err_errno("closing " + tmp.string(), e);
    }

    std::error_code ec;
    stdfs::permissions(tmp, perms, ec);
    if (ec) {
        stdfs::remove(tmp, ec);
        return err_io("setting permissions on " + tmp.string() + ": " + ec.message());
    }

    stdfs::rename(tmp, path, ec);
    if (ec) {
        stdfs::remove(tmp, ec);
        return err_io("renaming " + tmp.string() + " to " + path.string() + ": " + ec.message());
    }
    return {};
}

Result<void> write_atomic(const stdfs::path &path, std::string_view text, stdfs::perms perms) {
    return write_atomic(path,
                        std::span<const std::byte>(
                            reinterpret_cast<const std::byte *>(text.data()), text.size()),
                        perms);
}

Result<std::string> read_file(const stdfs::path &path) {
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return err_errno("opening " + path.string(), errno);
    }
    std::string out;
    char buf[65536];
    for (;;) {
        const std::size_t n = std::fread(buf, 1, sizeof buf, f);
        out.append(buf, n);
        if (n < sizeof buf) {
            if (std::ferror(f) != 0) {
                const int e = errno;
                std::fclose(f);
                return err_errno("reading " + path.string(), e);
            }
            break;
        }
    }
    std::fclose(f);
    return out;
}

bool is_dir(const stdfs::path &p) {
    std::error_code ec;
    return stdfs::is_directory(p, ec);
}

bool is_regular_file(const stdfs::path &p) {
    std::error_code ec;
    return stdfs::is_regular_file(p, ec);
}

bool exists_no_follow(const stdfs::path &p) {
    std::error_code ec;
    const auto st = stdfs::symlink_status(p, ec);
    return !ec && st.type() != stdfs::file_type::not_found;
}

} // namespace cork::fs
