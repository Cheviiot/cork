// Таблица команд как единый источник.
//
// Смысл таблицы в том, что справка, автодополнение и разбор не могут
// разойтись. Проверять это надо буквально: не «таблица непуста», а «каждая
// команда, которую понимает main, в ней есть, и наоборот». Расхождение здесь
// тихое — человек видит, что существующий ключ не дополняется, и решает, что
// его нет.

#include <algorithm>
#include <set>
#include <string>
#include <string_view>

#include "check.h"
#include "setup/config.hpp"
#include "cli/spec.hpp"
#include "i18n/messages.hpp"

using namespace cork::cli;

namespace {

void test_every_command_is_described() {
    CHECK(!command_table().empty());
    for (const auto &c : command_table()) {
        // Пустое описание превращает справку в список слов.
        CHECK(!c.name.empty());
        CHECK(!cork::i18n::tr(c.summary).empty());
        for (const auto &o : c.options) {
            CHECK(o.name.rfind("--", 0) == 0);
            CHECK(!cork::i18n::tr(o.help).empty());
            // Ключ со значением обязан это значение назвать: «--jobs <n>»
            // понятно, «--jobs» — нет.
            if (o.completes != Completes::Nothing) {
                CHECK(!o.value.empty());
            }
        }
    }
}

void test_names_are_unique() {
    std::set<std::string_view> names;
    for (const auto &c : command_table()) {
        CHECK(names.insert(c.name).second);

        // Повтор ключа внутри команды означал бы, что в справке он выведется
        // дважды, а в автодополнении предложится дважды.
        std::set<std::string_view> options;
        for (const auto &o : c.options) {
            CHECK(options.insert(o.name).second);
        }
    }
}

void test_common_options_are_everywhere() {
    // --root и --help есть у каждой команды. Если у какой-то нет, человек
    // узнает об этом, когда команда откажется их принимать.
    for (const auto &c : command_table()) {
        const auto has = [&c](std::string_view name) {
            return std::any_of(c.options.begin(), c.options.end(),
                               [name](const OptionSpec &o) { return o.name == name; });
        };
        CHECK(has("--root"));
        CHECK(has("--help"));
    }
}

void test_lookup() {
    CHECK(find_command("download") != nullptr);
    CHECK(find_command("doctor") != nullptr);
    CHECK(find_command("no-such-command") == nullptr);
}

void test_help_mentions_every_command() {
    const std::string help = render_help();
    for (const auto &c : command_table()) {
        CHECK(help.find(c.name) != std::string::npos);
    }
}

void test_command_help_mentions_every_option() {
    for (const auto &c : command_table()) {
        const std::string help = render_command_help(c);
        CHECK(help.find(c.name) != std::string::npos);
        for (const auto &o : c.options) {
            CHECK(help.find(o.name) != std::string::npos);
        }
        // Повторяемость отмечается: без пометки «--architecture» выглядит
        // как ключ, у которого второе упоминание отменяет первое.
        for (const auto &o : c.options) {
            if (o.repeatable) {
                CHECK(help.find(cork::i18n::tr(cork::i18n::Msg::Repeatable)) !=
                      std::string::npos);
                break;
            }
        }
    }
}

} // namespace

void test_target_spellings() {
    // Короткие имена — как были.
    CHECK_EQ(cork::setup::normalise_target("x64"), "x64");
    CHECK_EQ(cork::setup::normalise_target("x86"), "x86");
    CHECK_EQ(cork::setup::normalise_target("arm64"), "arm64");

    // Триплеты, которыми ту же цель называют clang, rust и vcpkg.
    CHECK_EQ(cork::setup::normalise_target("x86_64-pc-windows-msvc"), "x64");
    CHECK_EQ(cork::setup::normalise_target("i686-pc-windows-msvc"), "x86");
    CHECK_EQ(cork::setup::normalise_target("aarch64-pc-windows-msvc"), "arm64");
    CHECK_EQ(cork::setup::normalise_target("AMD64"), "x64");
    CHECK_EQ(cork::setup::normalise_target("win32"), "x86");

    // Неизвестное остаётся неизвестным: молча подставить сюда умолчание
    // значило бы собрать не под то, о чём просили.
    CHECK(cork::setup::normalise_target("riscv64").empty());
    CHECK(cork::setup::normalise_target("").empty());
    // Похожее, но не то: x86_64h — это не наша цель, и «начинается на x86»
    // не повод её принять.
    CHECK(cork::setup::normalise_target("x86_64h-apple-darwin").empty());
}

int main() {
    test_target_spellings();
    test_every_command_is_described();
    test_names_are_unique();
    test_common_options_are_everywhere();
    test_lookup();
    test_help_mentions_every_command();
    test_command_help_mentions_every_option();
    return cork::test::finish("test_cli");
}
