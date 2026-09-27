#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "base/fs.hpp"
#include "base/lock.hpp"
#include "base/version.hpp"
#include "cli/commands.hpp"
#include "i18n/messages.hpp"
#include "setup/digest.hpp"
#include "setup/doctor.hpp"
#include "setup/generation.hpp"
#include "setup/install.hpp"
#include "setup/receipt.hpp"
#include "setup/relocate.hpp"
#include "cork_embedded_helper.h"
#include "cork_embedded_toolchain.h"

namespace cork::cli {
namespace {

namespace stdfs = std::filesystem;

// Самый свежий каталог сборки, в котором что-то распаковано. Нужен, чтобы
// `cork install` без аргументов продолжал ровно ту работу, которую только
// что закончил `cork download`.
Result<stdfs::path> latest_staging(const setup::Root &root) {
    std::error_code ec;
    if (!stdfs::is_directory(root.staging(), ec)) {
        return err_not_found("nothing has been downloaded yet");
    }
    stdfs::path best;
    stdfs::file_time_type best_time{};
    for (const auto &entry : stdfs::directory_iterator(root.staging(), ec)) {
        if (ec || !entry.is_directory(ec)) {
            continue;
        }
        auto receipt = setup::Receipt::load(entry.path());
        if (!receipt || !receipt->at_least(setup::State::Staged)) {
            continue;
        }
        const auto when = entry.last_write_time(ec);
        if (best.empty() || when > best_time) {
            best = entry.path();
            best_time = when;
        }
    }
    if (best.empty()) {
        return err_not_found(
            "no unpacked build directory was found; run `cork download` first");
    }
    return best;
}

} // namespace

int cmd_install(const std::vector<std::string> &args) {
    stdfs::path staging_dir;
    stdfs::path root_dir;
    bool quick_digest = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--root" && i + 1 < args.size()) {
            root_dir = args[++i];
        } else if (args[i] == "--quick-digest") {
            quick_digest = true;
        } else if (args[i] == "-h" || args[i] == "--help") {
            return cmd_help(0);
        } else if (args[i].rfind("--", 0) == 0) {
            fmt::print(stderr, "cork install: unknown option {}\n", args[i]);
            return 2;
        } else {
            staging_dir = args[i];
        }
    }

    setup::Root root = setup::Root::from_environment();
    if (!root_dir.empty()) {
        root.base = stdfs::absolute(root_dir);
    }
    if (auto r = root.ensure(); !r.has_value()) {
        fmt::print(stderr, "cork install: {}\n", r.error().to_string());
        return 1;
    }

    if (staging_dir.empty()) {
        auto found = latest_staging(root);
        if (!found.has_value()) {
            fmt::print(stderr, "cork install: {}\n", found.error().to_string());
            return 1;
        }
        staging_dir = *found;
    }
    staging_dir = stdfs::absolute(staging_dir);

    if (!assets::kHelperEmbedded) {
        fmt::print(stderr,
                   "cork install: this binary was built without the PE helper.\n"
                   "Build it with tools/wine/build-helper.sh and re-configure the project.\n");
        return 1;
    }

    auto staging = setup::Staging::adopt(root, staging_dir);
    if (!staging.has_value()) {
        fmt::print(stderr, "cork install: {}\n", staging.error().to_string());
        return 1;
    }
    auto lock = Lock::acquire(root.locks(), "staging-" + staging_dir.filename().string(),
                              Lock::Mode::Exclusive);
    if (!lock.has_value()) {
        fmt::print(stderr, "cork install: {}\n", lock.error().to_string());
        return 1;
    }

    // Receipt обязателен. Опубликовать дерево, о происхождении которого нет
    // записи, значит вернуться ровно к тому, из чего всё это затевалось:
    // установка есть, а чем она является — неизвестно.
    auto receipt = setup::Receipt::load(staging_dir);
    if (!receipt.has_value()) {
        fmt::print(stderr, "cork install: {}\n", receipt.error().to_string());
        return 1;
    }

    const stdfs::path unpack = staging_dir / "unpack";
    if (fs::is_dir(unpack)) {
        i18n::say(i18n::Msg::Relocating);
        if (auto r = setup::relocate_build_tools(unpack, staging_dir); !r.has_value()) {
            fmt::print(stderr, "cork install: {}\n", r.error().to_string());
            return 1;
        }
        std::error_code ec;
        stdfs::remove_all(unpack, ec);
    }
    // Алиасы наборов инструментов заводятся до конфигурации: в неё
    // записывается имя настоящего набора, найденного на диске.
    auto real_toolset = setup::alias_platform_toolsets(staging_dir);
    if (!real_toolset.has_value()) {
        fmt::print(stderr, "cork install: {}\n", real_toolset.error().to_string());
        return 1;
    }

    if (auto r = setup::create_layout_links(staging_dir); !r.has_value()) {
        fmt::print(stderr, "cork install: {}\n", r.error().to_string());
        return 1;
    }

    std::error_code ec;
    const stdfs::path self = stdfs::read_symlink("/proc/self/exe", ec);
    if (ec) {
        fmt::print(stderr, "cork install: cannot determine own path: {}\n", ec.message());
        return 1;
    }

    setup::InstallOptions opts;
    opts.generation_root = staging_dir;
    opts.self_binary = self;
    opts.helper = assets::helper();
    opts.cmake_toolchain = assets::cmake_toolchain();
    opts.wine_id = wine_runtime_id();
    opts.version = kVersion;
    opts.commit = kGitRevision;
    opts.platform_toolset = *real_toolset;

    auto report = setup::install(opts);
    if (!report.has_value()) {
        fmt::print(stderr, "cork install: {}\n", report.error().to_string());
        return 1;
    }

    i18n::say(i18n::Msg::UsingMsvc, report->msvc_version);
    if (report->sdk_version.empty()) {
        i18n::say(i18n::Msg::NoSdkYet);
    } else {
        i18n::say(i18n::Msg::UsingSdk, report->sdk_version);
    }

    // Состав Wine записывается сейчас, пока дерево заведомо целое. Без этой
    // записи вопрос «сколько в нём должно быть символьных ссылок» остаётся
    // без ответа, а значит проверить его потом нечем.
    auto wine = setup::inspect_wine(root.runtime(opts.wine_id));
    if (!wine.has_value()) {
        fmt::print(stderr, "cork install: {}\n", wine.error().to_string());
        return 1;
    }
    if (!wine->present) {
        fmt::print(stderr,
                   "cork install: the Wine runtime {} is not installed at {}\n",
                   opts.wine_id, root.runtime(opts.wine_id).string());
        return 1;
    }
    receipt->wine.id = opts.wine_id;
    receipt->wine.symlinks = wine->symlinks;
    receipt->wine.mono_files = wine->mono_files;
    i18n::say(i18n::Msg::WineComposition, opts.wine_id, wine->symlinks, wine->mono_files);

    setup::DigestOptions dopts;
    dopts.mode = quick_digest ? setup::DigestMode::Structure : setup::DigestMode::Content;
    dopts.exclude = {setup::kReceiptFileName};
    i18n::say(i18n::Msg::ComputingDigest,
              i18n::tr(quick_digest ? i18n::Msg::DigestStructure : i18n::Msg::DigestContents));
    auto digest = setup::digest_tree(staging_dir, dopts);
    if (!digest.has_value()) {
        fmt::print(stderr, "cork install: {}\n", digest.error().to_string());
        return 1;
    }
    receipt->tree = *digest;
    i18n::say(i18n::Msg::TreeSummary, digest->stats.files, digest->stats.symlinks,
              digest->digest);

    if (auto r = receipt->advance(setup::State::Verified, kVersion); !r.has_value()) {
        fmt::print(stderr, "cork install: {}\n", r.error().to_string());
        return 1;
    }
    if (auto r = receipt->save(staging_dir); !r.has_value()) {
        fmt::print(stderr, "cork install: {}\n", r.error().to_string());
        return 1;
    }

    // Проверка идёт до публикации, а не после: смысл всей этой возни в том,
    // чтобы непроверенное дерево никогда не оказалось тем, на что указывает
    // current.
    setup::DoctorOptions dochk;
    dochk.generation = staging_dir;
    dochk.expect_published = false;
    auto check = setup::diagnose(root, dochk);
    if (!check.has_value()) {
        fmt::print(stderr, "cork install: {}\n", check.error().to_string());
        return 1;
    }
    if (!check->healthy()) {
        fmt::print(stderr, "cork install: the build directory did not pass its own checks:\n");
        for (const auto &c : check->checks) {
            if (c.severity == setup::Severity::Failure) {
                fmt::print(stderr, "  {}: {}\n", c.name, c.detail);
            }
        }
        // Расхождение именно по составу Wine — почти всегда не порча, а
        // изменение под ногами: состав снимается в начале установки, а
        // проверяется в конце, и пересборка runtime между этими моментами
        // даёт ровно такую картину. Сказать об этом дешевле, чем заставлять
        // разбираться заново.
        const bool wine_mismatch =
            std::any_of(check->checks.begin(), check->checks.end(), [](const setup::Check &c) {
                return c.severity == setup::Severity::Failure &&
                       c.name.rfind("wine ", 0) == 0;
            });
        if (wine_mismatch) {
            fmt::print(stderr,
                       "\nThe Wine runtime changed while this install was running.\n"
                       "If you rebuilt it just now, simply run `cork install` again.\n");
        }
        fmt::print(stderr,
                   "\nIt was left at {} and nothing was published.\n", staging_dir.string());
        return 1;
    }

    // Published записывается до переименования намеренно. Если публикация не
    // состоится, этот каталог так и останется в staging/, где его никто не
    // ищет и куда не указывает current, — а вот опубликованное дерево обязано
    // с первой же секунды нести верное состояние.
    if (auto r = receipt->advance(setup::State::Published, kVersion); !r.has_value()) {
        fmt::print(stderr, "cork install: {}\n", r.error().to_string());
        return 1;
    }
    if (auto r = receipt->save(staging_dir); !r.has_value()) {
        fmt::print(stderr, "cork install: {}\n", r.error().to_string());
        return 1;
    }

    const std::string name =
        report->sdk_version.empty() ? report->msvc_version
                                    : report->msvc_version + "-" + report->sdk_version;
    auto published = staging->publish(name, digest->digest);
    if (!published.has_value()) {
        fmt::print(stderr, "cork install: {}\n", published.error().to_string());
        return 1;
    }

    for (const auto &arch : report->targets) {
        i18n::say(i18n::Msg::InstalledWrappers, arch);
    }
    i18n::say(i18n::Msg::PublishedAt, published->string());
    i18n::say(i18n::Msg::AddToPath);
    fmt::print("  export PATH={}/bin/{}:$PATH\n", root.current().string(),
               report->targets.front());
    return 0;
}

} // namespace cork::cli
