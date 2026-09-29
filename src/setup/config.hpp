#pragma once

// Конфигурация установленного поколения — контракт между install и обёртками.
//
// Один файл в корне поколения, а не по копии в каждом bin/<arch>: копии
// расходятся, а источник истины должен быть один. Обёртка определяет свою
// цель по имени собственного каталога и поднимается к корню.
//
// У файла есть версия схемы, и несовпадение — внятный отказ, а не догадка.
// Без версии пустое поле означает сразу две разные вещи: «значения нет» и
// «установка сделана до того, как это поле появилось», — и различить их
// нечем.

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

#include "base/error.hpp"

namespace cork::setup {

inline constexpr const char *kConfigFileName = "cork.json";
inline constexpr const char *kSchemaName = "cork/install";
inline constexpr int kSchemaVersion = 1;

struct TargetPaths {
    std::string bin;          // vc/tools/msvc/<ver>/bin/Host<host>/<arch>
    std::string sdk_bin;      // kits/10/bin/<sdkver>/<host>
    std::string msbuild_bin;  // MSBuild/Current/Bin/<dotnet host>
    // VC/Redist/MSVC/<ver>/debug_nonredist/<arch>/Microsoft.VC<NNN>.DebugCRT
    //
    // Отладочные библиотеки времени выполнения. Программа, собранная с /MDd,
    // без них не стартует: msvcp140d.dll и vcruntime140_1d.dll — не те, что
    // Wine отдаёт как builtin, у него есть только выпускные.
    //
    // Не выводится по формуле, а находится при установке: в имени каталога
    // стоит номер набора инструментов, а версия редиста не обязана совпадать
    // с версией компилятора. Пусто означает ровно одно — на диске такого
    // каталога не нашлось.
    std::string debug_crt;
};

struct Config {
    std::string generation;
    std::string created_by_version;
    std::string created_by_commit;

    std::string host_arch;    // x64 или arm64
    std::string dotnet_host;  // amd64 или arm64

    std::string msvc_version;
    std::string platform_toolset;  // «145» и т.п., пусто если не определился

    // Все установленные наборы инструментов: короткое имя поколения («142»)
    // и полная версия каталога («14.29.30133»). Их может быть несколько:
    // проект, привязанный к конкретному PlatformToolset, должен собираться
    // настоящим набором, а подмена внутри поколения 14.x — запасной путь, а
    // не основной.
    std::map<std::string, std::string> toolsets;
    std::string sdk_version;       // пусто, пока SDK не установлен

    std::map<std::string, TargetPaths> targets;  // по целевой архитектуре

    std::string wine_id;        // версия своей сборки Wine
    std::string helper_path;    // относительно корня поколения
    std::string helper_sha256;

    [[nodiscard]] std::string to_json() const;
    static Result<Config> from_json(std::string_view);

    [[nodiscard]] Result<void> save(const std::filesystem::path &generation_root) const;
    static Result<Config> load(const std::filesystem::path &generation_root);
};

// Поднимается от каталога обёртки к корню поколения, в котором лежит
// cork.json. Обёртка запускается как <root>/bin/<arch>/cl, поэтому корень
// на два уровня выше — но поиск идёт вверх, а не по фиксированной глубине:
// так раскладка может измениться, не ломая обёртки.
Result<std::filesystem::path> find_generation_root(const std::filesystem::path &start);

// Приводит написание цели к тому, которым cork называет её внутри: x64, x86,
// arm64.
//
// Зачем вообще несколько написаний. Cork говорит «x64», а мир вокруг —
// триплетами: clang, rust, vcpkg и половина документации знают
// «x86_64-pc-windows-msvc». Человек, пришедший из тех инструментов, наберёт
// то, к чему привык, и получит «no target», хотя цель есть. Принимать оба
// написания стоит одной таблицы, а не заставлять переучиваться.
//
// Пустая строка на выходе означает, что написание неизвестно; вызывающий
// решает, ошибка это или повод взять умолчание.
[[nodiscard]] std::string normalise_target(std::string_view spelling);

} // namespace cork::setup
