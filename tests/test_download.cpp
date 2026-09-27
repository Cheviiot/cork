// Загрузка по HTTP: докачка и три способа, которыми сервер может испортить
// файл, если ему верить на слово.
//
// Сервер поднимается свой, на случайном порту. Сети наружу тесту не нужно.

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

#include "base/fs.hpp"
#include "base/sha256.hpp"
#include "check.h"
#include "local_server.h"
#include "net/http.hpp"

namespace fs = cork::fs;
using fs::stdfs::path;
using namespace cork::net;

namespace {

std::string make_payload() {
    // Достаточно длинное, чтобы обрезание середины было осмысленным.
    std::string s;
    for (int i = 0; i < 4096; ++i) {
        s += static_cast<char>('a' + (i % 26));
    }
    return s;
}

} // namespace

int main() {
    const path source_dir(CORK_SOURCE_DIR);
    const path script = source_dir / "tests/httpd/server.py";
    const path work = source_dir / "build/download-test";
    const path root = work / "www";

    fs::stdfs::remove_all(work);
    CHECK(fs::mkdir_p(root).has_value());

    const std::string payload = make_payload();
    const std::string want_hash = cork::Sha256::hex_of(std::string_view(payload));
    CHECK(fs::write_atomic(root / "payload.bin", std::string_view(payload)).has_value());

    cork::test::LocalServer server;
    if (!server.start(script, root)) {
        std::fprintf(stderr, "не удалось поднять локальный сервер\n");
        return 1;
    }

    // --- разбор Content-Range ---
    // Отдельно от сети: ошибка здесь означает молча испорченный файл.
    {
        std::int64_t start = -1;
        CHECK(parse_content_range_start("bytes 100-199/200", start));
        CHECK_EQ(std::to_string(start), std::string("100"));
        CHECK(parse_content_range_start(" bytes 0-9/10", start));
        CHECK_EQ(std::to_string(start), std::string("0"));
        CHECK(!parse_content_range_start("items 1-2/3", start));
        CHECK(!parse_content_range_start("", start));
        CHECK(!parse_content_range_start("bytes abc-", start));
        // Без дефиса это не диапазон.
        CHECK(!parse_content_range_start("bytes 100", start));
    }

    // --- обычная загрузка ---
    {
        const path dest = work / "plain.bin";
        auto r = download_file(server.url("/ok/payload.bin"), dest);
        CHECK(r.has_value());
        if (!r) {
            std::fprintf(stderr, "%s\n", r.error().to_string().c_str());
        }
        auto got = cork::Sha256::hex_of_file(dest);
        CHECK(got.has_value());
        if (got) {
            CHECK_EQ(*got, want_hash);
        }
        // Временный файл не должен оставаться.
        CHECK(!fs::exists_no_follow(path(dest.string() + ".part")));
    }

    // --- докачка ---
    // .part с половиной содержимого должен быть продолжен, а не перекачан.
    {
        const path dest = work / "resumed.bin";
        const path part = path(dest.string() + ".part");
        CHECK(fs::write_atomic(part, std::string_view(payload).substr(0, 2000)).has_value());

        auto r = download_file(server.url("/ok/payload.bin"), dest);
        CHECK(r.has_value());
        if (r) {
            CHECK(r->resumed);
            // По сети прошло только недостающее.
            CHECK_EQ(std::to_string(r->bytes_transferred), std::to_string(payload.size() - 2000));
        }
        auto got = cork::Sha256::hex_of_file(dest);
        CHECK(got.has_value());
        if (got) {
            CHECK_EQ(*got, want_hash);
        }
    }

    // --- сервер не умеет Range и отдаёт 200 ---
    // Тело приходит с нулевого байта. Дописать его к .part значит склеить
    // файл из двух начал; правильно — начать заново.
    {
        const path dest = work / "norange.bin";
        const path part = path(dest.string() + ".part");
        CHECK(fs::write_atomic(part, std::string_view(payload).substr(0, 2000)).has_value());

        auto r = download_file(server.url("/norange/payload.bin"), dest);
        CHECK(r.has_value());
        if (r) {
            CHECK(!r->resumed);
        }
        auto got = cork::Sha256::hex_of_file(dest);
        CHECK(got.has_value());
        if (got) {
            // Ключевая проверка: файл целый, а не склейка длиной 6096 байт.
            CHECK_EQ(*got, want_hash);
        }
    }

    // --- 206, но Content-Range начинается не там ---
    // Сам по себе код 206 ничего не доказывает.
    {
        const path dest = work / "badrange.bin";
        const path part = path(dest.string() + ".part");
        CHECK(fs::write_atomic(part, std::string_view(payload).substr(0, 2000)).has_value());

        auto r = download_file(server.url("/badrange/payload.bin"), dest);
        CHECK(r.has_value());
        auto got = cork::Sha256::hex_of_file(dest);
        CHECK(got.has_value());
        if (got) {
            CHECK_EQ(*got, want_hash);
        }
    }

    // --- 416: наш .part длиннее настоящего файла ---
    {
        const path dest = work / "unsatisfiable.bin";
        const path part = path(dest.string() + ".part");
        CHECK(fs::write_atomic(part, std::string_view(payload)).has_value());

        auto r = download_file(server.url("/416/payload.bin"), dest);
        CHECK(!r.has_value());
        // Устаревший .part должен быть выброшен, иначе следующая попытка
        // повторит тот же отказ бесконечно.
        CHECK(!fs::exists_no_follow(part));
    }

    // --- 404 с телом ---
    // В сообщении должно быть видно, что ответил сервер: иначе «404» на
    // относительном пути превращается в загадочную ошибку разбора JSON.
    {
        auto r = get(server.url("/notfound"));
        CHECK(!r.has_value());
        if (!r) {
            const std::string msg = r.error().to_string();
            CHECK(msg.find("404") != std::string::npos);
            CHECK(msg.find("Not found") != std::string::npos);
        }
    }

    // --- получение в память ---
    {
        auto r = get(server.url("/ok/payload.bin"));
        CHECK(r.has_value());
        if (r) {
            CHECK_EQ(cork::Sha256::hex_of(std::string_view(*r)), want_hash);
        }
    }

    // --- отмена через обратный вызов прогресса ---
    // Так работает Ctrl-C: недокачанное остаётся в .part и продолжится потом.
    {
        const path dest = work / "cancelled.bin";
        Options o;
        o.progress = [](std::int64_t, std::int64_t) { return false; };
        auto r = download_file(server.url("/ok/payload.bin"), dest, o);
        CHECK(!r.has_value());
        if (!r) {
            CHECK(r.error().code == cork::Error::Code::Interrupted);
        }
        CHECK(!fs::exists_no_follow(dest));
    }

    fs::stdfs::remove_all(work);
    return cork::test::finish("test_download");
}
