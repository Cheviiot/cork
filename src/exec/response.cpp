#include "exec/response.hpp"

#include <fmt/format.h>

#include "base/fs.hpp"

namespace cork::exec {
namespace {

namespace stdfs = std::filesystem;

bool is_separator(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

// UTF-16 в UTF-8. Своя реализация, а не codecvt: он объявлен устаревшим, а
// поведение при некорректных парах суррогатов нам нужно определённое —
// заменить, а не бросить. Обрывок суррогатной пары в response-файле означает
// испорченный файл, но ронять из-за него сборку не нужно: имя всё равно не
// совпадёт ни с чем на диске, и ошибка придёт от компилятора, с внятным
// текстом.
std::string utf16_to_utf8(const char *bytes, std::size_t size, bool big_endian) {
    std::string out;
    out.reserve(size);
    const auto unit = [&](std::size_t i) -> char32_t {
        const auto lo = static_cast<unsigned char>(bytes[i]);
        const auto hi = static_cast<unsigned char>(bytes[i + 1]);
        return big_endian ? static_cast<char32_t>((lo << 8) | hi)
                          : static_cast<char32_t>((hi << 8) | lo);
    };

    for (std::size_t i = 0; i + 1 < size; i += 2) {
        char32_t cp = unit(i);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < size) {
            const char32_t low = unit(i + 2);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            } else {
                cp = 0xFFFD;
            }
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;
        }

        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

} // namespace

std::vector<std::string> tokenize_response(std::string_view text) {
    std::vector<std::string> args;
    std::string current;
    bool in_quotes = false;
    bool have_token = false;

    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];

        if (c == '\\') {
            // Обратные слэши особенные только перед кавычкой: 2n слэшей дают n
            // слэшей и кавычку-разделитель, 2n+1 — n слэшей и буквальную
            // кавычку. Перед чем угодно другим слэш — обычный символ, и это
            // важно: путь C:\dir\ должен остаться собой.
            std::size_t slashes = 0;
            while (i < text.size() && text[i] == '\\') {
                ++slashes;
                ++i;
            }
            if (i < text.size() && text[i] == '"') {
                current.append(slashes / 2, '\\');
                if (slashes % 2 == 1) {
                    current.push_back('"');
                } else {
                    in_quotes = !in_quotes;
                }
                have_token = true;
            } else {
                current.append(slashes, '\\');
                if (slashes > 0) {
                    have_token = true;
                }
                --i;  // символ, на котором остановились, ещё не обработан
            }
            continue;
        }

        if (c == '"') {
            // Две кавычки подряд внутри закавыченного куска — это одна
            // буквальная кавычка. Правило редкое, но именно им пользуется
            // MSBuild, когда передаёт define со строковым значением.
            if (in_quotes && i + 1 < text.size() && text[i + 1] == '"') {
                current.push_back('"');
                ++i;
            } else {
                in_quotes = !in_quotes;
            }
            have_token = true;
            continue;
        }

        if (!in_quotes && is_separator(c)) {
            if (have_token) {
                args.push_back(current);
                current.clear();
                have_token = false;
            }
            continue;
        }

        current.push_back(c);
        have_token = true;
    }

    if (have_token) {
        args.push_back(current);
    }
    return args;
}

Result<std::vector<std::string>> read_response_file(const stdfs::path &path) {
    auto raw = fs::read_file(path);
    if (!raw) {
        return std::unexpected(
            std::move(raw).error().at(fmt::format("reading response file {}", path.string())));
    }
    const std::string &d = *raw;

    // Метка порядка байтов. UTF-16LE пишут и сам компилятор, и MSBuild;
    // прочитанный как байты, такой файл выглядит как текст с нулём после
    // каждой буквы и не токенизируется ни во что осмысленное.
    if (d.size() >= 2 && static_cast<unsigned char>(d[0]) == 0xFF &&
        static_cast<unsigned char>(d[1]) == 0xFE) {
        return tokenize_response(utf16_to_utf8(d.data() + 2, d.size() - 2, false));
    }
    if (d.size() >= 2 && static_cast<unsigned char>(d[0]) == 0xFE &&
        static_cast<unsigned char>(d[1]) == 0xFF) {
        return tokenize_response(utf16_to_utf8(d.data() + 2, d.size() - 2, true));
    }
    if (d.size() >= 3 && static_cast<unsigned char>(d[0]) == 0xEF &&
        static_cast<unsigned char>(d[1]) == 0xBB && static_cast<unsigned char>(d[2]) == 0xBF) {
        return tokenize_response(std::string_view(d).substr(3));
    }
    // Без метки — байты как есть. Перекодировать нечего и незачем: имена
    // файлов в Linux не обязаны быть корректным UTF-8, и «исправление»
    // кодировки превратило бы рабочий путь в нерабочий.
    return tokenize_response(d);
}

std::string render_response(const std::vector<std::string> &args) {
    std::string out;
    for (const auto &a : args) {
        if (!out.empty()) {
            out.push_back('\n');
        }
        const bool needs_quotes =
            a.empty() || a.find_first_of(" \t\n\v\"") != std::string::npos;
        if (!needs_quotes) {
            out += a;
            continue;
        }
        out.push_back('"');
        std::size_t slashes = 0;
        for (const char c : a) {
            if (c == '\\') {
                ++slashes;
                out.push_back(c);
                continue;
            }
            if (c == '"') {
                // Слэши перед кавычкой удваиваются, иначе она перестанет быть
                // буквальной при обратном разборе.
                out.append(slashes + 1, '\\');
                out.push_back('"');
            } else {
                out.push_back(c);
            }
            slashes = 0;
        }
        // То же самое для слэшей перед закрывающей кавычкой: путь, который
        // кончается на C:\dir\, иначе съел бы её.
        out.append(slashes, '\\');
        out.push_back('"');
    }
    out.push_back('\n');
    return out;
}

std::vector<ResponseArg> find_response_args(const std::vector<std::string> &args) {
    std::vector<ResponseArg> out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string &a = args[i];
        if (a.size() < 2 || a[0] != '@' || a[1] == '@') {
            continue;
        }
        out.push_back(ResponseArg{i, stdfs::path(a.substr(1))});
    }
    return out;
}

} // namespace cork::exec
