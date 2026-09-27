#pragma once

// Распаковка zip — VSIX и nupkg приезжают именно так.
//
// Здесь исправляется дефект номер один, единственный подтверждённый на живой
// установке: прежний распаковщик записывал символьную ссылку как обычный файл
// с путём внутри. В дереве Wine таких записей 172, и среди них весь
// lib/mono/4.5, то есть тот Wine Mono, без которого не стартует MSBuild.exe.
// При этом doctor рапортовал «All checks passed».
//
// Создание ссылки намеренно оставлено нашим кодом, а не отдано библиотеке:
// это то самое место, где нужен собственный тест, и решение «можно ли
// создавать эту ссылку» должно быть видно, а не спрятано внутри чужого
// archive_write_disk.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "base/error.hpp"

namespace cork::archive {

struct ZipStats {
    std::uint64_t files = 0;
    std::uint64_t directories = 0;
    std::uint64_t symlinks = 0;
    std::uint64_t bytes = 0;
};

struct ZipOptions {
    // Распаковать только записи с этим префиксом, сняв его с пути. Нужно для
    // nupkg, где нас интересует только поддерево "c/".
    std::string strip_prefix;

    // Имена записей в некоторых VSIX процентно закодированы.
    bool url_decode_names = true;

    // Вызывается на каждое имя записи; позволяет пропустить ненужное, не
    // распаковывая. Пустая функция — распаковывать всё.
    std::function<bool(std::string_view)> accept;
};

// Распаковывает archive_path в dest. Любая запись, уводящая за пределы dest —
// как именем, так и целью символьной ссылки, — это отказ, а не пропуск:
// архив, который так делает, испорчен или враждебен, и продолжать с ним
// нечего.
Result<ZipStats> extract_zip(const std::filesystem::path &archive_path,
                             const std::filesystem::path &dest, const ZipOptions & = {});

} // namespace cork::archive
