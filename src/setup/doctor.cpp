#include "setup/doctor.hpp"

#include <algorithm>
#include <system_error>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fmt/format.h>

#include "base/fs.hpp"
#include "base/sha256.hpp"
#include "setup/config.hpp"
#include "setup/digest.hpp"
#include "setup/prefix.hpp"
#include "setup/receipt.hpp"

namespace cork::setup {
namespace {

namespace stdfs = std::filesystem;

class Collector {
public:
    explicit Collector(Report &report, const DoctorOptions &opts)
        : report_(report), opts_(opts) {}

    void add(std::string name, Severity severity, std::string detail) {
        Check c{std::move(name), severity, std::move(detail)};
        if (opts_.on_check) {
            opts_.on_check(c);
        }
        report_.checks.push_back(std::move(c));
    }

    void ok(std::string name, std::string detail = {}) {
        add(std::move(name), Severity::Ok, std::move(detail));
    }
    void warn(std::string name, std::string detail) {
        add(std::move(name), Severity::Warning, std::move(detail));
    }
    void fail(std::string name, std::string detail) {
        add(std::move(name), Severity::Failure, std::move(detail));
    }

private:
    Report &report_;
    const DoctorOptions &opts_;
};

// Файл, без которого собрать нельзя. Отдельной функцией, потому что таких
// проверок много и каждая обязана назвать конкретный путь: «чего-то не
// хватает» — бесполезное сообщение.
bool require_file(Collector &c, std::string name, const stdfs::path &path) {
    if (fs::is_regular_file(path)) {
        c.ok(std::move(name), path.string());
        return true;
    }
    c.fail(std::move(name), fmt::format("{} is missing", path.string()));
    return false;
}


// Машинный тип из заголовка PE. Проверяется потому, что перепутанная цель —
// это не отказ сборки, а успешно собранный файл не для той архитектуры:
// ошибка, которая обнаруживается у пользователя, а не у нас.
Result<std::uint16_t> pe_machine(const stdfs::path &exe) {
    auto data = fs::read_file(exe);
    if (!data) {
        return std::unexpected(std::move(data).error());
    }
    const std::string &d = *data;
    if (d.size() < 0x40 || d[0] != 'M' || d[1] != 'Z') {
        return err_format(fmt::format("{} is not a PE image", exe.string()));
    }
    // Байты собираются через явный std::uint32_t: unsigned char в выражении
    // повышается до int, и получившееся знаковое значение потом молча
    // преобразуется в size_t. Смещение заголовка PE читается из файла, то есть
    // может быть любым, и полагаться на то, что старший бит окажется нулём,
    // здесь нечем.
    const auto at = [&d](std::size_t off) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(d[off]));
    };
    const std::size_t pe =
        at(0x3C) | (at(0x3D) << 8) | (at(0x3E) << 16) | (at(0x3F) << 24);
    if (pe + 6 > d.size() || d[pe] != 'P' || d[pe + 1] != 'E') {
        return err_format(fmt::format("{} has no PE header", exe.string()));
    }
    return static_cast<std::uint16_t>(at(pe + 4) | (at(pe + 5) << 8));
}

std::uint16_t expected_machine(std::string_view arch) {
    if (arch == "x86") return 0x014C;
    if (arch == "arm64") return 0xAA64;
    if (arch == "arm") return 0x01C4;
    return 0x8664;  // x64
}

// Запуск инструмента отдельным процессом. Вывод уходит в файл, а не в
// терминал: при успехе он не нужен, при отказе показывается целиком.
int run_quietly(const stdfs::path &exe, const std::vector<std::string> &args,
                const stdfs::path &cwd, const stdfs::path &log) {
    std::vector<char *> argv;
    std::string exe_str = exe.string();
    argv.push_back(exe_str.data());
    std::vector<std::string> copies = args;
    for (auto &a : copies) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        if (::chdir(cwd.c_str()) != 0) {
            ::_exit(127);
        }
        const int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            ::dup2(fd, STDOUT_FILENO);
            ::dup2(fd, STDERR_FILENO);
            ::close(fd);
        }
        ::execv(exe_str.c_str(), argv.data());
        ::_exit(127);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

} // namespace

bool Report::healthy() const {
    return std::none_of(checks.begin(), checks.end(),
                        [](const Check &c) { return c.severity == Severity::Failure; });
}

std::size_t Report::count(Severity s) const {
    return static_cast<std::size_t>(
        std::count_if(checks.begin(), checks.end(),
                      [s](const Check &c) { return c.severity == s; }));
}

Result<WineComposition> inspect_wine(const stdfs::path &runtime_root) {
    WineComposition out;
    std::error_code ec;
    if (!stdfs::is_directory(runtime_root, ec)) {
        return out;
    }
    out.present = true;

    // Mono лежит под share/wine/mono. Считается он отдельно от всего прочего
    // потому, что именно он разваливался молча: без его сборки MSBuild.exe не
    // стартует, а никакая проверка каталогов этого не замечает.
    const stdfs::path mono = runtime_root / "share" / "wine" / "mono";

    for (stdfs::recursive_directory_iterator it(
             runtime_root, stdfs::directory_options::skip_permission_denied, ec);
         it != stdfs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            return err_io(fmt::format("walking {}: {}", runtime_root.string(), ec.message()));
        }
        if (it->is_symlink(ec)) {
            ++out.symlinks;
            // По ссылке не спускаемся: её цель уже посчитана там, где лежит.
            continue;
        }
        if (it->is_regular_file(ec)) {
            const auto rel = it->path().lexically_relative(mono);
            if (!rel.empty() && rel.native().rfind("..", 0) != 0) {
                ++out.mono_files;
            }
        }
    }
    return out;
}

// Шаблон префикса не должен быть старше Wine, из которого он сделан.
//
// Не совпало — при каждом запуске поднимается rundll32
// setupapi,InstallHinfSection и прогоняет wine.inf заново. Это не поломка:
// всё работает, просто каждая сессия дорожает на десятки секунд.
//
// Ради этого и проверка. Симптом — «сборки почему-то медленные» — не ведёт ни
// к какой причине: в выводе ничего не появляется, ошибок нет, а пересобрать
// Wine и забыть пересобрать шаблон проще простого. Замерено: шаблон,
// отставший от runtime на восемь часов, добавлял к каждой сессии полный
// прогон wine.inf.
void check_prefix_template(Collector &c, const Root &root, const std::string &wine_id,
                           const stdfs::path &runtime) {
    const stdfs::path template_dir = root.base / "template" / wine_id;
    std::error_code ec;
    if (!stdfs::is_directory(template_dir, ec)) {
        // Шаблона нет — это не отказ: сессия просто создаст префикс с нуля.
        c.warn("prefix template",
               fmt::format("none at {}; every build will run wineboot from scratch",
                           template_dir.string()));
        return;
    }
    if (prefix_matches_wine(template_dir, runtime)) {
        c.ok("prefix template", "matches this Wine");
    } else {
        c.warn("prefix template",
               "older than this Wine, so every session re-runs wine.inf; rebuild it with "
               "`cork template --force`");
    }
}

Result<Report> diagnose(const Root &root, const DoctorOptions &opts) {
    Report report;
    report.level = opts.level;
    Collector c(report, opts);

    // 1. Какое поколение проверяем.
    stdfs::path generation = opts.generation;
    if (generation.empty()) {
        auto resolved = root.resolve_current();
        if (!resolved) {
            c.fail("current generation", resolved.error().to_string());
            return report;
        }
        generation = *resolved;
    } else if (!fs::is_dir(generation)) {
        c.fail("generation", fmt::format("{} is not a directory", generation.string()));
        return report;
    }
    report.generation = generation;
    c.ok("current generation", generation.string());

    // 2. Receipt. Без него о дереве нельзя сказать ничего, кроме того, что
    // оно есть, — а «оно есть» и выдают за проверку в тех случаях, ради
    // которых всё это написано.
    auto receipt = Receipt::load(generation);
    if (!receipt) {
        c.fail("receipt", receipt.error().to_string());
        return report;
    }
    auto state = receipt->state();
    if (!state) {
        c.fail("receipt", state.error().to_string());
        return report;
    }
    c.ok("receipt", fmt::format("state '{}', {} payloads, {} packages unpacked",
                                state_name(*state), receipt->payloads.size(),
                                receipt->unpacked.size()));

    if (opts.expect_published && *state < State::Published) {
        c.fail("state", fmt::format("generation is only '{}'; it was never published",
                                    state_name(*state)));
    } else if (*state < State::Staged) {
        c.fail("state", fmt::format("generation is only '{}'; nothing was unpacked",
                                    state_name(*state)));
    } else {
        c.ok("state", state_name(*state));
    }

    // 3. Конфигурация.
    auto cfg = Config::load(generation);
    if (!cfg) {
        c.fail("configuration", cfg.error().to_string());
        return report;
    }
    c.ok("configuration", fmt::format("MSVC {}{}", cfg->msvc_version,
                                      cfg->sdk_version.empty()
                                          ? ", no Windows SDK"
                                          : ", Windows SDK " + cfg->sdk_version));

    // 4. PE-хелпер: он вшит в бинарник и выложен на диск, поэтому его
    // подмену видно только по хешу.
    const stdfs::path helper = generation / cfg->helper_path;
    if (require_file(c, "pe helper", helper)) {
        auto hex = Sha256::hex_of_file(helper);
        if (!hex) {
            c.fail("pe helper hash", hex.error().to_string());
        } else if (!hex_equal(*hex, cfg->helper_sha256)) {
            c.fail("pe helper hash",
                   fmt::format("{} has {}, the configuration records {}", helper.string(), *hex,
                               cfg->helper_sha256));
        } else {
            c.ok("pe helper hash", *hex);
        }
    }

    // 5. Инструменты каждой цели.
    for (const auto &[arch, paths] : cfg->targets) {
        const stdfs::path bin = generation / paths.bin;
        require_file(c, fmt::format("cl.exe ({})", arch), bin / "cl.exe");
        require_file(c, fmt::format("link.exe ({})", arch), bin / "link.exe");
    }

    // 6. Windows SDK, если он заявлен.
    if (!cfg->sdk_version.empty()) {
        const stdfs::path inc = generation / "kits" / "10" / "include" / cfg->sdk_version;
        require_file(c, "sdk headers", inc / "ucrt" / "stdio.h");
        const stdfs::path lib = generation / "kits" / "10" / "lib" / cfg->sdk_version;
        if (fs::is_dir(lib / "um" / cfg->host_arch)) {
            c.ok("sdk libraries", (lib / "um" / cfg->host_arch).string());
        } else {
            c.fail("sdk libraries",
                   fmt::format("{} is missing", (lib / "um" / cfg->host_arch).string()));
        }
    }

    // 7. Своя сборка Wine. Здесь проверяется не наличие каталога, а состав:
    // число символьных ссылок и число файлов Mono против записанных в receipt.
    if (cfg->wine_id.empty()) {
        c.fail("wine runtime", "the configuration records no Wine runtime");
    } else {
        const stdfs::path runtime = root.runtime(cfg->wine_id);
        if (require_file(c, "wine binary", runtime / "bin" / "wine")) {
            auto comp = inspect_wine(runtime);
            if (!comp) {
                c.fail("wine composition", comp.error().to_string());
            } else {
                if (comp->symlinks != receipt->wine.symlinks) {
                    // Именно так выглядел дефект номер один: ссылки
                    // записывались обычными файлами, и их становилось ноль.
                    c.fail("wine symlinks",
                           fmt::format("{} present, receipt records {}", comp->symlinks,
                                       receipt->wine.symlinks));
                } else {
                    c.ok("wine symlinks", fmt::format("{}", comp->symlinks));
                }
                if (comp->mono_files != receipt->wine.mono_files) {
                    c.fail("wine mono",
                           fmt::format("{} files present, receipt records {}; MSBuild.exe will "
                                       "not start with an incomplete Mono",
                                       comp->mono_files, receipt->wine.mono_files));
                } else if (comp->mono_files == 0) {
                    c.fail("wine mono", "no Mono files at all");
                } else {
                    c.ok("wine mono", fmt::format("{} files", comp->mono_files));
                }
            }

            check_prefix_template(c, root, cfg->wine_id, runtime);
        }
    }

    if (opts.level == CheckLevel::Quick) {
        return report;
    }

    // 8. Глубокая проверка: дерево против записанного дайджеста.
    if (receipt->tree.digest.empty()) {
        c.warn("tree digest", "the receipt records no digest, so the tree cannot be verified");
    } else {
        DigestOptions dopts;
        dopts.mode = receipt->tree.mode;
        dopts.exclude = {kReceiptFileName};
        auto digest = digest_tree(generation, dopts);
        if (!digest) {
            c.fail("tree digest", digest.error().to_string());
        } else if (digest->digest != receipt->tree.digest) {
            c.fail("tree digest",
                   fmt::format("tree is {} ({} files, {} symlinks), receipt records {} ({} files, "
                               "{} symlinks)",
                               digest->digest, digest->stats.files, digest->stats.symlinks,
                               receipt->tree.digest, receipt->tree.stats.files,
                               receipt->tree.stats.symlinks));
        } else {
            c.ok("tree digest", fmt::format("{} ({} files, {} symlinks)", digest->digest,
                                            digest->stats.files, digest->stats.symlinks));
        }
    }

    if (opts.level != CheckLevel::Buildable) {
        return report;
    }

    // 9. Самая честная проверка: собрать что-нибудь. Всё предыдущее говорит о
    // составе дерева, и только эта — о том, что им можно пользоваться.
    const stdfs::path work = root.base / "run" / fmt::format("doctor-{}", ::getpid());
    if (auto r = fs::mkdir_p(work); !r) {
        c.fail("probe workspace", r.error().to_string());
        return report;
    }
    struct Cleanup {
        stdfs::path dir;
        ~Cleanup() {
            std::error_code ec;
            stdfs::remove_all(dir, ec);
        }
    } cleanup{work};

    // Без заголовков и без C-рантайма: проверяется тулчейн, а не Windows SDK,
    // и пробник должен собираться даже на установке без него.
    const stdfs::path source = work / "probe.c";
    if (auto r = fs::write_atomic(source, "int mainCRTStartup(void) { return 0; }\n"); !r) {
        c.fail("probe source", r.error().to_string());
        return report;
    }

    for (const auto &[arch, paths] : cfg->targets) {
        const stdfs::path wrapper = generation / "bin" / arch / "cl";
        if (!fs::is_regular_file(wrapper)) {
            c.fail(fmt::format("probe ({})", arch),
                   fmt::format("{} is missing", wrapper.string()));
            continue;
        }
        const stdfs::path exe = work / fmt::format("probe-{}.exe", arch);
        const stdfs::path log = work / fmt::format("probe-{}.log", arch);
        const int code = run_quietly(wrapper,
                                     {"/nologo", "probe.c", "/link", "/entry:mainCRTStartup",
                                      "/subsystem:console", "/out:" + exe.filename().string()},
                                     work, log);
        if (code != 0) {
            std::string output;
            if (auto text = fs::read_file(log); text.has_value()) {
                output = *text;
            }
            c.fail(fmt::format("probe ({})", arch),
                   fmt::format("cl exited with {}\n{}", code, output));
            continue;
        }
        auto machine = pe_machine(exe);
        if (!machine) {
            c.fail(fmt::format("probe ({})", arch), machine.error().to_string());
        } else if (*machine != expected_machine(arch)) {
            c.fail(fmt::format("probe ({})", arch),
                   fmt::format("produced machine type {:#06x}, expected {:#06x}", *machine,
                               expected_machine(arch)));
        } else {
            c.ok(fmt::format("probe ({})", arch),
                 fmt::format("compiled, linked, machine {:#06x}", *machine));
        }
    }

    return report;
}

} // namespace cork::setup
