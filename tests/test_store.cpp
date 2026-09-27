// Хранилище артефактов: содержимое, а не имя пакета, определяет, где лежит
// файл и считается ли он готовым.

#include <atomic>
#include <string>
#include <vector>

#include "base/fs.hpp"
#include "base/sha256.hpp"
#include "check.h"
#include "local_server.h"
#include "store/store.hpp"

namespace fs = cork::fs;
using fs::stdfs::path;
using namespace cork::store;

namespace {

// Счётчики атомарные, потому что fetch_all зовёт Progress из рабочих потоков,
// а не из того, который его вызвал. Обычные int здесь дают гонку, которую
// невооружённым глазом не видно: значения почти всегда сходятся, и тест
// проходит — пока однажды не перестанет.
struct CountingProgress : Progress {
    std::atomic<int> started{0};
    std::atomic<int> from_cache{0};
    std::atomic<int> downloaded{0};

    void on_start(const FetchRequest &) override { started.fetch_add(1, std::memory_order_relaxed); }
    void on_done(const FetchRequest &, bool cached) override {
        (cached ? from_cache : downloaded).fetch_add(1, std::memory_order_relaxed);
    }
};

} // namespace

int main() {
    const path source_dir(CORK_SOURCE_DIR);
    const path script = source_dir / "tests/httpd/server.py";
    const path work = source_dir / "build/store-test";
    const path www = work / "www";

    fs::stdfs::remove_all(work);
    CHECK(fs::mkdir_p(www).has_value());

    std::string payload;
    for (int i = 0; i < 1000; ++i) {
        payload += "cork store fixture\n";
    }
    const std::string hash = cork::Sha256::hex_of(std::string_view(payload));
    CHECK(fs::write_atomic(www / "blob.bin", std::string_view(payload)).has_value());

    cork::test::LocalServer server;
    if (!server.start(script, www)) {
        std::fprintf(stderr, "не удалось поднять локальный сервер\n");
        return 1;
    }

    ArtifactStore store(work / "store");

    FetchRequest req;
    req.id = BlobId{hash};
    req.url = server.url("/ok/blob.bin");
    req.expected_size = static_cast<std::int64_t>(payload.size());
    req.label = "blob.bin";

    // --- первое получение ---
    {
        CHECK(!store.has(req.id));
        CountingProgress p;
        auto got = store.acquire(req, p);
        CHECK(got.has_value());
        if (!got) {
            std::fprintf(stderr, "%s\n", got.error().to_string().c_str());
            return cork::test::finish("test_store");
        }
        CHECK_EQ(std::to_string(p.downloaded), std::string("1"));
        CHECK_EQ(std::to_string(p.from_cache), std::string("0"));
        CHECK(store.has(req.id));

        // Адресация содержимым: путь выводится из хеша, а не из имени пакета.
        CHECK(got->string().find(hash) != std::string::npos);
        CHECK(got->parent_path().filename().string() == hash.substr(0, 2));
    }

    // --- повтор берётся из хранилища ---
    // has() обязан быть stat, а не пересчётом гигабайтов, поэтому проверяем
    // ещё и что сеть не понадобилась: сервер остановлен.
    {
        server.stop();
        CountingProgress p;
        auto got = store.acquire(req, p);
        CHECK(got.has_value());
        CHECK_EQ(std::to_string(p.from_cache), std::string("1"));
        CHECK_EQ(std::to_string(p.downloaded), std::string("0"));
    }

    // --- несовпадение хеша ---
    // Испорченное не должно попасть ни в хранилище, ни остаться в tmp:
    // иначе следующая попытка «продолжит» заведомо неверный файл.
    {
        cork::test::LocalServer s2;
        CHECK(s2.start(script, www));

        FetchRequest bad;
        // Хеш чего-то другого — сервер отдаст blob.bin, и они не сойдутся.
        bad.id = BlobId{cork::Sha256::hex_of(std::string_view("something else"))};
        bad.url = s2.url("/ok/blob.bin");
        bad.label = "wrong-hash.bin";

        CountingProgress p;
        auto got = store.acquire(bad, p);
        CHECK(!got.has_value());
        if (!got) {
            CHECK(got.error().code == cork::Error::Code::Verification);
            CHECK(got.error().to_string().find("checksum mismatch") != std::string::npos);
        }
        CHECK(!store.has(bad.id));
        CHECK(!fs::exists_no_follow(work / "store/tmp" / bad.id.sha256));
    }

    // --- параллельное получение одного и того же блоба ---
    // Блокировка берётся на блоб, поэтому дубликаты в списке безвредны и
    // качается он ровно один раз.
    {
        cork::test::LocalServer s3;
        CHECK(s3.start(script, www));

        std::string other;
        for (int i = 0; i < 500; ++i) {
            other += "another fixture line\n";
        }
        const std::string other_hash = cork::Sha256::hex_of(std::string_view(other));
        CHECK(fs::write_atomic(www / "other.bin", std::string_view(other)).has_value());

        std::vector<FetchRequest> list;
        for (int i = 0; i < 6; ++i) {
            FetchRequest r;
            r.id = BlobId{other_hash};
            r.url = s3.url("/ok/other.bin");
            r.label = "other.bin#" + std::to_string(i);
            list.push_back(r);
        }

        CountingProgress p;
        auto res = fetch_all(store, list, 4, p);
        CHECK(res.has_value());
        if (!res) {
            std::fprintf(stderr, "%s\n", res.error().to_string().c_str());
        }
        CHECK(store.has(BlobId{other_hash}));
        // Скачан один раз, остальные пять взяты из хранилища.
        CHECK_EQ(std::to_string(p.downloaded), std::string("1"));
        CHECK_EQ(std::to_string(p.from_cache), std::string("5"));
    }

    // --- отказ одной задачи прекращает остальные ---
    {
        cork::test::LocalServer s4;
        CHECK(s4.start(script, www));

        std::vector<FetchRequest> list;
        FetchRequest bad;
        bad.id = BlobId{cork::Sha256::hex_of(std::string_view("nope"))};
        bad.url = s4.url("/ok/blob.bin");
        bad.label = "bad";
        list.push_back(bad);

        CountingProgress p;
        auto res = fetch_all(store, list, 2, p);
        CHECK(!res.has_value());
    }

    // --- внесение локального файла ---
    {
        const path local = work / "local.txt";
        CHECK(fs::write_atomic(local, std::string_view("local content")).has_value());
        auto in_store = store.put_file(local);
        CHECK(in_store.has_value());
        if (in_store) {
            const std::string expected = cork::Sha256::hex_of(std::string_view("local content"));
            CHECK(in_store->string().find(expected) != std::string::npos);
            CHECK(store.has(BlobId{expected}));
        }
    }

    // --- негодный идентификатор ---
    {
        FetchRequest junk;
        junk.id = BlobId{"not-a-digest"};
        junk.label = "junk";
        CountingProgress p;
        auto got = store.acquire(junk, p);
        CHECK(!got.has_value());
        CHECK(!store.has(junk.id));
    }

    fs::stdfs::remove_all(work);
    return cork::test::finish("test_store");
}
