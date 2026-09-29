#pragma once

// Настройка распакованного дерева: обёртки, конфигурация, PE-хелпер.
//
// Здесь нет ни одного шага «по возможности». Шаг, который при провале
// оставляет сообщение в выводе и позволяет объявить установку успешной, —
// это отложенная поломка: она проявится при первой сборке, и связать её с
// установкой уже не получится. Хелпер вшит в бинарник и просто выкладывается
// на диск, а всё остальное либо получается, либо установка отказывает.

#include <filesystem>
#include <map>
#include <span>
#include <string>

#include "base/error.hpp"

namespace cork::setup {

struct InstallOptions {
    std::filesystem::path generation_root;
    std::filesystem::path self_binary;   // копируется в bin/<arch>
    std::span<const std::byte> helper;   // вшитый cork-helper.exe
    std::span<const std::byte> cmake_toolchain;  // share/cork-toolchain.cmake
    // Шаблоны для чужих сборочных систем. Подстановки заполняются здесь, а
    // не в CLI: пути зависят от корня установки и набора целей, и знает их
    // только тот, кто раскладывает поколение.
    std::span<const std::byte> vcpkg_triplet;
    std::span<const std::byte> meson_cross;
    std::filesystem::path install_root;  // куда показывает toolchains/current
    std::string wine_id;
    std::string version;
    std::string commit;
    // Короткое имя настоящего набора инструментов, например «145». Пустое,
    // если на диске его не нашлось.
    std::string platform_toolset;
};

struct InstallReport {
    std::string msvc_version;
    std::string sdk_version;
    std::vector<std::string> targets;
};

Result<InstallReport> install(const InstallOptions &);

// Версия MSVC, лежащая в дереве. Сравнение по компонентам, а не лексическое:
// «последний по алфавиту каталог» работает ровно до тех пор, пока номера
// сборок одинаковой длины.
Result<std::string> find_msvc_version(const std::filesystem::path &generation_root);

// Все установленные наборы: короткое имя поколения («142») -> полная версия
// каталога («14.29.30133»). Короткое имя выводится из версии, потому что в
// дереве связь между ними нигде не записана, а .vcxproj пишет именно его.
std::map<std::string, std::string> find_toolsets(const std::filesystem::path &generation_root);

// Версия Windows SDK, если он установлен. Пусто — SDK ещё нет, и это не
// ошибка: срез, на котором собирается программа без заголовков, живёт без него.
std::string find_sdk_version(const std::filesystem::path &generation_root);

} // namespace cork::setup
