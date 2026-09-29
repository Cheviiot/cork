#pragma once

// Шаблон префикса Wine: один раз созданный, переносимый, готовый к
// клонированию.
//
// Без шаблона первая сборка в каждой новой сессии платит за wineboot —
// полминуты и полгигабайта записи. С шаблоном она платит за клонирование,
// которое на btrfs почти бесплатно.
//
// Но готовый префикс переносимым не является, и это главное, ради чего этот
// модуль существует. wineboot оставляет в нём следы того, кто и где его
// создал: символьные ссылки Documents, Desktop и прочие указывают в домашний
// каталог создателя, реестр хранит эти пути в Shell Folders, а dosdevices
// полон ссылок на устройства конкретной машины. Такой префикс, попав к
// другому пользователю, ведёт себя странно и непредсказуемо — например,
// компилятор пишет временные файлы в несуществующий каталог.
//
// Поэтому шаблон делается в два шага: деперсонализация убирает следы, а гейт
// переносимости проверяет, что их не осталось. Гейт обязателен — он
// единственное, что не даёт непереносимости просочиться в поставку, и
// сработать он должен на сборке, а не у пользователя.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "base/error.hpp"

namespace cork::setup {

struct DepersonaliseStats {
    std::uint64_t links_replaced = 0;   // симлинки в домашний каталог -> пустые каталоги
    std::uint64_t devices_removed = 0;  // com1..com9, lpt1..lpt9 из dosdevices
    std::uint64_t registry_edits = 0;   // строки в .reg, где путь заменён
    std::uint64_t shared_files = 0;     // файлов, разделённых с runtime через reflink
    std::uint64_t shared_bytes = 0;     // сколько места это освободило
};

// Убирает из префикса следы того, кто и где его создал.
[[nodiscard]] Result<DepersonaliseStats> depersonalise_prefix(const std::filesystem::path &prefix);

struct PortabilityViolation {
    std::filesystem::path file;
    std::uint64_t line = 0;  // 0 — сам путь, а не содержимое
    std::string detail;
};

// Ищет в префиксе всё, что привязывает его к этой машине: пути в домашний
// каталог, имя пользователя, DOS-форму того же самого.
//
// Возвращает список находок, а не первую: чинить их удобнее пачкой, а
// обрывать проверку на первой значит выяснять их по одной.
[[nodiscard]] Result<std::vector<PortabilityViolation>> check_prefix_portable(
    const std::filesystem::path &prefix, const std::string &home, const std::string &user);

// Возвращает профилю имя текущего пользователя: шаблон хранит его под
// нейтральным «default», потому что имя создателя шаблона к пользователю
// отношения не имеет. Без этого TEMP и Desktop указывали бы в каталог,
// которого в префиксе нет.
[[nodiscard]] Result<void> instantiate_prefix(const std::filesystem::path &prefix,
                                              const std::string &user);

// Делает из готового префикса шаблон: копирует, деперсонализует, проверяет.
// Непрошедшая проверка — отказ, и шаблон не появляется.
// Соответствует ли префикс этой сборке Wine.
//
// Wine кладёт в префикс .update-timestamp — время правки wine.inf того дерева,
// которым префикс создавали. Не совпало — при каждом запуске поднимается
// rundll32 setupapi,InstallHinfSection и прогоняет wine.inf заново. Ничего не
// ломается, просто каждая сессия дорожает на десятки секунд, и понять почему
// неоткуда: в выводе ничего не появляется.
//
// Отдельной функцией, потому что вопрос задают двое: doctor — чтобы
// предупредить, и template — чтобы не сделать шаблон из устаревшего префикса
// и не закрепить задержку навсегда.
[[nodiscard]] bool prefix_matches_wine(const std::filesystem::path &prefix,
                                       const std::filesystem::path &wine_runtime);

// Поднимает префикс с нуля: wineboot --init со stdio в /dev/null. Долго,
// порядка минуты, зато результат заведомо соответствует этому Wine.
[[nodiscard]] Result<void> boot_prefix(const std::filesystem::path &wine_runtime,
                                       const std::filesystem::path &prefix);

// wine_runtime нужен, чтобы разделить с ним одинаковые файлы: см.
// share_with_runtime в prefix.cpp. Пустой путь этот шаг пропускает.
[[nodiscard]] Result<DepersonaliseStats> build_prefix_template(
    const std::filesystem::path &prefix, const std::filesystem::path &destination,
    const std::filesystem::path &wine_runtime = {});

} // namespace cork::setup
