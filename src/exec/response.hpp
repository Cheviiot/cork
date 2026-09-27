#pragma once

// Response-файлы — аргументы вида «@путь».
//
// Ими пользуется всё, что генерирует длинные командные строки: CMake, MSBuild,
// ninja. Причина простая — командная строка Windows ограничена 32767
// символами, а список включений большого проекта её переполняет. Поэтому
// поддержка не опциональна: без неё сборка ломается ровно на тех проектах,
// ради которых всё это и делается.
//
// Кодировку MSVC определяет по метке порядка байтов, и UTF-16LE здесь не
// экзотика, а то, что пишет сам компилятор в /link-фазе и MSBuild. Файл без
// метки читается как есть, байт в байт: имена файлов в Linux — произвольные
// последовательности байтов, и «починить» их перекодировкой значит потерять.

#include <filesystem>
#include <string>
#include <vector>

#include "base/error.hpp"

namespace cork::exec {

// Разбирает содержимое response-файла на аргументы.
//
// Правила те же, что у CommandLineToArgvW внутри строки, с одним отличием:
// перевод строки здесь — такой же разделитель, как пробел. Первый аргумент
// НЕ считается именем программы, поэтому особого правила для него нет.
[[nodiscard]] std::vector<std::string> tokenize_response(std::string_view text);

// Читает файл и разбирает его. Метка порядка байтов распознаётся и снимается:
// UTF-16LE и UTF-16BE переводятся в UTF-8, UTF-8 остаётся как есть.
[[nodiscard]] Result<std::vector<std::string>> read_response_file(
    const std::filesystem::path &);

// Собирает аргументы обратно в текст response-файла, экранируя по тем же
// правилам. Нужно, чтобы записать файл с переведёнными путями.
[[nodiscard]] std::string render_response(const std::vector<std::string> &args);

// Ссылка на response-файл в списке аргументов.
struct ResponseArg {
    std::size_t index = 0;  // какой аргумент имеет вид "@..."
    std::filesystem::path path;
};

// Находит аргументы вида «@путь». Пустое «@» и «@@» не считаются: первое —
// не файл, второе MSVC трактует как обычный аргумент.
[[nodiscard]] std::vector<ResponseArg> find_response_args(
    const std::vector<std::string> &args);

} // namespace cork::exec
