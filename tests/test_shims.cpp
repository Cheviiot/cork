// Разбор аргументов cmd и findstr.
//
// Шимы сознательно узкие, и цена этой узости — в том, что отказ должен быть
// честным. Молча проигнорированный ключ findstr означает поиск не по тому,
// о чём просили, и выглядеть это будет как ошибка сборки в совершенно другом
// месте. Поэтому тест проверяет не только «что понимаем», но и «на чём
// отказываем».

#include <string>
#include <vector>

#include "check.h"
#include "exec/shims.hpp"
#include "exec/tools.hpp"

using namespace cork::exec;

namespace {

std::vector<std::string> a(std::initializer_list<const char *> items) {
    return std::vector<std::string>(items.begin(), items.end());
}

void test_shim_recognition() {
    CHECK(is_native_shim("cmd"));
    CHECK(is_native_shim("cmd.exe"));
    CHECK(is_native_shim("CMD.EXE"));
    CHECK(is_native_shim("findstr"));
    CHECK(!is_native_shim("cl"));
    CHECK(!is_native_shim("cmdline"));
    CHECK_EQ(shim_name("CMD.EXE"), std::string("cmd"));
    CHECK_EQ(shim_name("findstr"), std::string("findstr"));
}

void test_cmd_basic() {
    const auto inv = parse_cmd_c(a({"/c", "mytool", "--flag"}));
    CHECK(inv.ok);
    CHECK(inv.argv.size() == 2);
    if (inv.argv.size() == 2) {
        CHECK_EQ(inv.argv[0], std::string("mytool"));
        CHECK_EQ(inv.argv[1], std::string("--flag"));
    }
}

void test_cmd_ignores_harmless_switches() {
    // /q, /d и /s ничего не меняют в том, что нужно выполнить.
    const auto inv = parse_cmd_c(a({"/d", "/s", "/c", "mytool"}));
    CHECK(inv.ok);
    CHECK(inv.argv.size() == 1);
}

void test_cmd_keeps_shell_argument_boundaries() {
    // Аргументы, уже разобранные шеллом, повторно не разбираются. Склейка
    // через пробел и новый разрез превратили бы `prog "a b"` в три аргумента
    // вместо двух — и программа получила бы не то, что ей передали.
    const auto inv = parse_cmd_c(a({"/c", "prog", "a b", "c"}));
    CHECK(inv.ok);
    CHECK(inv.argv.size() == 3);
    if (inv.argv.size() == 3) {
        CHECK_EQ(inv.argv[1], std::string("a b"));
        CHECK_EQ(inv.argv[2], std::string("c"));
    }
}

void test_cmd_quotes_and_backslashes() {
    // Кавычка у cmd только группирует, а обратный слэш её не экранирует. Это
    // главное отличие от правил командной строки Windows: применив их здесь,
    // мы съели бы по слэшу в каждом пути.
    const auto inv =
        parse_cmd_c(a({"/c", "copytool \"C:\\dir with space\\\" dest"}));
    CHECK(inv.ok);
    CHECK(inv.argv.size() == 3);
    if (inv.argv.size() == 3) {
        CHECK_EQ(inv.argv[0], std::string("copytool"));
        CHECK_EQ(inv.argv[1], std::string("C:\\dir with space\\"));
        CHECK_EQ(inv.argv[2], std::string("dest"));
    }
}

void test_cmd_refuses_what_it_cannot_do() {
    const auto no_c = parse_cmd_c(a({"mytool"}));
    CHECK(!no_c.ok);
    CHECK(!no_c.error.empty());

    const auto empty = parse_cmd_c(a({"/c"}));
    CHECK(!empty.ok);

    const auto nothing = parse_cmd_c({});
    CHECK(!nothing.ok);
}

void test_findstr_patterns() {
    // Без /C: каждое слово — отдельный образец.
    const auto multi = parse_findstr(a({"error warning", "log.txt"}));
    CHECK(multi.ok);
    CHECK(multi.patterns.size() == 2);
    CHECK(multi.files.size() == 1);

    // С /C: образец берётся целиком, вместе с пробелами.
    const auto literal = parse_findstr(a({"/C:error warning", "log.txt"}));
    CHECK(literal.ok);
    CHECK(literal.patterns.size() == 1);
    if (literal.patterns.size() == 1) {
        CHECK_EQ(literal.patterns[0], std::string("error warning"));
    }
    CHECK(literal.literal);
}

void test_findstr_flags() {
    const auto opts = parse_findstr(a({"/I", "/N", "/V", "pattern"}));
    CHECK(opts.ok);
    CHECK(opts.ignore_case);
    CHECK(opts.line_numbers);
    CHECK(opts.invert);
    CHECK(!opts.names_only);

    // Слитная запись ключей — обычное дело в файлах сборки.
    const auto joined = parse_findstr(a({"/IN", "pattern"}));
    CHECK(joined.ok);
    CHECK(joined.ignore_case);
    CHECK(joined.line_numbers);
}

void test_findstr_refuses_unknown_flags() {
    // Ключ, которого мы не понимаем, обязан дать отказ. Тихо
    // проигнорированный /S превратил бы поиск по дереву в поиск по одному
    // каталогу, и разбираться пришлось бы с пустым результатом, а не с
    // сообщением.
    const auto rec = parse_findstr(a({"/S", "pattern"}));
    CHECK(!rec.ok);
    CHECK(!rec.error.empty());

    const auto begin = parse_findstr(a({"/B", "pattern"}));
    CHECK(!begin.ok);

    // Образец обязателен.
    const auto no_pattern = parse_findstr(a({"/I"}));
    CHECK(!no_pattern.ok);
}

} // namespace

int main() {
    test_shim_recognition();
    test_cmd_basic();
    test_cmd_ignores_harmless_switches();
    test_cmd_keeps_shell_argument_boundaries();
    test_cmd_quotes_and_backslashes();
    test_cmd_refuses_what_it_cannot_do();
    test_findstr_patterns();
    test_findstr_flags();
    test_findstr_refuses_unknown_flags();
    return cork::test::finish("test_shims");
}
