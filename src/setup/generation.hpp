#pragma once

// Поколения: сборка в стороне, публикация переименованием.
//
// Распаковка прямо в конечный каталог, да ещё поверх только что снесённого
// старого, оставляет после прерывания дерево, которое выглядит установленным:
// на вопрос «всё ли на месте» оно отвечает «да», а собрать ничего не может.
//
// Здесь поколение целиком собирается в staging/<uuid>, проверяется там же и
// только потом переименовывается на место. Каталог поколения назван так, что
// столкнуться не с чем: в имя входит дайджест дерева, поэтому публикация — это
// всегда rename в несуществующее имя, а переключение current — атомарная
// замена символической ссылки. Промежуточного состояния, в котором current
// указывает в никуда, не возникает ни на мгновение.
//
// Прерывание на любом шаге оставляет каталог в staging/, который ничем не
// притворяется и убирается сборкой мусора.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "base/error.hpp"

namespace cork::setup {

// Раскладка корня установок — единственное место, где она записана.
struct Root {
    std::filesystem::path base;

    [[nodiscard]] std::filesystem::path store() const { return base / "store"; }
    [[nodiscard]] std::filesystem::path staging() const { return base / "staging"; }
    [[nodiscard]] std::filesystem::path toolchains() const { return base / "toolchains"; }
    [[nodiscard]] std::filesystem::path current() const { return toolchains() / "current"; }
    [[nodiscard]] std::filesystem::path locks() const { return base / "locks"; }
    [[nodiscard]] std::filesystem::path runtime(std::string_view wine_id) const {
        return base / "runtime" / std::string(wine_id);
    }
    [[nodiscard]] std::filesystem::path prefix(std::string_view wine_id) const {
        return base / "prefix" / std::string(wine_id);
    }

    [[nodiscard]] Result<void> ensure() const;

    // Куда на самом деле указывает current, или отказ, если ссылки нет.
    [[nodiscard]] Result<std::filesystem::path> resolve_current() const;

    // Корень по умолчанию: $CORK_HOME или ~/.cork.
    static Root from_environment();
};

// Каталог, в котором собирается поколение. Пока он не опубликован и не
// оставлен явно, деструктор его удаляет: брошенный staging — это мусор, а не
// результат.
class Staging {
public:
    Staging(const Staging &) = delete;
    Staging &operator=(const Staging &) = delete;
    Staging(Staging &&) noexcept;
    Staging &operator=(Staging &&) noexcept;
    ~Staging();

    [[nodiscard]] static Result<Staging> create(const Root &);

    // Подхватывает уже существующий каталог сборки — так продолжается работа,
    // начатая предыдущей командой.
    [[nodiscard]] static Result<Staging> adopt(const Root &, const std::filesystem::path &);

    [[nodiscard]] const std::filesystem::path &path() const { return path_; }

    // Переименовывает поколение на место и переключает current. Возвращает
    // путь опубликованного каталога.
    //
    // name — человекочитаемая часть имени, например "14.51.36231-10.0.26100.0";
    // tree_digest дописывается к ней, чтобы имя было уникальным для этого
    // содержимого. Если такое поколение уже опубликовано, staging удаляется, а
    // current переключается на него: повторная установка того же самого — не
    // ошибка и не повод переписывать то, что уже проверено.
    [[nodiscard]] Result<std::filesystem::path> publish(std::string_view name,
                                                        std::string_view tree_digest);

    // Оставить каталог на диске. Нужно, когда сборка прервана, а её результат
    // хочется рассмотреть.
    void keep() { keep_ = true; }

private:
    Staging() = default;

    Root root_;
    std::filesystem::path path_;
    bool keep_ = false;
};

// Имя каталога поколения: читаемая часть плюс начало дайджеста дерева.
std::string generation_name(std::string_view name, std::string_view tree_digest);

struct GcStats {
    std::uint64_t staging_removed = 0;
    std::uint64_t bytes_freed = 0;
};

// Убирает брошенные каталоги сборки старше age. Каталог, которым кто-то
// занят, пропускается: занятость определяется блокировкой, а не временем,
// потому что долгая распаковка — это не заброшенность.
[[nodiscard]] Result<GcStats> collect_staging(const Root &, std::chrono::seconds age);

// Опубликованные поколения, от новых к старым.
[[nodiscard]] Result<std::vector<std::filesystem::path>> list_generations(const Root &);

} // namespace cork::setup
