#include "exec/filters.hpp"

#include <algorithm>
#include <cctype>

namespace cork::exec {
namespace {

bool ci_equal(char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
}

std::string backslashes_to_slashes(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// «z:\...» или «z:/...» где угодно в строке.
std::size_t find_root_drive(std::string_view line, char drive) {
    for (std::size_t i = 0; i + 2 < line.size(); ++i) {
        if (ci_equal(line[i], drive) && line[i + 1] == ':' &&
            (line[i + 2] == '\\' || line[i + 2] == '/')) {
            return i;
        }
    }
    return std::string_view::npos;
}

// Строка вида «z:\path\file.cpp(12): error C2065: ...».
bool is_diagnostic_line(std::string_view line, char drive) {
    if (line.size() < 3 || !ci_equal(line[0], drive) || line[1] != ':') {
        return false;
    }
    const std::size_t paren = line.find('(');
    if (paren == std::string_view::npos) {
        return false;
    }
    std::size_t i = paren + 1;
    std::size_t digits = 0;
    while (i < line.size() && (std::isdigit(static_cast<unsigned char>(line[i])) != 0)) {
        ++i;
        ++digits;
    }
    if (digits == 0) {
        return false;
    }
    // Может быть «(12,5)» — колонка.
    if (i < line.size() && line[i] == ',') {
        ++i;
        while (i < line.size() && (std::isdigit(static_cast<unsigned char>(line[i])) != 0)) {
            ++i;
        }
    }
    if (i >= line.size() || line[i] != ')') {
        return false;
    }
    const std::string_view rest = line.substr(i + 1);
    return starts_with(rest, ": error") || starts_with(rest, ": warning") ||
           starts_with(rest, ": note") || starts_with(rest, ": fatal error");
}

bool is_line_directive(std::string_view line) {
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    if (i >= line.size() || line[i] != '#') {
        return false;
    }
    ++i;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    return starts_with(line.substr(i), "line ");
}

} // namespace

std::string strip_cr(std::string_view line) {
    std::string out(line);
    const std::size_t at = out.find('\r');
    if (at != std::string::npos) {
        out.erase(at, 1);
    }
    return out;
}

std::string strip_root_drive(std::string_view line, const FilterConfig &cfg) {
    const std::size_t at = find_root_drive(line, cfg.root_drive);
    if (at == std::string_view::npos) {
        return std::string(line);
    }
    std::string out(line.substr(0, at));
    out += line.substr(at + 2);  // разделитель остаётся, уходит только «z:»
    return out;
}

std::string cl_stdout_filter(std::string_view line, const FilterConfig &cfg) {
    if (starts_with(line, "Note: including file:")) {
        return backslashes_to_slashes(strip_root_drive(line, cfg));
    }
    if (is_line_directive(line)) {
        return backslashes_to_slashes(strip_root_drive(line, cfg));
    }
    if (is_diagnostic_line(line, cfg.root_drive)) {
        return backslashes_to_slashes(strip_root_drive(line, cfg));
    }
    return std::string(line);
}

std::string cl_stderr_filter(std::string_view line, const FilterConfig &cfg) {
    if (starts_with(line, "Note: including file:")) {
        return backslashes_to_slashes(strip_root_drive(line, cfg));
    }
    // Диагностика в stderr тоже встречается: cl печатает туда часть ошибок
    // при /nologo. Без этой ветки пути в них остаются непреобразованными, и
    // редактор по такой строке файл не откроет.
    if (is_diagnostic_line(line, cfg.root_drive)) {
        return backslashes_to_slashes(strip_root_drive(line, cfg));
    }
    return std::string(line);
}

std::string dumpbin_stdout_filter(std::string_view line, const FilterConfig &cfg) {
    if (starts_with(line, "Dump of file ") || starts_with(line, "  PDB file found at ")) {
        return backslashes_to_slashes(strip_root_drive(line, cfg));
    }
    return std::string(line);
}

LineFilter filter_for(std::string_view tool, bool stderr_stream) {
    if (tool == "cl") {
        return stderr_stream ? &cl_stderr_filter : &cl_stdout_filter;
    }
    if (tool == "dumpbin" && !stderr_stream) {
        return &dumpbin_stdout_filter;
    }
    return nullptr;
}

} // namespace cork::exec
