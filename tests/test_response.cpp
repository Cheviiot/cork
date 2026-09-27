// Response-файлы: токенизация, кодировки, обратная сборка.
//
// Правила разбора выглядят простыми ровно до первого пути, кончающегося
// обратным слэшем, — а в MSVC такой путь есть в каждой второй команде
// (`/I"C:\dir\"`). Поэтому тест проверяет не «в общем работает», а именно те
// места, где наивная реализация тихо склеивает два аргумента в один.

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include <fmt/format.h>

#include "check.h"
#include "exec/response.hpp"

using namespace cork;
using namespace cork::exec;

namespace stdfs = std::filesystem;

namespace {

std::vector<std::string> tok(std::string_view s) { return tokenize_response(s); }

void test_plain_splitting() {
    CHECK(tok("").empty());
    CHECK(tok("   \t\r\n  ").empty());

    const auto a = tok("/nologo /c main.cpp");
    CHECK(a.size() == 3);
    CHECK(a[0] == "/nologo");
    CHECK(a[2] == "main.cpp");

    // Перевод строки — такой же разделитель, как пробел: MSBuild пишет
    // response-файлы по аргументу на строку.
    const auto b = tok("/nologo\n/c\r\nmain.cpp\n");
    CHECK(b.size() == 3);
    CHECK(b[1] == "/c");
    CHECK(b[2] == "main.cpp");
}

void test_quotes() {
    const auto a = tok("\"/I/path with spaces/include\"");
    CHECK(a.size() == 1);
    CHECK(a[0] == "/I/path with spaces/include");

    // Кавычка может открыться посреди аргумента: именно так выглядит
    // /I"C:\dir", и склеить это в два аргумента нельзя.
    const auto b = tok("/I\"C:\\Program Files\\inc\" /c");
    CHECK(b.size() == 2);
    CHECK(b[0] == "/IC:\\Program Files\\inc");
    CHECK(b[1] == "/c");

    // Две кавычки подряд внутри закавыченного — одна буквальная. Так MSBuild
    // передаёт define со строковым значением.
    const auto c = tok("\"/DNAME=\"\"value\"\"\"");
    CHECK(c.size() == 1);
    CHECK(c[0] == "/DNAME=\"value\"");
}

void test_backslashes() {
    // Обратный слэш не перед кавычкой — обычный символ. Если трактовать его
    // как экранирующий всегда, любой путь Windows разъедется.
    const auto a = tok("C:\\dir\\file.obj");
    CHECK(a.size() == 1);
    CHECK(a[0] == "C:\\dir\\file.obj");

    // Чётное число слэшей перед кавычкой: слэши делятся пополам, кавычка
    // остаётся разделителем.
    const auto b = tok("\"C:\\dir\\\\\" next");
    CHECK(b.size() == 2);
    CHECK(b[0] == "C:\\dir\\");
    CHECK(b[1] == "next");

    // Нечётное: последний слэш экранирует кавычку, и она попадает внутрь.
    const auto c = tok("\"a\\\"b\"");
    CHECK(c.size() == 1);
    CHECK(c[0] == "a\"b");
}

void test_round_trip() {
    // Всё, что собрано render_response, обязано разобраться обратно в то же
    // самое. Иначе переписанный response-файл тихо меняет смысл команды.
    const std::vector<std::string> cases = {
        "/nologo",
        "/I/usr/include",
        "/I/path with spaces/include",
        "C:\\dir\\",
        "C:\\dir with space\\",
        "/DNAME=\"value\"",
        "a\"b",
        "trailing\\\\",
        "",
        "\t",
    };
    for (const auto &c : cases) {
        const std::string text = render_response({c});
        const auto back = tokenize_response(text);
        CHECK(back.size() == 1);
        if (back.size() == 1) {
            CHECK_EQ(back[0], c);
        }
    }

    const std::string whole = render_response(cases);
    const auto back = tokenize_response(whole);
    // Пустой аргумент при обратном разборе сохраняется: он записан как "".
    CHECK(back.size() == cases.size());
    for (std::size_t i = 0; i < back.size() && i < cases.size(); ++i) {
        CHECK_EQ(back[i], cases[i]);
    }
}

stdfs::path scratch() {
    const char *base = std::getenv("CORK_TEST_TMP");
    stdfs::path dir = base != nullptr && *base != '\0' ? stdfs::path(base)
                                                       : stdfs::current_path() / ".testtmp";
    dir /= fmt::format("rsp-{}", ::getpid());
    std::error_code ec;
    stdfs::create_directories(dir, ec);
    return dir;
}

void write_bytes(const stdfs::path &p, const std::string &bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string utf16le(std::string_view ascii, bool bom = true) {
    std::string out;
    if (bom) {
        out += '\xFF';
        out += '\xFE';
    }
    for (const char c : ascii) {
        out.push_back(c);
        out.push_back('\0');
    }
    return out;
}

void test_encodings() {
    const stdfs::path dir = scratch();

    // UTF-16LE с меткой — то, что пишет сам компилятор. Прочитанный как
    // байты, он превращается в мусор с нулями между буквами.
    const stdfs::path wide = dir / "wide.rsp";
    write_bytes(wide, utf16le("/nologo\n/c\nmain.cpp\n"));
    auto a = read_response_file(wide);
    CHECK(a.has_value());
    if (a) {
        CHECK(a->size() == 3);
        CHECK_EQ((*a)[2], "main.cpp");
    }

    // UTF-16BE встречается реже, но метка однозначна, и игнорировать её
    // означало бы прочитать файл задом наперёд.
    std::string be;
    be += '\xFE';
    be += '\xFF';
    for (const char c : std::string("/W4")) {
        be.push_back('\0');
        be.push_back(c);
    }
    const stdfs::path bigend = dir / "be.rsp";
    write_bytes(bigend, be);
    auto b = read_response_file(bigend);
    CHECK(b.has_value());
    if (b) {
        CHECK(b->size() == 1);
        CHECK_EQ((*b)[0], "/W4");
    }

    // UTF-8 с меткой: метка снимается, иначе она приклеится к первому
    // аргументу и ключ перестанет опознаваться.
    const stdfs::path bom8 = dir / "bom8.rsp";
    write_bytes(bom8, "\xEF\xBB\xBF/EHsc /std:c++20");
    auto c = read_response_file(bom8);
    CHECK(c.has_value());
    if (c) {
        CHECK(c->size() == 2);
        CHECK_EQ((*c)[0], "/EHsc");
    }

    // Без метки байты сохраняются как есть: путь в Linux не обязан быть
    // корректным UTF-8, и «исправление» кодировки сделало бы его нерабочим.
    const stdfs::path raw = dir / "raw.rsp";
    write_bytes(raw, std::string("/Fo\xff\xfe.obj", 11));
    auto d = read_response_file(raw);
    CHECK(d.has_value());
    if (d) {
        CHECK(d->size() == 1);
        CHECK_EQ((*d)[0], std::string("/Fo\xff\xfe.obj", 11));
    }

    // Несуществующий файл — отказ, а не пустой список аргументов: пустой
    // список выглядит как «команда без аргументов» и запускает компилятор
    // впустую.
    auto missing = read_response_file(dir / "no-such.rsp");
    CHECK(!missing.has_value());

    std::error_code ec;
    stdfs::remove_all(dir, ec);
}

void test_finding_response_args() {
    const std::vector<std::string> args = {"/nologo", "@foo.rsp", "@", "@@literal",
                                           "main.cpp", "@/abs/path.rsp"};
    const auto found = find_response_args(args);
    CHECK(found.size() == 2);
    if (found.size() == 2) {
        CHECK(found[0].index == 1);
        CHECK_EQ(found[0].path.string(), "foo.rsp");
        CHECK(found[1].index == 5);
        CHECK_EQ(found[1].path.string(), "/abs/path.rsp");
    }
}

} // namespace

int main() {
    test_plain_splitting();
    test_quotes();
    test_backslashes();
    test_round_trip();
    test_encodings();
    test_finding_response_args();
    return cork::test::finish("test_response");
}
