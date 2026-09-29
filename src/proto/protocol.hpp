#pragma once

// Протокол обмена между нативной частью Cork и PE-хелпером.
//
// Почему файл, а не командная строка. Хелпер получает путь к request-файлу
// единственным аргументом, и всё остальное читает оттуда. Это снимает сразу
// две вещи: экранирование (кавычки, обратные слэши и пробелы в аргументах
// MSVC — обычное дело, и любая схема цитирования рано или поздно ломается) и
// предел в 32767 символов на командную строку Windows, в который упирались бы
// длинные списки включений.
//
// Почему байтовые строки, а не UTF-16. Эскиз предполагал UTF-16LE, но
// `wine_get_dos_file_name` принимает `char *`, то есть unix-путь в его
// «родном» виде. Если нести аргументы в UTF-16, пришлось бы конвертировать
// обратно перед каждым вызовом трансляции, а пути в Linux — это произвольные
// последовательности байтов, не обязанные быть корректным UTF-8. Поэтому по
// проводу идут байты как есть, а в UTF-16 хелпер переводит только то, что
// уходит в командную строку, и только через CP_UTF8.
//
// Почему нативная сторона решает, что является путём. Знание «какой ключ
// какого инструмента принимает путь» — это таблица, которая живёт и меняется
// на стороне CLI и покрыта обычными тестами. Хелпер же владеет механизмом
// трансляции, потому что только внутри PE доступен kernel32. Разделение
// политики и механизма: обе половины проверяются независимо.
//
// Файл компилируется дважды: обычным компилятором в составе cork_core и
// wineg++ в составе хелпера. Поэтому здесь нет ни fmt, ни std::expected, ни
// чего-либо из base/ — только стандартная библиотека уровня C++17.

#include <cstdint>
#include <string>
#include <vector>

namespace cork::proto {

inline constexpr std::uint32_t kRequestMagic = 0x52544e56u;  // "VNTR"
inline constexpr std::uint32_t kStatusMagic = 0x53544e56u;   // "VNTS"
inline constexpr std::uint32_t kVersion = 2;

enum RequestFlags : std::uint32_t {
    // Переводить аргументы, перечисленные в path_indices, из unix-путей в
    // DOS-пути средствами Wine.
    kTranslatePaths = 1u << 0,
    // Переписать response-файлы, перечисленные в response_files: перевести
    // пути внутри, записать новый файл и подменить аргумент "@..." на него.
    kTranslateResponseFiles = 1u << 1,
    // Не создавать Job Object. Аварийная лазейка на случай инструмента,
    // которому job мешает; обычный путь — с job.
    kNoJob = 1u << 2,
};

// Ссылка на путь внутри аргумента: индекс самого аргумента и смещение, с
// которого начинается путь. Смещение обязательно, потому что переводить надо
// только путь, а ключ перед ним обязан уцелеть: из «/I/usr/include» получается
// «/IZ:\\usr\\include», а не перевод всей строки целиком.
struct PathRef {
    std::uint32_t index = 0;
    std::uint32_t offset = 0;
};

// Response-файл, который надо переписать с переведёнными путями.
//
// Разбирает и токенизирует его нативная сторона: там живёт таблица «какой ключ
// какого инструмента принимает путь», и дублировать её в PE-половине значило бы
// завести два источника истины. Хелперу достаётся механизм — перевести и
// записать, — потому что wine_get_dos_file_name доступен только изнутри PE.
struct ResponseFile {
    // Какой аргумент имеет вид "@..." и должен быть заменён.
    std::uint32_t arg_index = 0;
    // Содержимое файла, разобранное на аргументы. Пути в них — unix-пути.
    std::vector<std::string> args;
    // Индексы внутри args и смещения, с которых начинается путь.
    std::vector<PathRef> path_refs;
};

struct Request {
    std::uint32_t flags = kTranslatePaths;

    // Пути ниже — unix-пути в том виде, в каком их знает нативная сторона.
    // Хелпер переводит их сам, безусловно: ему нужны DOS-пути, чтобы вообще
    // что-либо открыть.
    std::string exe;          // настоящий .exe инструмента
    std::string cwd;          // "" — наследовать рабочий каталог
    std::string status_path;  // куда хелпер обязан записать Status

    std::vector<std::string> args;   // без argv[0]
    std::vector<PathRef> path_refs;  // что и с какого места переводить

    // Переменные окружения, которые выставляются ТОЛЬКО потомку, а не самому
    // хелперу. Нужно из-за WINEDLLOVERRIDES: «vcruntime140=n» означает «только
    // native», и хелпер, собранный wineg++ против встроенного рантайма Wine,
    // с такой переменной просто не загружается. Прежней реализации это не
    // мешало — её relay компилировался MSVC и native ему был нужен взаправду.
    std::vector<std::string> child_env;  // KEY=VALUE

    // Пустой список означает, что переписывать нечего; сам флаг
    // kTranslateResponseFiles при этом может стоять.
    std::vector<ResponseFile> response_files;
};

enum class StatusKind : std::uint32_t {
    // Ребёнок запустился и завершился; code — его полный 32-битный код.
    ChildExited = 1,
    // CreateProcessW не смог запустить ребёнка; code — GetLastError().
    SpawnFailed = 2,
    // Запрос не разобран; code — номер причины (см. BadRequestReason).
    BadRequest = 3,
};

enum BadRequestReason : std::uint32_t {
    kReasonUnreadable = 1,
    kReasonBadMagic = 2,
    kReasonBadVersion = 3,
    kReasonTruncated = 4,
    kReasonBadIndex = 5,
    kReasonResponseWriteFailed = 6,
};

enum StatusFlags : std::uint32_t {
    kJobCreated = 1u << 0,
};

struct Status {
    StatusKind kind = StatusKind::BadRequest;
    std::uint32_t code = 0;
    std::uint32_t child_pid = 0;
    std::uint32_t flags = 0;
};

// Кодирование всегда успешно: структура уже валидна по построению.
std::vector<unsigned char> encode(const Request &);
std::vector<unsigned char> encode(const Status &);

// Разбор возвращает причину отказа, а не бросает: PE-сторона живёт без
// исключений, а нативной нужен номер, чтобы отличить «мусор в файле» от
// «ребёнок упал». Ноль означает успех.
std::uint32_t decode(const unsigned char *data, std::size_t size, Request &out);
std::uint32_t decode(const unsigned char *data, std::size_t size, Status &out);

} // namespace cork::proto
