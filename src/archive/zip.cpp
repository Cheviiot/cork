#include "archive/zip.hpp"

#include <cerrno>
#include <cstdio>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include <zip.h>

#include "base/fs.hpp"

namespace cork::archive {
namespace {

namespace stdfs = std::filesystem;

// Тип записи в zip кодируется старшими 16 битами external_attr — теми же
// битами, что st_mode в Unix, — но только если запись создана на Unix
// (create_system == ZIP_OPSYS_UNIX). Именно это поле и игнорировал прежний
// распаковщик: он всегда открывал файл на запись.
constexpr std::uint32_t kUnixModeShift = 16;

struct EntryMode {
    bool from_unix = false;
    std::uint32_t mode = 0;  // st_mode целиком, включая тип

    [[nodiscard]] bool is_symlink() const { return from_unix && S_ISLNK(mode); }
    [[nodiscard]] bool is_dir_bit() const { return from_unix && S_ISDIR(mode); }
    [[nodiscard]] std::uint32_t permissions() const { return mode & 0777u; }
};

EntryMode entry_mode(zip_t *za, zip_uint64_t index) {
    EntryMode out;
    zip_uint8_t opsys = 0;
    zip_uint32_t attributes = 0;
    if (zip_file_get_external_attributes(za, index, 0, &opsys, &attributes) != 0) {
        return out;
    }
    if (opsys != ZIP_OPSYS_UNIX) {
        return out;
    }
    out.from_unix = true;
    out.mode = attributes >> kUnixModeShift;
    return out;
}

std::string url_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(s[i + 1]);
            const int lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

std::string normalize_separators(std::string s) {
    for (auto &c : s) {
        if (c == '\\') {
            c = '/';
        }
    }
    return s;
}

Result<std::string> read_entry(zip_t *za, zip_uint64_t index, zip_uint64_t size,
                               const std::string &name) {
    zip_file_t *f = zip_fopen_index(za, index, 0);
    if (f == nullptr) {
        return err_format("opening zip entry \"" + name + "\": " + zip_strerror(za));
    }
    std::string data(static_cast<std::size_t>(size), '\0');
    zip_int64_t total = 0;
    while (static_cast<zip_uint64_t>(total) < size) {
        const zip_int64_t n =
            zip_fread(f, data.data() + total, size - static_cast<zip_uint64_t>(total));
        if (n < 0) {
            zip_fclose(f);
            return err_format("reading zip entry \"" + name + "\"");
        }
        if (n == 0) {
            break;
        }
        total += n;
    }
    zip_fclose(f);
    if (static_cast<zip_uint64_t>(total) != size) {
        return err_format("zip entry \"" + name + "\" is shorter than its declared size");
    }
    return data;
}

} // namespace

Result<ZipStats> extract_zip(const stdfs::path &archive_path, const stdfs::path &dest,
                             const ZipOptions &opts) {
    int err = 0;
    zip_t *za = zip_open(archive_path.c_str(), ZIP_RDONLY, &err);
    if (za == nullptr) {
        zip_error_t error;
        zip_error_init_with_code(&error, err);
        std::string message = zip_error_strerror(&error);
        zip_error_fini(&error);
        return err_format("opening " + archive_path.string() + ": " + message);
    }

    struct Closer {
        zip_t *z;
        ~Closer() { zip_close(z); }
    } closer{za};

    if (auto r = fs::mkdir_p(dest); !r.has_value()) {
        return std::unexpected(std::move(r.error()).at("preparing extraction directory"));
    }

    ZipStats stats;
    const zip_int64_t count = zip_get_num_entries(za, 0);

    for (zip_int64_t i = 0; i < count; ++i) {
        const auto index = static_cast<zip_uint64_t>(i);
        const char *raw_name = zip_get_name(za, index, 0);
        if (raw_name == nullptr) {
            return err_format("reading entry name from " + archive_path.string());
        }

        std::string name = normalize_separators(
            opts.url_decode_names ? url_decode(raw_name) : std::string(raw_name));

        if (!opts.strip_prefix.empty()) {
            if (name.rfind(opts.strip_prefix, 0) != 0) {
                continue;
            }
            name = name.substr(opts.strip_prefix.size());
            if (name.empty()) {
                continue;
            }
        }
        if (opts.accept && !opts.accept(name)) {
            continue;
        }

        auto target = fs::safe_join(dest, name);
        if (!target.has_value()) {
            return std::unexpected(
                std::move(target.error()).at("extracting " + archive_path.string()));
        }

        zip_stat_t st;
        zip_stat_init(&st);
        if (zip_stat_index(za, index, 0, &st) != 0) {
            return err_format("stat of zip entry \"" + name + "\"");
        }

        const EntryMode mode = entry_mode(za, index);

        // Каталог опознаётся и по завершающему слэшу, и по биту типа: разные
        // упаковщики пишут по-разному.
        if (name.back() == '/' || mode.is_dir_bit()) {
            if (auto r = fs::mkdir_p(*target); !r.has_value()) {
                return std::unexpected(std::move(r.error()));
            }
            ++stats.directories;
            continue;
        }

        if (auto r = fs::mkdir_p(target->parent_path()); !r.has_value()) {
            return std::unexpected(std::move(r.error()));
        }

        if (mode.is_symlink()) {
            // Тело записи символьной ссылки — это её цель.
            auto link_target = read_entry(za, index, st.size, name);
            if (!link_target.has_value()) {
                return std::unexpected(std::move(link_target.error()));
            }
            // Цель проверяется отдельно от имени: архив может положить
            // безобидное имя, целью которого будет ../../../etc/passwd.
            if (!fs::symlink_target_inside(dest, *target, *link_target)) {
                return err_verification("zip entry \"" + name + "\" is a symlink to \"" +
                                        *link_target + "\", which escapes " + dest.string());
            }
            std::error_code ec;
            stdfs::remove(*target, ec);
            if (::symlink(link_target->c_str(), target->c_str()) != 0) {
                return err_errno("creating symlink " + target->string(), errno);
            }
            ++stats.symlinks;
            continue;
        }

        auto data = read_entry(za, index, st.size, name);
        if (!data.has_value()) {
            return std::unexpected(std::move(data.error()));
        }

        // Права берутся из архива, если он их нёс: иначе теряется бит
        // исполнения, а с ним и работоспособность всего, что мы распаковали.
        stdfs::perms perms = stdfs::perms::owner_read | stdfs::perms::owner_write |
                             stdfs::perms::group_read | stdfs::perms::others_read;
        if (mode.from_unix && mode.permissions() != 0) {
            perms = static_cast<stdfs::perms>(mode.permissions());
        }

        if (auto r = fs::write_atomic(
                *target,
                std::span<const std::byte>(reinterpret_cast<const std::byte *>(data->data()),
                                           data->size()),
                perms);
            !r.has_value()) {
            return std::unexpected(std::move(r.error()).at("extracting \"" + name + "\""));
        }
        ++stats.files;
        stats.bytes += st.size;
    }

    return stats;
}

} // namespace cork::archive
