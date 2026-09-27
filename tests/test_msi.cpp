// Читатель CFBF и таблиц MSI.
//
// Основная часть теста работает на составном документе, собранном тут же из
// байтов: так проверяются ровно те места, на которых разбор ломается молча —
// цепочки секторов, мини-поток, порядок полей в пуле строк, хранение таблиц по
// столбцам. Настоящий .msi для этого не нужен, и тест идёт в CI без сети.
//
// Вторая роль — сверка с эталоном. При заданном CORK_MSI_DUMP тест вместо
// проверок печатает содержимое указанного пакета в том же формате, что
// `tools/msi_probe.py dump`, и результаты сравниваются построчно. Это и есть
// вторая независимая реализация, о которой говорит план.

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "archive/cfbf.hpp"
#include "archive/msi.hpp"
#include "base/fs.hpp"
#include "base/md5.hpp"
#include "check.h"

using namespace cork;
using archive::Cfbf;
using archive::Msi;

namespace {

void put16(std::string &s, std::size_t at, std::uint16_t v) {
    s[at] = static_cast<char>(v & 0xFF);
    s[at + 1] = static_cast<char>(v >> 8);
}

void put32(std::string &s, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        s[at + static_cast<std::size_t>(i)] = static_cast<char>((v >> (8 * i)) & 0xFF);
    }
}

void append16(std::string &s, std::uint16_t v) {
    s.push_back(static_cast<char>(v & 0xFF));
    s.push_back(static_cast<char>(v >> 8));
}

void append32(std::string &s, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
}

constexpr std::uint32_t kEnd = 0xFFFFFFFEu;
constexpr std::uint32_t kFree = 0xFFFFFFFFu;
constexpr std::uint32_t kFatMark = 0xFFFFFFFDu;
constexpr std::size_t kSector = 512;
constexpr std::size_t kMini = 64;
constexpr std::uint32_t kMiniCutoff = 4096;

// Собирает составной документ версии 3. Число секторов FAT считается по
// объёму данных, поэтому пакет с длинной строкой (70 КБ, то есть больше 128
// секторов) проверяет заодно и чтение FAT из нескольких секторов. Продолжение
// DIFAT за пределы заголовка не покрыто: для этого нужен файл от 55 МБ.
class Builder {
public:
    // Возвращает индекс записи каталога.
    void add(std::u16string name, std::string content) {
        streams_.push_back({std::move(name), std::move(content)});
    }

    std::string build() const {
        // Сколько секторов уйдёт под саму FAT, надо знать до того, как в неё
        // что-то записано: её секторы тоже нумеруются. Считаем с запасом —
        // лишний сектор FAT безвреден, нехватка ломает разбор.
        std::size_t payload = 0;
        for (const auto &s : streams_) {
            payload += s.content.size() + kSector;
        }
        const std::size_t fat_sectors = payload / kSector / (kSector / 4) + 2;
        std::vector<std::uint32_t> fat(fat_sectors * (kSector / 4), kFree);
        std::vector<std::string> sectors;

        auto alloc = [&](std::string body) -> std::uint32_t {
            body.resize(((body.size() + kSector - 1) / kSector) * kSector, '\0');
            std::uint32_t first = static_cast<std::uint32_t>(sectors.size());
            for (std::size_t off = 0; off < body.size(); off += kSector) {
                auto index = static_cast<std::uint32_t>(sectors.size());
                sectors.push_back(body.substr(off, kSector));
                if (index > first) {
                    fat[index - 1] = index;
                }
            }
            fat[sectors.size() - 1] = kEnd;
            return first;
        };

        // Первые сектора занимает сама FAT.
        for (std::size_t i = 0; i < fat_sectors; ++i) {
            sectors.emplace_back(kSector, '\0');
            fat[i] = kFatMark;
        }

        // Мелкие потоки собираются в мини-поток, крупные — в обычные секторы.
        std::string mini_stream;
        std::vector<std::uint32_t> mini_fat;
        std::vector<std::pair<std::uint32_t, std::uint64_t>> placed;  // старт, размер
        for (const auto &s : streams_) {
            if (s.content.size() < kMiniCutoff) {
                auto first = static_cast<std::uint32_t>(mini_stream.size() / kMini);
                std::string body = s.content;
                body.resize(((body.size() + kMini - 1) / kMini) * kMini, '\0');
                mini_stream += body;
                auto count = static_cast<std::uint32_t>(body.size() / kMini);
                for (std::uint32_t i = 0; i < count; ++i) {
                    mini_fat.push_back(i + 1 == count ? kEnd : first + i + 1);
                }
                placed.emplace_back(first, s.content.size());
            } else {
                placed.emplace_back(alloc(s.content), s.content.size());
            }
        }

        std::string mini_fat_body;
        mini_fat.resize(kSector / 4, kFree);
        for (std::uint32_t v : mini_fat) {
            append32(mini_fat_body, v);
        }
        std::uint32_t mini_fat_start = alloc(mini_fat_body);
        std::uint32_t mini_stream_start = mini_stream.empty() ? kEnd : alloc(mini_stream);

        // Каталог: нулевая запись — корень, он же держит мини-поток.
        std::string dir;
        auto entry = [&](std::u16string name, std::uint8_t type, std::uint32_t start,
                         std::uint64_t size) {
            std::string e(128, '\0');
            for (std::size_t i = 0; i < name.size(); ++i) {
                put16(e, i * 2, static_cast<std::uint16_t>(name[i]));
            }
            put16(e, 0x40, static_cast<std::uint16_t>((name.size() + 1) * 2));
            e[0x42] = static_cast<char>(type);
            put32(e, 0x44, kFree);
            put32(e, 0x48, kFree);
            put32(e, 0x4C, kFree);
            put32(e, 0x74, start);
            put32(e, 0x78, static_cast<std::uint32_t>(size));
            dir += e;
        };
        entry(u"Root Entry", 5, mini_stream_start, mini_stream.size());
        for (std::size_t i = 0; i < streams_.size(); ++i) {
            entry(streams_[i].name, 2, placed[i].first, placed[i].second);
        }
        std::uint32_t dir_start = alloc(dir);

        std::string fat_body;
        for (std::uint32_t v : fat) {
            append32(fat_body, v);
        }
        for (std::size_t i = 0; i < fat_sectors; ++i) {
            sectors[i] = fat_body.substr(i * kSector, kSector);
        }

        std::string header(kSector, '\0');
        static const unsigned char magic[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
        std::memcpy(header.data(), magic, sizeof magic);
        put16(header, 0x18, 3);   // версия формата
        put16(header, 0x1A, 3);
        put16(header, 0x1C, 0xFFFE);  // порядок байтов
        put16(header, 0x1E, 9);       // сектор 512
        put16(header, 0x20, 6);       // мини-сектор 64
        put32(header, 0x2C, static_cast<std::uint32_t>(fat_sectors));
        put32(header, 0x30, dir_start);
        put32(header, 0x38, kMiniCutoff);
        put32(header, 0x3C, mini_fat_start);
        put32(header, 0x40, 1);
        put32(header, 0x44, kFree);   // DIFAT продолжения нет
        put32(header, 0x48, 0);
        // DIFAT: номера секторов, в которых лежит FAT. Они идут первыми.
        for (std::size_t i = 0; i < 109; ++i) {
            put32(header, 0x4C + i * 4,
                  i < fat_sectors ? static_cast<std::uint32_t>(i) : kFree);
        }

        std::string out = header;
        for (const auto &s : sectors) {
            out += s;
        }
        return out;
    }

private:
    struct Stream {
        std::u16string name;
        std::string content;
    };
    std::vector<Stream> streams_;
};

// Кодирует имя потока так, как это делает установщик Windows: парами базы 64
// со смещением 0x3800, одиночным символом со смещением 0x4800 для нечётного
// хвоста и префиксом 0x4840 для таблиц.
std::u16string mangle(std::string_view name, bool table) {
    static constexpr std::string_view kAlphabet =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._";
    std::u16string out;
    if (table) {
        out.push_back(0x4840);
    }
    std::size_t i = 0;
    while (i < name.size()) {
        auto low = kAlphabet.find(name[i]);
        CHECK(low != std::string_view::npos);
        if (i + 1 < name.size()) {
            auto high = kAlphabet.find(name[i + 1]);
            if (high != std::string_view::npos) {
                out.push_back(static_cast<char16_t>(0x3800 + low + (high << 6)));
                i += 2;
                continue;
            }
        }
        out.push_back(static_cast<char16_t>(0x4800 + low));
        i += 1;
    }
    return out;
}

// Пул строк: пары «длина, счётчик ссылок». Нулевая пара занята кодовой
// страницей.
struct Pool {
    std::string info;
    std::string data;
    std::vector<std::string> added;

    Pool() {
        // Нулевая ячейка несёт кодовую страницу в поле длины и признак ширины
        // ссылок в старшем бите поля ссылок. Строки при этом нумеруются с
        // единицы, и байтов в _StringData она не занимает.
        append16(info, 65001);
        append16(info, 0);
    }

    std::uint32_t add(std::string_view s) {
        if (s.size() > 0xFFFF) {
            // Маркер длинной строки: нулевая длина, счётчик ссылок на месте.
            append16(info, 0);
            append16(info, 1);
            // Сама длина: младшие 16 бит в поле длины, старшие — в поле ссылок.
            append16(info, static_cast<std::uint16_t>(s.size() & 0xFFFF));
            append16(info, static_cast<std::uint16_t>(s.size() >> 16));
        } else {
            append16(info, static_cast<std::uint16_t>(s.size()));
            append16(info, 1);
        }
        data += s;
        added.emplace_back(s);
        return static_cast<std::uint32_t>(added.size());
    }
};

// Колонка: старший байт несёт признаки, 0x0800 — строковая, младший байт для
// чисел задаёт ширину.
constexpr std::uint16_t kStr = 0x9D00 | 0x0800;
constexpr std::uint16_t kInt2 = 0x9502;
constexpr std::uint16_t kInt4 = 0x9504;

// Таблица хранится по столбцам, поэтому строится тоже по столбцам.
std::string encode_table(const std::vector<std::uint16_t> &types,
                         const std::vector<std::vector<std::uint32_t>> &rows) {
    std::string out;
    for (std::size_t c = 0; c < types.size(); ++c) {
        for (const auto &row : rows) {
            std::uint32_t v = row[c];
            if (types[c] & 0x0800) {
                append16(out, static_cast<std::uint16_t>(v));
            } else if ((types[c] & 0xFF) == 2) {
                append16(out, v == 0 ? 0 : static_cast<std::uint16_t>(v + 0x8000));
            } else {
                append32(out, v == 0 ? 0 : v + 0x80000000u);
            }
        }
    }
    return out;
}

void test_cfbf_roundtrip() {
    Builder b;
    std::string big(5000, 'x');
    for (std::size_t i = 0; i < big.size(); ++i) {
        big[i] = static_cast<char>('a' + i % 26);
    }
    b.add(u"Small", "hello");
    b.add(u"Big", big);
    auto doc = Cfbf::parse(b.build());
    CHECK(doc.has_value());

    const Cfbf::Entry *small = nullptr;
    const Cfbf::Entry *large = nullptr;
    for (const auto &e : doc->entries()) {
        if (e.name == u"Small") small = &e;
        if (e.name == u"Big") large = &e;
    }
    CHECK(small != nullptr);
    CHECK(large != nullptr);

    auto s = doc->read(*small);
    CHECK(s.has_value());
    CHECK(*s == "hello");

    // Поток больше границы мини-потока читается по обычной FAT и обязан
    // совпасть побайтово, включая хвост, не кратный сектору.
    auto l = doc->read(*large);
    CHECK(l.has_value());
    CHECK(l->size() == big.size());
    CHECK(*l == big);
}

void test_cfbf_rejects_garbage() {
    auto bad = Cfbf::parse(std::string(600, '\0'));
    CHECK(!bad.has_value());
    CHECK(bad.error().code == Error::Code::Format);

    // Зацикленная цепочка: сектор ссылается сам на себя. Без проверки разбор
    // не завершился бы вовсе, а не отказал бы.
    Builder b;
    b.add(u"Big", std::string(5000, 'z'));
    std::string doc = b.build();

    // Номер первого сектора потока берётся из разбора, а не из знания о
    // раскладке: она зависит от числа секторов FAT и молча меняется вместе с
    // размером данных.
    auto good = Cfbf::parse(doc);
    CHECK(good.has_value());
    std::uint32_t start = 0;
    for (const auto &x : good->entries()) {
        if (x.name == u"Big") {
            start = x.start_sector;
        }
    }
    CHECK(start != 0);

    // FAT начинается сразу за заголовком, то есть с байта 512.
    put32(doc, 512 + start * 4, start);
    auto parsed = Cfbf::parse(doc);
    CHECK(parsed.has_value());
    const Cfbf::Entry *e = nullptr;
    for (const auto &x : parsed->entries()) {
        if (x.name == u"Big") {
            e = &x;
        }
    }
    CHECK(e != nullptr);
    auto r = parsed->read(*e);
    CHECK(!r.has_value());
    CHECK(r.error().code == Error::Code::Format);
}

void test_demangle() {
    struct Case {
        std::string_view name;
        bool table;
    };
    // Имена из настоящих пакетов: "Property" — та самая строка, которая при
    // перепутанном алфавите превращается в правдоподобное "z1YZO138".
    const Case cases[] = {
        {"Property", true},   {"File", true},      {"Media", true},
        {"Directory", true},  {"_StringPool", false}, {"_Columns", true},
        {"Win10SDK", false},  {"a", false},        {"Wix4DependencyProvider", true},
    };
    for (const auto &c : cases) {
        auto d = archive::demangle_stream_name(mangle(c.name, c.table));
        CHECK(d.name == c.name);
        CHECK(d.is_table == c.table);
    }
    // Не-мангленое имя проходит насквозь: так лежит SummaryInformation.
    auto plain = archive::demangle_stream_name(u"\x05SummaryInformation");
    CHECK(plain.name == "\x05SummaryInformation");
    CHECK(!plain.is_table);
}

// Собирает пакет из пяти таблиц: два каталога, два компонента, три файла в
// двух архивах, один из которых встроенный.
std::string build_package(bool with_long_string) {
    Pool pool;
    auto S = [&](std::string_view s) { return pool.add(s); };

    // Порядок добавления задаёт номера ссылок, поэтому все строки создаются
    // здесь, до того как из них собираются строки таблиц.
    std::uint32_t s_dir_table = S("Directory");
    std::uint32_t s_dir = S("Directory");
    std::uint32_t s_dir_parent = S("Directory_Parent");
    std::uint32_t s_default = S("DefaultDir");
    std::uint32_t s_comp_table = S("Component");
    std::uint32_t s_comp = S("Component");
    std::uint32_t s_comp_dir = S("Directory_");
    std::uint32_t s_file_table = S("File");
    std::uint32_t s_file = S("File");
    std::uint32_t s_file_comp = S("Component_");
    std::uint32_t s_filename = S("FileName");
    std::uint32_t s_filesize = S("FileSize");
    std::uint32_t s_attributes = S("Attributes");
    std::uint32_t s_sequence = S("Sequence");
    std::uint32_t s_media_table = S("Media");
    std::uint32_t s_diskid = S("DiskId");
    std::uint32_t s_lastseq = S("LastSequence");
    std::uint32_t s_cabinet = S("Cabinet");
    std::uint32_t s_hash_table = S("MsiFileHash");
    std::uint32_t s_hash_file = S("File_");
    std::uint32_t s_hash_options = S("Options");
    std::uint32_t s_hash1 = S("HashPart1");
    std::uint32_t s_hash2 = S("HashPart2");
    std::uint32_t s_hash3 = S("HashPart3");
    std::uint32_t s_hash4 = S("HashPart4");

    std::uint32_t s_target = S("TARGETDIR");
    std::uint32_t s_sourcedir = S("SourceDir");
    std::uint32_t s_incdir = S("IncludeDir");
    // Форма "коротк.|длинн.": на диск должно попасть длинное имя.
    std::uint32_t s_include = S("inclu~1|include");
    std::uint32_t s_c1 = S("C1");
    std::uint32_t s_c2 = S("C2");
    std::uint32_t s_f1 = S("f1");
    std::uint32_t s_f2 = S("f2");
    std::uint32_t s_f3 = S("f3");
    std::uint32_t s_n1 = S("stdio~1.h|stdio.h");
    std::uint32_t s_n2 = S("readme.txt");
    std::uint32_t s_n3 = S("embedded.txt");
    std::uint32_t s_cab1 = S("outer.cab");
    std::uint32_t s_cab2 = S("#inner.cab");
    if (with_long_string) {
        // Длинная строка кодируется двумя ячейками. Она добавляется последней,
        // чтобы её присутствие не сдвигало номера остальных ссылок.
        S(std::string(70000, 'L'));
    }

    Builder b;
    b.add(mangle("_StringPool", false), pool.info);
    b.add(mangle("_StringData", false), pool.data);

    // Схема: строки _Columns описывают все пять таблиц.
    std::vector<std::vector<std::uint32_t>> columns;
    auto col = [&](std::uint32_t table, std::uint32_t number, std::uint32_t name,
                   std::uint16_t type) {
        columns.push_back({table, number, name, type});
    };
    col(s_dir_table, 1, s_dir, kStr);
    col(s_dir_table, 2, s_dir_parent, kStr);
    col(s_dir_table, 3, s_default, kStr);
    col(s_comp_table, 1, s_comp, kStr);
    col(s_comp_table, 2, s_comp_dir, kStr);
    col(s_file_table, 1, s_file, kStr);
    col(s_file_table, 2, s_file_comp, kStr);
    col(s_file_table, 3, s_filename, kStr);
    col(s_file_table, 4, s_filesize, kInt4);
    col(s_file_table, 5, s_attributes, kInt2);
    col(s_file_table, 6, s_sequence, kInt2);
    col(s_media_table, 1, s_diskid, kInt2);
    col(s_media_table, 2, s_lastseq, kInt2);
    col(s_media_table, 3, s_cabinet, kStr);
    col(s_hash_table, 1, s_hash_file, kStr);
    col(s_hash_table, 2, s_hash_options, kInt2);
    col(s_hash_table, 3, s_hash1, kInt4);
    col(s_hash_table, 4, s_hash2, kInt4);
    col(s_hash_table, 5, s_hash3, kInt4);
    col(s_hash_table, 6, s_hash4, kInt4);
    b.add(mangle("_Columns", true),
          encode_table({kStr, kInt2, kStr, kInt2}, columns));

    b.add(mangle("Directory", true),
          encode_table({kStr, kStr, kStr},
                       {{s_target, 0, s_sourcedir}, {s_incdir, s_target, s_include}}));
    b.add(mangle("Component", true),
          encode_table({kStr, kStr}, {{s_c1, s_incdir}, {s_c2, s_target}}));
    b.add(mangle("File", true),
          encode_table({kStr, kStr, kStr, kInt4, kInt2, kInt2},
                       {{s_f1, s_c1, s_n1, 12, 0, 1},
                        {s_f2, s_c2, s_n2, 6, 0, 2},
                        {s_f3, s_c2, s_n3, 9, 0, 3}}));
    b.add(mangle("Media", true),
          encode_table({kInt2, kInt2, kStr}, {{1, 2, s_cab1}, {2, 3, s_cab2}}));
    // У первого файла хеш есть, у второго все четыре поля пустые. Пустой файл
    // в Windows SDK (empty.cpp) помечен именно так, и принять нули за хеш
    // значит отказать в распаковке всего пакета.
    b.add(mangle("MsiFileHash", true),
          encode_table({kStr, kInt2, kInt4, kInt4, kInt4, kInt4},
                       {{s_f1, 0, 0x01020304, 0x05060708, 0x090a0b0c, 0x0d0e0f10},
                        {s_f2, 0, 0, 0, 0, 0}}));
    return b.build();
}

void test_tables() {
    auto msi = Msi::parse(build_package(false));
    CHECK(msi.has_value());

    // Пять потоков-таблиц, но в схеме четыре: _Columns описывает остальные и
    // себя не описывает — ровно так же устроены настоящие пакеты.
    auto names = msi->table_names();
    CHECK(names.size() == 5);
    CHECK(msi->has_table("File"));
    CHECK(msi->has_table("Media"));
    CHECK(!msi->has_table("Registry"));

    auto missing = msi->table("Registry");
    CHECK(!missing.has_value());
    CHECK(missing.error().code == Error::Code::NotFound);

    auto media = msi->table("Media");
    CHECK(media.has_value());
    CHECK(media->size() == 2);
    // Числа хранятся со сдвинутым знаковым битом; без обратного сдвига здесь
    // было бы 0x8001.
    CHECK((*media)[0][0].number == 1);
    CHECK((*media)[0][1].number == 2);
    CHECK((*media)[0][2].text == "outer.cab");
    CHECK((*media)[1][2].text == "#inner.cab");
}

void test_files() {
    auto msi = Msi::parse(build_package(false));
    CHECK(msi.has_value());
    auto files = msi->files();
    CHECK(files.has_value());
    CHECK(files->size() == 3);

    const auto &f1 = (*files)[0];
    CHECK(f1.key == "f1");
    // Каталог TARGETDIR несёт DefaultDir "SourceDir" и в путь не попадает, а
    // из "inclu~1|include" берётся длинное имя.
    CHECK(f1.path == "include/stdio.h");
    CHECK(f1.size == 12);
    CHECK(f1.sequence == 1);
    CHECK(f1.cabinet == "outer.cab");

    const auto &f2 = (*files)[1];
    CHECK(f2.path == "readme.txt");
    CHECK(f2.cabinet == "outer.cab");

    // Третий файл по Sequence попадает уже во второй архив — встроенный.
    const auto &f3 = (*files)[2];
    CHECK(f3.path == "embedded.txt");
    CHECK(f3.cabinet == "#inner.cab");

    // Хеш собирается из четырёх чисел, каждое little-endian.
    CHECK(f1.md5.has_value());
    CHECK(Md5::to_hex(*f1.md5) == "04030201080706050c0b0a09100f0e0d");
    // Все четыре поля пустые — хеша нет, а не хеш из нулей.
    CHECK(!f2.md5.has_value());
    // Строки в MsiFileHash нет вовсе — тоже нет хеша, и это не ошибка.
    CHECK(!f3.md5.has_value());
}

void test_long_string_in_pool() {
    // Строка длиннее 65535 байт кодируется двумя ячейками пула. Если разбирать
    // её как обычную, все последующие строки сдвигаются, и таблицы читаются
    // правдоподобно, но неверно — поэтому проверяется именно то, что после неё
    // ничего не поехало.
    auto msi = Msi::parse(build_package(true));
    CHECK(msi.has_value());
    auto files = msi->files();
    CHECK(files.has_value());
    CHECK(files->size() == 3);
    CHECK((*files)[0].path == "include/stdio.h");
    CHECK((*files)[2].cabinet == "#inner.cab");
}

void test_md5_vectors() {
    // Векторы из RFC 1321.
    CHECK(Md5::to_hex(Md5::of("")) == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(Md5::to_hex(Md5::of("a")) == "0cc175b9c0f1b6a831c399e269772661");
    CHECK(Md5::to_hex(Md5::of("abc")) == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(Md5::to_hex(Md5::of("message digest")) == "f96b697d7cb7938d525a2f31aaf161d0");
    CHECK(Md5::to_hex(Md5::of("abcdefghijklmnopqrstuvwxyz")) ==
          "c3fcd3d76192e4007dfb496cca67e13b");
    CHECK(Md5::to_hex(Md5::of("12345678901234567890123456789012345678901234567890123456789012"
                              "345678901234567890")) == "57edf4a22be3c955ac49da2e2107b67a");
    // Ровно один блок и ровно граница дописывания длины: места, где путается
    // порядок байтов в поле длины.
    CHECK(Md5::to_hex(Md5::of(std::string(64, 'a'))) == "014842d480b571495a4a0363793f7367");
    CHECK(Md5::to_hex(Md5::of(std::string(55, 'a'))) == "ef1772b6dff9a122358552954ad0df65");
    CHECK(Md5::to_hex(Md5::of(std::string(56, 'a'))) == "3b0c8ac703f828b04c6c197006d17218");
}

// Выгрузка настоящего пакета для сверки с tools/msi_probe.py.
int dump(const char *path) {
    auto data = fs::read_file(path);
    if (!data) {
        fmt::print(stderr, "{}\n", data.error().to_string());
        return 1;
    }
    auto msi = Msi::parse(std::move(*data));
    if (!msi) {
        fmt::print(stderr, "{}\n", msi.error().to_string());
        return 1;
    }
    auto files = msi->files();
    if (!files) {
        fmt::print(stderr, "{}\n", files.error().to_string());
        return 1;
    }
    for (const auto &f : *files) {
        fmt::print("{}\t{}\t{}\t{}\t{}\t{}\n", f.sequence, f.size, f.attributes, f.cabinet,
                   f.md5 ? Md5::to_hex(*f.md5) : "-", f.path);
    }
    return 0;
}

} // namespace

int main() {
    if (const char *path = std::getenv("CORK_MSI_DUMP"); path && *path) {
        return dump(path);
    }
    test_cfbf_roundtrip();
    test_cfbf_rejects_garbage();
    test_demangle();
    test_tables();
    test_files();
    test_long_string_in_pool();
    test_md5_vectors();
    return test::finish("test_msi");
}
