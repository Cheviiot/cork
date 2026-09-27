#pragma once

// Индекс пакетов манифеста по id.
//
// Поиск учитывает ограничение версии, а не только chip и machineArch. Это
// легко упустить: ограничение приходит вместе с прочими условиями и выглядит
// как одно из них, а стоит его не проверить — запрос «нужна 2.0» при наличии
// 1.0 и 2.0 возвращает 1.0.
//
// Второе: отсутствие пакета и отсутствие подходящего варианта — разные
// исходы. Один ответ на оба вызывающий различить не сможет, а «такого пакета
// нет в манифесте» и «пакет есть, но не той версии» требуют от пользователя
// совершенно разных действий.

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/error.hpp"
#include "manifest/model.hpp"

namespace cork::manifest {

struct Constraints {
    std::optional<std::string> chip;
    std::optional<std::string> machine_arch;
    std::optional<VersionRange> version;
};

class Index {
public:
    // hostArch и language задают порядок предпочтения вариантов одного id.
    static Index build(const Document &, std::string_view host_arch, std::string_view language);

    [[nodiscard]] bool contains(std::string_view id) const;

    // Ошибка Code::NotFound с разным текстом: «нет такого пакета» против
    // «есть, но ни один вариант не подходит под ограничения». Во втором
    // случае в сообщение попадает список того, что есть, — без него
    // пользователю нечего делать с отказом.
    [[nodiscard]] Result<const Package *> find(std::string_view id, const Constraints &) const;

    [[nodiscard]] std::vector<const Package *> by_type(std::string_view type) const;
    [[nodiscard]] std::vector<std::string> ids_with_prefix(std::string_view lowercase_prefix) const;

private:
    // Ключ — id в нижнем регистре: манифест не выдерживает регистр, а
    // пользователь тем более. std::map, а не unordered_map, чтобы обход был
    // детерминированным — от этого зависит воспроизводимость выбора.
    std::map<std::string, std::vector<const Package *>, std::less<>> by_id_;
};

} // namespace cork::manifest
