#include <string>

#include "base/version.hpp"
#include "check.h"

using cork::format_helper_line;
using cork::format_version;

int main() {
    CHECK_EQ(format_version("0.1.0", "", false), std::string("cork 0.1.0"));

    // Ревизия укорачивается до двенадцати знаков.
    CHECK_EQ(format_version("0.1.0", "c41df67aabbccddeeff00112233", false),
             std::string("cork 0.1.0 (c41df67aabbc)"));

    CHECK_EQ(format_version("0.1.0", "c41df67aabbccddeeff00112233", true),
             std::string("cork 0.1.0 (c41df67aabbc-dirty)"));

    // Ревизия короче двенадцати знаков не должна обрезаться в мусор.
    CHECK_EQ(format_version("0.1.0", "abc", false), std::string("cork 0.1.0 (abc)"));

    // Отсутствие хелпера обязано быть видно словами, а не отсутствием строки:
    // release.yml раньше принимал пустоту за исправность и выпустил бы
    // бинарник, которым нельзя запустить ни один инструмент.
    CHECK_EQ(format_helper_line(""), std::string("PE helper: not embedded"));

    // Хеш печатается целиком: CI сверяет его с хешем файла, который только
    // что собрал, а укороченный сверить нельзя.
    const std::string sha(64, 'a');
    CHECK_EQ(format_helper_line(sha), "PE helper " + sha);

    // Две формы не должны совпадать ни по какому подстрочному поиску: если бы
    // «not embedded» содержало шестнадцатеричный хвост, grep в CI принял бы
    // его за хеш.
    CHECK(format_helper_line("").find("PE helper ") == std::string::npos);

    return cork::test::finish("test_version");
}
