#pragma once

// Выбор пакетов: из флагов командной строки — в плоский список того, что
// будет скачано.
//
// Четыре места, где резолвер обязан отказывать, а не молчать. Все они
// выглядят как мелочь и все дают одинаково неприятный итог: выбор, который
// считается успешным, а установка из него не собирает.
//
//   * Ненайденная обязательная зависимость — это отказ. Вернуть неполный граф
//     и поехать дальше значит доставить пользователю дерево без того, без
//     чего оно не работает.
//   * Ограничение версии зависимости обязано проверяться (см. Index).
//   * Явно запрошенная версия MSVC либо есть, либо её нет. Подстановка
//     default/latest с предупреждением — это выдача не того, что просили.
//   * Заданный --msvc-version задаёт версию, а не отменяет состав: Workload,
//     DIA, MSBuild и DevCmd должны остаться. Иначе установка выходит без
//     MSBuild.exe.

#include <optional>
#include <string>
#include <vector>

#include "base/error.hpp"
#include "manifest/index.hpp"

namespace cork::resolve {

// «Не задано / включить / выключить». Отдельный тип, а не указатель на bool:
// разыменовать по ошибке нечего.
using Tri = std::optional<bool>;

struct Options {
    std::vector<std::string> packages;  // явно перечисленные пользователем
    std::vector<std::string> ignore;
    std::vector<std::string> architectures;  // x86, x64, arm, arm64, host

    std::string host_arch = "x64";
    std::string language = "en";

    bool only_host = true;
    bool include_optional = false;
    bool skip_recommended = false;

    std::optional<std::string> msvc_version;
    // Дополнительные поколения компилятора, ставящиеся рядом с основным.
    // Нужны проектам, привязанным к конкретному PlatformToolset: подмена
    // работает только внутри поколения 14.x, а настоящий набор — всегда.
    std::vector<std::string> additional_msvc_versions;
    std::optional<std::string> sdk_version;

    Tri with_default;
    Tri with_workload;
    Tri with_msvc;
    Tri with_asan;
    Tri with_sdk;
    Tri with_atl;
    Tri with_dia;
    Tri with_msbuild;
    Tri with_devcmd;

    bool with_wdk = false;
};

struct SelectionIssue {
    enum class Kind {
        MissingRequiredDependency,
        IgnoredRequiredDependency,
        UnknownExplicitPackage,
        ExplicitVersionNotFound,
    };

    Kind kind;
    std::string subject;       // id пакета или версия
    std::string requested_by;  // кто его потребовал
    std::string detail;

    [[nodiscard]] std::string to_string() const;
};

struct Plan {
    // Топологически, без повторов: порядок детерминирован и воспроизводим.
    std::vector<const manifest::Package *> packages;

    // sha256 канонической записи выбора. Входит в имя поколения установки,
    // поэтому обязан зависеть только от того, что выбрано, и не зависеть от
    // порядка обхода хеш-таблиц.
    std::string digest;

    [[nodiscard]] std::int64_t download_size() const;
    [[nodiscard]] std::int64_t install_size() const;
};

// Отказ возвращается, если хоть одна проблема делает выбор недостоверным.
// Все такие проблемы собираются разом, а не по первой: пользователю нужен
// полный список, а не по одной ошибке за запуск.
Result<Plan> resolve_selection(const manifest::Index &, Options);

// Печать дерева зависимостей с той же фильтрацией, что и у resolve, — иначе
// показанное не совпадало бы с тем, что реально скачается.
std::string render_tree(const manifest::Index &, const Options &);

} // namespace cork::resolve
