#include "setup/receipt.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>

#include <fmt/format.h>
#include <simdjson.h>

#include "base/fs.hpp"
#include "base/json.hpp"
#include "base/sha256.hpp"

namespace cork::setup {
namespace {

namespace oj = simdjson::ondemand;
namespace stdfs = std::filesystem;

struct NamedState {
    State state;
    const char *name;
};

// Имена состояний — часть формата файла, а не подпись в выводе. Менять их
// нельзя: старый receipt должен читаться новым cork.
constexpr NamedState kStates[] = {
    {State::Resolved, "resolved"}, {State::Fetched, "fetched"},
    {State::Staged, "staged"},     {State::Verified, "verified"},
    {State::Published, "published"}, {State::Buildable, "buildable"},
};

std::string opt_string(oj::object &obj, std::string_view key) {
    auto field = obj[key];
    if (field.error() != simdjson::SUCCESS) {
        return {};
    }
    auto sv = field.get_string();
    return sv.error() == simdjson::SUCCESS ? std::string(sv.value_unsafe()) : std::string();
}

std::int64_t opt_int(oj::object &obj, std::string_view key) {
    auto field = obj[key];
    if (field.error() != simdjson::SUCCESS) {
        return 0;
    }
    auto v = field.get_int64();
    return v.error() == simdjson::SUCCESS ? v.value_unsafe() : 0;
}

} // namespace

const char *state_name(State s) {
    for (const auto &n : kStates) {
        if (n.state == s) {
            return n.name;
        }
    }
    return "unknown";
}

Result<State> state_from_name(std::string_view name) {
    for (const auto &n : kStates) {
        if (name == n.name) {
            return n.state;
        }
    }
    return err_config(fmt::format("unknown state '{}'", name));
}

std::string now_iso8601() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    return fmt::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", tm.tm_year + 1900, tm.tm_mon + 1,
                       tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

std::string selection_digest(std::vector<std::string> ids) {
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    Sha256 sha;
    for (const auto &id : ids) {
        // Длина перед значением: иначе выбор {"a", "bc"} и {"ab", "c"} дали бы
        // один дайджест.
        sha.update(fmt::format("{}\t{}\n", id.size(), id));
    }
    return "sha256:" + Sha256::to_hex(sha.finish());
}

Result<State> Receipt::state() const {
    if (log.empty()) {
        return err_config("receipt has no state log");
    }
    State best = log.front().state;
    for (const auto &e : log) {
        best = std::max(best, e.state);
    }
    return best;
}

bool Receipt::at_least(State s) const {
    auto current = state();
    return current.has_value() && *current >= s;
}

Result<void> Receipt::advance(State to, std::string_view version) {
    if (!log.empty()) {
        auto current = state();
        if (current.has_value() && to < *current) {
            return err_internal(fmt::format("cannot go back from '{}' to '{}'",
                                            state_name(*current), state_name(to)));
        }
    } else if (to != State::Resolved) {
        return err_internal(
            fmt::format("the first state must be 'resolved', not '{}'", state_name(to)));
    }
    log.push_back(StateEntry{to, now_iso8601(), std::string(version)});
    return {};
}

std::string Receipt::to_json() const {
    JsonWriter w;
    w.begin_object();
    w.field("schema", kReceiptSchemaName);
    w.field("schema_version", kReceiptSchemaVersion);

    w.key("source").begin_object();
    w.field("channel_url", source.channel_url);
    w.field("manifest_url", source.manifest_url);
    w.field("manifest_sha256", source.manifest_sha256);
    w.field("product_version", source.product_version);
    w.end_object();

    w.key("selection").begin_object();
    w.field("digest", selection.digest);
    w.key("packages").begin_array();
    for (const auto &id : selection.package_ids) {
        w.value(id);
    }
    w.end_array();
    w.end_object();

    w.key("payloads").begin_array();
    for (const auto &p : payloads) {
        w.begin_object();
        w.field("package", p.package_id);
        w.field("file", p.file_name);
        w.field("sha256", p.sha256);
        w.field("size", p.size);
        w.end_object();
    }
    w.end_array();

    w.key("unpacked").begin_array();
    for (const auto &u : unpacked) {
        w.begin_object();
        w.field("package", u.package_id);
        w.field("source", u.source);
        w.field("kind", u.kind);
        w.field("files", u.files);
        w.end_object();
    }
    w.end_array();

    w.key("patches").begin_array();
    for (const auto &p : patches) {
        w.begin_object();
        w.field("patch", p.patch);
        w.field("path", p.path);
        w.field("pre_sha256", p.pre_sha256);
        w.field("post_sha256", p.post_sha256);
        w.end_object();
    }
    w.end_array();

    w.key("tree").begin_object();
    w.field("digest", tree.digest);
    w.field("mode", tree.mode == DigestMode::Content ? "content" : "structure");
    w.field("files", tree.stats.files);
    w.field("directories", tree.stats.directories);
    w.field("symlinks", tree.stats.symlinks);
    w.field("bytes", tree.stats.bytes);
    w.end_object();

    w.key("wine").begin_object();
    w.field("id", wine.id);
    w.field("symlinks", wine.symlinks);
    w.field("mono_files", wine.mono_files);
    w.end_object();

    w.key("log").begin_array();
    for (const auto &e : log) {
        w.begin_object();
        w.field("state", state_name(e.state));
        w.field("at", e.at);
        w.field("version", e.version);
        w.end_object();
    }
    w.end_array();

    w.end_object();
    return w.take();
}

Result<Receipt> Receipt::from_json(std::string_view text) {
    simdjson::padded_string padded(text);
    oj::parser parser;
    auto doc = parser.iterate(padded);
    if (doc.error() != simdjson::SUCCESS) {
        return err_format(fmt::format("receipt is not valid JSON: {}",
                                      simdjson::error_message(doc.error())));
    }
    oj::object root;
    if (doc.get_object().get(root) != simdjson::SUCCESS) {
        return err_format("receipt is not a JSON object");
    }

    Receipt r;
    if (opt_string(root, "schema") != kReceiptSchemaName) {
        return err_config("receipt has a different schema");
    }
    const auto version = opt_int(root, "schema_version");
    if (version != kReceiptSchemaVersion) {
        // Внятный отказ вместо догадки: чужая версия схемы означает, что
        // поколение создано другим cork, и молча читать из него половину
        // полей хуже, чем сказать об этом.
        return err_config(fmt::format("receipt schema version is {}, this build understands {}",
                                      version, kReceiptSchemaVersion));
    }

    if (oj::object o; root["source"].get_object().get(o) == simdjson::SUCCESS) {
        r.source.channel_url = opt_string(o, "channel_url");
        r.source.manifest_url = opt_string(o, "manifest_url");
        r.source.manifest_sha256 = opt_string(o, "manifest_sha256");
        r.source.product_version = opt_string(o, "product_version");
    }

    if (oj::object o; root["selection"].get_object().get(o) == simdjson::SUCCESS) {
        r.selection.digest = opt_string(o, "digest");
        if (oj::array a; o["packages"].get_array().get(a) == simdjson::SUCCESS) {
            for (auto v : a) {
                std::string_view sv;
                if (v.get_string().get(sv) == simdjson::SUCCESS) {
                    r.selection.package_ids.emplace_back(sv);
                }
            }
        }
    }

    if (oj::array a; root["payloads"].get_array().get(a) == simdjson::SUCCESS) {
        for (auto v : a) {
            oj::object o;
            if (v.get_object().get(o) != simdjson::SUCCESS) {
                continue;
            }
            PayloadRecord p;
            p.package_id = opt_string(o, "package");
            p.file_name = opt_string(o, "file");
            p.sha256 = opt_string(o, "sha256");
            p.size = opt_int(o, "size");
            r.payloads.push_back(std::move(p));
        }
    }

    if (oj::array a; root["unpacked"].get_array().get(a) == simdjson::SUCCESS) {
        for (auto v : a) {
            oj::object o;
            if (v.get_object().get(o) != simdjson::SUCCESS) {
                continue;
            }
            UnpackRecord u;
            u.package_id = opt_string(o, "package");
            u.source = opt_string(o, "source");
            u.kind = opt_string(o, "kind");
            u.files = static_cast<std::uint64_t>(opt_int(o, "files"));
            r.unpacked.push_back(std::move(u));
        }
    }

    if (oj::array a; root["patches"].get_array().get(a) == simdjson::SUCCESS) {
        for (auto v : a) {
            oj::object o;
            if (v.get_object().get(o) != simdjson::SUCCESS) {
                continue;
            }
            PatchRecord p;
            p.patch = opt_string(o, "patch");
            p.path = opt_string(o, "path");
            p.pre_sha256 = opt_string(o, "pre_sha256");
            p.post_sha256 = opt_string(o, "post_sha256");
            r.patches.push_back(std::move(p));
        }
    }

    if (oj::object o; root["tree"].get_object().get(o) == simdjson::SUCCESS) {
        r.tree.digest = opt_string(o, "digest");
        r.tree.mode = opt_string(o, "mode") == "content" ? DigestMode::Content
                                                         : DigestMode::Structure;
        r.tree.stats.files = static_cast<std::uint64_t>(opt_int(o, "files"));
        r.tree.stats.directories = static_cast<std::uint64_t>(opt_int(o, "directories"));
        r.tree.stats.symlinks = static_cast<std::uint64_t>(opt_int(o, "symlinks"));
        r.tree.stats.bytes = static_cast<std::uint64_t>(opt_int(o, "bytes"));
    }

    if (oj::object o; root["wine"].get_object().get(o) == simdjson::SUCCESS) {
        r.wine.id = opt_string(o, "id");
        r.wine.symlinks = static_cast<std::uint64_t>(opt_int(o, "symlinks"));
        r.wine.mono_files = static_cast<std::uint64_t>(opt_int(o, "mono_files"));
    }

    if (oj::array a; root["log"].get_array().get(a) == simdjson::SUCCESS) {
        for (auto v : a) {
            oj::object o;
            if (v.get_object().get(o) != simdjson::SUCCESS) {
                continue;
            }
            auto s = state_from_name(opt_string(o, "state"));
            if (!s) {
                return std::unexpected(std::move(s).error());
            }
            r.log.push_back(StateEntry{*s, opt_string(o, "at"), opt_string(o, "version")});
        }
    }
    if (r.log.empty()) {
        return err_config("receipt has an empty state log");
    }
    return r;
}

Result<void> Receipt::save(const stdfs::path &root) const {
    const std::string text = to_json();
    if (text.empty()) {
        return err_internal("receipt did not serialise");
    }
    return fs::write_atomic(root / kReceiptFileName, text);
}

Result<Receipt> Receipt::load(const stdfs::path &root) {
    auto text = fs::read_file(root / kReceiptFileName);
    if (!text) {
        return std::unexpected(std::move(text).error().at("reading the receipt"));
    }
    return from_json(*text);
}

} // namespace cork::setup
