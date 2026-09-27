#include "archive/cfbf.hpp"

#include <cstring>
#include <limits>

#include <fmt/format.h>

namespace cork::archive {
namespace {

constexpr unsigned char kMagic[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};

constexpr std::uint32_t kMaxRegSect = 0xFFFFFFFAu;
constexpr std::uint32_t kDifSect = 0xFFFFFFFCu;
constexpr std::uint32_t kFatSect = 0xFFFFFFFDu;
constexpr std::uint32_t kEndOfChain = 0xFFFFFFFEu;
constexpr std::uint32_t kFreeSect = 0xFFFFFFFFu;

constexpr std::size_t kDirEntrySize = 128;
constexpr std::size_t kHeaderDifatCount = 109;

std::uint16_t le16(const char *p) {
    auto u = reinterpret_cast<const unsigned char *>(p);
    return static_cast<std::uint16_t>(u[0] | (u[1] << 8));
}

std::uint32_t le32(const char *p) {
    auto u = reinterpret_cast<const unsigned char *>(p);
    return static_cast<std::uint32_t>(u[0]) | (static_cast<std::uint32_t>(u[1]) << 8) |
           (static_cast<std::uint32_t>(u[2]) << 16) | (static_cast<std::uint32_t>(u[3]) << 24);
}

std::uint64_t le64(const char *p) {
    return static_cast<std::uint64_t>(le32(p)) | (static_cast<std::uint64_t>(le32(p + 4)) << 32);
}

} // namespace

std::string_view Cfbf::sector(std::uint32_t index) const {
    // Заголовок занимает ровно один сектор, а не всегда 512 байт: в версии 4
    // сектор равен 4096, и сектор N начинается с (N+1)*размер. Формула
    // «512 + N*размер» верна только для версии 3, а на версии 4 выдаёт мусор,
    // который выглядит как зацикленная цепочка — то есть ошибка проявляется
    // не там, где сделана.
    std::size_t start = static_cast<std::size_t>(index + 1) * sector_size_;
    if (start + sector_size_ > data_.size()) {
        return {};
    }
    return std::string_view(data_).substr(start, sector_size_);
}

Result<std::vector<std::uint32_t>> Cfbf::chain(std::uint32_t start,
                                               const std::vector<std::uint32_t> &fat,
                                               std::string_view what) const {
    std::vector<std::uint32_t> out;
    std::uint32_t current = start;
    // Цепочка не может быть длиннее таблицы: каждый сектор встречается в ней
    // ровно один раз. Превышение означает петлю, а не большой файл, и ловить
    // её надо здесь — иначе разбор просто не завершится.
    while (current <= kMaxRegSect) {
        if (current >= fat.size()) {
            return err_format(fmt::format("{}: sector {} is outside the allocation table", what,
                                          current));
        }
        out.push_back(current);
        if (out.size() > fat.size()) {
            return err_format(fmt::format("{}: sector chain loops", what));
        }
        current = fat[current];
    }
    if (current != kEndOfChain && current != kFreeSect) {
        return err_format(fmt::format("{}: sector chain ends with {:#x}", what, current));
    }
    return out;
}

Result<Cfbf> Cfbf::parse(std::string data) {
    Cfbf c;
    c.data_ = std::move(data);
    const std::string &d = c.data_;

    if (d.size() < 512 || std::memcmp(d.data(), kMagic, sizeof kMagic) != 0) {
        return err_format("not a compound file (bad signature)");
    }

    std::uint16_t sector_shift = le16(d.data() + 0x1E);
    std::uint16_t mini_sector_shift = le16(d.data() + 0x20);
    // Разрешены ровно два размера сектора: 512 (версия 3) и 4096 (версия 4).
    // Любой другой сдвиг — либо испорченный файл, либо сдвиг настолько
    // большой, что 1<<shift переполнится.
    if (sector_shift != 9 && sector_shift != 12) {
        return err_format(fmt::format("unsupported sector shift {}", sector_shift));
    }
    if (mini_sector_shift != 6) {
        return err_format(fmt::format("unsupported mini sector shift {}", mini_sector_shift));
    }
    c.sector_size_ = 1u << sector_shift;
    c.mini_sector_size_ = 1u << mini_sector_shift;

    c.fat_sector_count_ = le32(d.data() + 0x2C);
    c.dir_start_ = le32(d.data() + 0x30);
    c.mini_cutoff_ = le32(d.data() + 0x38);
    c.mini_fat_start_ = le32(d.data() + 0x3C);
    c.mini_fat_count_ = le32(d.data() + 0x40);
    c.difat_start_ = le32(d.data() + 0x44);
    c.difat_count_ = le32(d.data() + 0x48);

    if (auto r = c.read_fat(); !r) {
        return std::unexpected(std::move(r).error());
    }
    if (auto r = c.read_directory(); !r) {
        return std::unexpected(std::move(r).error());
    }
    if (auto r = c.read_mini_stream(); !r) {
        return std::unexpected(std::move(r).error());
    }
    return c;
}

Result<void> Cfbf::read_fat() {
    // DIFAT — список номеров секторов, в которых лежит сама FAT. Первые 109
    // записей в заголовке, остальные цепочкой; в наших файлах хватает
    // заголовка, но пропустить продолжение нельзя: 109 секторов FAT
    // адресуют около 55 МБ при секторе 512 байт, а MSI такого размера есть.
    std::vector<std::uint32_t> difat;
    difat.reserve(kHeaderDifatCount + difat_count_);
    for (std::size_t i = 0; i < kHeaderDifatCount; ++i) {
        difat.push_back(le32(data_.data() + 0x4C + i * 4));
    }

    std::uint32_t sect = difat_start_;
    std::uint32_t guard = 0;
    const std::uint32_t per_sector = sector_size_ / 4;
    while (sect <= kMaxRegSect) {
        if (++guard > difat_count_ + 1) {
            return err_format("DIFAT chain is longer than the header declares");
        }
        std::string_view s = sector(sect);
        if (s.size() != sector_size_) {
            return err_format(fmt::format("DIFAT sector {} is past the end of file", sect));
        }
        for (std::uint32_t i = 0; i + 1 < per_sector; ++i) {
            difat.push_back(le32(s.data() + i * 4));
        }
        sect = le32(s.data() + (per_sector - 1) * 4);
    }

    fat_.clear();
    fat_.reserve(static_cast<std::size_t>(fat_sector_count_) * per_sector);
    std::uint32_t used = 0;
    for (std::uint32_t entry : difat) {
        if (entry > kMaxRegSect) {
            continue;
        }
        if (used++ >= fat_sector_count_) {
            break;
        }
        std::string_view s = sector(entry);
        if (s.size() != sector_size_) {
            return err_format(fmt::format("FAT sector {} is past the end of file", entry));
        }
        for (std::uint32_t i = 0; i < per_sector; ++i) {
            fat_.push_back(le32(s.data() + i * 4));
        }
    }
    if (used != fat_sector_count_) {
        return err_format(fmt::format("expected {} FAT sectors, found {}", fat_sector_count_, used));
    }

    if (mini_fat_start_ <= kMaxRegSect) {
        auto sectors = chain(mini_fat_start_, fat_, "mini FAT");
        if (!sectors) {
            return std::unexpected(std::move(sectors).error());
        }
        if (sectors->size() != mini_fat_count_) {
            return err_format(fmt::format("expected {} mini FAT sectors, chain has {}",
                                          mini_fat_count_, sectors->size()));
        }
        mini_fat_.reserve(sectors->size() * per_sector);
        for (std::uint32_t s : *sectors) {
            std::string_view v = sector(s);
            for (std::uint32_t i = 0; i < per_sector; ++i) {
                mini_fat_.push_back(le32(v.data() + i * 4));
            }
        }
    }
    return {};
}

Result<void> Cfbf::read_directory() {
    auto sectors = chain(dir_start_, fat_, "directory");
    if (!sectors) {
        return std::unexpected(std::move(sectors).error());
    }
    const std::size_t per_sector = sector_size_ / kDirEntrySize;
    for (std::uint32_t s : *sectors) {
        std::string_view v = sector(s);
        for (std::size_t i = 0; i < per_sector; ++i) {
            const char *p = v.data() + i * kDirEntrySize;
            std::uint16_t name_bytes = le16(p + 0x40);
            auto type = static_cast<EntryType>(static_cast<unsigned char>(p[0x42]));
            if (type == EntryType::Unused) {
                continue;
            }
            // Длина в байтах, включая завершающий нуль, и не больше 64.
            // Нечётная или завышенная длина — испорченная запись, а не повод
            // прочитать за границу сектора.
            if (name_bytes < 2 || name_bytes > 64 || name_bytes % 2 != 0) {
                return err_format(fmt::format("directory entry has name length {}", name_bytes));
            }
            Entry e;
            e.type = type;
            e.name.reserve(name_bytes / 2 - 1);
            for (std::size_t k = 0; k + 1 < static_cast<std::size_t>(name_bytes) / 2; ++k) {
                e.name.push_back(static_cast<char16_t>(le16(p + k * 2)));
            }
            e.start_sector = le32(p + 0x74);
            e.size = le64(p + 0x78);
            entries_.push_back(std::move(e));
        }
    }
    if (entries_.empty() || entries_.front().type != EntryType::Root) {
        return err_format("compound file has no root entry");
    }
    return {};
}

Result<void> Cfbf::read_mini_stream() {
    const Entry &root = entries_.front();
    if (root.size == 0) {
        return {};
    }
    auto sectors = chain(root.start_sector, fat_, "mini stream");
    if (!sectors) {
        return std::unexpected(std::move(sectors).error());
    }
    mini_stream_.reserve(sectors->size() * sector_size_);
    for (std::uint32_t s : *sectors) {
        std::string_view v = sector(s);
        if (v.size() != sector_size_) {
            return err_format(fmt::format("mini stream sector {} is past the end of file", s));
        }
        mini_stream_.append(v);
    }
    return {};
}

Result<std::string> Cfbf::read(const Entry &e) const {
    if (e.size == 0) {
        return std::string{};
    }
    if (e.size > data_.size()) {
        return err_format(fmt::format("stream claims {} bytes, file is {}", e.size, data_.size()));
    }
    const auto size = static_cast<std::size_t>(e.size);

    // Мелкие потоки живут не в обычных секторах, а в «мини-потоке» —
    // отдельном непрерывном буфере, нарезанном на куски по 64 байта. Граница
    // задана заголовком, а не константой: у версии 4 она тоже 4096, но это
    // совпадение, а не правило.
    const bool mini = e.size < mini_cutoff_ && e.type != EntryType::Root;
    const std::vector<std::uint32_t> &fat = mini ? mini_fat_ : fat_;
    const std::uint32_t unit = mini ? mini_sector_size_ : sector_size_;

    auto sectors = chain(e.start_sector, fat, mini ? "mini stream chain" : "stream");
    if (!sectors) {
        return std::unexpected(std::move(sectors).error());
    }

    std::string out;
    out.reserve(size);
    for (std::uint32_t s : *sectors) {
        if (out.size() >= size) {
            break;
        }
        std::string_view piece;
        if (mini) {
            std::size_t off = static_cast<std::size_t>(s) * unit;
            if (off + unit > mini_stream_.size()) {
                return err_format(fmt::format("mini sector {} is past the end of the mini stream", s));
            }
            piece = std::string_view(mini_stream_).substr(off, unit);
        } else {
            piece = sector(s);
            if (piece.size() != unit) {
                return err_format(fmt::format("sector {} is past the end of file", s));
            }
        }
        out.append(piece.substr(0, std::min<std::size_t>(unit, size - out.size())));
    }
    if (out.size() != size) {
        return err_format(fmt::format("stream is {} bytes, chain yields {}", size, out.size()));
    }
    return out;
}

} // namespace cork::archive
