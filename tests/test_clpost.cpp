// Постобработка препроцессированного вывода.
//
// Два разных вопроса, и оба легко ответить неверно: какие файлы вообще
// появятся после /P, и что внутри них можно трогать. Второй опаснее.
// Препроцессированный текст состоит в основном из пользовательского кода со
// строковыми литералами, и замена «по всему файлу» испортила бы те из них,
// что похожи на пути, — молча, потому что компилироваться такой файл будет
// по-прежнему.

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include <fmt/format.h>

#include "base/fs.hpp"
#include "check.h"
#include "exec/clpost.hpp"

using namespace cork;
using namespace cork::exec;

namespace stdfs = std::filesystem;

namespace {

std::vector<std::string> a(std::initializer_list<const char *> items) {
    return std::vector<std::string>(items.begin(), items.end());
}

stdfs::path scratch() {
    const char *base = std::getenv("CORK_TEST_TMP");
    stdfs::path dir = base != nullptr && *base != '\0' ? stdfs::path(base)
                                                       : stdfs::current_path() / ".testtmp";
    dir /= fmt::format("clpost-{}", ::getpid());
    std::error_code ec;
    stdfs::remove_all(dir, ec);
    stdfs::create_directories(dir, ec);
    return dir;
}

void write_file(const stdfs::path &p, std::string_view text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string read_file(const stdfs::path &p) {
    auto r = fs::read_file(p);
    return r.has_value() ? *r : std::string();
}

void test_no_outputs_without_p() {
    const stdfs::path cwd("/work");
    // Обычная компиляция файлов не производит.
    CHECK(preprocessed_outputs(a({"/nologo", "/c", "main.cpp"}), cwd).empty());
    // /EP без /P шлёт текст в stdout — файла нет, чинит фильтр потока.
    CHECK(preprocessed_outputs(a({"/EP", "main.cpp"}), cwd).empty());
    // /Fi без /P ничего не включает: ключ относится к препроцессированию.
    CHECK(preprocessed_outputs(a({"/Fiout.i", "main.cpp"}), cwd).empty());
}

void test_default_name() {
    const stdfs::path cwd("/work");
    const auto out = preprocessed_outputs(a({"/P", "src/main.cpp"}), cwd);
    CHECK(out.size() == 1);
    if (out.size() == 1) {
        // Файл появляется в текущем каталоге, а не рядом с исходником: cl
        // берёт от исходника только имя.
        CHECK_EQ(out[0].string(), std::string("/work/main.i"));
    }
}

void test_several_sources() {
    const stdfs::path cwd("/work");
    const auto out = preprocessed_outputs(a({"/P", "a.c", "b.cxx", "lib.lib", "/DX=1"}), cwd);
    CHECK(out.size() == 2);
    if (out.size() == 2) {
        CHECK_EQ(out[0].string(), std::string("/work/a.i"));
        CHECK_EQ(out[1].string(), std::string("/work/b.i"));
    }
}

void test_absolute_unix_paths_are_not_switches() {
    // Здесь вся неприятность задачи. На Windows ключ начинается со слэша, а
    // путь — с буквы диска. У нас пути unix, и «/work/main.c» выглядит ровно
    // как ключ. Приняв его за ключ, мы не найдём ни одного исходника и тихо
    // ничего не постобработаем — что и случилось, пока тест пользовался
    // только относительными путями.
    const stdfs::path cwd("/work");
    const auto out =
        preprocessed_outputs(a({"/nologo", "/P", "/I/abs/inc", "/work/src/main.c"}), cwd);
    CHECK(out.size() == 1);
    if (out.size() == 1) {
        CHECK_EQ(out[0].string(), std::string("/work/main.i"));
    }

    // При этом настоящие ключи ключами и остаются, включая те, что начинаются
    // с тех же букв, что каталоги: /wd4996 против /work/...
    const auto only_switches =
        preprocessed_outputs(a({"/P", "/W4", "/wd4996", "/std:c++20", "/EHsc"}), cwd);
    CHECK(only_switches.empty());
}

void test_explicit_source_switches() {
    // /Tc и /Tp называют исходник прямо, вместе с языком, и одним аргументом.
    const stdfs::path cwd("/work");
    const auto c = preprocessed_outputs(a({"/P", "/Tcmain.x"}), cwd);
    CHECK(c.size() == 1);
    if (c.size() == 1) {
        CHECK_EQ(c[0].string(), std::string("/work/main.i"));
    }

    const auto cpp = preprocessed_outputs(a({"/P", "/Tp/abs/other.x"}), cwd);
    CHECK(cpp.size() == 1);
    if (cpp.size() == 1) {
        CHECK_EQ(cpp[0].string(), std::string("/work/other.i"));
    }
}

void test_explicit_fi() {
    const stdfs::path cwd("/work");
    const auto abs = preprocessed_outputs(a({"/P", "/Fi/tmp/out.i", "main.cpp"}), cwd);
    CHECK(abs.size() == 1);
    if (abs.size() == 1) {
        CHECK_EQ(abs[0].string(), std::string("/tmp/out.i"));
    }

    const auto rel = preprocessed_outputs(a({"/P", "/Fiout.i", "main.cpp"}), cwd);
    CHECK(rel.size() == 1);
    if (rel.size() == 1) {
        CHECK_EQ(rel[0].string(), std::string("/work/out.i"));
    }

    // Имя, оканчивающееся разделителем, — каталог: туда лягут файлы по именам
    // исходников. Приняв его за имя файла, мы бы искали не тот путь.
    const auto dir = preprocessed_outputs(a({"/P", "/Fipre/", "a.c", "b.c"}), cwd);
    CHECK(dir.size() == 2);
    if (dir.size() == 2) {
        CHECK_EQ(dir[0].string(), std::string("/work/pre/a.i"));
        CHECK_EQ(dir[1].string(), std::string("/work/pre/b.i"));
    }

    // Регистр ключей у cl не важен.
    const auto lower = preprocessed_outputs(a({"/p", "/fiout.i", "main.cpp"}), cwd);
    CHECK(lower.size() == 1);
}

void test_rewrites_line_directives() {
    const stdfs::path dir = scratch();
    const stdfs::path file = dir / "out.i";

    // Пути внутри #line записаны как строковые литералы C: разделители в них
    // удвоены.
    write_file(file,
               "#line 1 \"Z:\\\\home\\\\user\\\\main.c\"\n"
               "int main(void) { return 0; }\n"
               "#line 12 \"Z:\\\\usr\\\\include\\\\stdio.h\"\n");

    auto changed = rewrite_preprocessed_file(file);
    CHECK(changed.has_value());
    CHECK(changed.has_value() && *changed);

    const std::string got = read_file(file);
    CHECK(got.find("\"/home/user/main.c\"") != std::string::npos);
    CHECK(got.find("\"/usr/include/stdio.h\"") != std::string::npos);
    CHECK(got.find("Z:") == std::string::npos);
    // Код между директивами не тронут.
    CHECK(got.find("int main(void) { return 0; }") != std::string::npos);

    std::error_code ec;
    stdfs::remove_all(dir, ec);
}

void test_leaves_user_string_literals_alone() {
    // Вот ради чего обработка сужена до директив. Эта строка — код
    // пользователя, и путь в ней принадлежит программе, а не сборке. Замена
    // «по всему файлу» изменила бы поведение собранного двоичного файла, и
    // компилироваться он продолжил бы как ни в чём не бывало.
    const stdfs::path dir = scratch();
    const stdfs::path file = dir / "out.i";
    const std::string source =
        "#line 1 \"Z:\\\\home\\\\user\\\\main.c\"\n"
        "const char *path = \"Z:\\\\windows\\\\system32\";\n"
        "const char *note = \"see #line 5 \\\"Z:\\\\fake\\\"\";\n";
    write_file(file, source);

    auto changed = rewrite_preprocessed_file(file);
    CHECK(changed.has_value());

    const std::string got = read_file(file);
    // Директива исправлена.
    CHECK(got.find("#line 1 \"/home/user/main.c\"") != std::string::npos);
    // А литералы пользователя — нет.
    CHECK(got.find("\"Z:\\\\windows\\\\system32\"") != std::string::npos);
    CHECK(got.find("see #line 5") != std::string::npos);
    CHECK(got.find("Z:\\\\fake") != std::string::npos);

    std::error_code ec;
    stdfs::remove_all(dir, ec);
}

void test_does_not_touch_a_clean_file() {
    // Если менять нечего, файл не переписывается: лишняя запись сдвинула бы
    // время изменения и заставила бы сборочную систему пересобрать всё, что
    // от него зависит.
    const stdfs::path dir = scratch();
    const stdfs::path file = dir / "clean.i";
    write_file(file, "#line 1 \"/home/user/main.c\"\nint x;\n");

    const auto before = stdfs::last_write_time(file);
    auto changed = rewrite_preprocessed_file(file);
    CHECK(changed.has_value());
    CHECK(changed.has_value() && !*changed);
    CHECK(stdfs::last_write_time(file) == before);

    std::error_code ec;
    stdfs::remove_all(dir, ec);
}

void test_missing_file_is_not_an_error() {
    // Компилятор мог отказать раньше, чем создал файл. Это его дело, и
    // сообщать об ошибке постобработки поверх его собственной диагностики
    // значит сбить с толку.
    const stdfs::path dir = scratch();
    auto r = rewrite_preprocessed_file(dir / "never-created.i");
    CHECK(r.has_value());
    CHECK(r.has_value() && !*r);

    std::error_code ec;
    stdfs::remove_all(dir, ec);
}

void test_other_drive_letters_are_left_alone() {
    // Буква диска берётся из настройки сеанса. Путь на настоящем диске C: —
    // это путь внутри префикса Wine, и превращать его в unix-путь нельзя.
    const stdfs::path dir = scratch();
    const stdfs::path file = dir / "out.i";
    write_file(file, "#line 1 \"C:\\\\windows\\\\inc\\\\a.h\"\n");

    FilterConfig cfg;
    cfg.root_drive = 'z';
    auto changed = rewrite_preprocessed_file(file, cfg);
    CHECK(changed.has_value());

    const std::string got = read_file(file);
    // Слэши стали прямыми, но буква диска осталась: это не наш корень.
    CHECK(got.find("\"C:/windows/inc/a.h\"") != std::string::npos);

    std::error_code ec;
    stdfs::remove_all(dir, ec);
}

} // namespace

int main() {
    test_no_outputs_without_p();
    test_default_name();
    test_several_sources();
    test_absolute_unix_paths_are_not_switches();
    test_explicit_source_switches();
    test_explicit_fi();
    test_rewrites_line_directives();
    test_leaves_user_string_literals_alone();
    test_does_not_touch_a_clean_file();
    test_missing_file_is_not_an_error();
    test_other_drive_letters_are_left_alone();
    return cork::test::finish("test_clpost");
}
