#pragma once

// Таблица инструментов и разбор их аргументов.
//
// Что именно является путём, решается здесь, на нативной стороне, а перевод
// выполняет PE-хелпер. Угадывать это по форме аргумента — тупик: правило
// «начинается со слэша» совпадает с любым ключом MSVC, и спасает его только
// дополнительная проверка «существует ли родительский каталог», то есть
// корректность начинает зависеть от состояния файловой системы, а не от
// знания о том, что за ключ перед нами.
//
// Здесь основной механизм — таблица «инструмент → ключи, принимающие путь», а
// прежние регулярки остаются запасным вариантом для ключей вне таблицы. Так
// неизвестный ключ, принимающий путь, не перестаёт работать, а известный
// перестаёт зависеть от угадывания.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "proto/protocol.hpp"

namespace cork::exec {

// Где лежит настоящий .exe инструмента.
enum class ToolDir {
    MsvcBin,   // vc/tools/msvc/<ver>/bin/Host<host>/<arch>
    SdkBin,    // kits/10/bin/<sdkver>/<host>
    MsBuild,   // MSBuild/Current/Bin/<dotnet host>
};

struct ToolSpec {
    std::string_view name;
    std::string_view exe;
    ToolDir dir;
    // MSBuild печатает собственный вывод, который должен доехать до
    // пользователя нетронутым.
    bool raw_output = false;
};

// Все имена, на которые откликается бинарник, кроме собственных подкоманд.
const std::vector<ToolSpec> &tool_table();

// Ищет инструмент по имени вызова (регистр не важен, ".exe" отбрасывается).
const ToolSpec *find_tool(std::string_view invoked_as);

// Инструменты, которым Wine не нужен вовсе.
bool is_native_shim(std::string_view name);

// Имя шима без расширения и в нижнем регистре — то, что ждёт run_shim.
std::string shim_name(std::string_view invoked_as);

enum class PathMode {
    Table,   // таблица ключей, регулярки как запасной вариант
    Legacy,  // только прежние регулярки
    Off,     // не переводить ничего
};

PathMode path_mode_from_env();

// Возвращает ссылки на пути: какой аргумент и с какого места переводить.
//
// Ссылки, а не переписанные строки: перевод делает хелпер, у которого есть
// настоящая таблица дисков префикса. Разделение политики и механизма.
std::vector<proto::PathRef> path_argument_refs(std::string_view tool,
                                               const std::vector<std::string> &args,
                                               PathMode mode);

// Прежняя эвристика по форме аргумента, вынесенная отдельно: она нужна и как
// запасной вариант, и как предмет отдельного теста.
bool looks_like_path_argument(std::string_view arg);

} // namespace cork::exec
