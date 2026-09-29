#include "setup/config.hpp"

#include <algorithm>
#include <cctype>

#include <simdjson.h>

#include <fmt/format.h>

#include "base/fs.hpp"
#include "base/json.hpp"

namespace cork::setup {
namespace {

namespace oj = simdjson::ondemand;
namespace stdfs = std::filesystem;

std::string opt_string(oj::object &obj, std::string_view key) {
    auto field = obj[key];
    if (field.error() != simdjson::SUCCESS) {
        return {};
    }
    auto sv = field.get_string();
    return sv.error() == simdjson::SUCCESS ? std::string(sv.value_unsafe()) : std::string();
}

} // namespace

std::string Config::to_json() const {
    JsonWriter w;
    w.begin_object();
    w.field("schema", kSchemaName);
    w.field("schema_version", kSchemaVersion);
    w.field("generation", generation);
    w.field("created_by_version", created_by_version);
    w.field("created_by_commit", created_by_commit);
    w.field("host_arch", host_arch);
    w.field("dotnet_host", dotnet_host);
    w.field("msvc_version", msvc_version);
    w.field("platform_toolset", platform_toolset);
    w.field("sdk_version", sdk_version);
    w.field("wine_id", wine_id);
    w.field("helper_path", helper_path);
    w.field("helper_sha256", helper_sha256);

    w.key("toolsets").begin_object();
    for (const auto &[short_name, full_version] : toolsets) {
        w.field(short_name, full_version);
    }
    w.end_object();

    // targets — std::map, то есть порядок ключей задан сортировкой, а не
    // порядком вставки. Конфигурация входит в дайджест поколения, и
    // одинаковое содержимое обязано давать одинаковые байты.
    w.key("targets").begin_object();
    for (const auto &[arch, paths] : targets) {
        w.key(arch).begin_object();
        w.field("bin", paths.bin);
        w.field("sdk_bin", paths.sdk_bin);
        w.field("msbuild_bin", paths.msbuild_bin);
        w.field("debug_crt", paths.debug_crt);
        w.end_object();
    }
    w.end_object();

    w.end_object();
    return w.take();
}

Result<Config> Config::from_json(std::string_view json) {
    oj::parser parser;
    simdjson::padded_string padded(json);
    auto doc_result = parser.iterate(padded);
    if (doc_result.error() != simdjson::SUCCESS) {
        return err_config(std::string("parsing ") + kConfigFileName + ": " +
                          simdjson::error_message(doc_result.error()));
    }
    oj::document doc = std::move(doc_result.value_unsafe());

    auto root = doc.get_object();
    if (root.error() != simdjson::SUCCESS) {
        return err_config(std::string(kConfigFileName) + " is not a JSON object");
    }
    oj::object o = root.value_unsafe();

    const std::string schema = opt_string(o, "schema");
    if (schema != kSchemaName) {
        return err_config(fmt::format("{} has schema \"{}\", expected \"{}\"", kConfigFileName,
                                      schema, kSchemaName));
    }

    std::int64_t version = 0;
    if (auto f = o["schema_version"]; f.error() == simdjson::SUCCESS) {
        auto v = f.get_int64();
        if (v.error() == simdjson::SUCCESS) {
            version = v.value_unsafe();
        }
    }
    if (version != kSchemaVersion) {
        // Разные направления расхождения требуют от пользователя разного, и
        // сообщение обязано это сказать.
        if (version > kSchemaVersion) {
            return err_config(fmt::format(
                "{} was written by a newer cork (schema {} > {}); upgrade cork",
                kConfigFileName, version, kSchemaVersion));
        }
        return err_config(fmt::format(
            "{} has schema {} but this Cork expects {}; re-run `cork install`",
            kConfigFileName, version, kSchemaVersion));
    }

    Config c;
    c.generation = opt_string(o, "generation");
    c.created_by_version = opt_string(o, "created_by_version");
    c.created_by_commit = opt_string(o, "created_by_commit");
    c.host_arch = opt_string(o, "host_arch");
    c.dotnet_host = opt_string(o, "dotnet_host");
    c.msvc_version = opt_string(o, "msvc_version");
    c.platform_toolset = opt_string(o, "platform_toolset");
    c.sdk_version = opt_string(o, "sdk_version");
    c.wine_id = opt_string(o, "wine_id");
    c.helper_path = opt_string(o, "helper_path");
    c.helper_sha256 = opt_string(o, "helper_sha256");

    if (auto ts = o["toolsets"]; ts.error() == simdjson::SUCCESS) {
        if (auto obj = ts.get_object(); obj.error() == simdjson::SUCCESS) {
            for (auto field : obj.value_unsafe()) {
                auto key = field.unescaped_key();
                std::string_view value;
                if (key.error() == simdjson::SUCCESS &&
                    field.value().get_string().get(value) == simdjson::SUCCESS) {
                    c.toolsets.emplace(std::string(key.value_unsafe()), std::string(value));
                }
            }
        }
    }

    auto targets = o["targets"];
    if (targets.error() == simdjson::SUCCESS) {
        auto to = targets.get_object();
        if (to.error() == simdjson::SUCCESS) {
            for (auto field : to.value_unsafe()) {
                auto key = field.unescaped_key();
                if (key.error() != simdjson::SUCCESS) {
                    continue;
                }
                auto value = field.value().get_object();
                if (value.error() != simdjson::SUCCESS) {
                    return err_config(std::string(kConfigFileName) + ": target \"" +
                                      std::string(key.value_unsafe()) + "\" is not an object");
                }
                oj::object vo = value.value_unsafe();
                TargetPaths tp;
                tp.bin = opt_string(vo, "bin");
                tp.sdk_bin = opt_string(vo, "sdk_bin");
                tp.msbuild_bin = opt_string(vo, "msbuild_bin");
                tp.debug_crt = opt_string(vo, "debug_crt");
                c.targets.emplace(std::string(key.value_unsafe()), std::move(tp));
            }
        }
    }

    // Обязательные поля проверяются явно: установка без них нерабочая, и
    // узнать об этом лучше здесь, чем при первом же вызове компилятора.
    if (c.host_arch.empty()) {
        return err_config(std::string(kConfigFileName) + ": host_arch is missing");
    }
    if (c.msvc_version.empty()) {
        return err_config(std::string(kConfigFileName) + ": msvc_version is missing");
    }
    if (c.targets.empty()) {
        return err_config(std::string(kConfigFileName) + ": no targets");
    }
    return c;
}

Result<void> Config::save(const stdfs::path &generation_root) const {
    return fs::write_atomic(generation_root / kConfigFileName, std::string_view(to_json()));
}

Result<Config> Config::load(const stdfs::path &generation_root) {
    auto content = fs::read_file(generation_root / kConfigFileName);
    if (!content.has_value()) {
        return std::unexpected(
            std::move(content.error()).at("reading the installation config"));
    }
    return from_json(*content);
}

Result<stdfs::path> find_generation_root(const stdfs::path &start) {
    stdfs::path dir = start;
    // Поиск вверх, а не фиксированная глубина: раскладка может измениться, и
    // обёртки при этом не должны ломаться.
    for (int depth = 0; depth < 8; ++depth) {
        if (fs::is_regular_file(dir / kConfigFileName)) {
            return dir;
        }
        const stdfs::path parent = dir.parent_path();
        if (parent == dir) {
            break;
        }
        dir = parent;
    }
    return err_config("no " + std::string(kConfigFileName) + " found above " + start.string() +
                      "; this installation is incomplete, run `cork install`");
}

std::string normalise_target(std::string_view spelling) {
    std::string lower(spelling);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Триплет опознаётся по началу: всё, что идёт после архитектуры
    // (vendor, система, abi), для нас ничего не меняет — цель у Cork всегда
    // windows-msvc, другой она быть не может.
    const auto starts_with = [&lower](std::string_view prefix) {
        return lower.size() >= prefix.size() && lower.compare(0, prefix.size(), prefix) == 0;
    };

    if (lower == "x64" || lower == "x86_64" || lower == "amd64" || lower == "win64" ||
        starts_with("x86_64-") || starts_with("amd64-")) {
        return "x64";
    }
    if (lower == "x86" || lower == "i386" || lower == "i686" || lower == "win32" ||
        starts_with("i686-") || starts_with("i386-")) {
        return "x86";
    }
    if (lower == "arm64" || lower == "aarch64" || starts_with("aarch64-") ||
        starts_with("arm64-")) {
        return "arm64";
    }
    return {};
}

} // namespace cork::setup
