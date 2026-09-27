#include "exec/clpost.hpp"

#include <cctype>

#include <fmt/format.h>

#include "base/fs.hpp"

namespace cork::exec {
namespace {

namespace stdfs = std::filesystem;

bool is_switch(std::string_view a) { return !a.empty() && (a[0] == '/' || a[0] == '-'); }

// Ключ без учёта регистра: cl принимает и /P, и /p.
bool switch_is(std::string_view arg, std::string_view name) {
    if (!is_switch(arg) || arg.size() != name.size() + 1) {
        return false;
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(arg[i + 1])) !=
            std::tolower(static_cast<unsigned char>(name[i]))) {
            return false;
        }
    }
    return true;
}

// Значение ключа вида /Fi<путь>. Возвращает false, если это не тот ключ.
bool switch_value(std::string_view arg, std::string_view name, std::string_view &value) {
    if (!is_switch(arg) || arg.size() < name.size() + 1) {
        return false;
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(arg[i + 1])) !=
            std::tolower(static_cast<unsigned char>(name[i]))) {
            return false;
        }
    }
    value = arg.substr(name.size() + 1);
    return true;
}

// Расширения, по которым cl узнаёт исходник. Всё прочее в командной строке —
// библиотеки, объектные файлы и ключи, и .i из них не получается.
bool has_source_extension(const stdfs::path &p) {
    std::string ext = p.extension().string();
    for (char &c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".c++" ||
           ext == ".cp";
}

// Исходник ли это.
//
// Здесь вся неприятность задачи. На Windows ключ начинается со слэша, а путь —
// с буквы диска, и различить их тривиально. У нас пути unix, и «/work/main.c»
// начинается ровно так же, как «/W4». Одного признака «начинается со слэша»
// достаточно, чтобы принять каждый абсолютный путь за ключ и не найти ни
// одного исходника.
//
// Различаем по двум признакам сразу: расширение должно быть исходниковым, и
// аргумент должен выглядеть как путь, а не как ключ со значением. Ключи cl
// короткие и односегментные («/W4», «/wd4996», «/Tpfile.cpp»), а абсолютный
// путь содержит разделитель внутри. Остаётся редкий случай «/I/inc/x.c» —
// каталог включений, кончающийся расширением исходника; им можно пренебречь,
// а вот явные /Tc и /Tp пренебрежения не терпят, они указывают исходник прямо.
bool is_source_argument(std::string_view arg, stdfs::path &source) {
    if (arg.size() > 3 && (arg[0] == '/' || arg[0] == '-') &&
        (arg[1] == 'T' || arg[1] == 't') && (arg[2] == 'c' || arg[2] == 'p' ||
                                             arg[2] == 'C' || arg[2] == 'P')) {
        // /Tc и /Tp задают язык и имя файла одним аргументом.
        source = stdfs::path(arg.substr(3));
        return true;
    }
    const stdfs::path p{arg};
    if (!has_source_extension(p)) {
        return false;
    }
    if (arg[0] != '/' && arg[0] != '-') {
        source = p;
        return true;
    }
    // Начинается со слэша: путь, только если в нём есть ещё один разделитель.
    if (arg.find('/', 1) != std::string_view::npos) {
        source = p;
        return true;
    }
    return false;
}

// Путь внутри директивы #line записан как строковый литерал C: разделители в
// нём удвоены, «Z:\\home\\user». Разворачиваем это в обычный unix-путь.
std::string unescape_dos_path(std::string_view literal, const FilterConfig &cfg) {
    std::string out;
    out.reserve(literal.size());
    for (std::size_t i = 0; i < literal.size(); ++i) {
        if (literal[i] == '\\' && i + 1 < literal.size() && literal[i + 1] == '\\') {
            out.push_back('/');
            ++i;
            continue;
        }
        if (literal[i] == '\\') {
            out.push_back('/');
            continue;
        }
        out.push_back(literal[i]);
    }
    // Буква диска снимается после разворачивания: до него «z:» и «z:\\» — это
    // разные строки, и проверять пришлось бы обе.
    if (out.size() >= 2 && out[1] == ':' &&
        std::tolower(static_cast<unsigned char>(out[0])) ==
            std::tolower(static_cast<unsigned char>(cfg.root_drive))) {
        out.erase(0, 2);
    }
    return out;
}

// Переписывает строку, если это директива #line с путём. Всё остальное
// остаётся нетронутым — и это не осторожность, а необходимость:
// препроцессированный текст полон строковых литералов пользователя, и замена
// «по всему файлу» испортила бы те из них, что похожи на пути.
std::string rewrite_line_directive(std::string_view line, const FilterConfig &cfg) {
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    if (i >= line.size() || line[i] != '#') {
        return std::string(line);
    }
    ++i;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    // cl пишет «#line N "путь"». Форму «# N "путь"» без слова line он не
    // использует, но она законна и стоит один сравнительный вызов.
    if (line.size() - i >= 4 && line.compare(i, 4, "line") == 0) {
        i += 4;
    }

    const std::size_t open = line.find('"', i);
    if (open == std::string_view::npos) {
        return std::string(line);
    }
    // Закрывающая кавычка — последняя в строке: путь может содержать
    // экранированную кавычку, и поиск первой оборвал бы его посередине.
    const std::size_t close = line.rfind('"');
    if (close <= open) {
        return std::string(line);
    }

    const std::string_view inside = line.substr(open + 1, close - open - 1);
    const std::string fixed = unescape_dos_path(inside, cfg);
    if (fixed == inside) {
        return std::string(line);
    }
    std::string out;
    out.reserve(line.size());
    out += line.substr(0, open + 1);
    out += fixed;
    out += line.substr(close);
    return out;
}

} // namespace

std::vector<stdfs::path> preprocessed_outputs(const std::vector<std::string> &args,
                                              const stdfs::path &cwd) {
    bool to_file = false;  // /P — вывод в файл
    std::string fi;
    bool have_fi = false;
    std::vector<stdfs::path> sources;

    for (const auto &a : args) {
        if (switch_is(a, "P")) {
            to_file = true;
            continue;
        }
        if (switch_is(a, "EP")) {
            // /EP сам по себе шлёт текст в stdout, и файла не появляется.
            // Вместе с /P он лишь убирает директивы #line — файл при этом
            // создаётся, и обрабатывать его всё равно надо, просто чинить в
            // нём будет нечего.
            continue;
        }
        if (std::string_view v; switch_value(a, "Fi", v)) {
            fi = std::string(v);
            have_fi = true;
            continue;
        }
        // Аргумент в форме «@файл» сюда не попадает: response-файлы уже
        // раскрыты вызывающим, и их содержимое пришло обычными аргументами.
        if (stdfs::path p; is_source_argument(a, p)) {
            sources.push_back(std::move(p));
        }
    }

    if (!to_file) {
        return {};
    }

    const auto absolute = [&cwd](const stdfs::path &p) {
        return p.is_absolute() ? p : cwd / p;
    };

    std::vector<stdfs::path> out;
    if (have_fi && !fi.empty()) {
        const stdfs::path target(fi);
        // Имя, оканчивающееся разделителем, — это каталог: cl положит туда
        // файлы под именами исходников. Без этой ветки мы бы искали один файл
        // с именем каталога и ничего не нашли.
        const bool is_dir = fi.back() == '/' || fi.back() == '\\' || fs::is_dir(absolute(target));
        if (!is_dir) {
            out.push_back(absolute(target));
            return out;
        }
        for (const auto &s : sources) {
            out.push_back(absolute(target) / stdfs::path(s).filename().replace_extension(".i"));
        }
        return out;
    }

    for (const auto &s : sources) {
        // Без /Fi файл появляется в текущем каталоге, а не рядом с исходником:
        // cl берёт от исходника только имя.
        out.push_back(cwd / stdfs::path(s).filename().replace_extension(".i"));
    }
    return out;
}

Result<bool> rewrite_preprocessed_file(const stdfs::path &path, const FilterConfig &cfg) {
    if (!fs::is_regular_file(path)) {
        // Компилятор мог отказать раньше, чем создал файл. Это его дело, а не
        // повод сообщать об ошибке постобработки.
        return false;
    }
    auto content = fs::read_file(path);
    if (!content) {
        return std::unexpected(std::move(content).error().at(
            fmt::format("post-processing {}", path.string())));
    }

    std::string out;
    out.reserve(content->size());
    bool changed = false;

    std::size_t start = 0;
    while (start <= content->size()) {
        std::size_t end = content->find('\n', start);
        const bool last = end == std::string::npos;
        if (last) {
            end = content->size();
        }
        const std::string_view line = std::string_view(*content).substr(start, end - start);

        std::string fixed = rewrite_line_directive(line, cfg);
        if (fixed != line) {
            changed = true;
        }
        out += fixed;
        if (!last) {
            out.push_back('\n');
        }
        if (last) {
            break;
        }
        start = end + 1;
    }

    if (!changed) {
        // Файл не переписывается, если менять нечего: лишняя запись сдвинула
        // бы время изменения и заставила бы сборочную систему пересобрать всё,
        // что от него зависит.
        return false;
    }
    if (auto r = fs::write_atomic(path, out); !r) {
        return std::unexpected(std::move(r).error());
    }
    return true;
}

} // namespace cork::exec
