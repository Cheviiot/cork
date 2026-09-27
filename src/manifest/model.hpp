#pragma once

// Модель установочного манифеста Visual Studio — того самого, которым
// пользуется собственный установщик Microsoft.
//
// Разбор отделён от выбора пакетов намеренно: здесь только то, что написано в
// файле, без единого решения о том, что ставить. Все решения — в resolve.

#include <cstdint>
#include <string>
#include <vector>

#include "manifest/version.hpp"

namespace cork::manifest {

struct Payload {
    std::string file_name;
    std::string url;
    std::string sha256;
    std::int64_t size = 0;

    // Имя без каталогов: в манифесте fileName иногда приходит с путём внутри
    // пакета, а нам нужно имя файла на диске.
    [[nodiscard]] std::string base_name() const;
};

struct Dependency {
    // Ключом в JSON служит id зависимости, но объектная форма умеет его
    // переопределить полем "id". Здесь уже разрешённое значение.
    std::string id;
    VersionRange version;

    enum class Kind { Required, Recommended, Optional };
    Kind kind = Kind::Required;

    [[nodiscard]] const char *kind_name() const;
};

struct LocalizedResource {
    std::string language;
    std::string title;
    std::string description;
    std::string license;
};

struct Package {
    std::string id;
    std::string type;
    std::string version_text;
    Version version;

    std::string chip;
    std::string machine_arch;
    std::string product_arch;
    std::string language;

    std::vector<Payload> payloads;
    // Порядок сохраняется таким, каким он в файле: разрешение зависимостей
    // должно быть детерминированным, а обход по хеш-таблице этого не даёт.
    std::vector<Dependency> dependencies;
    std::vector<LocalizedResource> localized;
    std::int64_t install_size = 0;

    // id + версия + архитектуры. Отличает варианты одного пакета друг от
    // друга и служит именем в кэше.
    [[nodiscard]] std::string key() const;
    [[nodiscard]] std::int64_t download_size() const;
    [[nodiscard]] const LocalizedResource *localized_for(std::string_view language) const;
};

struct Document {
    std::string product_display_version;
    std::vector<Package> packages;
};

} // namespace cork::manifest
