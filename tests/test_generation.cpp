// Дайджест дерева, receipt и атомарная публикация поколения.
//
// Здесь проверяется главное свойство среза: прерывание на любом шаге не должно
// оставлять дерева, которое выглядит установленным. Поэтому тесты смотрят не
// на «получилось ли», а на то, что видно снаружи в промежуточных состояниях.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include <fmt/format.h>

#include "base/fs.hpp"
#include "check.h"
#include "setup/digest.hpp"
#include "setup/generation.hpp"
#include "setup/receipt.hpp"

#include "cork_embedded_toolchain.h"

using namespace cork;
using namespace cork::setup;

namespace stdfs = std::filesystem;

namespace {

// Временный каталог под корнем сборки, а не в /tmp: на этой машине /tmp —
// tmpfs и почти полон, а дерево поколения занимает заметное место.
class Sandbox {
public:
    Sandbox() {
        const char *base = std::getenv("CORK_TEST_TMP");
        stdfs::path parent = base != nullptr && *base != '\0' ? stdfs::path(base)
                                                              : stdfs::current_path() / ".testtmp";
        path_ = parent / fmt::format("gen-{}", ::getpid());
        std::error_code ec;
        stdfs::remove_all(path_, ec);
        stdfs::create_directories(path_, ec);
    }
    ~Sandbox() {
        std::error_code ec;
        stdfs::remove_all(path_, ec);
    }
    [[nodiscard]] const stdfs::path &path() const { return path_; }

private:
    stdfs::path path_;
};

void write_file(const stdfs::path &p, std::string_view content) {
    std::error_code ec;
    stdfs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

void test_digest_is_stable_and_sorted() {
    Sandbox box;
    const stdfs::path a = box.path() / "a";
    const stdfs::path b = box.path() / "b";

    // Два дерева с одинаковым содержимым, созданные в разном порядке.
    write_file(a / "one.txt", "one");
    write_file(a / "dir" / "two.txt", "two");
    write_file(b / "dir" / "two.txt", "two");
    write_file(b / "one.txt", "one");

    DigestOptions opts;
    opts.mode = DigestMode::Content;
    auto da = digest_tree(a, opts);
    auto db = digest_tree(b, opts);
    CHECK(da.has_value());
    CHECK(db.has_value());
    CHECK_EQ(da->digest, db->digest);
    CHECK(da->stats.files == 2);
    CHECK(da->stats.directories == 1);
}

void test_digest_notices_a_symlink_turned_into_a_file() {
    // Дефект номер один: символьная ссылка записывается обычным файлом с
    // путём внутри. Содержимое такого файла выглядит правдоподобно, размер
    // похож, и проверка, смотрящая только на содержимое, подмену пропустит.
    Sandbox box;
    const stdfs::path good = box.path() / "good";
    const stdfs::path bad = box.path() / "bad";

    write_file(good / "target.txt", "payload");
    std::error_code ec;
    stdfs::create_symlink("target.txt", good / "link.txt", ec);
    CHECK(!ec);

    write_file(bad / "target.txt", "payload");
    write_file(bad / "link.txt", "target.txt");

    DigestOptions opts;
    opts.mode = DigestMode::Content;
    auto dg = digest_tree(good, opts);
    auto db = digest_tree(bad, opts);
    CHECK(dg.has_value());
    CHECK(db.has_value());
    CHECK(dg->digest != db->digest);
    CHECK(dg->stats.symlinks == 1);
    CHECK(db->stats.symlinks == 0);
}

void test_digest_modes_and_exclusions() {
    Sandbox box;
    const stdfs::path root = box.path() / "tree";
    write_file(root / "keep.txt", "before");
    write_file(root / "receipt.json", "irrelevant");

    DigestOptions structure;
    structure.exclude = {"receipt.json"};
    auto before = digest_tree(root, structure);
    CHECK(before.has_value());

    // Структурный дайджест не читает содержимое, поэтому правка того же
    // размера его не меняет.
    write_file(root / "keep.txt", "AFTER!");
    auto after = digest_tree(root, structure);
    CHECK(after.has_value());
    CHECK_EQ(after->digest, before->digest);

    // Содержательный — меняет.
    DigestOptions content;
    content.mode = DigestMode::Content;
    content.exclude = {"receipt.json"};
    auto c1 = digest_tree(root, content);
    write_file(root / "keep.txt", "AFTER?");
    auto c2 = digest_tree(root, content);
    CHECK(c1.has_value());
    CHECK(c2.has_value());
    CHECK(c1->digest != c2->digest);

    // Исключённый файл не влияет ни в одном режиме: receipt содержит
    // дайджест и не может входить в него же.
    write_file(root / "receipt.json", "completely different");
    auto c3 = digest_tree(root, content);
    CHECK(c3.has_value());
    CHECK_EQ(c3->digest, c2->digest);
}

void test_receipt_roundtrip() {
    Receipt r;
    r.source.channel_url = "https://aka.ms/vs/18/stable/channel";
    r.source.manifest_url = "https://example/manifest";
    r.source.manifest_sha256 = "sha256:abc";
    r.source.product_version = "18.10.2";
    r.selection.package_ids = {"B", "A"};
    r.selection.digest = selection_digest(r.selection.package_ids);
    r.payloads.push_back(PayloadRecord{"pkg", "file.vsix", "sha256:dd", 123});
    r.unpacked.push_back(UnpackRecord{"pkg", "file.vsix", "zip", 7});
    r.tree.digest = "sha256:tree";
    r.tree.mode = DigestMode::Content;
    r.tree.stats.files = 5;
    r.tree.stats.symlinks = 2;
    r.wine.id = "11.18-mono11.3.0";
    r.wine.symlinks = 196;
    r.wine.mono_files = 123;
    CHECK(r.advance(State::Resolved, "0.1.0").has_value());
    CHECK(r.advance(State::Fetched, "0.1.0").has_value());

    const std::string text = r.to_json();
    CHECK(!text.empty());
    auto back = Receipt::from_json(text);
    CHECK(back.has_value());
    CHECK_EQ(back->source.product_version, "18.10.2");
    CHECK(back->selection.package_ids.size() == 2);
    CHECK_EQ(back->selection.digest, r.selection.digest);
    CHECK(back->payloads.size() == 1);
    CHECK(back->payloads[0].size == 123);
    CHECK(back->unpacked.size() == 1);
    CHECK(back->tree.mode == DigestMode::Content);
    CHECK(back->wine.symlinks == 196);
    CHECK(back->log.size() == 2);
    auto state = back->state();
    CHECK(state.has_value());
    CHECK(*state == State::Fetched);
    // Повторная запись того же — те же байты.
    CHECK_EQ(back->to_json(), text);
}

void test_selection_digest_is_order_independent() {
    CHECK_EQ(selection_digest({"a", "b", "c"}), selection_digest({"c", "a", "b"}));
    CHECK(selection_digest({"a", "b"}) != selection_digest({"a", "b", "c"}));
    // Длина перед значением: иначе эти два выбора совпали бы.
    CHECK(selection_digest({"ab", "c"}) != selection_digest({"a", "bc"}));
}

void test_receipt_states() {
    Receipt r;
    // Первым состоянием может быть только resolved: журнал, начинающийся с
    // середины, означал бы, что предыдущие этапы никто не выполнял.
    CHECK(!r.advance(State::Published, "0.1.0").has_value());
    CHECK(r.advance(State::Resolved, "0.1.0").has_value());
    CHECK(r.advance(State::Staged, "0.1.0").has_value());
    CHECK(r.at_least(State::Resolved));
    CHECK(!r.at_least(State::Verified));
    // Назад нельзя: это переписывание истории, а не продолжение.
    CHECK(!r.advance(State::Resolved, "0.1.0").has_value());
    // Повтор текущего — можно: перепроверка законна.
    CHECK(r.advance(State::Staged, "0.1.0").has_value());
}

void test_receipt_rejects_a_foreign_schema() {
    auto wrong_schema = Receipt::from_json(R"({"schema":"something/else"})");
    CHECK(!wrong_schema.has_value());
    CHECK(wrong_schema.error().code == Error::Code::Config);

    auto wrong_version =
        Receipt::from_json(R"({"schema":"cork/receipt","schema_version":99})");
    CHECK(!wrong_version.has_value());
    CHECK(wrong_version.error().code == Error::Code::Config);

    auto no_log = Receipt::from_json(R"({"schema":"cork/receipt","schema_version":1})");
    CHECK(!no_log.has_value());

    auto garbage = Receipt::from_json("not json at all");
    CHECK(!garbage.has_value());
}

// Готовое поколение в каталоге сборки: минимум, нужный публикации.
Receipt make_staged_receipt() {
    Receipt r;
    (void)r.advance(State::Resolved, "test");
    (void)r.advance(State::Staged, "test");
    return r;
}

void test_publish_is_atomic_and_idempotent() {
    Sandbox box;
    Root root{box.path() / "home"};
    CHECK(root.ensure().has_value());

    // Пока ничего не опубликовано, current не существует — и это внятный
    // отказ, а не пустой путь.
    auto nothing = root.resolve_current();
    CHECK(!nothing.has_value());

    auto staging = Staging::create(root);
    CHECK(staging.has_value());
    // Имя запоминается до публикации: после неё объект больше не владеет
    // каталогом и путь у него пуст.
    const stdfs::path staging_path = staging->path();
    write_file(staging_path / "bin" / "cl.exe", "tool");
    CHECK(make_staged_receipt().save(staging_path).has_value());

    DigestOptions dopts;
    dopts.exclude = {kReceiptFileName};
    auto digest = digest_tree(staging_path, dopts);
    CHECK(digest.has_value());

    auto published = staging->publish("14.51-10.0", digest->digest);
    CHECK(published.has_value());
    CHECK(fs::is_regular_file(*published / "bin" / "cl.exe"));

    // current указывает именно сюда, и это настоящий каталог.
    auto current = root.resolve_current();
    CHECK(current.has_value());
    CHECK_EQ(current->string(), published->string());

    // Каталог сборки исчез: он переименован, а не скопирован.
    CHECK(!fs::is_dir(staging_path));
    CHECK(staging->path().empty());

    // Имя поколения несёт начало дайджеста, поэтому одно и то же дерево
    // получает одно и то же имя, а разные деревья не сталкиваются.
    CHECK(published->filename().string().rfind("14.51-10.0-", 0) == 0);

    // Повторная публикация того же содержимого — не ошибка и не перезапись:
    // каталог тот же, current просто переставляется.
    auto again = Staging::create(root);
    CHECK(again.has_value());
    write_file(again->path() / "bin" / "cl.exe", "tool");
    CHECK(make_staged_receipt().save(again->path()).has_value());
    auto digest2 = digest_tree(again->path(), dopts);
    CHECK(digest2.has_value());
    CHECK_EQ(digest2->digest, digest->digest);
    auto republished = again->publish("14.51-10.0", digest2->digest);
    CHECK(republished.has_value());
    CHECK_EQ(republished->string(), published->string());

    auto generations = list_generations(root);
    CHECK(generations.has_value());
    CHECK(generations->size() == 1);
}

void test_publishing_a_second_generation_switches_current() {
    Sandbox box;
    Root root{box.path() / "home"};
    CHECK(root.ensure().has_value());

    DigestOptions dopts;
    dopts.exclude = {kReceiptFileName};

    const auto publish_one = [&](std::string_view content) {
        auto s = Staging::create(root);
        CHECK(s.has_value());
        write_file(s->path() / "bin" / "cl.exe", content);
        CHECK(make_staged_receipt().save(s->path()).has_value());
        auto d = digest_tree(s->path(), dopts);
        CHECK(d.has_value());
        auto p = s->publish("14.51-10.0", d->digest);
        CHECK(p.has_value());
        return *p;
    };

    const stdfs::path first = publish_one("first");
    const stdfs::path second = publish_one("second");
    CHECK(first != second);

    // Старое поколение никуда не делось: переключение current его не трогает,
    // и запущенная сборка, которая уже держит его путь, продолжает работать.
    CHECK(fs::is_regular_file(first / "bin" / "cl.exe"));
    auto current = root.resolve_current();
    CHECK(current.has_value());
    CHECK_EQ(current->string(), second.string());

    auto generations = list_generations(root);
    CHECK(generations.has_value());
    CHECK(generations->size() == 2);
}

void test_abandoned_staging_is_not_an_installation() {
    Sandbox box;
    Root root{box.path() / "home"};
    CHECK(root.ensure().has_value());

    {
        // Каталог сборки, брошенный без публикации: ровно то, что остаётся
        // после Ctrl-C посреди распаковки.
        auto s = Staging::create(root);
        CHECK(s.has_value());
        write_file(s->path() / "bin" / "cl.exe", "half done");
        // Деструктор убирает его сам.
    }

    // Снаружи ничего не изменилось: установки как не было, так и нет.
    CHECK(!root.resolve_current().has_value());
    auto generations = list_generations(root);
    CHECK(generations.has_value());
    CHECK(generations->empty());
}

void test_gc_removes_only_abandoned_directories() {
    Sandbox box;
    Root root{box.path() / "home"};
    CHECK(root.ensure().has_value());

    auto kept = Staging::create(root);
    CHECK(kept.has_value());
    kept->keep();
    write_file(kept->path() / "unpack" / "file", "data");

    // Возраст нулевой — свежий каталог сборки убирать нельзя, распаковка идёт
    // минутами и её каталог тоже «старый» по времени изменения.
    auto stats = collect_staging(root, std::chrono::hours(24));
    CHECK(stats.has_value());
    CHECK(stats->staging_removed == 0);
    CHECK(fs::is_dir(kept->path()));

    // С нулевым порогом — убирает.
    auto swept = collect_staging(root, std::chrono::seconds(0));
    CHECK(swept.has_value());
    CHECK(swept->staging_removed == 1);
    CHECK(!fs::is_dir(kept->path()));
}

// Файл тулчейна CMake выкладывается в поколение из вшитой копии, и проверять
// тут надо не «он есть», а что в нём осталось то, без чего он бесполезен.
// CMAKE_TRY_COMPILE_TARGET_TYPE — как раз такая строка: без неё проверка
// компилятора пытается запустить собранный Windows-бинарник, и настройка
// падает у каждого, кто собирает через CMake. Пропажу такой строки не видно
// ничем, кроме проверки вроде этой.
void test_cmake_toolchain_is_embedded() {
    const auto bytes = assets::cmake_toolchain();
    CHECK(bytes.size() > 200);
    const std::string text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    CHECK(text.find("CMAKE_SYSTEM_NAME Windows") != std::string::npos);
    CHECK(text.find("CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY") != std::string::npos);
    CHECK(text.find("CORK_TARGET") != std::string::npos);
    // Инструменты зовутся без .exe: это нативные обёртки, а cl.exe CMake
    // запустить не сможет.
    CHECK(text.find("NAMES cl ") != std::string::npos);
    CHECK(text.find("cl.exe") == std::string::npos);
}

} // namespace

int main() {
    test_cmake_toolchain_is_embedded();
    test_digest_is_stable_and_sorted();
    test_digest_notices_a_symlink_turned_into_a_file();
    test_digest_modes_and_exclusions();
    test_receipt_roundtrip();
    test_selection_digest_is_order_independent();
    test_receipt_states();
    test_receipt_rejects_a_foreign_schema();
    test_publish_is_atomic_and_idempotent();
    test_publishing_a_second_generation_switches_current();
    test_abandoned_staging_is_not_an_installation();
    test_gc_removes_only_abandoned_directories();
    return cork::test::finish("test_generation");
}
