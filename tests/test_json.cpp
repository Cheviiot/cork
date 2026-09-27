// Писатель JSON.
//
// Проверяется в первую очередь то, ради чего он написан вместо второй
// библиотеки: байт-в-байт предсказуемый вывод и отказ вместо повреждённого
// документа. Receipt входит в дайджест поколения, поэтому «тот же смысл, но
// другие байты» означало бы «дерево изменилось».

#include <string>

#include "base/json.hpp"
#include "check.h"

using namespace cork;

namespace {

void test_scalars_and_nesting() {
    JsonWriter w;
    w.begin_object();
    w.field("name", "cork");
    w.field("count", 3);
    w.field("ok", true);
    w.key("nothing").null();
    w.key("list").begin_array();
    w.value("a");
    w.value(std::int64_t{-1});
    w.end_array();
    w.key("nested").begin_object();
    w.field("inner", "value");
    w.end_object();
    w.end_object();

    const std::string expected =
        "{\n"
        "    \"name\": \"cork\",\n"
        "    \"count\": 3,\n"
        "    \"ok\": true,\n"
        "    \"nothing\": null,\n"
        "    \"list\": [\n"
        "        \"a\",\n"
        "        -1\n"
        "    ],\n"
        "    \"nested\": {\n"
        "        \"inner\": \"value\"\n"
        "    }\n"
        "}\n";
    std::string got = w.take();
    CHECK(w.error().empty());
    CHECK_EQ(got, expected);
}

void test_empty_containers() {
    JsonWriter w;
    w.begin_object();
    w.key("empty_array").begin_array();
    w.end_array();
    w.key("empty_object").begin_object();
    w.end_object();
    w.end_object();

    const std::string expected =
        "{\n"
        "    \"empty_array\": [],\n"
        "    \"empty_object\": {}\n"
        "}\n";
    std::string got = w.take();
    CHECK(w.error().empty());
    CHECK_EQ(got, expected);
}

void test_escaping() {
    JsonWriter w;
    w.begin_object();
    w.field("quote", "say \"hi\"");
    w.field("backslash", "C:\\path");
    w.field("control", std::string("a\nb\tc\x01"));
    // Путь в Linux — произвольные байты, не обязанные быть корректным UTF-8.
    // Подменять их на U+FFFD значит потерять имя файла, которое потом не
    // найдётся.
    w.field("raw_bytes", std::string("\xff\xfe", 2));
    w.end_object();

    const std::string expected =
        "{\n"
        "    \"quote\": \"say \\\"hi\\\"\",\n"
        "    \"backslash\": \"C:\\\\path\",\n"
        "    \"control\": \"a\\nb\\tc\\u0001\",\n"
        "    \"raw_bytes\": \"\xff\xfe\"\n"
        "}\n";
    std::string got = w.take();
    CHECK(w.error().empty());
    CHECK_EQ(got, expected);
}

void test_structure_errors() {
    {
        // Значение без ключа внутри объекта.
        JsonWriter w;
        w.begin_object();
        w.value("orphan");
        w.end_object();
        CHECK(!w.error().empty());
        CHECK(w.take().empty());
    }
    {
        // Ключ без значения.
        JsonWriter w;
        w.begin_object();
        w.key("dangling");
        w.end_object();
        CHECK(!w.error().empty());
        CHECK(w.take().empty());
    }
    {
        // Массив закрыт как объект.
        JsonWriter w;
        w.begin_array();
        w.end_object();
        CHECK(!w.error().empty());
        CHECK(w.take().empty());
    }
    {
        // Незакрытый документ. Наполовину написанный JSON хуже, чем ничего:
        // его кто-нибудь попробует разобрать.
        JsonWriter w;
        w.begin_object();
        w.field("a", "b");
        CHECK(w.take().empty());
        CHECK(!w.error().empty());
    }
    {
        // Ключ вне объекта.
        JsonWriter w;
        w.begin_array();
        w.key("nope");
        w.end_array();
        CHECK(!w.error().empty());
    }
}

void test_deterministic() {
    const auto build = [] {
        JsonWriter w;
        w.begin_object();
        w.field("b", "2");
        w.field("a", "1");
        w.end_object();
        return w.take();
    };
    // Порядок задаётся вызовами, а не сортировкой, и повторный прогон обязан
    // дать те же байты.
    CHECK_EQ(build(), build());
    CHECK(build().find("\"b\"") < build().find("\"a\""));
}

} // namespace

int main() {
    test_scalars_and_nesting();
    test_empty_containers();
    test_escaping();
    test_structure_errors();
    test_deterministic();
    return cork::test::finish("test_json");
}
