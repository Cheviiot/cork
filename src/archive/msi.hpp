#pragma once

// Таблицы установщика Windows поверх составного документа.
//
// Из двадцати с лишним таблиц для распаковки нужны пять: Directory, Component,
// File, Media и (для проверки) MsiFileHash. Остальное — последовательности
// действий, диалоги и условия установки, то есть сценарий установки, который
// нас не касается: мы не устанавливаем пакет, а достаём из него файлы.
//
// Замер по 109 пакетам Windows SDK из локального кэша (tools/msi_probe.py):
// 11035 файлов, 8486 записей MsiFileHash, 179 внешних архивов и 35 встроенных
// потоков-архивов, 17 файлов без сжатия, ни одного файла, разрезанного между
// двумя архивами. Это и определило объём читателя.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "archive/cfbf.hpp"
#include "base/error.hpp"

namespace cork::archive {

// Биты File.Attributes, которые что-то значат для распаковки.
inline constexpr std::uint32_t kMsiFileNoncompressed = 0x2000;

struct MsiColumn {
    std::string name;
    std::uint16_t type = 0;
};

// Значение ячейки: либо строка из пула, либо число. Тип известен из схемы,
// поэтому variant не нужен — нужное поле выбирает читатель таблицы.
struct MsiCell {
    std::string text;
    std::int32_t number = 0;
};

using MsiRow = std::vector<MsiCell>;

struct MsiFile {
    std::string key;       // File — первичный ключ, он же имя внутри архива
    std::string path;      // путь относительно корня установки, разделитель '/'
    std::uint64_t size = 0;
    std::uint32_t attributes = 0;
    std::uint32_t sequence = 0;

    // Имя архива из Media. Пустое — файл лежит рядом с .msi отдельным файлом;
    // с ведущим '#' — архив спрятан потоком внутри самого .msi.
    std::string cabinet;

    // MD5 из MsiFileHash, если он есть. Бесплатная проверка целостности:
    // пакет сам носит контрольные суммы своего содержимого.
    std::optional<std::array<std::uint8_t, 16>> md5;
};

class Msi {
public:
    [[nodiscard]] static Result<Msi> parse(std::string data);

    // Имена таблиц, объявленных в схеме пакета.
    [[nodiscard]] std::vector<std::string> table_names() const;

    [[nodiscard]] bool has_table(const std::string &name) const;

    // Разобранная таблица. Отсутствие таблицы — отказ, а не пустой список:
    // «таблицы нет» и «таблица пуста» означают разное, и для обязательных
    // таблиц первое — испорченный пакет.
    [[nodiscard]] Result<std::vector<MsiRow>> table(const std::string &name) const;
    [[nodiscard]] const std::vector<MsiColumn> *columns(const std::string &name) const;

    // Всё, что нужно для распаковки, собранное из пяти таблиц.
    [[nodiscard]] Result<std::vector<MsiFile>> files() const;

    // Содержимое потока по имени уже размангленному (например "Binary.Foo" или
    // имя встроенного архива без ведущего '#').
    [[nodiscard]] Result<std::string> stream(const std::string &name) const;

private:
    Msi() = default;

    [[nodiscard]] Result<void> read_string_pool();
    [[nodiscard]] Result<void> read_schema();
    [[nodiscard]] Result<std::vector<MsiRow>> decode(const std::string &stream_name,
                                                     const std::vector<MsiColumn> &cols) const;
    [[nodiscard]] const std::string &string(std::uint32_t index) const;

    std::optional<Cfbf> cfbf_;
    std::unordered_map<std::string, const Cfbf::Entry *> streams_;
    std::vector<std::string> strings_;
    std::unordered_map<std::string, std::vector<MsiColumn>> schema_;
    bool long_string_refs_ = false;
};

// Обратный перевод имени потока MSI в обычное имя. Второе значение — признак
// «это таблица», который несёт префиксный символ 0x4840.
struct Demangled {
    std::string name;
    bool is_table = false;
};
[[nodiscard]] Demangled demangle_stream_name(std::u16string_view);

} // namespace cork::archive
