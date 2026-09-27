#pragma once

// Составной документ OLE (Compound File Binary Format) — контейнер, в котором
// лежит .msi. Внутри это маленькая файловая система: таблица распределения
// секторов (FAT), дерево каталога и отдельный «мини-поток» для всего мелкого.
//
// Читается целиком в память намеренно. Самый большой .msi в наборе Windows SDK
// — 68 МБ, а обращение к таблицам идёт вразнобой по всему файлу, так что
// потоковое чтение не дало бы ни экономии, ни скорости, зато добавило бы
// состояние и пути отказа там, где их сейчас нет.
//
// Свой читатель, а не `wine msiexec`, потому что иначе распаковка зависела бы
// от работоспособности Wine и префикса, давала бы код возврата вместо ответа
// «в каком файле и почему», и не тестировалась бы в CI без Wine. Формат
// оказался маленьким и предсказуемым: всё нужное — ниже.

#include <cstdint>
#include <string>
#include <vector>

#include "base/error.hpp"

namespace cork::archive {

class Cfbf {
public:
    enum class EntryType : std::uint8_t {
        Unused = 0,
        Storage = 1,
        Stream = 2,
        Root = 5,
    };

    struct Entry {
        // Имя в кодовых единицах UTF-16, без перевода: имена потоков MSI
        // закодированы символами из диапазона 0x3800..0x4840, и их смысл
        // теряется при переводе в UTF-8. Разбирает их msi::demangle.
        std::u16string name;
        EntryType type = EntryType::Unused;
        std::uint64_t size = 0;
        std::uint32_t start_sector = 0;
    };

    // data становится собственностью читателя: вся выдача — это ссылки внутрь
    // него, поэтому буфер обязан пережить Cfbf.
    [[nodiscard]] static Result<Cfbf> parse(std::string data);

    [[nodiscard]] const std::vector<Entry> &entries() const { return entries_; }

    // Содержимое потока. Возвращает копию: потоки не непрерывны в файле, они
    // собираются из секторов, поэтому вида «ссылка на кусок буфера» здесь не
    // существует.
    [[nodiscard]] Result<std::string> read(const Entry &) const;

private:
    Cfbf() = default;

    [[nodiscard]] Result<void> read_fat();
    [[nodiscard]] Result<void> read_directory();
    [[nodiscard]] Result<void> read_mini_stream();
    [[nodiscard]] Result<std::vector<std::uint32_t>> chain(std::uint32_t start,
                                                           const std::vector<std::uint32_t> &fat,
                                                           std::string_view what) const;
    [[nodiscard]] std::string_view sector(std::uint32_t index) const;

    std::string data_;
    std::uint32_t sector_size_ = 0;
    std::uint32_t mini_sector_size_ = 0;
    std::uint32_t mini_cutoff_ = 0;
    std::uint32_t fat_sector_count_ = 0;
    std::uint32_t dir_start_ = 0;
    std::uint32_t mini_fat_start_ = 0;
    std::uint32_t mini_fat_count_ = 0;
    std::uint32_t difat_start_ = 0;
    std::uint32_t difat_count_ = 0;

    std::vector<std::uint32_t> fat_;
    std::vector<std::uint32_t> mini_fat_;
    std::vector<Entry> entries_;
    std::string mini_stream_;
};

} // namespace cork::archive
