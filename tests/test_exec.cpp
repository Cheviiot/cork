// Чистая логика обёртки: какие аргументы считаются путями, как фильтруется
// вывод и как полный код возврата превращается в код процесса. Всё без Wine.

#include <string>
#include <vector>

#include "base/fs.hpp"
#include "check.h"
#include "exec/filters.hpp"
#include "exec/runner.hpp"
#include "exec/tools.hpp"

using namespace cork::exec;
namespace fs = cork::fs;

namespace {

// «индекс@смещение» — так в одной строке видно и что переводится, и с какого
// места: ключ перед путём обязан уцелеть.
std::string indices_of(std::string_view tool, const std::vector<std::string> &args,
                       PathMode mode = PathMode::Table) {
    std::string out;
    for (const auto &ref : path_argument_refs(tool, args, mode)) {
        if (!out.empty()) {
            out += ",";
        }
        out += std::to_string(ref.index) + "@" + std::to_string(ref.offset);
    }
    return out;
}

} // namespace

void test_wine_noise_is_told_from_program_output() {
    using cork::exec::is_wine_diagnostic;

    // То, что печатает сам Wine. Префикс «<hex потока>:<уровень>:» ставит он
    // и только он.
    CHECK(is_wine_diagnostic("021c:err:winediag:nodrv_CreateWindow Application tried"));
    CHECK(is_wine_diagnostic("0220:fixme:dbghelp:elf_search_auxv can't find symbol"));
    CHECK(is_wine_diagnostic("0048:warn:module:something"));
    CHECK(is_wine_diagnostic("wine: Unhandled page fault on read access to 0000000000000000"));

    // То, что печатает программа или компилятор. Спутать нельзя: ни одна из
    // этих строк не должна пропасть из вывода.
    CHECK(!is_wine_diagnostic("main.c(3): error C2065: 'x': undeclared identifier"));
    CHECK(!is_wine_diagnostic("Note: including file: /usr/include/stdio.h"));
    CHECK(!is_wine_diagnostic("err:something without a thread id"));
    CHECK(!is_wine_diagnostic("0220 err: not the right shape"));
    CHECK(!is_wine_diagnostic("deadbeef:this:is:not:a:level"));
    CHECK(!is_wine_diagnostic(""));
    // Вывод программы, начинающийся с шестнадцатеричного числа и двоеточия,
    // но без уровня Wine, остаётся на месте: так печатают дампы и таблицы.
    CHECK(!is_wine_diagnostic("00401000: 48 89 5c 24 08"));
}

void test_crash_report_paths_become_unix() {
    using cork::exec::crash_report_filter;

    // Кадр трассировки winedbg: путь в скобках должен открываться в
    // редакторе, иначе символы есть, а перейти к строке нельзя.
    const std::string frame =
        "=>0 0x0000014000720a inner+0xa(p=0x0) [Z:\\home\\me\\boom.c:2] in boom (0000)";
    const std::string fixed = crash_report_filter(frame);
    CHECK(fixed.find("[/home/me/boom.c:2]") != std::string::npos);
    CHECK(fixed.find("Z:") == std::string::npos);

    // Всё остальное не трогается: формат сообщений чужой программы нам
    // неизвестен, и переписывать в нём что попало нельзя.
    CHECK_EQ(crash_report_filter("plain program output"), "plain program output");
    CHECK_EQ(crash_report_filter("drive Z: mentioned but no bracket"),
             "drive Z: mentioned but no bracket");
}

int main() {
    test_wine_noise_is_told_from_program_output();
    test_crash_report_paths_become_unix();
    // --- таблица инструментов ---
    {
        CHECK(find_tool("cl") != nullptr);
        CHECK(find_tool("CL.EXE") != nullptr);   // регистр и расширение не важны
        CHECK(find_tool("msbuild") != nullptr);
        CHECK(find_tool("nosuchtool") == nullptr);
        CHECK(is_native_shim("cmd"));
        CHECK(is_native_shim("findstr.exe"));
        CHECK(!is_native_shim("cl"));

        const ToolSpec *msbuild = find_tool("msbuild");
        CHECK(msbuild != nullptr);
        if (msbuild != nullptr) {
            // Вывод MSBuild должен доезжать нетронутым.
            CHECK(msbuild->raw_output);
        }
        const ToolSpec *cl = find_tool("cl");
        if (cl != nullptr) {
            CHECK(!cl->raw_output);
        }
    }

    // --- какие аргументы являются путями ---
    {
        // Ключ с приклеенным значением и позиционный исходник.
        CHECK_EQ(indices_of("cl", {"/nologo", "/I/usr/include", "hello.cpp"}),
                 std::string("1@2,2@0"));

        // Короткие ключи без пути не должны попадать. Именно на них держалась
        // прежняя эвристика с проверкой существования каталога.
        CHECK_EQ(indices_of("cl", {"/nologo", "/W4", "/EHsc", "/c"}), std::string(""));

        // /Zi — отладочная информация, а не путь, хотя и начинается как /Z...
        CHECK_EQ(indices_of("cl", {"/Zi", "/Fohello.obj"}), std::string("1@3"));

        // Ключи компоновщика со значением через двоеточие.
        CHECK_EQ(indices_of("link", {"/nologo", "/OUT:bin/app.exe", "/LIBPATH:/usr/lib",
                                     "hello.obj"}),
                 std::string("1@5,2@9,3@0"));

        // Значение в следующем аргументе.
        CHECK_EQ(indices_of("midl", {"/out", "generated", "iface.idl"}), std::string("1@0,2@0"));

        // Режим off не переводит ничего.
        CHECK_EQ(indices_of("cl", {"/I/usr/include", "a.cpp"}, PathMode::Off), std::string(""));
    }

    // --- запасная эвристика ---
    // Таблица никогда не будет полной, поэтому неизвестный ключ с абсолютным
    // путём всё равно должен распознаваться.
    {
        CHECK(looks_like_path_argument("-I/usr/include"));
        CHECK(looks_like_path_argument("/I/usr/include"));
        CHECK(looks_like_path_argument("/Fo/usr/include"));
        CHECK(looks_like_path_argument("/MANIFESTINPUT:/usr/include"));
        CHECK(looks_like_path_argument("/usr/include/stdio.h"));
        // «/tmp» путём не считается: его родитель — корень. Так вела себя и
        // прежняя реализация, и это то, что отделяет путь от короткого ключа.
        CHECK(!looks_like_path_argument("/tmp"));
        // Короткий ключ путём не является.
        CHECK(!looks_like_path_argument("/P"));
        CHECK(!looks_like_path_argument("/c"));
        // Каталог не существует — не путь.
        CHECK(!looks_like_path_argument("/no/such/directory/anywhere/file.h"));
        // Шим cmd принимает «//c», и это не путь.
        CHECK(!looks_like_path_argument("//c"));

        // Неизвестный ключ неизвестного инструмента подхватывается запасной
        // веткой, а не теряется.
        // Запасная ветка тоже обязана дать смещение, а не ноль.
        CHECK_EQ(indices_of("cl", {"/totallyunknown:/usr/include"}), std::string("0@16"));
    }

    // --- фильтры вывода ---
    {
        CHECK_EQ(cl_stdout_filter("Note: including file: z:\\usr\\include\\stdio.h"),
                 std::string("Note: including file: /usr/include/stdio.h"));

        CHECK_EQ(cl_stdout_filter("z:\\home\\u\\a.cpp(12): error C2065: undeclared"),
                 std::string("/home/u/a.cpp(12): error C2065: undeclared"));

        // С колонкой.
        CHECK_EQ(cl_stdout_filter("z:\\home\\u\\a.cpp(12,5): warning C4100: unused"),
                 std::string("/home/u/a.cpp(12,5): warning C4100: unused"));

        CHECK_EQ(cl_stdout_filter("#line 1 \"z:\\\\home\\\\u\\\\a.cpp\""),
                 std::string("#line 1 \"//home//u//a.cpp\""));

        // Обычная строка не трогается.
        CHECK_EQ(cl_stdout_filter("Microsoft (R) C/C++ Optimizing Compiler"),
                 std::string("Microsoft (R) C/C++ Optimizing Compiler"));

        // «cl : command line warning» не должна пострадать от замены.
        CHECK_EQ(cl_stdout_filter("cl : command line warning D9002: ignoring unknown option"),
                 std::string("cl : command line warning D9002: ignoring unknown option"));

        CHECK_EQ(dumpbin_stdout_filter("Dump of file z:\\home\\u\\a.obj"),
                 std::string("Dump of file /home/u/a.obj"));

        // Диагностика в stderr тоже переводится, не только в stdout.
        CHECK_EQ(cl_stderr_filter("z:\\home\\u\\a.cpp(3): fatal error C1083: cannot open"),
                 std::string("/home/u/a.cpp(3): fatal error C1083: cannot open"));

        CHECK_EQ(strip_cr("line\r"), std::string("line"));
        CHECK_EQ(strip_cr("no carriage return"), std::string("no carriage return"));

        CHECK(filter_for("cl", false) != nullptr);
        CHECK(filter_for("link", false) == nullptr);
        CHECK(filter_for("dumpbin", true) == nullptr);
    }

    // --- отображение кода возврата ---
    {
        // Ради этого случая и существует передача полного кода: mt.exe
        // отдаёт 0x41020001, CMake ждёт 0xbb, а Unix сохранил бы только 0x01.
        CHECK_EQ(std::to_string(map_exit_code("mt", 0x41020001u)), std::to_string(0xbb));
        // Другому инструменту то же значение не отображается.
        CHECK_EQ(std::to_string(map_exit_code("cl", 0x41020001u)), std::string("1"));
        CHECK_EQ(std::to_string(map_exit_code("cl", 0)), std::string("0"));
        CHECK_EQ(std::to_string(map_exit_code("cl", 2)), std::string("2"));
        // Старший байт теряется, и это неизбежно: Unix отдаёт восемь бит.
        CHECK_EQ(std::to_string(map_exit_code("cl", 0x100u)), std::string("0"));
    }

    return cork::test::finish("test_exec");
}
