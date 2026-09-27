// Символьные ссылки при распаковке zip.
//
// В архиве Wine таких записей под две сотни, и распаковщик, который их не
// понимает, превращает каждую в обычный файл с путём внутри — включая весь
// lib/mono/4.5, то есть Wine Mono, от которого зависит MSBuild.exe. Ошибка
// тихая: дерево выглядит целым.
//
// Фикстура собирается тем же libzip с теми же внешними атрибутами, какие
// ставит `zip -y`: create_system = UNIX, старшие 16 бит external_attr —
// это st_mode, то есть 0120777 для ссылки.

#include <string>
#include <sys/stat.h>

#include <zip.h>

#include "archive/zip.hpp"
#include "base/fs.hpp"
#include "check.h"

namespace fs = cork::fs;
using fs::stdfs::path;
using cork::archive::extract_zip;

namespace {

constexpr std::uint32_t kModeShift = 16;

// Добавляет запись и проставляет ей unix-атрибуты — именно так они выглядят в
// настоящем архиве, собранном `zip -y`.
bool add_entry(zip_t *za, const char *name, const std::string &content, std::uint32_t st_mode) {
    zip_source_t *src = zip_source_buffer(za, content.data(), content.size(), 0);
    if (src == nullptr) {
        return false;
    }
    const zip_int64_t idx = zip_file_add(za, name, src, ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8);
    if (idx < 0) {
        zip_source_free(src);
        return false;
    }
    return zip_file_set_external_attributes(za, static_cast<zip_uint64_t>(idx), 0, ZIP_OPSYS_UNIX,
                                            st_mode << kModeShift) == 0;
}

// Содержимое живёт до zip_close: libzip читает из буфера отложенно.
struct Fixture {
    std::string plain = "hello";
    std::string script = "#!/bin/sh\n";
    std::string link_to_wine = "wine";
    std::string link_deep = "../gac/System/4.0.0.0/System.dll";
};

bool build_zip(const path &file, Fixture &f) {
    int err = 0;
    zip_t *za = zip_open(file.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
    if (za == nullptr) {
        return false;
    }
    bool ok = true;
    ok = ok && zip_dir_add(za, "bin", ZIP_FL_ENC_UTF_8) >= 0;
    ok = ok && zip_dir_add(za, "lib", ZIP_FL_ENC_UTF_8) >= 0;
    ok = ok && zip_dir_add(za, "lib/gac", ZIP_FL_ENC_UTF_8) >= 0;
    ok = ok && zip_dir_add(za, "lib/mono", ZIP_FL_ENC_UTF_8) >= 0;
    ok = ok && add_entry(za, "bin/plain.txt", f.plain, S_IFREG | 0644);
    ok = ok && add_entry(za, "bin/script.sh", f.script, S_IFREG | 0755);
    // Ровно тот случай, что ломался: bin/msiexec -> wine, четыре байта.
    ok = ok && add_entry(za, "bin/msiexec", f.link_to_wine, S_IFLNK | 0777);
    // И тот, что составлял 123 файла из 172: ссылка вглубь дерева.
    ok = ok && add_entry(za, "lib/mono/System.dll", f.link_deep, S_IFLNK | 0777);
    zip_close(za);
    return ok;
}

} // namespace

int main() {
    const path work = path(CORK_SOURCE_DIR) / "build/zip-test";
    fs::stdfs::remove_all(work);
    CHECK(fs::mkdir_p(work).has_value());

    // --- обычная распаковка ---
    {
        const path archive = work / "good.zip";
        Fixture f;
        CHECK(build_zip(archive, f));

        const path dest = work / "out";
        auto stats = extract_zip(archive, dest);
        CHECK(stats.has_value());
        if (!stats) {
            std::fprintf(stderr, "%s\n", stats.error().to_string().c_str());
            return cork::test::finish("test_zip");
        }

        CHECK_EQ(std::to_string(stats->symlinks), std::string("2"));
        CHECK_EQ(std::to_string(stats->files), std::string("2"));

        // Вот это и есть регрессия: ссылка обязана остаться ссылкой.
        CHECK(fs::stdfs::is_symlink(dest / "bin/msiexec"));
        CHECK_EQ(fs::stdfs::read_symlink(dest / "bin/msiexec").string(), std::string("wine"));
        CHECK(fs::stdfs::is_symlink(dest / "lib/mono/System.dll"));
        CHECK_EQ(fs::stdfs::read_symlink(dest / "lib/mono/System.dll").string(),
                 std::string("../gac/System/4.0.0.0/System.dll"));

        // Обычный файл остаётся обычным, и содержимое цело.
        CHECK(fs::is_regular_file(dest / "bin/plain.txt"));
        auto content = fs::read_file(dest / "bin/plain.txt");
        CHECK(content.has_value());
        if (content) {
            CHECK_EQ(*content, std::string("hello"));
        }

        // Бит исполнения не теряется: без него распакованное дерево
        // нерабочее, а понять это можно только запустив.
        const auto perms = fs::stdfs::status(dest / "bin/script.sh").permissions();
        CHECK((perms & fs::stdfs::perms::owner_exec) != fs::stdfs::perms::none);
        const auto plain_perms = fs::stdfs::status(dest / "bin/plain.txt").permissions();
        CHECK((plain_perms & fs::stdfs::perms::owner_exec) == fs::stdfs::perms::none);
    }

    // --- имя записи уводит за корень ---
    {
        const path archive = work / "slip-name.zip";
        int err = 0;
        zip_t *za = zip_open(archive.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
        CHECK(za != nullptr);
        if (za != nullptr) {
            std::string content = "pwned";
            CHECK(add_entry(za, "../../escaped.txt", content, S_IFREG | 0644));
            zip_close(za);

            const path dest = work / "out-slip-name";
            auto stats = extract_zip(archive, dest);
            CHECK(!stats.has_value());
            CHECK(!fs::exists_no_follow(work / "escaped.txt"));
        }
    }

    // --- цель символьной ссылки уводит за корень ---
    // Проверка имени этого не ловит: имя здесь совершенно безобидное.
    {
        const path archive = work / "slip-link.zip";
        int err = 0;
        zip_t *za = zip_open(archive.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
        CHECK(za != nullptr);
        if (za != nullptr) {
            std::string target = "../../../../etc/passwd";
            CHECK(add_entry(za, "innocent/name", target, S_IFLNK | 0777));
            zip_close(za);

            const path dest = work / "out-slip-link";
            auto stats = extract_zip(archive, dest);
            CHECK(!stats.has_value());
            if (!stats) {
                CHECK(stats.error().to_string().find("escapes") != std::string::npos);
            }
            CHECK(!fs::exists_no_follow(dest / "innocent/name"));
        }
    }

    // --- абсолютная цель ссылки ---
    {
        const path archive = work / "abs-link.zip";
        int err = 0;
        zip_t *za = zip_open(archive.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
        CHECK(za != nullptr);
        if (za != nullptr) {
            std::string target = "/etc/passwd";
            CHECK(add_entry(za, "innocent/abs", target, S_IFLNK | 0777));
            zip_close(za);

            auto stats = extract_zip(archive, work / "out-abs-link");
            CHECK(!stats.has_value());
        }
    }

    // --- снятие префикса ---
    // Так распаковывается nupkg, где нужно только поддерево "c/".
    {
        const path archive = work / "prefix.zip";
        int err = 0;
        zip_t *za = zip_open(archive.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
        CHECK(za != nullptr);
        if (za != nullptr) {
            std::string inside = "yes";
            std::string outside = "no";
            CHECK(add_entry(za, "c/include/ntddk.h", inside, S_IFREG | 0644));
            CHECK(add_entry(za, "build/other.props", outside, S_IFREG | 0644));
            zip_close(za);

            cork::archive::ZipOptions o;
            o.strip_prefix = "c/";
            const path dest = work / "out-prefix";
            auto stats = extract_zip(archive, dest, o);
            CHECK(stats.has_value());
            CHECK(fs::is_regular_file(dest / "include/ntddk.h"));
            CHECK(!fs::exists_no_follow(dest / "build/other.props"));
        }
    }

    fs::stdfs::remove_all(work);
    return cork::test::finish("test_zip");
}
