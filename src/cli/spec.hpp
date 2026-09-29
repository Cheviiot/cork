#pragma once

// Описание командной строки одной таблицей.
//
// Таблица здесь — не украшение, а единственный источник. Из неё выводятся
// справка, автодополнение и проверка неизвестных ключей. Стоит развести их по
// разным местам, и они разойдутся: список для автодополнения перестанет
// совпадать с тем, что на самом деле принимает разборщик, и узнать об этом
// можно будет только от человека, у которого не дополнился существующий ключ.
//
// Сам разбор остаётся в командах: у каждой свои правила, и сводить их к общему
// механизму значило бы либо обеднить, либо усложнить. Таблица отвечает на
// вопрос «что бывает», а не «как это понять».
//
// Исключение — команда, которая передаёт свои аргументы другой. Она обязана
// сверяться с таблицей (см. command_accepts), иначе примет всё, что примет
// та, и объявленное в справке разойдётся с действительным.

#include <string_view>
#include <vector>

#include "i18n/messages.hpp"

namespace cork::cli {

// Что предлагать после ключа, которому нужно значение.
enum class Completes {
    Nothing,   // значения нет, это флаг
    File,
    Directory,
    MsvcVersion,
    SdkVersion,
    Architecture,
    Package,
    SessionKey,
};

// Имя ключа и его значение остаются строками: они часть интерфейса и не
// переводятся. Описание — ключ каталога, потому что читает его человек.
struct OptionSpec {
    std::string_view name;
    std::string_view value;  // «<version>»; пусто у флагов
    i18n::Msg help;
    Completes completes = Completes::Nothing;
    bool repeatable = false;
};

struct CommandSpec {
    std::string_view name;
    std::string_view usage;    // хвост строки использования
    i18n::Msg summary;         // одна строка для общей справки
    std::vector<OptionSpec> options;
    Completes positional = Completes::Nothing;
};

// Все команды в порядке, в котором их показывать.
const std::vector<CommandSpec> &command_table();

// Команда по имени или nullptr.
const CommandSpec *find_command(std::string_view name);

// Объявлен ли такой ключ у этой команды.
//
// Нужно там, где одна команда передаёт аргументы другой: без проверки она
// молча примет чужие ключи, и справка перестанет описывать то, что есть.
[[nodiscard]] bool command_accepts(const CommandSpec &, std::string_view option);

// Общая справка и справка по одной команде, на текущем языке.
std::string render_help();
std::string render_command_help(const CommandSpec &);

} // namespace cork::cli
