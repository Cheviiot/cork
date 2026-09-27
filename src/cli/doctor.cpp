#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "cli/commands.hpp"
#include "i18n/messages.hpp"
#include "setup/doctor.hpp"
#include "setup/generation.hpp"

namespace cork::cli {
namespace {

namespace stdfs = std::filesystem;

const char *mark(setup::Severity s) {
    switch (s) {
    case setup::Severity::Ok: return "ok  ";
    case setup::Severity::Warning: return "warn";
    case setup::Severity::Failure: return "FAIL";
    }
    return "????";
}

std::string humanize_bytes(std::uint64_t n) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(n);
    std::size_t u = 0;
    while (v >= 1024.0 && u + 1 < std::size(units)) {
        v /= 1024.0;
        ++u;
    }
    return u == 0 ? fmt::format("{} {}", n, units[u]) : fmt::format("{:.1f} {}", v, units[u]);
}

} // namespace

int cmd_doctor(const std::vector<std::string> &args) {
    setup::DoctorOptions opts;
    stdfs::path root_dir;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string &arg = args[i];
        if (arg == "--deep") {
            opts.level = setup::CheckLevel::Deep;
        } else if (arg == "--build") {
            // Самый честный уровень и самый медленный: он запускает настоящий
            // компилятор под Wine на каждую цель.
            opts.level = setup::CheckLevel::Buildable;
        } else if (arg == "--root" && i + 1 < args.size()) {
            root_dir = args[++i];
        } else if (arg == "--generation" && i + 1 < args.size()) {
            opts.generation = args[++i];
        } else if (arg == "-h" || arg == "--help") {
            return cmd_help(0);
        } else if (arg.rfind("--", 0) == 0) {
            fmt::print(stderr, "cork doctor: unknown option {}\n", arg);
            return 2;
        } else {
            opts.generation = arg;
        }
    }

    setup::Root root = setup::Root::from_environment();
    if (!root_dir.empty()) {
        root.base = stdfs::absolute(root_dir);
    }

    // Проверки печатаются по мере выполнения, а не пачкой в конце: глубокий
    // уровень читает гигабайты, и до его результата надо видеть, что работа
    // идёт.
    opts.on_check = [](const setup::Check &c) {
        if (c.detail.empty()) {
            fmt::print("[{}] {}\n", mark(c.severity), c.name);
        } else {
            fmt::print("[{}] {}: {}\n", mark(c.severity), c.name, c.detail);
        }
    };

    auto report = setup::diagnose(root, opts);
    if (!report.has_value()) {
        fmt::print(stderr, "cork doctor: {}\n", report.error().to_string());
        return 1;
    }

    const auto failures = report->count(setup::Severity::Failure);
    const auto warnings = report->count(setup::Severity::Warning);
    i18n::say(i18n::Msg::ChecksSummary, report->checks.size(), failures, warnings);

    if (failures > 0) {
        // Несобираемая установка обязана сказать об этом кодом возврата.
        // Проверка, которая при поломке печатает «всё хорошо», хуже её
        // отсутствия: на неё полагаются.
        i18n::say(i18n::Msg::CannotBuild);
        return 1;
    }
    if (opts.level == setup::CheckLevel::Quick) {
        i18n::say(i18n::Msg::DoctorHint);
    }
    return 0;
}

int cmd_gc(const std::vector<std::string> &args) {
    stdfs::path root_dir;
    int hours = 24;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string &arg = args[i];
        if (arg == "--root" && i + 1 < args.size()) {
            root_dir = args[++i];
        } else if (arg == "--older-than-hours" && i + 1 < args.size()) {
            hours = std::max(0, std::atoi(args[++i].c_str()));
        } else if (arg == "-h" || arg == "--help") {
            return cmd_help(0);
        } else {
            fmt::print(stderr, "cork gc: unknown option {}\n", arg);
            return 2;
        }
    }

    setup::Root root = setup::Root::from_environment();
    if (!root_dir.empty()) {
        root.base = stdfs::absolute(root_dir);
    }

    auto stats = setup::collect_staging(root, std::chrono::hours(hours));
    if (!stats.has_value()) {
        fmt::print(stderr, "cork gc: {}\n", stats.error().to_string());
        return 1;
    }
    i18n::say(i18n::Msg::GcRemoved, stats->staging_removed,
              humanize_bytes(stats->bytes_freed));

    auto generations = setup::list_generations(root);
    if (generations.has_value()) {
        auto current = root.resolve_current();
        i18n::say(i18n::Msg::GenerationCount, generations->size());
        for (const auto &g : *generations) {
            const bool is_current = current.has_value() && *current == g;
            fmt::print("  {}{}\n", g.filename().string(),
                       is_current ? fmt::format("  ({})", i18n::tr(i18n::Msg::CurrentMarker))
                                  : std::string());
        }
    }
    return 0;
}

} // namespace cork::cli
