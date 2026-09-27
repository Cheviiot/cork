#include "exec/tools.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <set>

namespace cork::exec {
namespace {

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool starts_with_ci(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

// Ключи, принимающие путь сразу за собой, без разделителя: /Fooutput.obj.
// Собраны по документации MSVC и по тому, что реально встречается в сборках
// CMake и MSBuild.
const std::map<std::string, std::vector<std::string>> &prefix_options() {
    static const std::map<std::string, std::vector<std::string>> table = {
        {"cl",
         {"/I", "/FI", "/Fo", "/Fe", "/Fd", "/Fp", "/Fi", "/Fa", "/Fm", "/FR", "/Fr", "/Yc",
          "/Yu", "/AI", "/Zi"}},
        {"link", {"/OUT:", "/PDB:", "/IMPLIB:", "/DEF:", "/LIBPATH:", "/MANIFESTFILE:",
                  "/MANIFESTINPUT:", "/PGD:", "/ORDER:@"}},
        {"lib", {"/OUT:", "/DEF:", "/LIBPATH:", "/LIST:"}},
        {"rc", {"/fo", "/i", "/I"}},
        {"mt", {"/out:", "/manifest", "/inputresource:", "/outputresource:"}},
        {"midl", {"/out", "/I", "/h", "/tlb", "/iid", "/proxy", "/dlldata", "/header"}},
        {"mc", {"/r", "/h", "/x"}},
        {"ml", {"/Fo", "/Fl", "/Fm", "/I"}},
        {"ml64", {"/Fo", "/Fl", "/Fm", "/I"}},
        {"armasm", {"-o", "-i"}},
        {"armasm64", {"-o", "-i"}},
        {"dumpbin", {"/OUT:"}},
        {"nmake", {"/F"}},
    };
    return table;
}

// Ключи со значением в следующем аргументе.
const std::map<std::string, std::vector<std::string>> &separate_options() {
    static const std::map<std::string, std::vector<std::string>> table = {
        {"midl", {"/out", "/h", "/tlb", "/iid", "/proxy", "/dlldata", "/I"}},
        {"mt", {"-out", "-manifest"}},
        {"armasm", {"-o", "-i"}},
        {"armasm64", {"-o", "-i"}},
    };
    return table;
}

// Ключи, которые начинаются так же, как «путьные», но пути не принимают. Без
// этого списка /Zi (отладочная информация, без аргумента) попал бы под /Z...
const std::set<std::string> &not_paths() {
    static const std::set<std::string> table = {"/ZI", "/Zi", "/Zc", "/Zp", "/Za", "/Ze",
                                                "/FC", "/FS", "/Fx"};
    return table;
}

bool is_option(std::string_view arg) {
    return !arg.empty() && (arg.front() == '/' || arg.front() == '-');
}

} // namespace

const std::vector<ToolSpec> &tool_table() {
    static const std::vector<ToolSpec> table = {
        {"cl", "cl.exe", ToolDir::MsvcBin, false},
        {"link", "link.exe", ToolDir::MsvcBin, false},
        {"lib", "lib.exe", ToolDir::MsvcBin, false},
        {"ml", "ml.exe", ToolDir::MsvcBin, false},
        {"ml64", "ml64.exe", ToolDir::MsvcBin, false},
        {"nmake", "nmake.exe", ToolDir::MsvcBin, false},
        {"armasm", "armasm.exe", ToolDir::MsvcBin, false},
        {"armasm64", "armasm64.exe", ToolDir::MsvcBin, false},
        {"dumpbin", "dumpbin.exe", ToolDir::MsvcBin, false},
        {"mc", "mc.exe", ToolDir::SdkBin, false},
        {"midl", "midl.exe", ToolDir::SdkBin, false},
        {"mt", "mt.exe", ToolDir::SdkBin, false},
        {"rc", "rc.exe", ToolDir::SdkBin, false},
        {"msbuild", "MSBuild.exe", ToolDir::MsBuild, true},
    };
    return table;
}

const ToolSpec *find_tool(std::string_view invoked_as) {
    std::string name = to_lower(invoked_as);
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0) {
        name.resize(name.size() - 4);
    }
    for (const auto &spec : tool_table()) {
        if (spec.name == name) {
            return &spec;
        }
    }
    return nullptr;
}

std::string shim_name(std::string_view invoked_as) {
    std::string lower = to_lower(invoked_as);
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".exe") == 0) {
        lower.resize(lower.size() - 4);
    }
    return lower;
}

bool is_native_shim(std::string_view name) {
    const std::string lower = shim_name(name);
    return lower == "cmd" || lower == "findstr";
}

PathMode path_mode_from_env() {
    const char *v = std::getenv("CORK_PATHS");
    if (v == nullptr) {
        return PathMode::Table;
    }
    const std::string mode = to_lower(v);
    if (mode == "legacy") {
        return PathMode::Legacy;
    }
    if (mode == "off" || mode == "none" || mode == "0") {
        return PathMode::Off;
    }
    return PathMode::Table;
}

std::optional<std::size_t> path_offset_in_argument(std::string_view arg) {
    // Точное воспроизведение прежних четырёх правил, в том же порядке: они
    // остаются запасным вариантом для ключей вне таблицы, и расхождение с
    // ними означало бы, что известный ранее ключ вдруг перестал работать.
    //
    //   -I/path, /I/path           один знак ключа
    //   -Fo/path                   два знака
    //   /MANIFESTINPUT:/path       два и более знака, затем двоеточие
    //   /abs/path                  голый абсолютный путь
    //
    // Решает не форма сама по себе, а проверка, что родительский каталог
    // существует и не является корнем. Именно она отделяет «/I/usr/include»
    // от короткого ключа «/P» — и она же означает, что «/tmp» путём не
    // считается: его родитель это «/».
    const auto alpha = [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; };

    std::size_t offset = std::string_view::npos;
    if (arg.size() >= 3 && (arg[0] == '-' || arg[0] == '/') && alpha(arg[1]) && arg[2] == '/') {
        offset = 2;
    } else if (arg.size() >= 4 && (arg[0] == '-' || arg[0] == '/') && alpha(arg[1]) &&
               alpha(arg[2]) && arg[3] == '/') {
        offset = 3;
    } else if (arg.size() >= 4 && (arg[0] == '-' || arg[0] == '/') && alpha(arg[1]) &&
               alpha(arg[2])) {
        std::size_t i = 3;
        while (i < arg.size() && alpha(arg[i])) {
            ++i;
        }
        if (i + 1 < arg.size() && arg[i] == ':' && arg[i + 1] == '/') {
            offset = i + 1;
        }
    }
    if (offset == std::string_view::npos && !arg.empty() && arg[0] == '/') {
        // «//c» — это ключ шима cmd, а не путь.
        if (arg.size() > 1 && arg[1] == '/') {
            return std::nullopt;
        }
        offset = 0;
    }
    if (offset == std::string_view::npos) {
        return std::nullopt;
    }

    const std::string_view path = arg.substr(offset);
    if (path.size() < 2) {
        return std::nullopt;
    }

    const std::filesystem::path p(path);
    const std::filesystem::path parent = p.parent_path();
    if (parent.empty() || parent == "/") {
        return std::nullopt;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(parent, ec)) {
        return std::nullopt;
    }
    return offset;
}

bool looks_like_path_argument(std::string_view arg) {
    return path_offset_in_argument(arg).has_value();
}

std::vector<proto::PathRef> path_argument_refs(std::string_view tool,
                                               const std::vector<std::string> &args,
                                               PathMode mode) {
    std::vector<proto::PathRef> out;
    if (mode == PathMode::Off) {
        return out;
    }

    const std::string key = to_lower(tool);
    const auto prefix_it = prefix_options().find(key);
    const auto separate_it = separate_options().find(key);

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string &arg = args[i];
        if (arg.empty()) {
            continue;
        }

        if (mode == PathMode::Table) {
            // Ключ со значением в следующем аргументе.
            bool consumed = false;
            if (separate_it != separate_options().end()) {
                for (const auto &opt : separate_it->second) {
                    if (arg.size() == opt.size() && starts_with_ci(arg, opt)) {
                        if (i + 1 < args.size()) {
                            // Значение целиком является путём.
                            out.push_back({static_cast<std::uint32_t>(i + 1), 0});
                            ++i;
                        }
                        consumed = true;
                        break;
                    }
                }
            }
            if (consumed) {
                continue;
            }

            if (not_paths().count(arg) != 0) {
                continue;
            }

            // Ключ с приклеенным значением.
            bool matched = false;
            if (prefix_it != prefix_options().end()) {
                for (const auto &opt : prefix_it->second) {
                    if (arg.size() > opt.size() && starts_with_ci(arg, opt)) {
                        // Переводить надо путь после ключа, а сам ключ
                        // сохранить: из «/I/usr/include» получается
                        // «/IZ:\\usr\\include».
                        out.push_back({static_cast<std::uint32_t>(i),
                                       static_cast<std::uint32_t>(opt.size())});
                        matched = true;
                        break;
                    }
                }
            }
            if (matched) {
                continue;
            }

            // Позиционный аргумент — исходный файл, объектник, библиотека.
            if (!is_option(arg)) {
                out.push_back({static_cast<std::uint32_t>(i), 0});
                continue;
            }
        }

        // Запасной вариант для ключей вне таблицы и весь режим legacy.
        // Без него неизвестный ключ, принимающий путь, перестал бы работать
        // молча — а таблица никогда не будет полной.
        if (const auto offset = path_offset_in_argument(arg); offset.has_value()) {
            out.push_back({static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(*offset)});
        }
    }

    // Аргумент мог попасть дважды: и от таблицы, и от эвристики. Оставляем
    // первое попадание — таблица точнее.
    std::stable_sort(out.begin(), out.end(), [](const proto::PathRef &a, const proto::PathRef &b) {
        return a.index < b.index;
    });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const proto::PathRef &a, const proto::PathRef &b) {
                              return a.index == b.index;
                          }),
              out.end());
    return out;
}

} // namespace cork::exec
