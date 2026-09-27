// Полнота и пригодность каталогов локализации.
//
// Перевод ломается двумя способами, и оба тихие. Первый — пропущенная строка:
// посреди русского текста вылезает английская, и заметит это только тот, кто
// на неё наткнётся. Второй хуже: перевод с другим набором подстановок. Формат
// берётся из каталога в рантайме, поэтому компилятор его не видит, и
// «Скачано: {} из {}» вместо «Downloaded {}» — это не кривой текст, а
// исключение fmt::format_error посреди работающей установки.
//
// Поэтому проверяется не «каталог непуст», а построчно: каждая строка
// переведена, и у перевода ровно те же подстановки, что у оригинала.

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "check.h"
#include "cli/spec.hpp"
#include "i18n/messages.hpp"

using namespace cork;
using i18n::Msg;

namespace {

constexpr std::size_t kCount = static_cast<std::size_t>(Msg::Count);

// Подстановки в порядке появления. «{{» — это экранированная скобка, а не
// подстановка, и считать её означало бы ловить несуществующие расхождения.
std::vector<std::string> placeholders(std::string_view s) {
    std::vector<std::string> found;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '{') {
            continue;
        }
        if (i + 1 < s.size() && s[i + 1] == '{') {
            ++i;
            continue;
        }
        const std::size_t end = s.find('}', i);
        if (end == std::string_view::npos) {
            found.emplace_back("<unterminated>");
            break;
        }
        found.emplace_back(s.substr(i, end - i + 1));
        i = end;
    }
    return found;
}

void test_catalogs_are_complete() {
    CHECK(!i18n::languages().empty());
    for (const auto lang : i18n::languages()) {
        const std::size_t translated = i18n::translated_count(lang);
        if (translated != kCount) {
            fmt::print(stderr, "catalogue '{}': {} of {} strings translated\n", lang, translated,
                       kCount);
        }
        CHECK(translated == kCount);
    }
}

void test_placeholders_match_the_original() {
    for (const auto lang : i18n::languages()) {
        i18n::set_language(lang);
        for (std::size_t i = 0; i < kCount; ++i) {
            const auto m = static_cast<Msg>(i);
            const auto want = placeholders(i18n::original(m));
            const auto got = placeholders(i18n::tr(m));
            if (want != got) {
                fmt::print(stderr, "catalogue '{}', message {}: \"{}\" vs \"{}\"\n", lang, i,
                           i18n::original(m), i18n::tr(m));
            }
            CHECK(want == got);
        }
    }
    i18n::set_language("en");
}

// Каталог, в котором строка переведена сама в себя, обычно означает забытую
// строку, а не удачное совпадение. Английские имена вроде «bash, zsh or fish»
// — законное исключение, поэтому проверка мягкая: она считает долю.
void test_translation_is_not_a_copy() {
    for (const auto lang : i18n::languages()) {
        if (lang == "en") {
            continue;
        }
        i18n::set_language(lang);
        std::size_t identical = 0;
        for (std::size_t i = 0; i < kCount; ++i) {
            const auto m = static_cast<Msg>(i);
            if (i18n::tr(m) == i18n::original(m)) {
                ++identical;
            }
        }
        CHECK(identical * 10 < kCount);
    }
    i18n::set_language("en");
}

void test_language_selection() {
    i18n::set_language("ru_RU.UTF-8");
    CHECK(i18n::language() == "ru");
    i18n::set_language("RU");
    CHECK(i18n::language() == "ru");
    // Язык без каталога — не отказ и не пустой вывод, а английский.
    i18n::set_language("kl_GL");
    CHECK(i18n::language() == "en");
    i18n::set_language("en");
}

// Ради этого всё и затевалось: справка действительно меняет язык, а не
// остаётся английской из-за того, что таблица держит литералы.
void test_help_follows_the_language() {
    i18n::set_language("en");
    const std::string english = cli::render_help();
    i18n::set_language("ru");
    const std::string russian = cli::render_help();
    CHECK(english != russian);

    // Имена команд — часть интерфейса и обязаны пережить перевод: человек их
    // набирает, а не читает.
    for (const auto &c : cli::command_table()) {
        CHECK(russian.find(c.name) != std::string::npos);
        for (const auto &o : c.options) {
            const std::string help = cli::render_command_help(c);
            CHECK(help.find(o.name) != std::string::npos);
        }
    }
    i18n::set_language("en");
}

} // namespace

int main() {
    test_catalogs_are_complete();
    test_placeholders_match_the_original();
    test_translation_is_not_a_copy();
    test_language_selection();
    test_help_follows_the_language();
    return cork::test::finish("test_i18n");
}
