#include "resolve/resolve.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

#include <fmt/format.h>

#include "base/sha256.hpp"

namespace cork::resolve {
namespace {

using manifest::Constraints;
using manifest::Dependency;
using manifest::Index;
using manifest::Package;

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool contains(const std::vector<std::string> &list, std::string_view v) {
    return std::find(list.begin(), list.end(), v) != list.end();
}

bool ignored(const Options &o, std::string_view id) {
    const std::string lower = to_lower(id);
    for (const auto &i : o.ignore) {
        if (to_lower(i) == lower) {
            return true;
        }
    }
    return false;
}

struct MsvcEntry {
    const char *generation;  // "15" или "16" — схема имён пакетов
    const char *sdk;
    const char *tool_version;
};

// Соответствие --msvc-version тому, какой SDK тянется по умолчанию и какой
// фрагмент версии стоит в id пакетов. Таблица заполнена наблюдением за тем,
// как Microsoft называет свои пакеты: вывести это правило неоткуда, его можно
// только записать.
const std::map<std::string, MsvcEntry> &msvc_table() {
    static const std::map<std::string, MsvcEntry> table = {
        {"preview", {"16", "", "Preview"}},
        {"16.0", {"16", "10.0.17763", "14.20"}},
        {"16.1", {"16", "10.0.18362", "14.21"}},
        {"16.2", {"16", "10.0.18362", "14.22"}},
        {"16.3", {"16", "10.0.18362", "14.23"}},
        {"16.4", {"16", "10.0.18362", "14.24"}},
        {"16.5", {"16", "10.0.18362", "14.25"}},
        {"16.6", {"16", "10.0.18362", "14.26"}},
        {"16.7", {"16", "10.0.18362", "14.27"}},
        {"16.8", {"16", "10.0.18362", "14.28"}},
        {"16.9", {"16", "10.0.19041", "14.28.16.9"}},
        {"16.10", {"16", "10.0.19041", "14.29.16.10"}},
        {"16.11", {"16", "10.0.19041", "14.29.16.11"}},
        {"17.0", {"16", "10.0.19041", "14.30.17.0"}},
        {"17.1", {"16", "10.0.19041", "14.31.17.1"}},
        {"17.2", {"16", "10.0.19041", "14.32.17.2"}},
        {"17.3", {"16", "10.0.19041", "14.33.17.3"}},
        {"17.4", {"16", "10.0.22621", "14.34.17.4"}},
        {"17.5", {"16", "10.0.22621", "14.35.17.5"}},
        {"17.6", {"16", "10.0.22621", "14.36.17.6"}},
        {"17.7", {"16", "10.0.22621", "14.37.17.7"}},
        {"17.8", {"16", "10.0.22621", "14.38.17.8"}},
        {"17.9", {"16", "10.0.22621", "14.39.17.9"}},
        {"17.10", {"16", "10.0.22621", "14.40.17.10"}},
        {"17.11", {"16", "10.0.22621", "14.41.17.11"}},
        {"17.12", {"16", "10.0.22621", "14.42.17.12"}},
        {"17.13", {"16", "10.0.22621", "14.43.17.13"}},
        {"17.14", {"16", "10.0.26100", "14.44.17.14"}},
        {"18.0", {"16", "10.0.26100", "14.50.18.0"}},
        {"15.4", {"15", "10.0.16299", "14.11"}},
        {"15.5", {"15", "10.0.16299", "14.12"}},
        {"15.6", {"15", "10.0.16299", "14.13"}},
        {"15.7", {"15", "10.0.17134", "14.14"}},
        {"15.8", {"15", "10.0.17134", "14.15"}},
        {"15.9", {"15", "10.0.17763", "14.16"}},
    };
    return table;
}

// Разбор id вида "Win10SDK_10.0.19041" или "Win11SDK_10.0.26100".
//
// Отдельная функция с явным «не распознал» вместо среза по месту. Срез
// key[9:] без проверки длины выглядит безобидно ровно до первого id, который
// равен "Win10SDK" целиком, — а такие в манифесте есть.
struct SdkId {
    std::string family;   // win10sdk / win11sdk
    std::string version;  // 10.0.26100
};

std::optional<SdkId> parse_sdk_id(std::string_view lowercase_id) {
    for (const auto *family : {"win10sdk", "win11sdk"}) {
        const std::string_view f(family);
        if (lowercase_id.rfind(f, 0) != 0) {
            continue;
        }
        std::string_view rest = lowercase_id.substr(f.size());
        if (rest.empty()) {
            return std::nullopt;  // «Win10SDK» без версии — не наш случай
        }
        if (rest.front() == '_' || rest.front() == '-' || rest.front() == '.') {
            rest.remove_prefix(1);
        }
        if (rest.empty()) {
            return std::nullopt;
        }
        return SdkId{std::string(f), std::string(rest)};
    }
    return std::nullopt;
}

void add_if_wanted(Options &o, Tri flag, const char *pkg) {
    if (!flag.has_value()) {
        return;
    }
    if (*flag) {
        o.packages.emplace_back(pkg);
    } else {
        o.ignore.emplace_back(pkg);
    }
}

// --- раскрытие графа зависимостей --------------------------------------------

struct Expander {
    const Index &idx;
    const Options &opts;
    std::set<std::string> included;  // по Package::key()
    std::vector<const Package *> out;
    std::vector<SelectionIssue> issues;

    void visit(const std::string &id, const Constraints &c, const std::string &requested_by,
               bool required) {
        if (ignored(opts, id)) {
            // Игнорирование обязательной зависимости — не то же самое, что
            // игнорирование запрошенного пользователем пакета: первое почти
            // наверняка ошибка, и молчать о нём нельзя.
            if (required && !requested_by.empty()) {
                issues.push_back({SelectionIssue::Kind::IgnoredRequiredDependency, id,
                                  requested_by, "required dependency is in --ignore"});
            }
            return;
        }

        auto found = idx.find(id, c);
        if (!found.has_value()) {
            if (!required) {
                return;  // необязательную можно пропустить молча
            }
            issues.push_back({requested_by.empty()
                                  ? SelectionIssue::Kind::UnknownExplicitPackage
                                  : SelectionIssue::Kind::MissingRequiredDependency,
                              id, requested_by, found.error().message});
            return;
        }

        const Package *p = *found;
        if (opts.only_host && !host_compatible(*p)) {
            return;
        }
        if (!target_compatible(*p)) {
            return;
        }

        const std::string key = p->key();
        if (!included.insert(key).second) {
            return;
        }
        out.push_back(p);

        // Порядок обхода — тот, в котором зависимости записаны в манифесте.
        // Он детерминирован самим разбором, поэтому сортировать нечего: выбор
        // воспроизводим без дополнительных усилий.
        for (const auto &dep : p->dependencies) {
            if (dep.kind == Dependency::Kind::Optional && !opts.include_optional) {
                continue;
            }
            if (dep.kind == Dependency::Kind::Recommended && opts.skip_recommended) {
                continue;
            }
            Constraints dc;
            if (!dep.version.is_any()) {
                dc.version = dep.version;
            }
            visit(dep.id, dc, p->id, dep.kind == Dependency::Kind::Required);
        }
    }

    // Часть пакетов кодирует хостовую архитектуру прямо в id, например
    // Microsoft.VisualCpp.Tools.HostARM64.TargetX64.
    bool host_compatible(const Package &p) const {
        if (opts.host_arch.empty()) {
            return true;
        }
        const std::string id = to_lower(p.id);
        for (const auto *a : {"x86", "x64", "arm64"}) {
            if (id.find(std::string("host") + a) != std::string::npos) {
                return opts.host_arch == a;
            }
        }
        for (const auto *field : {&p.chip, &p.machine_arch, &p.product_arch}) {
            const std::string v = to_lower(*field);
            if (v.empty() || v == "neutral") {
                continue;
            }
            if (v != opts.host_arch) {
                return false;
            }
        }
        return true;
    }

    bool target_compatible(const Package &p) const {
        if (opts.architectures.empty()) {
            return true;
        }
        const std::string id = to_lower(p.id);
        for (const auto *a : {"x86", "x64", "arm64", "arm"}) {
            const std::string needle = std::string(".target") + a;
            const std::size_t at = id.find(needle);
            if (at == std::string::npos) {
                continue;
            }
            // ".targetarm" не должен совпасть с ".targetarm64": проверяем,
            // что дальше не буква и не цифра.
            const std::size_t after = at + needle.size();
            if (after < id.size() && (std::isalnum(static_cast<unsigned char>(id[after])) != 0)) {
                continue;
            }
            return contains(opts.architectures, a);
        }
        return true;
    }
};

} // namespace

std::string SelectionIssue::to_string() const {
    switch (kind) {
        case Kind::MissingRequiredDependency:
            return fmt::format("required dependency \"{}\" of \"{}\" is unavailable: {}", subject,
                               requested_by, detail);
        case Kind::IgnoredRequiredDependency:
            return fmt::format("\"{}\" is required by \"{}\" but was excluded with --ignore",
                               subject, requested_by);
        case Kind::UnknownExplicitPackage:
            return fmt::format("requested package \"{}\" is unavailable: {}", subject, detail);
        case Kind::ExplicitVersionNotFound:
            return fmt::format("requested MSVC version {} is not in this manifest{}", subject,
                               detail.empty() ? "" : "; " + detail);
    }
    return subject;
}

std::int64_t Plan::download_size() const {
    std::int64_t sum = 0;
    for (const auto *p : packages) {
        sum += p->download_size();
    }
    return sum;
}

std::int64_t Plan::install_size() const {
    std::int64_t sum = 0;
    for (const auto *p : packages) {
        sum += p->install_size;
    }
    return sum;
}

namespace {

// Выбор пакетов одного поколения компилятора.
//
// Вынесено отдельно, потому что поколений может быть несколько: проект,
// привязанный к v142, должен собираться настоящим v142, а не тем, что
// подвернулось. Дерево это выдерживает по устройству — Microsoft хранит
// наборы рядом, в vc/tools/msvc/<версия>/, — так что от нас требуется только
// выбрать пакеты для каждого.
Result<void> select_msvc_generation(Options &opts, const manifest::Index &idx,
                                    const std::string &version, bool want_x86_or_x64,
                                    std::vector<SelectionIssue> &issues) {
    const auto it = msvc_table().find(version);
    if (it == msvc_table().end()) {
        return err_config("unsupported MSVC toolchain version " + version);
    }
    const MsvcEntry &entry = it->second;
    const std::string tv = entry.tool_version;

    // Явно запрошенная версия либо есть, либо её нет. Предупреждение с
    // подстановкой default/latest — это выдача пользователю не того, что
    // он просил, и узнаёт он об этом много позже.
    const std::string probe =
        entry.generation == std::string("15")
            ? "Microsoft.VisualStudio.Component.VC.Tools." + tv
            : "Microsoft.VisualStudio.Component.VC." + tv +
                  (tv == "Preview" ? ".Tools" : "") + ".x86.x64";
    if (!idx.contains(probe)) {
        issues.push_back({SelectionIssue::Kind::ExplicitVersionNotFound, version,
                          "", "looked for " + probe});
    } else if (entry.generation == std::string("15")) {
        add_if_wanted(opts, opts.with_msvc, ("Microsoft.VisualStudio.Component.VC.Tools." + tv).c_str());
    } else {
        const std::string base =
            "Microsoft.VisualStudio.Component.VC." + tv + (tv == "Preview" ? ".Tools" : "");
        if (want_x86_or_x64) {
            add_if_wanted(opts, opts.with_msvc, (base + ".x86.x64").c_str());
            add_if_wanted(opts, opts.with_asan, ("Microsoft.VC." + tv + ".ASAN.X86").c_str());
            add_if_wanted(opts, opts.with_atl,
                          ("Microsoft.VisualStudio.Component.VC." + tv + ".ATL").c_str());
        }
        if (contains(opts.architectures, "arm")) {
            add_if_wanted(opts, opts.with_msvc,
                          ("Microsoft.VisualStudio.Component.VC." + tv + ".ARM").c_str());
            add_if_wanted(opts, opts.with_atl,
                          ("Microsoft.VisualStudio.Component.VC." + tv + ".ATL.ARM").c_str());
        }
        if (contains(opts.architectures, "arm64")) {
            add_if_wanted(opts, opts.with_msvc,
                          ("Microsoft.VisualStudio.Component.VC." + tv + ".ARM64").c_str());
            add_if_wanted(opts, opts.with_atl,
                          ("Microsoft.VisualStudio.Component.VC." + tv + ".ATL.ARM64").c_str());
        }
    }
    if (!opts.sdk_version.has_value() && *entry.sdk != '\0') {
        opts.sdk_version = entry.sdk;
    }
    return {};
}

} // namespace

Result<Plan> resolve_selection(const Index &idx, Options opts) {
    std::vector<SelectionIssue> issues;

    if (opts.architectures.empty()) {
        opts.architectures = {"host", "x86", "x64", "arm", "arm64"};
    }
    if (!opts.host_arch.empty() && contains(opts.architectures, "host")) {
        opts.architectures.push_back(opts.host_arch);
    }

    const bool user_listed_packages = !opts.packages.empty();

    // Правило умолчаний формулируется один раз и явно: набор компонентов
    // включён всегда, кроме случая, когда пользователь перечислил пакеты сам.
    // --msvc-version задаёт версию, а не отменяет состав. Соблазн исключить
    // случай заданной версии из условия фолбэка велик и стоит дорого:
    // установка выходит без MSBuild.exe, и молча.
    if (!opts.with_default.has_value() && !user_listed_packages) {
        opts.with_default = true;
    }
    if (opts.with_default.has_value()) {
        for (Tri *flag : {&opts.with_workload, &opts.with_msvc, &opts.with_asan, &opts.with_sdk,
                          &opts.with_atl, &opts.with_dia, &opts.with_msbuild, &opts.with_devcmd}) {
            if (!flag->has_value()) {
                *flag = *opts.with_default;
            }
        }
    }
    if (opts.sdk_version.has_value() && !opts.with_sdk.has_value()) {
        opts.with_sdk = true;
    }

    // Пакеты, названные пользователем, обрабатываются отдельно от
    // производных: их отсутствие — всегда ошибка, тогда как производный набор
    // зависит от того, что вообще есть в манифесте.
    std::vector<std::string> explicit_packages = opts.packages;
    opts.packages.clear();

    const bool want_x86_or_x64 =
        contains(opts.architectures, "x86") || contains(opts.architectures, "x64");

    if (opts.msvc_version.has_value()) {
        if (auto r = select_msvc_generation(opts, idx, *opts.msvc_version, want_x86_or_x64,
                                            issues);
            !r.has_value()) {
            return std::unexpected(std::move(r).error());
        }
        // Дополнительные поколения ставятся рядом с основным. Основное
        // определяет версию установки, остальные существуют ради проектов,
        // привязанных именно к ним.
        for (const auto &extra : opts.additional_msvc_versions) {
            if (auto r = select_msvc_generation(opts, idx, extra, want_x86_or_x64, issues);
                !r.has_value()) {
                return std::unexpected(std::move(r).error());
            }
        }
    } else {
        add_if_wanted(opts, opts.with_workload, "Microsoft.VisualStudio.Workload.VCTools");
        if (want_x86_or_x64) {
            add_if_wanted(opts, opts.with_msvc, "Microsoft.VisualStudio.Component.VC.Tools.x86.x64");
            add_if_wanted(opts, opts.with_asan, "Microsoft.VisualCpp.ASAN.X86");
            add_if_wanted(opts, opts.with_atl, "Microsoft.VisualStudio.Component.VC.ATL");
        }
        if (contains(opts.architectures, "arm")) {
            add_if_wanted(opts, opts.with_msvc, "Microsoft.VisualStudio.Component.VC.Tools.ARM");
            add_if_wanted(opts, opts.with_atl, "Microsoft.VisualStudio.Component.VC.ATL.ARM");
        }
        if (contains(opts.architectures, "arm64")) {
            add_if_wanted(opts, opts.with_msvc, "Microsoft.VisualStudio.Component.VC.Tools.ARM64");
            add_if_wanted(opts, opts.with_atl, "Microsoft.VisualStudio.Component.VC.ATL.ARM64");
        }
    }

    // Даже при заданной версии MSVC состав остаётся полным: именно это и было
    // сломано. Workload здесь добавляется отдельно, потому что выше его
    // добавляет только ветка без версии.
    if (opts.msvc_version.has_value()) {
        add_if_wanted(opts, opts.with_workload, "Microsoft.VisualStudio.Workload.VCTools");
    }

    // --- Windows SDK ---
    if (opts.with_sdk.has_value()) {
        if (!*opts.with_sdk) {
            for (const auto &family : {"win10sdk", "win11sdk"}) {
                for (const auto &id : idx.ids_with_prefix(family)) {
                    opts.ignore.push_back(id);
                }
            }
            // Исключить только сами пакеты SDK недостаточно: компоненты
            // Microsoft.VisualStudio.Component.Windows*SDK* требуют их
            // обязательной зависимостью, и отказ от пакета при оставленном
            // компоненте — это ровно тот противоречивый выбор, о котором
            // резолвер обязан сообщить. Поэтому уходят и они.
            for (const auto &id :
                 idx.ids_with_prefix("microsoft.visualstudio.component.windows")) {
                if (id.find("sdk") != std::string::npos) {
                    opts.ignore.push_back(id);
                }
            }
        } else if (opts.sdk_version.has_value()) {
            bool found = false;
            std::vector<std::string> available;
            for (const auto &family : {"win10sdk", "win11sdk"}) {
                for (const auto &id : idx.ids_with_prefix(family)) {
                    const auto parsed = parse_sdk_id(id);
                    if (!parsed.has_value()) {
                        continue;  // id без версии — просто не наш кандидат
                    }
                    available.push_back(parsed->version);
                    // Запрошенная версия может быть указана и укороченно
                    // (10.0.26100 против 10.0.26100.0).
                    if (parsed->version == to_lower(*opts.sdk_version) ||
                        parsed->version.rfind(to_lower(*opts.sdk_version), 0) == 0) {
                        opts.packages.push_back(id);
                        found = true;
                    } else {
                        opts.ignore.push_back(id);
                    }
                }
            }
            if (!found) {
                std::sort(available.begin(), available.end());
                std::string list;
                for (const auto &v : available) {
                    if (!list.empty()) {
                        list += ", ";
                    }
                    list += v;
                }
                return err_not_found("Windows SDK version " + *opts.sdk_version +
                                     " not found (available: " + list + ")");
            }
        }
    }

    add_if_wanted(opts, opts.with_dia, "Microsoft.VisualCpp.DIA.SDK");
    add_if_wanted(opts, opts.with_msbuild, "Microsoft.Build");
    add_if_wanted(opts, opts.with_msbuild, "Microsoft.Build.Dependencies");
    add_if_wanted(opts, opts.with_devcmd, "Microsoft.VisualStudio.VC.vcvars");
    add_if_wanted(opts, opts.with_devcmd, "Microsoft.VisualStudio.PackageGroup.VsDevCmd");

    if (opts.with_wdk) {
        opts.packages.emplace_back("Component.Microsoft.Windows.DriverKit.BuildTools");
    }

    // --- раскрытие ---
    Expander ex{idx, opts, {}, {}, {}};

    // Сначала то, что назвал пользователь: их отсутствие — безусловная ошибка.
    for (const auto &id : explicit_packages) {
        ex.visit(id, Constraints{}, "", true);
    }
    // Затем производный набор. Его элементы могут отсутствовать в конкретном
    // манифесте по разным причинам, поэтому они не обязательны.
    for (const auto &id : opts.packages) {
        ex.visit(id, Constraints{}, "", false);
    }

    issues.insert(issues.end(), ex.issues.begin(), ex.issues.end());
    if (!issues.empty()) {
        std::string message = "package selection failed:";
        for (const auto &i : issues) {
            message += "\n  - " + i.to_string();
        }
        return err_not_found(std::move(message));
    }

    Plan plan;
    plan.packages = std::move(ex.out);

    // Дайджест считается по каноническому перечню ключей в порядке плана:
    // он входит в имя поколения установки, и одинаковый выбор обязан давать
    // одинаковое имя.
    Sha256 h;
    for (const auto *p : plan.packages) {
        h.update(p->key());
        h.update("\n");
    }
    plan.digest = Sha256::to_hex(h.finish());
    return plan;
}

std::string render_tree(const Index &idx, const Options &opts) {
    std::string out;
    std::set<std::string> printed;

    // Рекурсия через явный стек не нужна: глубина графа пакетов VS невелика,
    // а читаемость рекурсивного обхода здесь важнее.
    struct Printer {
        const Index &idx;
        const Options &opts;
        std::set<std::string> &printed;
        std::string &out;

        void go(const std::string &id, const manifest::Constraints &c, const char *annotation,
                int depth) {
            const std::string indent(static_cast<std::size_t>(depth) * 2, ' ');
            if (ignored(opts, id)) {
                return;
            }
            auto found = idx.find(id, c);
            if (!found.has_value()) {
                out += fmt::format("{}{} (not found){}\n", indent, id,
                                   annotation ? annotation : "");
                return;
            }
            const Package *p = *found;
            const std::string key = p->key();
            if (!printed.insert(key).second) {
                out += fmt::format("{}{}{} (see above)\n", indent, p->id,
                                   annotation ? annotation : "");
                return;
            }
            out += fmt::format("{}{}@{}{}\n", indent, p->id, p->version_text,
                               annotation ? annotation : "");
            for (const auto &dep : p->dependencies) {
                if (dep.kind == Dependency::Kind::Optional && !opts.include_optional) {
                    continue;
                }
                if (dep.kind == Dependency::Kind::Recommended && opts.skip_recommended) {
                    continue;
                }
                manifest::Constraints dc;
                if (!dep.version.is_any()) {
                    dc.version = dep.version;
                }
                const char *ann = dep.kind == Dependency::Kind::Optional      ? " [optional]"
                                  : dep.kind == Dependency::Kind::Recommended ? " [recommended]"
                                                                              : "";
                go(dep.id, dc, ann, depth + 1);
            }
        }
    };

    Printer printer{idx, opts, printed, out};
    for (const auto &id : opts.packages) {
        printer.go(id, manifest::Constraints{}, nullptr, 0);
    }
    return out;
}

} // namespace cork::resolve
