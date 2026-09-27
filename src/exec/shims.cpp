#include "exec/shims.hpp"

#include <cctype>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <regex>
#include <string>

#include <unistd.h>

#include <fmt/format.h>

namespace cork::exec {
namespace {

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char &c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// Разбор строки по правилам cmd. От правил командной строки Windows они
// отличаются тем, что кавычка здесь только группирует и обратный слэш её не
// экранирует: «\» в cmd — обычный символ пути, а не escape. Применить сюда
// правила CommandLineToArgvW значило бы съесть слэши в каждом пути.
std::vector<std::string> split_cmd_line(std::string_view line) {
    std::vector<std::string> out;
    std::string current;
    bool in_quotes = false;
    bool have = false;

    for (const char c : line) {
        if (c == '"') {
            in_quotes = !in_quotes;
            have = true;
            continue;
        }
        if (!in_quotes && (c == ' ' || c == '\t')) {
            if (have) {
                out.push_back(current);
                current.clear();
                have = false;
            }
            continue;
        }
        current.push_back(c);
        have = true;
    }
    if (have) {
        out.push_back(current);
    }
    return out;
}

// Эти команды cmd выполняет сам, внешней программы для них не существует.
bool is_builtin(std::string_view lower) {
    return lower == "echo" || lower == "set" || lower == "if" || lower == "for" ||
           lower == "call" || lower == "goto" || lower == "rem" || lower == "cd" ||
           lower == "copy" || lower == "del" || lower == "md" || lower == "mkdir" ||
           lower == "rd" || lower == "rmdir" || lower == "type" || lower == "move";
}

int grep_stream(std::istream &in, const FindstrOptions &opts, const std::string &label,
                bool with_label, bool &matched_any) {
    std::vector<std::regex> patterns;
    if (!opts.literal) {
        auto flags = std::regex::ECMAScript;
        if (opts.ignore_case) {
            flags |= std::regex::icase;
        }
        for (const auto &p : opts.patterns) {
            try {
                patterns.emplace_back(p, flags);
            } catch (const std::regex_error &) {
                // Недействительное регулярное выражение — ищем как литерал.
                // findstr поступает так же: его диалект беднее, и то, что для
                // него текст, для ECMAScript может быть синтаксисом.
                patterns.clear();
                break;
            }
        }
    }
    const bool as_literal = opts.literal || patterns.empty();

    std::string line;
    std::uint64_t number = 0;
    while (std::getline(in, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        bool hit = false;
        if (as_literal) {
            const std::string haystack = opts.ignore_case ? to_lower(line) : line;
            for (const auto &p : opts.patterns) {
                const std::string needle = opts.ignore_case ? to_lower(p) : p;
                if (haystack.find(needle) != std::string::npos) {
                    hit = true;
                    break;
                }
            }
        } else {
            for (const auto &re : patterns) {
                if (std::regex_search(line, re)) {
                    hit = true;
                    break;
                }
            }
        }
        if (hit == opts.invert) {
            continue;
        }
        matched_any = true;
        if (opts.names_only) {
            // /M печатает имя файла один раз и переходит к следующему.
            fmt::print("{}\n", label);
            return 0;
        }
        if (with_label) {
            fmt::print("{}:", label);
        }
        if (opts.line_numbers) {
            fmt::print("{}:", number);
        }
        fmt::print("{}\n", line);
    }
    return 0;
}

} // namespace

CmdInvocation parse_cmd_c(const std::vector<std::string> &args) {
    CmdInvocation out;

    std::size_t i = 0;
    // Перед /c могут стоять ключи, которые для нас ничего не значат: /q гасит
    // эхо, которого у нас нет, /s влияет на снятие кавычек, /d отключает
    // autorun. Пропускаем их молча — они не меняют того, что надо выполнить.
    for (; i < args.size(); ++i) {
        const std::string lower = to_lower(args[i]);
        if (lower == "/q" || lower == "/d" || lower == "/s" || lower == "/a" ||
            lower == "/u") {
            continue;
        }
        break;
    }
    if (i >= args.size()) {
        out.error = "cmd: nothing to run";
        return out;
    }
    const std::string lower = to_lower(args[i]);
    if (lower != "/c" && lower != "/k") {
        out.error = fmt::format("cmd: only /c is supported, got '{}'", args[i]);
        return out;
    }
    ++i;

    if (i >= args.size()) {
        out.error = "cmd: nothing to run";
        return out;
    }

    // Дальше есть ровно два случая, и путать их нельзя.
    //
    // Один аргумент — это команда одной строкой, `cmd /c "copy a b"`, как её
    // пишет MSBuild. Её надо разобрать самим: границы слов внутри кавычек
    // ещё никем не расставлены.
    //
    // Несколько аргументов — их уже разобрал шелл, и разбирать повторно
    // нельзя. Склеить через пробел и снова разрезать значит потерять ровно ту
    // информацию, ради которой существуют кавычки: `cmd /c prog 'a b'`
    // превратится в `prog a b`, то есть в три аргумента вместо двух.
    if (args.size() - i == 1) {
        out.argv = split_cmd_line(args[i]);
    } else {
        out.argv.assign(args.begin() + static_cast<std::ptrdiff_t>(i), args.end());
    }
    if (out.argv.empty()) {
        out.error = "cmd: nothing to run";
        return out;
    }
    out.ok = true;
    return out;
}

FindstrOptions parse_findstr(const std::vector<std::string> &args) {
    FindstrOptions out;
    bool pattern_taken = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string &a = args[i];
        if (a.size() >= 2 && (a[0] == '/' || a[0] == '-')) {
            const std::string lower = to_lower(a);
            // /C:строка задаёт образец целиком, включая пробелы, и отменяет
            // разбиение образца по словам.
            if (lower.rfind("/c:", 0) == 0) {
                out.patterns.push_back(a.substr(3));
                out.literal = true;
                pattern_taken = true;
                continue;
            }
            bool understood = true;
            for (std::size_t k = 1; k < lower.size() && understood; ++k) {
                switch (lower[k]) {
                case 'i': out.ignore_case = true; break;
                case 'v': out.invert = true; break;
                case 'n': out.line_numbers = true; break;
                case 'm': out.names_only = true; break;
                case 'l': out.literal = true; break;
                case 'r': out.literal = false; break;
                // /S (рекурсивно), /B, /E, /X, /P и прочее в файлах сборки
                // MSVC не встречаются. Тихо проигнорировать их нельзя: поиск
                // отработает не так, как просили, и это будет выглядеть как
                // ошибка сборки.
                default:
                    understood = false;
                    break;
                }
            }
            if (!understood) {
                out.ok = false;
                out.error = fmt::format("findstr: unsupported option '{}'", a);
                return out;
            }
            continue;
        }

        if (!pattern_taken) {
            // Первый свободный аргумент — образцы, разделённые пробелами.
            // Без /C: findstr считает каждое слово отдельным образцом.
            std::string word;
            for (const char c : a) {
                if (c == ' ') {
                    if (!word.empty()) {
                        out.patterns.push_back(word);
                        word.clear();
                    }
                } else {
                    word.push_back(c);
                }
            }
            if (!word.empty()) {
                out.patterns.push_back(word);
            }
            pattern_taken = true;
            continue;
        }
        out.files.push_back(a);
    }

    if (out.patterns.empty()) {
        out.ok = false;
        out.error = "findstr: no pattern given";
    }
    return out;
}

int run_shim(const std::string &name, const std::vector<std::string> &args) {
    if (name == "cmd") {
        const CmdInvocation inv = parse_cmd_c(args);
        if (!inv.ok) {
            fmt::print(stderr, "cork: {}\n", inv.error);
            return 1;
        }
        const std::string lower = to_lower(inv.argv[0]);
        if (is_builtin(lower)) {
            // Встроенные команды cmd мы не эмулируем: «echo» и «copy» кажутся
            // простыми, но их поведение в деталях (кавычки, перенаправление,
            // подстановка переменных) воспроизвести без самого cmd нельзя, а
            // воспроизвести наполовину — значит выдать неверный результат
            // вместо отказа.
            fmt::print(stderr,
                       "cork: cmd builtin '{}' is not emulated.\n"
                       "Rewrite the build step to call the program directly.\n",
                       inv.argv[0]);
            return 1;
        }

        std::vector<char *> argv;
        std::vector<std::string> copies = inv.argv;
        argv.reserve(copies.size() + 1);
        for (auto &a : copies) {
            argv.push_back(a.data());
        }
        argv.push_back(nullptr);
        // execvp, а не fork: шим и есть тот процесс, который должен стать
        // вызванной программой. Лишний уровень только исказил бы код возврата
        // и сигналы.
        ::execvp(argv[0], argv.data());
        fmt::print(stderr, "cork: cannot run '{}': {}\n", inv.argv[0], std::strerror(errno));
        return 127;
    }

    if (name == "findstr") {
        const FindstrOptions opts = parse_findstr(args);
        if (!opts.ok) {
            fmt::print(stderr, "cork: {}\n", opts.error);
            return 2;
        }
        bool matched = false;
        if (opts.files.empty()) {
            grep_stream(std::cin, opts, "(stdin)", false, matched);
        } else {
            const bool with_label = opts.files.size() > 1 && !opts.names_only;
            for (const auto &f : opts.files) {
                std::ifstream in(f);
                if (!in) {
                    fmt::print(stderr, "cork: findstr: cannot open '{}'\n", f);
                    continue;
                }
                grep_stream(in, opts, f, with_label, matched);
            }
        }
        // findstr возвращает 1, когда ничего не нашлось. Сборочные файлы на
        // это полагаются: «если не нашли — делаем то-то».
        return matched ? 0 : 1;
    }

    fmt::print(stderr, "cork: no shim named '{}'\n", name);
    return 127;
}

} // namespace cork::exec
