// Кодек request/status компилируется и в cork_core, и в PE-хелпер. Здесь он
// проверяется на хосте, без Wine: если разбор молча принимает мусор, хелпер
// повиснет или запустит не то, а диагностировать это уже внутри Wine на
// порядок дороже.

#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "proto/protocol.hpp"

using namespace cork::proto;

int main() {
    // --- полный оборот запроса ---
    {
        Request in;
        in.flags = kTranslatePaths | kTranslateResponseFiles;
        in.exe = "/home/cheviiot/.cork/vc/tools/msvc/14.44/bin/Hostx64/x64/cl.exe";
        in.cwd = "/home/cheviiot/проект/сборка";
        in.status_path = "/home/cheviiot/.cork/run/123.status";
        in.args = {"/nologo", "-I/usr/include", "@rsp.txt", "hello.cpp"};
        in.path_refs = {{1, 2}, {3, 0}};

        const auto bytes = encode(in);
        Request out;
        CHECK_EQ(std::to_string(decode(bytes.data(), bytes.size(), out)), std::string("0"));
        CHECK_EQ(out.exe, in.exe);
        CHECK_EQ(out.cwd, in.cwd);
        CHECK_EQ(out.status_path, in.status_path);
        CHECK(out.flags == in.flags);
        CHECK(out.args == in.args);
        CHECK(out.path_refs.size() == in.path_refs.size());
        if (out.path_refs.size() == in.path_refs.size()) {
            for (std::size_t i = 0; i < out.path_refs.size(); ++i) {
                CHECK(out.path_refs[i].index == in.path_refs[i].index);
                // Смещение обязано доехать: без него хелпер перевёл бы
                // «/I/usr/include» целиком, вместе с ключом.
                CHECK(out.path_refs[i].offset == in.path_refs[i].offset);
            }
        }
    }

    // --- байтовая прозрачность ---
    // Путь в Linux — произвольная последовательность байтов. Протокол не имеет
    // права её испортить, иначе сборка в каталоге с «неправильным» именем
    // сломается необъяснимо. Проверяются и не-UTF-8 байты, и встроенный ноль.
    {
        Request in;
        in.exe = "/x/cl.exe";
        in.status_path = "/x/s";
        // Длина берётся из самого литерала: захардкоженное число здесь уже
        // однажды оказалось больше литерала и давало чтение за границей.
        static constexpr char kWeird[] = "/tmp/\xff\xfe\x80 not utf8";
        static constexpr char kWithNul[] = "a\0b";
        std::string weird(kWeird, sizeof kWeird - 1);
        std::string with_nul(kWithNul, sizeof kWithNul - 1);
        in.args = {weird, with_nul, ""};
        in.path_refs = {{0, 0}};

        const auto bytes = encode(in);
        Request out;
        CHECK_EQ(std::to_string(decode(bytes.data(), bytes.size(), out)), std::string("0"));
        CHECK(out.args.size() == 3);
        CHECK(out.args[0] == weird);
        CHECK(out.args[1] == with_nul);
        CHECK(out.args[2].empty());
    }

    // --- пустой запрос без аргументов ---
    {
        Request in;
        in.exe = "/x/cl.exe";
        const auto bytes = encode(in);
        Request out;
        CHECK_EQ(std::to_string(decode(bytes.data(), bytes.size(), out)), std::string("0"));
        CHECK(out.args.empty());
        CHECK(out.path_refs.empty());
        CHECK(out.cwd.empty());
    }

    // --- отказы разбора ---
    {
        Request in;
        in.exe = "/x/cl.exe";
        in.args = {"a"};
        const auto good = encode(in);

        Request out;
        // Пустой ввод.
        CHECK_EQ(std::to_string(decode(nullptr, 0, out)), std::to_string(kReasonTruncated));

        // Чужая сигнатура: status подсунут вместо request.
        const auto status_bytes = encode(Status{});
        CHECK_EQ(std::to_string(decode(status_bytes.data(), status_bytes.size(), out)),
                 std::to_string(kReasonBadMagic));

        // Обрезка на каждой длине должна давать отказ, а не мусор в полях.
        for (std::size_t n = 1; n < good.size(); ++n) {
            Request partial;
            const std::uint32_t rc = decode(good.data(), n, partial);
            CHECK(rc != 0);
        }

        // Версия из будущего.
        auto future = good;
        future[4] = 99;
        CHECK_EQ(std::to_string(decode(future.data(), future.size(), out)),
                 std::to_string(kReasonBadVersion));
    }

    // --- окружение потомка ---
    // Переменные, ломающие сам хелпер, должны доезжать отдельным списком.
    {
        Request in;
        in.exe = "/x/cl.exe";
        in.child_env = {"WINEDLLOVERRIDES=vcruntime140=n", "FOO=bar"};
        const auto bytes = encode(in);
        Request out;
        CHECK_EQ(std::to_string(decode(bytes.data(), bytes.size(), out)), std::string("0"));
        CHECK(out.child_env == in.child_env);
    }

    // --- индекс пути за пределами args ---
    // Испорченный индекс — это не «пропустим этот путь»: непереведённый путь
    // уедет в компилятор, и он упадёт на ненайденном заголовке, а причина
    // будет выглядеть как угодно, только не как испорченный запрос.
    {
        Request in;
        in.exe = "/x/cl.exe";
        in.args = {"a"};
        in.path_refs = {{5, 0}};
        const auto bytes = encode(in);
        Request out;
        CHECK_EQ(std::to_string(decode(bytes.data(), bytes.size(), out)),
                 std::to_string(kReasonBadIndex));
    }

    // --- объявленная длина строки больше файла ---
    // Без сверки с остатком такой заголовок заставил бы выделить четыре
    // гигабайта прежде, чем обнаружилась бы обрезка.
    {
        Request in;
        in.exe = "/x/cl.exe";
        auto bytes = encode(in);
        // Поле exe начинается после magic, version, flags = 12 байт.
        bytes[12] = 0xff;
        bytes[13] = 0xff;
        bytes[14] = 0xff;
        bytes[15] = 0xff;
        Request out;
        CHECK_EQ(std::to_string(decode(bytes.data(), bytes.size(), out)),
                 std::to_string(kReasonTruncated));
    }

    // --- полный оборот статуса ---
    {
        Status in;
        in.kind = StatusKind::ChildExited;
        // Именно ради этого значения всё и затевалось: mt.exe отдаёт
        // 0x41020001, и до нативной стороны оно обязано доехать целиком, а не
        // усечённым до байта.
        in.code = 0x41020001u;
        in.child_pid = 4242;
        in.flags = kJobCreated;

        const auto bytes = encode(in);
        Status out;
        CHECK_EQ(std::to_string(decode(bytes.data(), bytes.size(), out)), std::string("0"));
        CHECK(out.kind == StatusKind::ChildExited);
        CHECK_EQ(std::to_string(out.code), std::to_string(0x41020001u));
        CHECK_EQ(std::to_string(out.child_pid), std::string("4242"));
        CHECK(out.flags == kJobCreated);
    }

    // --- response-файлы ---
    {
        Request in;
        in.exe = "/x/cl.exe";
        in.args = {"/nologo", "@/tmp/a.rsp"};
        ResponseFile rf;
        rf.arg_index = 1;
        rf.args = {"/I/usr/include", "main.cpp"};
        rf.path_refs = {{0, 2}};
        in.response_files = {rf};
        const auto bytes = encode(in);
        Request out;
        CHECK_EQ(std::to_string(decode(bytes.data(), bytes.size(), out)), std::string("0"));
        CHECK(out.response_files.size() == 1);
        if (out.response_files.size() == 1) {
            const auto &got = out.response_files[0];
            CHECK(got.arg_index == 1);
            CHECK(got.args.size() == 2);
            CHECK_EQ(got.args[0], std::string("/I/usr/include"));
            CHECK(got.path_refs.size() == 1);
            CHECK(got.path_refs[0].offset == 2);
        }
    }

    // --- response-файл ссылается на несуществующий аргумент ---
    // Тот же довод, что и для path_refs: непереведённый путь уедет в
    // компилятор, и разбираться придётся с его сообщением, а не с нашим.
    {
        Request in;
        in.exe = "/x/cl.exe";
        in.args = {"@/tmp/a.rsp"};
        ResponseFile rf;
        rf.arg_index = 0;
        rf.args = {"main.cpp"};
        rf.path_refs = {{9, 0}};
        in.response_files = {rf};
        const auto bytes = encode(in);
        Request out;
        CHECK_EQ(std::to_string(decode(bytes.data(), bytes.size(), out)),
                 std::to_string(kReasonBadIndex));
    }

    // --- response-файл подменяет несуществующий аргумент ---
    {
        Request in;
        in.exe = "/x/cl.exe";
        in.args = {"/nologo"};
        ResponseFile rf;
        rf.arg_index = 7;
        in.response_files = {rf};
        const auto bytes = encode(in);
        Request out;
        CHECK_EQ(std::to_string(decode(bytes.data(), bytes.size(), out)),
                 std::to_string(kReasonBadIndex));
    }

    // --- статус с неизвестным kind ---
    {
        Status in;
        in.kind = StatusKind::ChildExited;
        auto bytes = encode(in);
        bytes[8] = 77;
        Status out;
        CHECK(decode(bytes.data(), bytes.size(), out) != 0);
    }

    return cork::test::finish("test_protocol");
}
