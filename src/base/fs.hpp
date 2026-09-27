#pragma once

// Файловые операции, общие для всего проекта.
//
// Главное здесь — safe_join и symlink_target_inside. Распаковка архивов —
// самое опасное место во всём дереве, и проверка выхода за корень должна
// быть в одном месте, в нашем коде и под тестом, а не спрятана внутри чужой
// библиотеки. Причём проверять надо не только имя записи, но и цель символьной
// ссылки: архив может положить безобидное имя, целью которого будет ../../..

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "base/error.hpp"

namespace cork::fs {

namespace stdfs = std::filesystem;

// Присоединяет relative к base и отказывает, если результат выходит за пределы
// base. Отказ даётся и на абсолютный путь, и на путь с ".." любой глубины, и на
// корневое имя вроде "C:" — сравнение идёт по лексически нормализованному пути,
// без обращения к диску, поэтому работает и для ещё не существующих файлов.
[[nodiscard]] Result<stdfs::path> safe_join(const stdfs::path &base, std::string_view relative);

// Проверяет, что символьная ссылка, создаваемая в link_path с целью target, не
// уводит за пределы base. Относительная цель разрешается от каталога ссылки —
// именно так её потом разрешит ядро. Абсолютная цель отвергается всегда:
// законной нужды в ней внутри распаковываемого дерева нет.
[[nodiscard]] bool symlink_target_inside(const stdfs::path &base, const stdfs::path &link_path,
                                         std::string_view target);

[[nodiscard]] Result<void> mkdir_p(const stdfs::path &);

// Запись «всё или ничего»: временный файл рядом с целью, затем rename. Прерывание
// оставляет либо прежнее содержимое, либо новое, но никогда не половину. Временный
// файл создаётся в том же каталоге намеренно — rename атомарен только в пределах
// одной файловой системы.
[[nodiscard]] Result<void> write_atomic(const stdfs::path &, std::span<const std::byte> data,
                                        stdfs::perms perms = stdfs::perms::owner_read |
                                                             stdfs::perms::owner_write |
                                                             stdfs::perms::group_read |
                                                             stdfs::perms::others_read);
[[nodiscard]] Result<void> write_atomic(const stdfs::path &, std::string_view text,
                                        stdfs::perms perms = stdfs::perms::owner_read |
                                                             stdfs::perms::owner_write |
                                                             stdfs::perms::group_read |
                                                             stdfs::perms::others_read);

[[nodiscard]] Result<std::string> read_file(const stdfs::path &);

[[nodiscard]] bool is_dir(const stdfs::path &);
[[nodiscard]] bool is_regular_file(const stdfs::path &);
[[nodiscard]] bool exists_no_follow(const stdfs::path &);

} // namespace cork::fs
