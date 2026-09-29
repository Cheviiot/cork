// Установка рантайма Wine из опубликованного артефакта.
//
// Главное здесь не «скачалось», а то, что установленным считается только
// дерево, которое приехало целиком и совпало с закреплённым хешем. Всё
// остальное — отказ, после которого в runtime/ ничего не остаётся: рантайм,
// распакованный наполовину, выглядит установленным и не работает, и разбираться
// в этом придётся посреди чужой сборки.

#include <string>

#include <zip.h>

#include "base/fs.hpp"
#include "base/sha256.hpp"
#include "check.h"
#include "local_server.h"
#include "setup/runtime.hpp"
#include "store/store.hpp"

namespace fs = cork::fs;
using fs::stdfs::path;
using namespace cork::setup;

namespace {

constexpr std::uint32_t kModeShift = 16;

struct Silent : cork::store::Progress {};

// Содержимое живёт до zip_close: libzip читает из буфера отложенно.
struct RuntimeFixture {
    std::string wine = "#!/bin/sh\nexit 0\n";
    std::string dll = "MZ fake builtin\n";
    std::string link = "wine";
};

bool build_runtime_zip(const path &file, RuntimeFixture &f) {
    int err = 0;
    zip_t *za = zip_open(file.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
    if (za == nullptr) {
        return false;
    }
    const auto add = [&](const char *name, const std::string &content, std::uint32_t mode) {
        zip_source_t *src = zip_source_buffer(za, content.data(), content.size(), 0);
        if (src == nullptr) {
            return false;
        }
        const zip_int64_t idx = zip_file_add(za, name, src, ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8);
        if (idx < 0) {
            zip_source_free(src);
            return false;
        }
        return zip_file_set_external_attributes(za, static_cast<zip_uint64_t>(idx), 0,
                                                ZIP_OPSYS_UNIX, mode << kModeShift) == 0;
    };
    bool ok = zip_dir_add(za, "bin", ZIP_FL_ENC_UTF_8) >= 0;
    ok = ok && add("bin/wine", f.wine, 0100755);
    ok = ok && add("bin/wineserver", f.link, 0120777);
    ok = ok && add("lib/wine/x86_64-windows/kernel32.dll", f.dll, 0100644);
    if (zip_close(za) != 0) {
        return false;
    }
    return ok;
}

std::string pin_line(std::string_view id, std::string_view arch, std::string_view sha,
                     std::string_view url) {
    return std::string(id) + " " + std::string(arch) + " " + std::string(sha) + " " +
           std::string(url) + "\n";
}

void test_pins_parse_and_reject_nonsense() {
    auto ok = parse_runtime_pins("# комментарий\n\n"
                                 "11.18-mono11.3.0 amd64 "
                                 "0000000000000000000000000000000000000000000000000000000000000000 "
                                 "https://example.invalid/a.zip\n");
    CHECK(ok.has_value());
    CHECK(ok->size() == 1);
    CHECK_EQ((*ok)[0].wine_id, "11.18-mono11.3.0");
    CHECK_EQ((*ok)[0].host_arch, "amd64");

    // Строка, из которой выпало поле, не пропускается, а останавливает
    // разбор: молча пропущенная строка означает «рантайма нет», и выяснять
    // это пришлось бы на машине, где ещё ничего не установлено.
    CHECK(!parse_runtime_pins("11.18 amd64 https://example.invalid/a.zip\n").has_value());
    CHECK(!parse_runtime_pins("11.18 amd64 notahash https://example.invalid/a.zip\n").has_value());
    CHECK(!parse_runtime_pins("11.18 amd64 "
                              "0000000000000000000000000000000000000000000000000000000000000000 "
                              "https://example.invalid/a.zip лишнее\n")
               .has_value());

    // Вшитый список обязан разбираться. Пустым он быть может — артефакта пока
    // нет, — а неразбираемым не может никогда.
    CHECK(builtin_runtime_pins().has_value());
}

void test_install_fetches_verifies_and_publishes(const path &work, const path &www,
                                                 const std::string &base_url) {
    RuntimeFixture f;
    const path archive = www / "cork-wine-linux-amd64.zip";
    CHECK(build_runtime_zip(archive, f));
    auto bytes = fs::read_file(archive);
    CHECK(bytes.has_value());
    const std::string hash = cork::Sha256::hex_of(*bytes);

    Root root{work / "home"};
    CHECK(root.ensure().has_value());
    cork::store::ArtifactStore store{root.store()};
    Silent progress;

    const std::string_view arch = host_arch();
    auto pins = parse_runtime_pins(
        pin_line("11.18-test", arch, hash, base_url + "/ok/cork-wine-linux-amd64.zip"));
    CHECK(pins.has_value());

    auto first = install_runtime(root, "11.18-test", *pins, store, progress);
    CHECK(first.has_value());
    CHECK(!first->already_present);
    CHECK(fs::is_regular_file(root.runtime("11.18-test") / "bin" / "wine"));
    // Символьная ссылка приезжает ссылкой, а не файлом с текстом внутри: на
    // этом дефекте предыдущая реализация теряла весь Wine Mono.
    CHECK(fs::stdfs::is_symlink(root.runtime("11.18-test") / "bin" / "wineserver"));
    CHECK(first->symlinks == 1);

    // Повторный вызов не трогает ни сеть, ни дерево.
    auto again = install_runtime(root, "11.18-test", *pins, store, progress);
    CHECK(again.has_value());
    CHECK(again->already_present);
}

void test_wrong_hash_leaves_nothing_behind(const path &work, const path &www,
                                           const std::string &base_url) {
    RuntimeFixture f;
    f.wine = "#!/bin/sh\nexit 1\n";
    const path archive = www / "tampered.zip";
    CHECK(build_runtime_zip(archive, f));

    Root root{work / "home2"};
    CHECK(root.ensure().has_value());
    cork::store::ArtifactStore store{root.store()};
    Silent progress;

    // Хеш от другого содержимого: ровно то, что увидел бы пользователь, если
    // бы файл по закреплённому адресу подменили.
    const std::string wrong = cork::Sha256::hex_of(std::string_view("not this archive"));
    auto pins =
        parse_runtime_pins(pin_line("11.18-test", host_arch(), wrong, base_url + "/ok/tampered.zip"));
    CHECK(pins.has_value());

    auto r = install_runtime(root, "11.18-test", *pins, store, progress);
    CHECK(!r.has_value());
    CHECK(!fs::exists_no_follow(root.runtime("11.18-test")));
}

void test_no_pin_says_so_plainly(const path &work) {
    Root root{work / "home3"};
    CHECK(root.ensure().has_value());
    cork::store::ArtifactStore store{root.store()};
    Silent progress;

    auto empty = parse_runtime_pins("# пока нечего публиковать\n");
    CHECK(empty.has_value());
    auto r = install_runtime(root, "11.18-test", *empty, store, progress);
    CHECK(!r.has_value());
    // Разные случаи — разные слова: «ещё не публиковали» и «нет для этой
    // версии» требуют от человека разного, и одинаковый текст сделал бы
    // выбор за него неверно.
    CHECK(r.error().to_string().find("has been published yet") != std::string::npos);

    auto other = parse_runtime_pins(
        pin_line("11.20-other", host_arch(),
                 "0000000000000000000000000000000000000000000000000000000000000000",
                 "https://example.invalid/a.zip"));
    CHECK(other.has_value());
    auto r2 = install_runtime(root, "11.18-test", *other, store, progress);
    CHECK(!r2.has_value());
    CHECK(r2.error().to_string().find("no published Wine runtime") != std::string::npos);
}

void test_archive_without_wine_is_refused(const path &work, const path &www,
                                          const std::string &base_url) {
    // Архив, в котором есть что угодно, кроме bin/wine. Распаковаться он
    // распакуется, и без этой проверки в runtime/ осталось бы дерево, которое
    // сломается не здесь, а при первой сборке.
    const path archive = www / "empty.zip";
    int err = 0;
    zip_t *za = zip_open(archive.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
    CHECK(za != nullptr);
    std::string junk = "nothing useful\n";
    zip_source_t *src = zip_source_buffer(za, junk.data(), junk.size(), 0);
    CHECK(src != nullptr);
    CHECK(zip_file_add(za, "readme.txt", src, ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8) >= 0);
    CHECK(zip_close(za) == 0);

    auto bytes = fs::read_file(archive);
    CHECK(bytes.has_value());

    Root root{work / "home4"};
    CHECK(root.ensure().has_value());
    cork::store::ArtifactStore store{root.store()};
    Silent progress;
    auto pins = parse_runtime_pins(pin_line("11.18-test", host_arch(),
                                            cork::Sha256::hex_of(*bytes),
                                            base_url + "/ok/empty.zip"));
    CHECK(pins.has_value());

    auto r = install_runtime(root, "11.18-test", *pins, store, progress);
    CHECK(!r.has_value());
    CHECK(r.error().to_string().find("no bin/wine") != std::string::npos);
    CHECK(!fs::exists_no_follow(root.runtime("11.18-test")));
}

} // namespace

int main() {
    const path source_dir(CORK_SOURCE_DIR);
    const path script = source_dir / "tests/httpd/server.py";
    const path work = source_dir / "build/runtime-test";
    const path www = work / "www";

    fs::stdfs::remove_all(work);
    CHECK(fs::mkdir_p(www).has_value());

    cork::test::LocalServer server;
    CHECK(server.start(script, www));
    const std::string base_url = server.url("");

    test_pins_parse_and_reject_nonsense();
    test_install_fetches_verifies_and_publishes(work, www, base_url);
    test_wrong_hash_leaves_nothing_behind(work, www, base_url);
    test_no_pin_says_so_plainly(work);
    test_archive_without_wine_is_refused(work, www, base_url);

    server.stop();
    fs::stdfs::remove_all(work);
    return cork::test::finish("test_runtime");
}
