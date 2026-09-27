#include <string>

#include "base/version.hpp"
#include "check.h"

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

    return cork::test::finish("test_version");
}
