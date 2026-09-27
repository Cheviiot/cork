#include "setup/digest.hpp"

#include <algorithm>
#include <system_error>

#include <fmt/format.h>

#include "base/sha256.hpp"

namespace cork::setup {
namespace {

namespace stdfs = std::filesystem;

struct Entry {
    std::string path;
    char kind = 'f';  // f — файл, d — каталог, l — символьная ссылка
    bool executable = false;
    std::uint64_t size = 0;
    std::string detail;  // sha256 файла или цель ссылки
};

// Путь относительно корня, всегда с '/' в качестве разделителя. Сравнение
// пойдёт по байтам этой строки, поэтому важно, чтобы она не зависела от
// платформы.
std::string relative_of(const stdfs::path &root, const stdfs::path &p) {
    std::string s = p.lexically_relative(root).generic_string();
    return s;
}

} // namespace

Result<TreeDigest> digest_tree(const stdfs::path &root, const DigestOptions &opts) {
    std::error_code ec;
    if (!stdfs::is_directory(root, ec)) {
        return err_not_found(fmt::format("{} is not a directory", root.string()));
    }

    std::vector<std::string> exclude = opts.exclude;
    std::sort(exclude.begin(), exclude.end());
    const auto excluded = [&exclude](const std::string &rel) {
        return std::binary_search(exclude.begin(), exclude.end(), rel);
    };

    std::vector<Entry> entries;
    TreeStats stats;

    // follow_directory_symlink намеренно выключен: ссылка на каталог — это
    // запись со своей целью, а не второй проход по тому же поддереву. С
    // переходом по ней обход мог бы и вовсе не завершиться.
    stdfs::recursive_directory_iterator it(
        root, stdfs::directory_options::skip_permission_denied, ec);
    if (ec) {
        return err_io(fmt::format("walking {}: {}", root.string(), ec.message()));
    }

    std::uint64_t seen = 0;
    for (const stdfs::directory_entry &de : it) {
        const std::string rel = relative_of(root, de.path());
        if (excluded(rel)) {
            // Исключается и поддерево: иначе пришлось бы перечислять каждый
            // файл внутри исключённого каталога.
            if (de.is_directory(ec)) {
                it.disable_recursion_pending();
            }
            continue;
        }

        Entry e;
        e.path = rel;

        auto status = de.symlink_status(ec);
        if (ec) {
            return err_io(fmt::format("reading {}: {}", de.path().string(), ec.message()));
        }

        if (stdfs::is_symlink(status)) {
            e.kind = 'l';
            auto target = stdfs::read_symlink(de.path(), ec);
            if (ec) {
                return err_io(
                    fmt::format("reading link {}: {}", de.path().string(), ec.message()));
            }
            e.detail = target.generic_string();
            ++stats.symlinks;
        } else if (stdfs::is_directory(status)) {
            e.kind = 'd';
            ++stats.directories;
        } else if (stdfs::is_regular_file(status)) {
            e.kind = 'f';
            e.size = de.file_size(ec);
            if (ec) {
                return err_io(
                    fmt::format("sizing {}: {}", de.path().string(), ec.message()));
            }
            // Из прав берётся только бит исполнения: остальные зависят от
            // umask распаковщика и различались бы между машинами, ничего не
            // говоря о целости дерева. А вот потеря +x у cl.exe — поломка.
            e.executable = (status.permissions() & stdfs::perms::owner_exec) != stdfs::perms::none;
            if (opts.mode == DigestMode::Content) {
                auto hex = Sha256::hex_of_file(de.path());
                if (!hex) {
                    return std::unexpected(std::move(hex).error());
                }
                e.detail = std::move(*hex);
            }
            ++stats.files;
            stats.bytes += e.size;
        } else {
            return err_unsupported(
                fmt::format("{} is neither a file, a directory nor a symlink", de.path().string()));
        }

        entries.push_back(std::move(e));
        if (opts.on_progress && ++seen % 1000 == 0) {
            opts.on_progress(seen);
        }
    }

    // Порядок обхода каталога не определён ничем, поэтому перечисление
    // сортируется. Без этого один и тот же каталог давал бы разные дайджесты
    // на разных файловых системах.
    std::sort(entries.begin(), entries.end(),
              [](const Entry &a, const Entry &b) { return a.path < b.path; });

    Sha256 sha;
    for (const auto &e : entries) {
        // Поля разделены табуляцией, записи — переводом строки. В путях оба
        // символа возможны, поэтому перед хешированием длина пути пишется
        // явно: иначе "a\tb" и файл "a" с полем "b" дали бы один дайджест.
        sha.update(fmt::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\n", e.kind, e.path.size(), e.path,
                               e.executable ? 1 : 0, e.size, e.detail.size(), e.detail));
    }

    TreeDigest out;
    out.digest = "sha256:" + Sha256::to_hex(sha.finish());
    out.mode = opts.mode;
    out.stats = stats;
    return out;
}

} // namespace cork::setup
