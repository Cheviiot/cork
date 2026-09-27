#pragma once

// Шимы: cmd и findstr, выполняемые нативно, без Wine.
//
// Зачем они вообще нужны. Сборочные файлы MSVC вызывают `cmd /c ...` для
// мелочей вроде копирования файла и `findstr` для фильтрации вывода, и путь
// к ним берётся из PATH. Если их там нет, сборка падает на шаге, который к
// компиляции отношения не имеет.
//
// Почему не отдать их настоящему cmd.exe из Wine. Соблазн велик и его надо
// подавить: каждый такой вызов — это ещё один процесс Wine со своим циклом
// wineserver, а команда внутри него получит unix-пути, которые никто не
// перевёл. Сборка утонет во вложенных Wine-процессах, причём на операциях
// уровня «скопируй файл».
//
// Поэтому шимы сознательно узкие. Они покрывают те формы вызова, которые
// действительно встречаются в файлах сборки MSVC, и отказываются от всего
// остального — с внятным сообщением, а не с попыткой угадать.

#include <string>
#include <vector>

namespace cork::exec {

// Выполняет шим. Возвращает код возврата процесса.
//
// name — имя, под которым нас позвали («cmd» или «findstr»), без .exe.
int run_shim(const std::string &name, const std::vector<std::string> &args);

// Разбор командной строки cmd /c. Вынесен отдельно ради теста: правила
// кавычек у cmd свои, и ошибка здесь тихо меняет смысл команды.
struct CmdInvocation {
    std::vector<std::string> argv;  // что запускать, уже разобранное
    bool ok = false;
    std::string error;
};

CmdInvocation parse_cmd_c(const std::vector<std::string> &args);

// Преобразование ключей findstr в поведение grep. Возвращает список
// аргументов для нативного поиска и образцы.
struct FindstrOptions {
    bool ignore_case = false;   // /I
    bool literal = false;       // /L и /C: — искать как есть, не как regex
    bool invert = false;        // /V
    bool line_numbers = false;  // /N
    bool names_only = false;    // /M — печатать только имена файлов
    std::vector<std::string> patterns;
    std::vector<std::string> files;
    bool ok = true;
    std::string error;
};

FindstrOptions parse_findstr(const std::vector<std::string> &args);

} // namespace cork::exec
