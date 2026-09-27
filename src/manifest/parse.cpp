#include "manifest/parse.hpp"

#include <simdjson.h>

namespace cork::manifest {
namespace {

namespace oj = simdjson::ondemand;

// Отсутствующее поле — норма: манифест не обязан заполнять все поля у каждого
// пакета. А вот поле неожиданного типа — это уже расхождение с форматом, и
// молчать о нём нельзя, иначе пакет тихо разберётся неправильно.
std::string opt_string(oj::object &obj, std::string_view key, bool &type_error) {
    auto field = obj[key];
    if (field.error() == simdjson::NO_SUCH_FIELD) {
        return {};
    }
    if (field.error() != simdjson::SUCCESS) {
        type_error = true;
        return {};
    }
    auto sv = field.get_string();
    if (sv.error() != simdjson::SUCCESS) {
        type_error = true;
        return {};
    }
    return std::string(sv.value_unsafe());
}

std::int64_t opt_int(oj::object &obj, std::string_view key, bool &type_error) {
    auto field = obj[key];
    if (field.error() == simdjson::NO_SUCH_FIELD) {
        return 0;
    }
    if (field.error() != simdjson::SUCCESS) {
        type_error = true;
        return 0;
    }
    auto v = field.get_int64();
    if (v.error() != simdjson::SUCCESS) {
        type_error = true;
        return 0;
    }
    return v.value_unsafe();
}

Dependency::Kind parse_kind(std::string_view text) {
    // Регистр в манифестах не выдержан, сравнение без учёта регистра.
    if (text.size() == 8 && (text[0] == 'O' || text[0] == 'o')) {
        return Dependency::Kind::Optional;
    }
    if (text.size() == 11 && (text[0] == 'R' || text[0] == 'r')) {
        return Dependency::Kind::Recommended;
    }
    return Dependency::Kind::Required;
}

bool parse_payloads(oj::value node, std::vector<Payload> &out) {
    auto arr = node.get_array();
    if (arr.error() != simdjson::SUCCESS) {
        return false;
    }
    for (auto item : arr.value_unsafe()) {
        auto obj = item.get_object();
        if (obj.error() != simdjson::SUCCESS) {
            return false;
        }
        oj::object o = obj.value_unsafe();
        bool bad = false;
        Payload p;
        p.file_name = opt_string(o, "fileName", bad);
        p.url = opt_string(o, "url", bad);
        p.sha256 = opt_string(o, "sha256", bad);
        p.size = opt_int(o, "size", bad);
        if (bad) {
            return false;
        }
        out.push_back(std::move(p));
    }
    return true;
}

bool parse_localized(oj::value node, std::vector<LocalizedResource> &out) {
    auto arr = node.get_array();
    if (arr.error() != simdjson::SUCCESS) {
        return false;
    }
    for (auto item : arr.value_unsafe()) {
        auto obj = item.get_object();
        if (obj.error() != simdjson::SUCCESS) {
            return false;
        }
        oj::object o = obj.value_unsafe();
        bool bad = false;
        LocalizedResource lr;
        lr.language = opt_string(o, "language", bad);
        lr.title = opt_string(o, "title", bad);
        lr.description = opt_string(o, "description", bad);
        lr.license = opt_string(o, "license", bad);
        if (bad) {
            return false;
        }
        out.push_back(std::move(lr));
    }
    return true;
}

// Зависимости в манифесте записаны двумя способами: "id": "версия" и
// "id": {"version": "...", "type": "Optional", "id": "другой-id"}. Объектная
// форма умеет переопределить сам id, и это не редкость.
bool parse_dependencies(oj::value node, std::vector<Dependency> &out) {
    auto obj = node.get_object();
    if (obj.error() != simdjson::SUCCESS) {
        return false;
    }
    for (auto field : obj.value_unsafe()) {
        auto key = field.unescaped_key();
        if (key.error() != simdjson::SUCCESS) {
            return false;
        }
        Dependency dep;
        dep.id = std::string(key.value_unsafe());

        oj::value value = field.value();
        if (value.type() == oj::json_type::string) {
            auto sv = value.get_string();
            if (sv.error() != simdjson::SUCCESS) {
                return false;
            }
            dep.version = VersionRange::parse(sv.value_unsafe());
        } else if (value.type() == oj::json_type::object) {
            auto inner = value.get_object();
            if (inner.error() != simdjson::SUCCESS) {
                return false;
            }
            oj::object io = inner.value_unsafe();
            bool bad = false;
            const std::string version = opt_string(io, "version", bad);
            const std::string type = opt_string(io, "type", bad);
            const std::string id = opt_string(io, "id", bad);
            if (bad) {
                return false;
            }
            dep.version = VersionRange::parse(version);
            dep.kind = parse_kind(type);
            if (!id.empty()) {
                dep.id = id;
            }
        } else {
            // Ни строка, ни объект — формат разошёлся с ожидаемым.
            return false;
        }
        out.push_back(std::move(dep));
    }
    return true;
}

bool parse_install_sizes(oj::value node, std::int64_t &total) {
    auto obj = node.get_object();
    if (obj.error() != simdjson::SUCCESS) {
        return false;
    }
    for (auto field : obj.value_unsafe()) {
        auto v = field.value().get_int64();
        if (v.error() == simdjson::SUCCESS) {
            total += v.value_unsafe();
        }
    }
    return true;
}

} // namespace

Result<Document> parse_installer_manifest(std::string_view json) {
    oj::parser parser;
    // simdjson требует запас за концом буфера; копия неизбежна, зато дальше
    // разбор идёт без единого выделения на каждое поле.
    simdjson::padded_string padded(json);

    auto doc_result = parser.iterate(padded);
    if (doc_result.error() != simdjson::SUCCESS) {
        return err_format(std::string("parsing installer manifest: ") +
                          simdjson::error_message(doc_result.error()));
    }
    oj::document doc = std::move(doc_result.value_unsafe());

    Document out;

    auto info = doc["info"];
    if (info.error() == simdjson::SUCCESS) {
        auto info_obj = info.get_object();
        if (info_obj.error() == simdjson::SUCCESS) {
            oj::object o = info_obj.value_unsafe();
            bool bad = false;
            out.product_display_version = opt_string(o, "productDisplayVersion", bad);
        }
    }

    auto packages = doc["packages"];
    if (packages.error() != simdjson::SUCCESS) {
        return err_format("installer manifest has no \"packages\" array");
    }
    auto arr = packages.get_array();
    if (arr.error() != simdjson::SUCCESS) {
        return err_format("installer manifest \"packages\" is not an array");
    }

    for (auto item : arr.value_unsafe()) {
        auto obj = item.get_object();
        if (obj.error() != simdjson::SUCCESS) {
            return err_format("installer manifest: package entry is not an object");
        }
        oj::object o = obj.value_unsafe();

        bool bad = false;
        Package p;
        p.id = opt_string(o, "id", bad);
        p.type = opt_string(o, "type", bad);
        p.version_text = opt_string(o, "version", bad);
        p.chip = opt_string(o, "chip", bad);
        p.machine_arch = opt_string(o, "machineArch", bad);
        p.product_arch = opt_string(o, "productArch", bad);
        p.language = opt_string(o, "language", bad);
        if (bad) {
            return err_format("installer manifest: package \"" + p.id +
                              "\" has a field of unexpected type");
        }
        if (p.id.empty()) {
            return err_format("installer manifest: package entry without an id");
        }
        p.version = Version::parse(p.version_text);

        if (auto f = o["payloads"]; f.error() == simdjson::SUCCESS) {
            if (!parse_payloads(f.value_unsafe(), p.payloads)) {
                return err_format("installer manifest: bad payloads in package " + p.id);
            }
        }
        if (auto f = o["localizedResources"]; f.error() == simdjson::SUCCESS) {
            if (!parse_localized(f.value_unsafe(), p.localized)) {
                return err_format("installer manifest: bad localizedResources in package " + p.id);
            }
        }
        if (auto f = o["dependencies"]; f.error() == simdjson::SUCCESS) {
            if (!parse_dependencies(f.value_unsafe(), p.dependencies)) {
                return err_format("installer manifest: bad dependencies in package " + p.id);
            }
        }
        if (auto f = o["installSizes"]; f.error() == simdjson::SUCCESS) {
            if (!parse_install_sizes(f.value_unsafe(), p.install_size)) {
                return err_format("installer manifest: bad installSizes in package " + p.id);
            }
        }

        out.packages.push_back(std::move(p));
    }

    return out;
}

Result<std::string> installer_manifest_url(std::string_view channel_json) {
    oj::parser parser;
    simdjson::padded_string padded(channel_json);

    auto doc_result = parser.iterate(padded);
    if (doc_result.error() != simdjson::SUCCESS) {
        return err_format(std::string("parsing channel manifest: ") +
                          simdjson::error_message(doc_result.error()));
    }
    oj::document doc = std::move(doc_result.value_unsafe());

    auto items = doc["channelItems"];
    if (items.error() != simdjson::SUCCESS) {
        return err_format("channel manifest has no \"channelItems\"");
    }
    auto arr = items.get_array();
    if (arr.error() != simdjson::SUCCESS) {
        return err_format("channel manifest \"channelItems\" is not an array");
    }

    for (auto item : arr.value_unsafe()) {
        auto obj = item.get_object();
        if (obj.error() != simdjson::SUCCESS) {
            continue;
        }
        oj::object o = obj.value_unsafe();
        bool bad = false;
        if (opt_string(o, "type", bad) != "Manifest") {
            continue;
        }
        auto payloads = o["payloads"];
        if (payloads.error() != simdjson::SUCCESS) {
            continue;
        }
        std::vector<Payload> list;
        if (!parse_payloads(payloads.value_unsafe(), list) || list.empty()) {
            continue;
        }
        if (!list.front().url.empty()) {
            return list.front().url;
        }
    }

    return err_not_found("channel manifest contains no installer manifest entry");
}

} // namespace cork::manifest
