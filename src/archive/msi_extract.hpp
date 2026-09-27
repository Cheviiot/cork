#pragma once

// Распаковка .msi: таблицы говорят, какой файл куда кладётся и в каком архиве
// лежит, cab.hpp достаёт содержимое.
//
// Соседние файлы (внешние архивы и файлы без сжатия) ищутся через переданный
// колбэк, а не по каталогу рядом с .msi. Причина конкретная: у нас пейлоады
// пакета лежат в хранилище по sha256, а не рядом друг с другом, и «каталог
// рядом» не существует. Заодно это делает распаковку тестируемой без
// раскладывания файлов на диск в ожидаемых местах.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include "base/error.hpp"

namespace cork::archive {

struct MsiExtractStats {
    std::uint64_t files = 0;
    std::uint64_t bytes = 0;
    std::uint64_t from_external_cab = 0;
    std::uint64_t from_embedded_cab = 0;
    std::uint64_t noncompressed = 0;
    std::uint64_t hashes_verified = 0;
};

// По имени соседнего файла (архива или самого файла, когда он не сжат) вернуть
// путь к нему. Отказ означает «такого пейлоада нет», и распаковка на этом
// останавливается: пропустить файл молча нельзя — так установка выходит
// неполной и при этом считается успешной.
using MsiSideFile = std::function<Result<std::filesystem::path>(std::string_view name)>;

struct MsiExtractOptions {
    MsiSideFile locate;

    // Вызывается на каждый путь до распаковки; false — пропустить. Пустая
    // функция означает «распаковать всё».
    std::function<bool(std::string_view)> accept;

    // Сверять MD5 из MsiFileHash. Стоит примерно ничего по сравнению с самой
    // распаковкой, поэтому включено по умолчанию.
    bool verify_hashes = true;
};

[[nodiscard]] Result<MsiExtractStats> extract_msi(const std::filesystem::path &msi_path,
                                                  const std::filesystem::path &dest,
                                                  const MsiExtractOptions &);

} // namespace cork::archive
