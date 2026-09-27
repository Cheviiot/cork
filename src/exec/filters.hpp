#pragma once

// Обратное преобразование вывода инструментов: из DOS-путей в unix-пути.
//
// Нужно независимо от того, переводим мы аргументы или нет. Даже если бы
// компилятор получал только относительные пути, он сам разрешил бы их
// относительно текущего каталога, который под Wine лежит на диске Z:, и
// напечатал бы «Z:\home\...» и в «Note: including file:», и в диагностике.
// Разбор ошибок в IDE и в CI на таких путях ломается.

#include <string>
#include <string_view>

namespace cork::exec {

// Буква диска, под которой Wine видит корень файловой системы. Обычно Z:, но
// берётся из окружения сеанса, а не зашивается: префикс может быть настроен
// иначе, и тогда фильтр по константе просто перестал бы срабатывать.
struct FilterConfig {
    char root_drive = 'z';
};

// Убирает первое вхождение «<буква>:» перед разделителем пути, сохраняя сам
// разделитель. Именно первое: так вела себя исходная замена, и строки вида
// «cl : command line warning» не должны пострадать.
std::string strip_root_drive(std::string_view line, const FilterConfig & = {});

// Фильтр stdout компилятора: «Note: including file:», директивы #line и
// строки диагностики с путём в начале.
std::string cl_stdout_filter(std::string_view line, const FilterConfig & = {});

// Диагностика cl в stderr не несёт префикса диска, кроме строк включения.
std::string cl_stderr_filter(std::string_view line, const FilterConfig & = {});

// dumpbin печатает путь в «Dump of file» и «PDB file found at».
std::string dumpbin_stdout_filter(std::string_view line, const FilterConfig & = {});

// Возвращает фильтр по имени инструмента; nullptr — вывод не трогать.
using LineFilter = std::string (*)(std::string_view, const FilterConfig &);
LineFilter filter_for(std::string_view tool, bool stderr_stream);

// Убирает возврат каретки, который Wine-хостируемые программы ставят перед
// переводом строки.
std::string strip_cr(std::string_view line);

} // namespace cork::exec
