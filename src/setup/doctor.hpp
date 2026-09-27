#pragma once

// Проверка установки. Отвечает на вопрос «можно ли этим собрать», а не «есть
// ли такие каталоги».
//
// Разница не теоретическая. Проверка «есть ли каталог» и «есть ли bin/wine»
// довольна деревом, в котором вместо двух сотен символьных ссылок ноль:
// распаковщик записал их обычными файлами с путём внутри. Вместе с ними
// исчезает работающий lib/mono/4.5, без которого MSBuild.exe не стартует
// вовсе, — а каталоги при этом на месте все до одного.
//
// Поэтому здесь проверяется не форма, а состав, и сравнивается он не с
// ожиданиями кода, а с receipt — с записью о том, что было создано на самом
// деле. Дерево, у которого receipt не сходится с содержимым, неисправно, даже
// если выглядит целым.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "base/error.hpp"
#include "setup/generation.hpp"

namespace cork::setup {

enum class CheckLevel {
    // Читает receipt и конфигурацию, проверяет то, что можно проверить, не
    // читая дерево целиком: состояние, ключевые файлы, состав Wine и Mono.
    Quick,
    // То же плюс полный обход дерева против записанного дайджеста.
    Deep,
    // То же плюс настоящая компиляция и компоновка пробника на каждую цель.
    Buildable,
};

enum class Severity {
    Ok,
    // Установка работает, но что-то не в лучшем виде.
    Warning,
    // Собрать этим нельзя.
    Failure,
};

struct Check {
    std::string name;
    Severity severity = Severity::Ok;
    std::string detail;
};

struct Report {
    std::vector<Check> checks;
    CheckLevel level = CheckLevel::Quick;
    std::filesystem::path generation;

    [[nodiscard]] bool healthy() const;
    [[nodiscard]] std::size_t count(Severity) const;
};

struct DoctorOptions {
    CheckLevel level = CheckLevel::Quick;
    // Какое поколение проверять. Пусто — то, на которое указывает current.
    std::filesystem::path generation;
    // Требовать, чтобы поколение было опубликовано. Снимается только при
    // проверке каталога сборки перед публикацией: там «ещё не опубликовано» —
    // это нормальное состояние, а не поломка.
    bool expect_published = true;
    // Вызывается по мере выполнения проверок: глубокий уровень обходит
    // гигабайты, и молчать всё это время нельзя.
    std::function<void(const Check &)> on_check;
};

[[nodiscard]] Result<Report> diagnose(const Root &, const DoctorOptions &);

// Состав дерева Wine: сколько в нём символьных ссылок и сколько файлов в
// сборке Mono. Вынесено наружу, потому что этими же числами install заполняет
// receipt — иначе сравнивать было бы не с чем.
struct WineComposition {
    std::uint64_t symlinks = 0;
    std::uint64_t mono_files = 0;
    bool present = false;
};

[[nodiscard]] Result<WineComposition> inspect_wine(const std::filesystem::path &runtime_root);

} // namespace cork::setup
