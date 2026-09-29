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

// Диагностика самого Wine, а не программы: «0220:err:winediag:...»,
// «0220:fixme:dbghelp:...», «wine: Unhandled page fault...».
//
// Отличать её от вывода программы нужно затем, что при падении собранной
// программы Wine печатает поверх полезного отчёта две строки про
// отсутствующий графический драйвер (он пытается открыть окно отладчика) и
// несколько fixme из dbghelp. Человек, у которого упала его программа,
// читает первым делом их, а они не про него.
//
// Признак — префикс «<hex>:<уровень>:» в начале строки: его ставит сам Wine,
// и подделать его выводом программы можно только нарочно.
[[nodiscard]] bool is_wine_diagnostic(std::string_view line);

// Отчёт winedbg о падении: строки трассировки вида
// «=>0 0x... inner+0xa() [Z:\home\...\boom.c:2] in boom (...)».
//
// Фильтруется отдельно от вывода программы и по узкому признаку — скобке с
// путём вида «[<буква>:\». Вывод чужой программы трогать нельзя: она может
// печатать что угодно, и формат её сообщений нам неизвестен. А вот отчёт о
// падении печатает Wine, и путь в нём обязан быть тем, по которому редактор
// откроет файл: иначе символы в трассировке есть, а перейти к строке
// нельзя.
std::string crash_report_filter(std::string_view line, const FilterConfig & = {});

// Возвращает фильтр по имени инструмента; nullptr — вывод не трогать.
using LineFilter = std::string (*)(std::string_view, const FilterConfig &);
LineFilter filter_for(std::string_view tool, bool stderr_stream);

// Убирает возврат каретки, который Wine-хостируемые программы ставят перед
// переводом строки.
std::string strip_cr(std::string_view line);

} // namespace cork::exec
