#pragma once

// Распаковка CAB поверх libmspack — того же движка, что стоит за cabextract.
//
// Своя mspack_system нужна ровно ради одного: 35 архивов из 109 пакетов
// Windows SDK лежат не отдельными файлами, а потоками внутри самого .msi.
// Без подмены ввода-вывода их пришлось бы сначала выкладывать во временные
// файлы — то есть писать на диск гигабайты ради того, чтобы тут же их прочесть.
//
// Замер по 171 внешнему и 35 встроенным архивам (tools/msi_probe.py): только
// MSZIP (224 папки) и LZX (29), формат 1.3, ни одного архива с продолжением в
// соседнем. Quantum не встречается, многотомных наборов нет — то есть всё,
// что здесь нужно, libmspack умеет без оговорок.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/error.hpp"

namespace cork::archive {

struct CabEntry {
    std::string name;
    std::uint64_t size = 0;
};

class Cab {
public:
    Cab(const Cab &) = delete;
    Cab &operator=(const Cab &) = delete;
    Cab(Cab &&) noexcept;
    Cab &operator=(Cab &&) noexcept;
    ~Cab();

    [[nodiscard]] static Result<Cab> open(const std::filesystem::path &);

    // Архив, уже лежащий в памяти: встроенный поток .msi. Буфер обязан
    // пережить Cab — копии не делается намеренно, встроенные архивы бывают по
    // несколько мегабайт, а .msi уже целиком в памяти.
    [[nodiscard]] static Result<Cab> open_memory(std::string_view bytes, std::string label);

    [[nodiscard]] const std::vector<CabEntry> &entries() const { return entries_; }

    // Распаковывает одну запись по имени. Родительские каталоги создаются.
    [[nodiscard]] Result<void> extract(std::string_view name,
                                       const std::filesystem::path &dest) const;

private:
    Cab();

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<CabEntry> entries_;
};

} // namespace cork::archive
