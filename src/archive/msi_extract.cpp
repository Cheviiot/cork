#include "archive/msi_extract.hpp"

#include <algorithm>
#include <map>
#include <vector>

#include <fmt/format.h>

#include "archive/cab.hpp"
#include "archive/msi.hpp"
#include "base/fs.hpp"
#include "base/md5.hpp"

namespace cork::archive {
namespace {

namespace stdfs = std::filesystem;

Result<void> verify(const stdfs::path &path, const std::array<std::uint8_t, 16> &expected,
                    std::string_view key) {
    auto content = fs::read_file(path);
    if (!content) {
        return std::unexpected(std::move(content).error());
    }
    Md5 md5;
    md5.update(*content);
    auto got = md5.finish();
    if (got != expected) {
        return err_verification(fmt::format("'{}': MsiFileHash says {}, content is {}", key,
                                            Md5::to_hex(expected), Md5::to_hex(got)));
    }
    return {};
}

} // namespace

Result<MsiExtractStats> extract_msi(const stdfs::path &msi_path, const stdfs::path &dest,
                                    const MsiExtractOptions &opts) {
    auto data = fs::read_file(msi_path);
    if (!data) {
        return std::unexpected(std::move(data).error().at(fmt::format("opening {}", msi_path.string())));
    }
    auto msi = Msi::parse(std::move(*data));
    if (!msi) {
        return std::unexpected(std::move(msi).error().at(fmt::format("reading {}", msi_path.string())));
    }
    auto files = msi->files();
    if (!files) {
        return std::unexpected(std::move(files).error().at(fmt::format("reading {}", msi_path.string())));
    }

    // Файлы группируются по архиву, чтобы каждый архив открывался ровно один
    // раз: у пакетов с десятком Media иначе получилось бы десять открытий на
    // каждый файл. map, а не unordered_map, ради воспроизводимого порядка —
    // отчёт о распаковке входит в дайджест поколения.
    std::map<std::string, std::vector<const MsiFile *>> by_cabinet;
    for (const auto &f : *files) {
        if (opts.accept && !opts.accept(f.path)) {
            continue;
        }
        by_cabinet[f.cabinet].push_back(&f);
    }

    MsiExtractStats stats;
    for (const auto &[cabinet, group] : by_cabinet) {
        if (cabinet.empty()) {
            // Файл не в архиве: он лежит отдельным пейлоадом под своим именем.
            // Встречается редко (17 файлов на 109 пакетов Windows SDK, и все —
            // вложенные установщики), но пропустить его молча нельзя.
            for (const MsiFile *f : group) {
                if ((f->attributes & kMsiFileNoncompressed) == 0) {
                    return err_format(fmt::format(
                        "file '{}' names no cabinet but is not marked as uncompressed", f->key));
                }
                if (!opts.locate) {
                    return err_unsupported(
                        fmt::format("file '{}' is stored uncompressed and no source was provided",
                                    f->key));
                }
                auto name = f->path.substr(f->path.find_last_of('/') + 1);
                auto src = opts.locate(name);
                if (!src) {
                    return std::unexpected(std::move(src).error().at(
                        fmt::format("locating uncompressed file '{}'", name)));
                }
                auto target = fs::safe_join(dest, f->path);
                if (!target) {
                    return std::unexpected(std::move(target).error());
                }
                if (auto r = fs::mkdir_p(target->parent_path()); !r) {
                    return std::unexpected(std::move(r).error());
                }
                std::error_code ec;
                stdfs::copy_file(*src, *target, stdfs::copy_options::overwrite_existing, ec);
                if (ec) {
                    return err_io(fmt::format("copying '{}' to {}: {}", name, target->string(),
                                              ec.message()));
                }
                ++stats.files;
                ++stats.noncompressed;
                stats.bytes += f->size;
            }
            continue;
        }

        // Ведущий '#' означает, что архив спрятан потоком внутри самого .msi,
        // а не лежит отдельным файлом. Так устроен каждый третий пакет SDK.
        const bool embedded = cabinet.front() == '#';
        std::string embedded_bytes;
        Result<Cab> cab = err_internal("unreachable");
        if (embedded) {
            auto stream = msi->stream(cabinet.substr(1));
            if (!stream) {
                return std::unexpected(std::move(stream).error().at(
                    fmt::format("opening embedded cabinet '{}'", cabinet)));
            }
            embedded_bytes = std::move(*stream);
            cab = Cab::open_memory(embedded_bytes, cabinet);
        } else {
            if (!opts.locate) {
                return err_unsupported(
                    fmt::format("cabinet '{}' is external and no source was provided", cabinet));
            }
            auto src = opts.locate(cabinet);
            if (!src) {
                return std::unexpected(
                    std::move(src).error().at(fmt::format("locating cabinet '{}'", cabinet)));
            }
            cab = Cab::open(*src);
        }
        if (!cab) {
            return std::unexpected(std::move(cab).error());
        }

        for (const MsiFile *f : group) {
            auto target = fs::safe_join(dest, f->path);
            if (!target) {
                return std::unexpected(std::move(target).error().at(
                    fmt::format("placing '{}' from {}", f->key, msi_path.string())));
            }
            // Имя внутри архива — первичный ключ таблицы File, а не имя файла
            // на диске: одно и то же содержимое может устанавливаться под
            // разными именами, и связывает их именно ключ.
            if (auto r = cab->extract(f->key, *target); !r) {
                return std::unexpected(std::move(r).error());
            }
            if (opts.verify_hashes && f->md5) {
                if (auto r = verify(*target, *f->md5, f->key); !r) {
                    return std::unexpected(std::move(r).error());
                }
                ++stats.hashes_verified;
            }
            ++stats.files;
            stats.bytes += f->size;
            if (embedded) {
                ++stats.from_embedded_cab;
            } else {
                ++stats.from_external_cab;
            }
        }
    }
    return stats;
}

} // namespace cork::archive
