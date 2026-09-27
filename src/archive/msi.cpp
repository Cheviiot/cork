#include "archive/msi.hpp"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

namespace cork::archive {
namespace {

// Порядок алфавита ровно такой: цифры, прописные, строчные, точка,
// подчёркивание. Любая другая перестановка даёт правдоподобные имена из тех же
// букв — "Property" превращается в "z1YZO138" — и ошибка выглядит как
// повреждённый файл, а не как опечатка в таблице.
constexpr std::string_view kAlphabet =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._";

constexpr char16_t kTableMarker = 0x4840;
constexpr char16_t kPairBase = 0x3800;
constexpr char16_t kSingleBase = 0x4800;

// Биты Column.Type.
constexpr std::uint16_t kColString = 0x0800;

std::uint32_t le_bytes(const char *p, std::size_t n) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < n; ++i) {
        v |= static_cast<std::uint32_t>(static_cast<unsigned char>(p[i])) << (8 * i);
    }
    return v;
}

std::uint16_t le16(const char *p) { return static_cast<std::uint16_t>(le_bytes(p, 2)); }

// Форма DefaultDir — "[коротк.|длинн.][:исходн.]". Нужно длинное целевое имя:
// короткое существует ради 8.3, а исходная часть описывает раскладку носителя
// установки, а не устанавливаемого дерева.
std::string_view target_name(std::string_view default_dir) {
    if (auto colon = default_dir.find(':'); colon != std::string_view::npos) {
        default_dir = default_dir.substr(0, colon);
    }
    if (auto bar = default_dir.find('|'); bar != std::string_view::npos) {
        default_dir = default_dir.substr(bar + 1);
    }
    return default_dir;
}

} // namespace

Demangled demangle_stream_name(std::u16string_view name) {
    Demangled out;
    out.name.reserve(name.size() * 2);
    for (char16_t ch : name) {
        if (ch == kTableMarker) {
            // Префиксный маркер «дальше имя таблицы». Символа он не кодирует, и
            // оставленный в строке не даёт имени совпасть ни с чем.
            out.is_table = true;
            continue;
        }
        if (ch >= kPairBase && ch < kSingleBase) {
            unsigned v = static_cast<unsigned>(ch - kPairBase);
            // Обе половины значащие всегда: нулевой индекс — это цифра '0',
            // законная в именах вроде "Win10". Пропуск второй половины при
            // нулевом старшем шестибитнике молча её теряет.
            out.name.push_back(kAlphabet[v & 0x3F]);
            out.name.push_back(kAlphabet[(v >> 6) & 0x3F]);
        } else if (ch >= kSingleBase && ch < kTableMarker) {
            out.name.push_back(kAlphabet[static_cast<unsigned>(ch - kSingleBase)]);
        } else if (ch < 0x80) {
            out.name.push_back(static_cast<char>(ch));
        } else {
            // Имя вне обеих схем и не ASCII. В пакетах такого не встречается,
            // но молча его терять нельзя: пусть будет видно в диагностике.
            out.name += fmt::format("\\u{:04x}", static_cast<unsigned>(ch));
        }
    }
    return out;
}

Result<Msi> Msi::parse(std::string data) {
    auto cfbf = Cfbf::parse(std::move(data));
    if (!cfbf) {
        return std::unexpected(std::move(cfbf).error());
    }
    Msi m;
    m.cfbf_ = std::move(*cfbf);
    for (const auto &e : m.cfbf_->entries()) {
        if (e.type != Cfbf::EntryType::Stream) {
            continue;
        }
        m.streams_.emplace(demangle_stream_name(e.name).name, &e);
    }
    if (auto r = m.read_string_pool(); !r) {
        return std::unexpected(std::move(r).error());
    }
    if (auto r = m.read_schema(); !r) {
        return std::unexpected(std::move(r).error());
    }
    return m;
}

Result<std::string> Msi::stream(const std::string &name) const {
    auto it = streams_.find(name);
    if (it == streams_.end()) {
        return err_not_found(fmt::format("stream '{}' is not in the package", name));
    }
    return cfbf_->read(*it->second);
}

Result<void> Msi::read_string_pool() {
    auto info = stream("_StringPool");
    if (!info) {
        return std::unexpected(std::move(info).error().at("reading the string pool"));
    }
    auto data = stream("_StringData");
    if (!data) {
        return std::unexpected(std::move(data).error().at("reading the string pool"));
    }
    if (info->size() < 4) {
        return err_format("string pool is shorter than its header");
    }

    // Ловушка, на которой легко ошибиться и не заметить: в паре идёт СНАЧАЛА
    // длина, ПОТОМ счётчик ссылок. Перепутав их, получаешь полностью мусорный
    // индекс, который при этом выглядит правдоподобно — строки читаются, но
    // не те.
    //
    // Нулевая ячейка — не строка: в поле длины у неё лежит кодовая страница
    // (65001 у всех пакетов SDK), а старший бит поля ссылок означает, что
    // ссылки на строки трёхбайтовые. Прочитать её как обычную пару значило бы
    // отъесть от _StringData столько байт, каков номер кодовой страницы.
    std::uint16_t codepage_refs = le16(info->data() + 2);
    long_string_refs_ = (codepage_refs & 0x8000) != 0;

    // Строки нумеруются с единицы: нулевая ячейка занята кодовой страницей, а
    // нулевая ссылка означает «значения нет».
    strings_.clear();
    strings_.emplace_back();

    std::size_t offset = 0;
    for (std::size_t i = 1; i * 4 + 4 <= info->size(); ++i) {
        std::uint32_t length = le16(info->data() + i * 4);
        std::uint16_t refs = le16(info->data() + i * 4 + 2);
        if (length == 0 && refs != 0) {
            // Строка длиннее 65535 байт занимает две ячейки. Первая — только
            // маркер: длина ноль, а в поле ссылок настоящий счётчик ссылок.
            // Длина целиком лежит во второй: младшие шестнадцать бит в поле
            // длины, старшие — в поле ссылок.
            //
            // Соблазн взять старшие биты из поля ссылок ПЕРВОЙ ячейки велик, и
            // ошибка эта тихая: на 107 пакетах из 109 длинных строк просто нет,
            // а на двух оставшихся весь пул уезжает, и таблицы наполняются
            // обрывками чужих строк вместо путей.
            if ((i + 1) * 4 + 4 > info->size()) {
                return err_format("string pool ends inside a long string header");
            }
            ++i;
            length = (static_cast<std::uint32_t>(le16(info->data() + i * 4 + 2)) << 16) |
                     le16(info->data() + i * 4);
        }
        if (offset + length > data->size()) {
            return err_format(fmt::format("string {} runs past the end of the string data",
                                          strings_.size()));
        }
        strings_.emplace_back(data->substr(offset, length));
        offset += length;
    }
    return {};
}

const std::string &Msi::string(std::uint32_t index) const {
    static const std::string empty;
    return index < strings_.size() ? strings_[index] : empty;
}

Result<void> Msi::read_schema() {
    // Схема самой _Columns захардкожена — иначе её неоткуда взять. Четыре
    // колонки: имя таблицы, номер колонки, имя колонки, тип. Первая и третья
    // строковые, вторая и четвёртая — двухбайтовые числа.
    const std::vector<MsiColumn> meta = {
        {"Table", static_cast<std::uint16_t>(0x9D00 | kColString)},
        {"Number", 0x9502},
        {"Name", static_cast<std::uint16_t>(0x9D00 | kColString)},
        {"Type", 0x9502},
    };
    auto rows = decode("_Columns", meta);
    if (!rows) {
        return std::unexpected(std::move(rows).error().at("reading the table schema"));
    }

    std::unordered_map<std::string, std::vector<std::pair<std::int32_t, MsiColumn>>> collected;
    for (const auto &row : *rows) {
        if (row.size() != 4) {
            return err_format("_Columns row does not have four cells");
        }
        MsiColumn col;
        col.name = row[2].text;
        col.type = static_cast<std::uint16_t>(row[3].number);
        collected[row[0].text].emplace_back(row[1].number, std::move(col));
    }
    for (auto &[table, cols] : collected) {
        // Номер колонки задаёт порядок хранения, а строки _Columns приходят в
        // своём. Без сортировки колонки читались бы не теми ширинами.
        std::sort(cols.begin(), cols.end(),
                  [](const auto &a, const auto &b) { return a.first < b.first; });
        std::vector<MsiColumn> ordered;
        ordered.reserve(cols.size());
        for (auto &[number, col] : cols) {
            ordered.push_back(std::move(col));
        }
        schema_.emplace(table, std::move(ordered));
    }
    return {};
}

Result<std::vector<MsiRow>> Msi::decode(const std::string &stream_name,
                                        const std::vector<MsiColumn> &cols) const {
    auto it = streams_.find(stream_name);
    if (it == streams_.end()) {
        // Пустая таблица не получает потока вовсе — это законно и означает
        // «строк нет», а не «таблицы нет». Отличает их схема, а не наличие
        // потока, поэтому здесь пустой список, а не отказ.
        return std::vector<MsiRow>{};
    }
    auto raw = cfbf_->read(*it->second);
    if (!raw) {
        return std::unexpected(std::move(raw).error().at(fmt::format("reading table '{}'", stream_name)));
    }
    if (raw->empty()) {
        return std::vector<MsiRow>{};
    }

    const std::size_t ref_size = long_string_refs_ ? 3 : 2;
    std::vector<std::size_t> widths;
    widths.reserve(cols.size());
    std::size_t row_size = 0;
    for (const auto &c : cols) {
        // Строковая колонка хранит ссылку в пул; ширина ссылки на весь пакет
        // одна и объявлена в заголовке пула. Числовая хранит значение, и её
        // размер — младший байт типа.
        std::size_t w = (c.type & kColString) ? ref_size : ((c.type & 0xFF) == 2 ? 2 : 4);
        widths.push_back(w);
        row_size += w;
    }
    if (row_size == 0) {
        return err_format(fmt::format("table '{}' has zero-width rows", stream_name));
    }
    const std::size_t rows = raw->size() / row_size;

    std::vector<MsiRow> out(rows, MsiRow(cols.size()));
    std::size_t pos = 0;
    // Данные хранятся ПО СТОЛБЦАМ: сначала все значения первой колонки, затем
    // все значения второй. Чтение построчно даёт мусор, который тоже выглядит
    // правдоподобно.
    for (std::size_t c = 0; c < cols.size(); ++c) {
        for (std::size_t r = 0; r < rows; ++r) {
            std::uint32_t v = le_bytes(raw->data() + pos, widths[c]);
            pos += widths[c];
            if (cols[c].type & kColString) {
                out[r][c].text = string(v);
            } else if (v == 0) {
                // Ноль означает «значения нет», а не «значение ноль»: иначе
                // отсутствующее число превратилось бы в -0x8000.
                out[r][c].number = 0;
            } else if (widths[c] == 2) {
                out[r][c].number = static_cast<std::int32_t>(v) - 0x8000;
            } else {
                out[r][c].number = static_cast<std::int32_t>(v - 0x80000000u);
            }
        }
    }
    return out;
}

bool Msi::has_table(const std::string &name) const { return schema_.contains(name); }

const std::vector<MsiColumn> *Msi::columns(const std::string &name) const {
    auto it = schema_.find(name);
    return it == schema_.end() ? nullptr : &it->second;
}

std::vector<std::string> Msi::table_names() const {
    std::vector<std::string> out;
    out.reserve(schema_.size());
    for (const auto &[name, _] : schema_) {
        out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

Result<std::vector<MsiRow>> Msi::table(const std::string &name) const {
    auto it = schema_.find(name);
    if (it == schema_.end()) {
        return err_not_found(fmt::format("table '{}' is not in the package", name));
    }
    return decode(name, it->second);
}

namespace {

// Индекс колонки по имени. Схема пакета — данные, а не наш код, поэтому
// отсутствие ожидаемой колонки должно давать внятный отказ, а не выход за
// границу вектора.
Result<std::size_t> column_index(const std::vector<MsiColumn> &cols, std::string_view name,
                                 std::string_view table) {
    for (std::size_t i = 0; i < cols.size(); ++i) {
        if (cols[i].name == name) {
            return i;
        }
    }
    return err_format(fmt::format("table '{}' has no column '{}'", table, name));
}

} // namespace

Result<std::vector<MsiFile>> Msi::files() const {
    struct Need {
        const char *table;
        std::vector<const char *> cols;
    };

    // Собираем индексы колонок заранее и одинаково для всех пяти таблиц:
    // любая недостающая колонка обязана назвать себя до того, как начнётся
    // разбор строк.
    auto index_of = [this](const char *table, const char *col) -> Result<std::size_t> {
        const auto *cols = columns(table);
        if (!cols) {
            return err_not_found(fmt::format("table '{}' is not in the package", table));
        }
        return column_index(*cols, col, table);
    };

    auto dir_rows = table("Directory");
    if (!dir_rows) {
        return std::unexpected(std::move(dir_rows).error().at("listing package files"));
    }
    auto comp_rows = table("Component");
    if (!comp_rows) {
        return std::unexpected(std::move(comp_rows).error().at("listing package files"));
    }
    auto file_rows = table("File");
    if (!file_rows) {
        return std::unexpected(std::move(file_rows).error().at("listing package files"));
    }
    auto media_rows = table("Media");
    if (!media_rows) {
        return std::unexpected(std::move(media_rows).error().at("listing package files"));
    }

    std::size_t d_key = 0, d_parent = 0, d_default = 0;
    std::size_t c_key = 0, c_dir = 0;
    std::size_t f_key = 0, f_comp = 0, f_name = 0, f_size = 0, f_attr = 0, f_seq = 0;
    std::size_t m_last = 0, m_cab = 0;
    {
        const std::pair<std::size_t *, std::pair<const char *, const char *>> wanted[] = {
            {&d_key, {"Directory", "Directory"}},
            {&d_parent, {"Directory", "Directory_Parent"}},
            {&d_default, {"Directory", "DefaultDir"}},
            {&c_key, {"Component", "Component"}},
            {&c_dir, {"Component", "Directory_"}},
            {&f_key, {"File", "File"}},
            {&f_comp, {"File", "Component_"}},
            {&f_name, {"File", "FileName"}},
            {&f_size, {"File", "FileSize"}},
            {&f_attr, {"File", "Attributes"}},
            {&f_seq, {"File", "Sequence"}},
            {&m_last, {"Media", "LastSequence"}},
            {&m_cab, {"Media", "Cabinet"}},
        };
        for (const auto &[slot, where] : wanted) {
            auto i = index_of(where.first, where.second);
            if (!i) {
                return std::unexpected(std::move(i).error().at("listing package files"));
            }
            *slot = *i;
        }
    }

    struct Dir {
        std::string parent;
        std::string name;
    };
    std::unordered_map<std::string, Dir> dirs;
    for (const auto &r : *dir_rows) {
        dirs.emplace(r[d_key].text, Dir{r[d_parent].text, std::string(target_name(r[d_default].text))});
    }

    // Путь каталога собирается вверх по родителям. Глубина ограничена не ради
    // экономии, а потому что Directory_Parent — это данные из пакета: цикл в
    // них не должен подвешивать распаковку.
    std::unordered_map<std::string, std::string> resolved;
    auto dir_path = [&](const std::string &key) -> std::string {
        std::vector<const std::string *> stack;
        std::string current = key;
        for (int depth = 0; depth < 64; ++depth) {
            if (current.empty()) {
                break;
            }
            if (auto cached = resolved.find(current); cached != resolved.end()) {
                std::string path = cached->second;
                for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
                    if (!(*it)->empty() && **it != ".") {
                        path = path.empty() ? **it : path + "/" + **it;
                    }
                }
                return path;
            }
            auto it = dirs.find(current);
            if (it == dirs.end()) {
                break;
            }
            stack.push_back(&it->second.name);
            if (it->second.parent == current) {
                break;
            }
            current = it->second.parent;
        }
        std::string path;
        for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
            if (!(*it)->empty() && **it != "." && **it != "SourceDir") {
                path = path.empty() ? **it : path + "/" + **it;
            }
        }
        resolved.emplace(key, path);
        return path;
    };

    std::unordered_map<std::string, std::string> comp_dir;
    for (const auto &r : *comp_rows) {
        comp_dir.emplace(r[c_key].text, r[c_dir].text);
    }

    // Media задаёт, какой архив покрывает какой диапазон Sequence. Границы
    // сортируются, потому что порядок строк в таблице ничем не гарантирован.
    std::vector<std::pair<std::int32_t, std::string>> media;
    media.reserve(media_rows->size());
    for (const auto &r : *media_rows) {
        media.emplace_back(r[m_last].number, r[m_cab].text);
    }
    std::sort(media.begin(), media.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });

    std::unordered_map<std::string, std::array<std::uint8_t, 16>> hashes;
    if (has_table("MsiFileHash")) {
        auto rows = table("MsiFileHash");
        if (!rows) {
            return std::unexpected(std::move(rows).error().at("listing package files"));
        }
        auto h_file = index_of("MsiFileHash", "File_");
        if (!h_file) {
            return std::unexpected(std::move(h_file).error().at("listing package files"));
        }
        std::array<std::size_t, 4> parts{};
        bool ok = true;
        for (int i = 0; i < 4; ++i) {
            auto idx = index_of("MsiFileHash", fmt::format("HashPart{}", i + 1).c_str());
            if (!idx) {
                ok = false;
                break;
            }
            parts[static_cast<std::size_t>(i)] = *idx;
        }
        if (ok) {
            for (const auto &r : *rows) {
                std::array<std::uint8_t, 16> md5{};
                bool any = false;
                for (int i = 0; i < 4; ++i) {
                    auto v = static_cast<std::uint32_t>(r[parts[static_cast<std::size_t>(i)]].number);
                    any = any || v != 0;
                    for (int b = 0; b < 4; ++b) {
                        md5[static_cast<std::size_t>(i * 4 + b)] =
                            static_cast<std::uint8_t>(v >> (8 * b));
                    }
                }
                // Все четыре поля пустые — это «хеш не записан», а не хеш,
                // равный нулю. Так помечен пустой файл: в Windows SDK это
                // empty.cpp, и его настоящий MD5 (d41d8cd9…) в таблицу не
                // попал. Принять нули за хеш значило бы отказать в распаковке
                // пакета из-за файла, который заведомо цел.
                if (any) {
                    hashes.emplace(r[*h_file].text, md5);
                }
            }
        }
    }

    std::vector<MsiFile> out;
    out.reserve(file_rows->size());
    for (const auto &r : *file_rows) {
        MsiFile f;
        f.key = r[f_key].text;
        f.size = static_cast<std::uint64_t>(std::max<std::int32_t>(r[f_size].number, 0));
        f.attributes = static_cast<std::uint32_t>(r[f_attr].number);
        f.sequence = static_cast<std::uint32_t>(std::max<std::int32_t>(r[f_seq].number, 0));

        std::string_view name = target_name(r[f_name].text);
        auto comp = comp_dir.find(r[f_comp].text);
        if (comp == comp_dir.end()) {
            return err_format(fmt::format("file '{}' refers to component '{}', which is not in the package",
                                          f.key, r[f_comp].text));
        }
        std::string dir = dir_path(comp->second);
        f.path = dir.empty() ? std::string(name) : dir + "/" + std::string(name);

        for (const auto &[last, cab] : media) {
            if (static_cast<std::int32_t>(f.sequence) <= last) {
                f.cabinet = cab;
                break;
            }
        }
        if (auto h = hashes.find(f.key); h != hashes.end()) {
            f.md5 = h->second;
        }
        out.push_back(std::move(f));
    }
    return out;
}

} // namespace cork::archive
