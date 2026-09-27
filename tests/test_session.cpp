// Клонирование деревьев, ключи сессий и деперсонализация префикса.
//
// Проверяется то, что ломается молча: клонирование, потерявшее символьную
// ссылку или права на исполнение, даёт префикс, который выглядит целым и не
// работает; ключ сессии, зависящий от подкаталога, разводит одну сборку по
// десятку префиксов; а гейт переносимости, пропустивший путь в домашний
// каталог сборщика, отдаёт этот путь пользователю.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include <fmt/format.h>

#include "base/clone.hpp"
#include "base/fs.hpp"
#include "check.h"
#include "exec/session.hpp"
#include "setup/prefix.hpp"

using namespace cork;

namespace stdfs = std::filesystem;

namespace {

class Sandbox {
public:
    Sandbox() {
        const char *base = std::getenv("CORK_TEST_TMP");
        stdfs::path parent = base != nullptr && *base != '\0' ? stdfs::path(base)
                                                              : stdfs::current_path() / ".testtmp";
        path_ = parent / fmt::format("session-{}-{}", ::getpid(), ++counter_);
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
    static inline int counter_ = 0;
    stdfs::path path_;
};

void write_file(const stdfs::path &p, std::string_view text) {
    std::error_code ec;
    stdfs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string read_file(const stdfs::path &p) {
    auto r = fs::read_file(p);
    return r.has_value() ? *r : std::string();
}

void test_clone_preserves_what_matters() {
    Sandbox box;
    const stdfs::path from = box.path() / "src";
    const stdfs::path to = box.path() / "dst";

    write_file(from / "plain.txt", "hello");
    write_file(from / "bin" / "tool", "#!/bin/sh\n");
    stdfs::permissions(from / "bin" / "tool", stdfs::perms::owner_all | stdfs::perms::group_read |
                                                  stdfs::perms::group_exec);
    std::error_code ec;
    stdfs::create_directory(from / "empty", ec);
    stdfs::create_symlink("plain.txt", from / "link.txt", ec);
    CHECK(!ec);

    auto stats = fs::clone_tree(from, to);
    CHECK(stats.has_value());
    if (!stats) {
        return;
    }
    CHECK(stats->files == 2);
    CHECK(stats->symlinks == 1);
    CHECK(stats->directories == 2);

    CHECK_EQ(read_file(to / "plain.txt"), std::string("hello"));

    // Символьная ссылка осталась ссылкой. Скопированная содержимым, она
    // превратилась бы в обычный файл с путём внутри — ровно та порча, от
    // которой дерево выглядит целым и не работает.
    CHECK(stdfs::is_symlink(to / "link.txt", ec));
    CHECK_EQ(stdfs::read_symlink(to / "link.txt", ec).string(), std::string("plain.txt"));

    // Бит исполнения сохранён: без него cl.exe в префиксе не запустится.
    const auto perms = stdfs::status(to / "bin" / "tool", ec).permissions();
    CHECK((perms & stdfs::perms::owner_exec) != stdfs::perms::none);

    // Время изменения перенесено, иначе Wine решит, что префикс обновился, и
    // запустит обновление на каждой сборке.
    CHECK(stdfs::last_write_time(to / "plain.txt", ec) ==
          stdfs::last_write_time(from / "plain.txt", ec));

    CHECK(stdfs::is_directory(to / "empty", ec));
}

void test_clone_refuses_to_merge() {
    Sandbox box;
    const stdfs::path from = box.path() / "src";
    const stdfs::path to = box.path() / "dst";
    write_file(from / "a.txt", "a");
    write_file(to / "b.txt", "b");

    // Смешение двух деревьев даёт результат, который потом невозможно
    // объяснить: часть файлов оттуда, часть отсюда.
    auto r = fs::clone_tree(from, to);
    CHECK(!r.has_value());
    CHECK(!r.has_value() && r.error().code == Error::Code::Conflict);
}

void test_session_key_follows_the_build_root() {
    Sandbox box;
    const stdfs::path root = box.path() / "project";
    const stdfs::path deep = root / "src" / "module" / "detail";
    std::error_code ec;
    stdfs::create_directories(deep, ec);
    write_file(root / "CMakeCache.txt", "");

    // Вызовы компилятора из разных подкаталогов одной сборки обязаны попасть
    // в одну сессию. Иначе каждая подпапка заведёт свой префикс, и сборка
    // среднего проекта съест диск.
    const std::string from_root = exec::session_key(root);
    const std::string from_deep = exec::session_key(deep);
    CHECK_EQ(from_deep, from_root);

    // А другой проект получает другой ключ, даже если каталог называется так
    // же: в ключ входит хеш полного пути.
    const stdfs::path other = box.path() / "elsewhere" / "project";
    stdfs::create_directories(other, ec);
    write_file(other / "CMakeCache.txt", "");
    CHECK(exec::session_key(other) != from_root);

    // Имя проекта видно в ключе: список сессий должен читаться глазами.
    CHECK(from_root.rfind("project-", 0) == 0);
}

// Всё под CMakeFiles принадлежит своей сборке.
//
// Регрессия на измеренное: configure Ogre 14.5.2 заводил по сессии на каждую
// проверку компилятора, потому что try_compile кладёт в свой каталог
// настоящий CMakeCache.txt. Тридцать два префикса Wine на один configure —
// это не «медленно», это неработоспособно, и заметить такое без теста можно
// только по таймеру.
//
// Отдельно проверяется определение компилятора: оно происходит раньше, чем
// CMake запишет CMakeCache.txt в каталог сборки, поэтому подъём из пробы
// корня не нашёл бы и правило «пропускать пробы по имени» тут не спасает.
void test_cmake_probes_share_the_parent_session() {
    Sandbox box;
    const stdfs::path root = box.path() / "project";
    std::error_code ec;
    stdfs::create_directories(root, ec);
    write_file(root / "CMakeCache.txt", "");

    const std::string expected = exec::session_key(root);

    for (const stdfs::path &probe :
         {root / "build" / "CMakeFiles" / "CMakeScratch" / "TryCompile-a1b2c3",
          root / "build" / "CMakeFiles" / "4.3.0" / "CompilerIdC",
          root / "build" / "CMakeFiles" / "4.3.0" / "CompilerIdCXX"}) {
        stdfs::create_directories(probe, ec);
        // У пробы свой CMakeCache.txt — именно он и сбивал поиск с толку.
        write_file(probe / "CMakeCache.txt", "");
        CHECK_EQ(exec::session_key(probe), expected);
    }

    // Самый ранний случай: CMakeCache.txt в каталоге сборки ещё не записан, а
    // компилятор уже определяется. Ключом обязан стать каталог сборки, а не
    // сама проба.
    const stdfs::path early = box.path() / "fresh" / "build";
    const stdfs::path early_probe = early / "CMakeFiles" / "4.3.0" / "CompilerIdCXX";
    stdfs::create_directories(early_probe, ec);
    write_file(early_probe / "CMakeCache.txt", "");
    CHECK_EQ(exec::session_key(early_probe), exec::session_key(early));

    // А обычный каталог сборки корнем быть обязан: без него всё это лечение
    // превратилось бы в «никогда не считать CMakeCache.txt признаком».
    const stdfs::path build = root / "build";
    write_file(build / "CMakeCache.txt", "");
    const std::string build_key = exec::session_key(build);
    CHECK(build_key != expected);
    CHECK(build_key.rfind("build-", 0) == 0);
}

void test_session_key_can_be_overridden() {
    Sandbox box;
    ::setenv("CORK_SESSION", "explicit-key", 1);
    CHECK_EQ(exec::session_key(box.path()), std::string("explicit-key"));
    ::unsetenv("CORK_SESSION");
}

// Минимальный префикс: ровно то, что трогает деперсонализация.
stdfs::path make_prefix(const Sandbox &box, const std::string &home, const std::string &user) {
    const stdfs::path prefix = box.path() / "prefix";
    const stdfs::path profile = prefix / "drive_c" / "users" / user;
    std::error_code ec;
    stdfs::create_directories(profile / "AppData" / "Roaming", ec);
    stdfs::create_directories(prefix / "dosdevices", ec);
    stdfs::create_directories(stdfs::path(home) / "Documents", ec);

    // Ссылки наружу — и на верхнем уровне, и глубже: настоящий wineboot
    // делает и те, и другие.
    stdfs::create_symlink(stdfs::path(home) / "Documents", profile / "Documents", ec);
    stdfs::create_symlink(stdfs::path(home) / "Documents",
                          profile / "AppData" / "Roaming" / "Templates", ec);
    // Ссылка внутрь префикса законна и трогать её незачем.
    stdfs::create_symlink("../drive_c", prefix / "dosdevices" / "c:", ec);
    stdfs::create_symlink("/dev/ttyS0", prefix / "dosdevices" / "com1", ec);

    std::string dos_home = "Z:";
    for (const char c : home) {
        dos_home += (c == '/') ? "\\\\" : std::string(1, c);
    }
    write_file(prefix / "user.reg",
               fmt::format("[Software\\\\Wine]\n"
                           "\"Personal\"=\"{}\\\\Documents\"\n"
                           "\"Temp\"=\"C:\\\\\\\\users\\\\\\\\{}\\\\\\\\Temp\"\n",
                           dos_home, user));
    write_file(prefix / "system.reg",
               fmt::format("[Installer]\n\"InstallSource\"=\"{}\\\\build\\\\wine\\\\support\\\\\"\n",
                           dos_home));
    return prefix;
}

void test_depersonalise_removes_every_trace() {
    Sandbox box;
    const std::string home = (box.path() / "fakehome").string();
    const std::string user = "builder";
    const stdfs::path prefix = make_prefix(box, home, user);

    auto stats = setup::depersonalise_prefix(prefix);
    CHECK(stats.has_value());
    if (!stats) {
        return;
    }
    // Обе ссылки наружу — и верхнего уровня, и вложенная.
    CHECK(stats->links_replaced == 2);
    CHECK(stats->devices_removed == 1);

    std::error_code ec;
    // Ссылка внутрь префикса не тронута.
    CHECK(stdfs::is_symlink(prefix / "dosdevices" / "c:", ec));
    CHECK(!stdfs::exists(prefix / "dosdevices" / "com1", ec));

    // Профиль переименован в нейтральное: имя сборщика к пользователю
    // отношения не имеет.
    CHECK(stdfs::is_directory(prefix / "drive_c" / "users" / "default", ec));
    CHECK(!stdfs::exists(prefix / "drive_c" / "users" / user, ec));

    // Главная проверка: гейт переносимости больше ничего не находит.
    auto violations = setup::check_prefix_portable(prefix, home, user);
    CHECK(violations.has_value());
    if (violations.has_value() && !violations->empty()) {
        for (const auto &v : *violations) {
            fmt::print(stderr, "  осталось: {}: {}\n", v.file.string(), v.detail);
        }
    }
    CHECK(violations.has_value() && violations->empty());
}

void test_portability_gate_catches_what_matters() {
    Sandbox box;
    const std::string home = (box.path() / "fakehome").string();
    const stdfs::path prefix = make_prefix(box, home, "builder");

    // До деперсонализации находок должно быть много — иначе гейт не работает
    // и «чисто» после него ничего не значит.
    auto before = setup::check_prefix_portable(prefix, home, "builder");
    CHECK(before.has_value());
    CHECK(before.has_value() && before->size() >= 3);

    bool saw_symlink = false;
    bool saw_registry = false;
    for (const auto &v : *before) {
        saw_symlink = saw_symlink || v.detail.rfind("symlink", 0) == 0;
        saw_registry = saw_registry || v.line > 0;
    }
    // Ищутся обе формы: цель ссылки и содержимое реестра. Одна без другой
    // пропускает половину привязок.
    CHECK(saw_symlink);
    CHECK(saw_registry);
}

void test_instantiate_restores_the_user_name() {
    Sandbox box;
    const std::string home = (box.path() / "fakehome").string();
    const stdfs::path prefix = make_prefix(box, home, "builder");
    CHECK(setup::depersonalise_prefix(prefix).has_value());

    CHECK(setup::instantiate_prefix(prefix, "someone").has_value());

    std::error_code ec;
    CHECK(stdfs::is_directory(prefix / "drive_c" / "users" / "someone", ec));
    CHECK(!stdfs::exists(prefix / "drive_c" / "users" / "default", ec));
    // И реестр знает профиль под новым именем, иначе TEMP указывал бы в
    // каталог, которого больше нет.
    const std::string reg = read_file(prefix / "user.reg");
    CHECK(reg.find("users\\\\someone\\\\Temp") != std::string::npos);
    CHECK(reg.find("users\\\\default\\\\") == std::string::npos);
}

// Шаблон обязан содержать каталоги, которые сборка не создаёт сама.
//
// Регрессия на измеренное: только что поднятый wineboot'ом префикс не имеет
// ни users/<кто>/Temp, ни windows/temp — Wine заводит %TEMP% при первом
// обращении. Префикс, которым уже пользовались, их имеет, поэтому шаблон,
// сделанный из такого, работал, а честно собранный с нуля — нет: cl отвечал
// «D8037: cannot create temporary il file» и больше ничего.
//
// Проверяется после instantiate, а не сразу после сборки шаблона: каталоги
// обязаны пережить переименование профиля, иначе толку от них никакого.
void test_template_has_the_directories_a_build_needs() {
    Sandbox box;
    const std::string home = (box.path() / "fakehome").string();
    const stdfs::path prefix = make_prefix(box, home, "builder");
    const stdfs::path tmpl = box.path() / "template";

    auto built = setup::build_prefix_template(prefix, tmpl);
    CHECK(built.has_value());

    std::error_code ec;
    CHECK(stdfs::is_directory(tmpl / "drive_c" / "windows" / "temp", ec));
    CHECK(stdfs::is_directory(tmpl / "drive_c" / "users" / "default" / "Temp", ec));

    CHECK(setup::instantiate_prefix(tmpl, "someone").has_value());
    CHECK(stdfs::is_directory(tmpl / "drive_c" / "users" / "someone" / "Temp", ec));
    CHECK(stdfs::is_directory(tmpl / "drive_c" / "windows" / "temp", ec));
}

} // namespace

int main() {
    test_template_has_the_directories_a_build_needs();
    test_clone_preserves_what_matters();
    test_clone_refuses_to_merge();
    test_session_key_follows_the_build_root();
    test_cmake_probes_share_the_parent_session();
    test_session_key_can_be_overridden();
    test_depersonalise_removes_every_trace();
    test_portability_gate_catches_what_matters();
    test_instantiate_restores_the_user_name();
    return cork::test::finish("test_session");
}
