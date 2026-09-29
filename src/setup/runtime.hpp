#pragma once

// Установка рантайма Wine из опубликованного артефакта.
//
// Рантайм — единственная часть установки, которую Cork не может ни собрать на
// месте, ни взять у системы: сборка Wine занимает десятки минут, а системный
// Wine не поддерживается сознательно. Значит он приезжает готовым, и вопрос
// только в том, откуда и с какой проверкой.
//
// Список закреплений вшит в бинарник. Внешний файл рядом с программой — ещё
// одна вещь, которая теряется при копировании, и ещё один путь отказа в самом
// начале, когда ещё ничего не установлено.
//
// Проверяется хеш, а не источник. Артефакт лежит на github, и доверять тут
// нечему: подмена файла по тому же адресу не отличима от обычной загрузки
// ничем, кроме содержимого. Поэтому sha256 закреплён в дереве и обновляется
// рукой человека, а не тем, что ответил сервер.

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "base/error.hpp"
#include "setup/generation.hpp"
#include "store/store.hpp"

namespace cork::setup {

struct RuntimePin {
    std::string wine_id;
    std::string host_arch;
    std::string sha256;
    std::string url;
};

// Разбор списка закреплений. Строка неверной формы — отказ, а не пропуск:
// пропущенная строка означает «рантайма нет», и разбираться в этом пришлось
// бы уже на машине, где ничего не установлено.
[[nodiscard]] Result<std::vector<RuntimePin>> parse_runtime_pins(std::string_view text);

// Вшитый список.
[[nodiscard]] Result<std::vector<RuntimePin>> builtin_runtime_pins();

// Архитектура хоста в тех же словах, что и в списке: amd64, arm64.
[[nodiscard]] std::string_view host_arch();

struct RuntimeInstall {
    bool already_present = false;
    std::filesystem::path path;
    std::uint64_t files = 0;
    std::uint64_t symlinks = 0;
};

// Ставит рантайм в root.runtime(wine_id), если его там ещё нет.
//
// Распаковка идёт в сторону и переименовывается на место: прерывание не
// должно оставить полураспакованное дерево, у которого есть bin/wine и нет
// половины DLL. Такое дерево выглядит установленным и не работает.
//
// Хранилище приходит снаружи, а не заводится здесь: у download его путь может
// быть переопределён --store, и качать рантайм мимо того же кэша значило бы
// скачать его второй раз при первой же переустановке.
[[nodiscard]] Result<RuntimeInstall> install_runtime(const Root &, std::string_view wine_id,
                                                     const std::vector<RuntimePin> &,
                                                     store::ArtifactStore &, store::Progress &);

} // namespace cork::setup
