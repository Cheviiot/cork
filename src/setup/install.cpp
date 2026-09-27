#include "setup/install.hpp"

#include <algorithm>
#include <map>
#include <vector>

#include <fmt/format.h>

#include "base/fs.hpp"
#include "base/sha256.hpp"
#include "exec/tools.hpp"
#include "manifest/version.hpp"
#include "setup/layout.hpp"

namespace cork::setup {
namespace {

namespace stdfs = std::filesystem;

// Все целевые архитектуры, которые вообще бывают. Что из них реально
// установлено, определяется наличием cl.exe.
const std::vector<std::string> &all_targets() {
    static const std::vector<std::string> t = {"x86", "x64", "arm", "arm64"};
    return t;
}

std::string host_arch_here() {
#if defined(__aarch64__)
    return "arm64";
#else
    return "x64";
#endif
}

std::string dotnet_host_for(std::string_view host) {
    return host == "arm64" ? "arm64" : "amd64";
}

// Разные выпуски MSVC писали каталог хоста в разном регистре. Приводим к
// одному виду, иначе путь из конфигурации не совпадёт с тем, что на диске.
Result<void> normalise_host_dirs(const stdfs::path &bin_dir) {
    const std::vector<std::pair<std::string, std::string>> renames = {
        {"HostX64", "Hostx64"},
        {"HostX86", "Hostx86"},
        {"HostARM64", "Hostarm64"},
        {"HostArm64", "Hostarm64"},
    };
    for (const auto &[from, to] : renames) {
        const stdfs::path src = bin_dir / from;
        const stdfs::path dst = bin_dir / to;
        if (fs::is_dir(src) && !fs::exists_no_follow(dst)) {
            std::error_code ec;
            stdfs::rename(src, dst, ec);
            if (ec) {
                return err_io("renaming " + src.string() + ": " + ec.message());
            }
        }
    }
    // Внутри каталога хоста цель тоже встречается в верхнем регистре.
    for (const auto &host : {"Hostx64", "Hostx86", "Hostarm64"}) {
        const stdfs::path host_dir = bin_dir / host;
        if (!fs::is_dir(host_dir)) {
            continue;
        }
        for (const auto &[from, to] : std::vector<std::pair<std::string, std::string>>{
                 {"X64", "x64"}, {"X86", "x86"}, {"ARM64", "arm64"}, {"ARM", "arm"}}) {
            const stdfs::path src = host_dir / from;
            const stdfs::path dst = host_dir / to;
            if (fs::is_dir(src) && !fs::exists_no_follow(dst)) {
                std::error_code ec;
                stdfs::rename(src, dst, ec);
                if (ec) {
                    return err_io("renaming " + src.string() + ": " + ec.message());
                }
            }
        }
    }
    return {};
}

// Каталог отладочных библиотек времени выполнения, относительно корня
// поколения. Ищется, а не выводится по формуле: версия редиста не обязана
// совпадать с версией компилятора, а в имени самого каталога стоит номер
// набора инструментов («Microsoft.VC145.DebugCRT»). Формула из двух
// предположений сломается на первом же несовпадении и сломается молча —
// программа просто не запустится.
std::string find_debug_crt(const stdfs::path &root, const std::string &arch) {
    const stdfs::path redist = root / "VC" / "Redist" / "MSVC";
    std::error_code ec;
    if (!stdfs::is_directory(redist, ec)) {
        return {};
    }
    for (const auto &version : stdfs::directory_iterator(redist, ec)) {
        const stdfs::path by_arch = version.path() / "debug_nonredist" / arch;
        if (!stdfs::is_directory(by_arch, ec)) {
            continue;
        }
        for (const auto &component : stdfs::directory_iterator(by_arch, ec)) {
            // Признак — не имя, а содержимое: ровно та библиотека, которой не
            // хватало. Так каталог остаётся найденным, даже если Microsoft
            // переименует компонент.
            if (fs::is_regular_file(component.path() / "vcruntime140d.dll")) {
                return stdfs::relative(component.path(), root).string();
            }
        }
    }
    return {};
}

// vctip.exe отправляет телеметрию наружу и под Wine известен как источник
// проблем. Удаляется, а не переименовывается: смысла в нём для нас нет.
void remove_vctip(const stdfs::path &dir) {
    if (!fs::is_dir(dir)) {
        return;
    }
    std::error_code ec;
    for (auto it = stdfs::recursive_directory_iterator(dir, ec);
         it != stdfs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            break;
        }
        if (!it->is_regular_file(ec)) {
            continue;
        }
        std::string name = it->path().filename().string();
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (name == "vctip.exe") {
            std::error_code rm;
            stdfs::remove(it->path(), rm);
        }
    }
}

Result<void> copy_file_exact(const stdfs::path &from, const stdfs::path &to) {
    auto content = fs::read_file(from);
    if (!content.has_value()) {
        return std::unexpected(std::move(content.error()));
    }
    return fs::write_atomic(to, std::string_view(*content),
                            stdfs::perms::owner_all | stdfs::perms::group_read |
                                stdfs::perms::group_exec | stdfs::perms::others_read |
                                stdfs::perms::others_exec);
}

// Символьная ссылка, обновляемая при расхождении. Настоящий файл на этом
// месте не трогается: его туда положил не install.
Result<void> link_to(const std::string &target, const stdfs::path &link) {
    std::error_code ec;
    if (stdfs::is_symlink(link, ec)) {
        const stdfs::path current = stdfs::read_symlink(link, ec);
        if (!ec && current.string() == target) {
            return {};
        }
        stdfs::remove(link, ec);
    } else if (fs::exists_no_follow(link)) {
        return {};
    }
    stdfs::create_symlink(target, link, ec);
    if (ec) {
        return err_io("creating symlink " + link.string() + ": " + ec.message());
    }
    return {};
}

} // namespace

Result<std::string> find_msvc_version(const stdfs::path &root) {
    const stdfs::path msvc_root = root / "vc" / "tools" / "msvc";
    if (!fs::is_dir(msvc_root)) {
        return err_not_found("no MSVC toolchain under " + msvc_root.string() +
                             "; run `cork download` first");
    }

    std::vector<std::string> candidates;
    std::error_code ec;
    for (const auto &entry : stdfs::directory_iterator(msvc_root, ec)) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        // Каталог считается пригодным только если в нём есть всё нужное:
        // частично распакованное дерево не должно выглядеть установленным.
        if (fs::is_dir(entry.path() / "bin") && fs::is_dir(entry.path() / "include") &&
            fs::is_dir(entry.path() / "lib")) {
            candidates.push_back(name);
        }
    }
    if (candidates.empty()) {
        return err_not_found("no usable MSVC version under " + msvc_root.string());
    }

    // Сравнение по компонентам версии, а не по алфавиту: «последний по
    // алфавиту» ломается на первом же номере сборки другой длины.
    std::sort(candidates.begin(), candidates.end(),
              [](const std::string &a, const std::string &b) {
                  return manifest::Version::parse(a) < manifest::Version::parse(b);
              });
    return candidates.back();
}

std::map<std::string, std::string> find_toolsets(const stdfs::path &root) {
    std::map<std::string, std::string> out;
    const stdfs::path msvc_root = root / "vc" / "tools" / "msvc";
    std::error_code ec;
    for (const auto &entry : stdfs::directory_iterator(msvc_root, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_directory(ec)) {
            continue;
        }
        const std::string version = entry.path().filename().string();
        if (!fs::is_dir(entry.path() / "bin") || !fs::is_dir(entry.path() / "include")) {
            continue;
        }
        // Короткое имя поколения выводится из версии: 14.29 -> 142, 14.51 ->
        // 145. Именно его пишет .vcxproj в PlatformToolset, и связать одно с
        // другим больше нечем — в дереве это никак не записано.
        const auto dot = version.find('.');
        if (dot == std::string::npos || dot + 2 > version.size()) {
            continue;
        }
        const std::string major = version.substr(0, dot);
        const std::string minor = version.substr(dot + 1, 1);
        out[major + minor] = version;
    }
    return out;
}

std::string find_sdk_version(const stdfs::path &root) {
    const stdfs::path include = root / "kits" / "10" / "include";
    if (!fs::is_dir(include)) {
        return {};
    }
    std::vector<std::string> versions;
    std::error_code ec;
    for (const auto &entry : stdfs::directory_iterator(include, ec)) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (name.rfind("10.", 0) == 0) {
            versions.push_back(name);
        }
    }
    if (versions.empty()) {
        return {};
    }
    std::sort(versions.begin(), versions.end(), [](const std::string &a, const std::string &b) {
        return manifest::Version::parse(a) < manifest::Version::parse(b);
    });
    return versions.back();
}

Result<InstallReport> install(const InstallOptions &opts) {
    const stdfs::path root = opts.generation_root;
    if (!fs::is_dir(root)) {
        return err_not_found("nothing to install at " + root.string());
    }

    const std::string host = host_arch_here();
    const std::string dotnet_host = dotnet_host_for(host);

    auto msvc_version = find_msvc_version(root);
    if (!msvc_version.has_value()) {
        return std::unexpected(std::move(msvc_version.error()));
    }
    const stdfs::path msvc_dir = root / "vc" / "tools" / "msvc" / *msvc_version;

    if (auto r = normalise_host_dirs(msvc_dir / "bin"); !r.has_value()) {
        return std::unexpected(std::move(r.error()));
    }
    remove_vctip(msvc_dir / "bin");

    Config cfg;
    cfg.host_arch = host;
    cfg.dotnet_host = dotnet_host;
    cfg.msvc_version = *msvc_version;
    cfg.sdk_version = find_sdk_version(root);
    cfg.platform_toolset = opts.platform_toolset;
    cfg.toolsets = find_toolsets(root);
    cfg.wine_id = opts.wine_id;
    cfg.created_by_version = opts.version;
    cfg.created_by_commit = opts.commit;
    cfg.generation = *msvc_version + (cfg.sdk_version.empty() ? "" : "-" + cfg.sdk_version);
    cfg.helper_path = "bin/cork-helper.exe";

    InstallReport report;
    report.msvc_version = *msvc_version;
    report.sdk_version = cfg.sdk_version;

    const stdfs::path bin_root = root / "bin";
    if (auto r = fs::mkdir_p(bin_root); !r.has_value()) {
        return std::unexpected(std::move(r.error()));
    }

    // Хелпер выкладывается из вшитого массива. Раньше он собирался только что
    // установленным cl.exe во время install — то есть установка зависела от
    // работоспособности того, что сама же и установила.
    const stdfs::path helper_path = root / cfg.helper_path;
    if (auto r = fs::write_atomic(helper_path, opts.helper,
                                  stdfs::perms::owner_all | stdfs::perms::group_read |
                                      stdfs::perms::group_exec | stdfs::perms::others_read |
                                      stdfs::perms::others_exec);
        !r.has_value()) {
        return std::unexpected(std::move(r.error()).at("placing the PE helper"));
    }
    cfg.helper_sha256 = Sha256::hex_of(opts.helper);

    // Файл тулчейна CMake: без него кросс-сборка проекта на CMake требует от
    // человека написать его самому, а написанный самостоятельно почти всегда
    // забывает CMAKE_TRY_COMPILE_TARGET_TYPE и упирается в проверку
    // компилятора, которая пытается запустить собранный Windows-бинарник.
    if (!opts.cmake_toolchain.empty()) {
        const stdfs::path share = root / "share";
        if (auto r = fs::mkdir_p(share); !r.has_value()) {
            return std::unexpected(std::move(r.error()));
        }
        if (auto r = fs::write_atomic(share / "cork-toolchain.cmake", opts.cmake_toolchain,
                                      stdfs::perms::owner_read | stdfs::perms::owner_write |
                                          stdfs::perms::group_read | stdfs::perms::others_read);
            !r.has_value()) {
            return std::unexpected(std::move(r.error()).at("placing the CMake toolchain file"));
        }
    }

    for (const auto &arch : all_targets()) {
        const stdfs::path cl_exe =
            msvc_dir / "bin" / ("Host" + host) / arch / "cl.exe";
        if (!fs::is_regular_file(cl_exe)) {
            continue;
        }

        const stdfs::path arch_dir = bin_root / arch;
        if (auto r = fs::mkdir_p(arch_dir); !r.has_value()) {
            return std::unexpected(std::move(r.error()));
        }

        // Своя копия бинарника в каждом каталоге, а не ссылка на общий:
        // обёртка определяет собственное расположение через /proc/self/exe,
        // который разрешает символьные ссылки. Ссылка на уровень выше
        // привела бы к тому, что обёртка считает себя лежащей в bin/.
        if (auto r = copy_file_exact(opts.self_binary, arch_dir / "cork"); !r.has_value()) {
            return std::unexpected(std::move(r.error()).at("installing the per-target binary"));
        }

        for (const auto &spec : exec::tool_table()) {
            const std::string name(spec.name);
            if (auto r = link_to("cork", arch_dir / name); !r.has_value()) {
                return std::unexpected(std::move(r.error()));
            }
            if (auto r = link_to("cork", arch_dir / (name + ".exe")); !r.has_value()) {
                return std::unexpected(std::move(r.error()));
            }
        }
        for (const auto &name : {"cmd", "findstr"}) {
            if (auto r = link_to("cork", arch_dir / name); !r.has_value()) {
                return std::unexpected(std::move(r.error()));
            }
        }

        cfg.targets[arch] =
            derive_target_paths(cfg.msvc_version, cfg.sdk_version, host, arch, dotnet_host);
        cfg.targets[arch].debug_crt = find_debug_crt(root, arch);
        report.targets.push_back(arch);
    }

    if (cfg.targets.empty()) {
        return err_not_found(fmt::format(
            "no target architecture found under {}/bin/Host{}/*", msvc_dir.string(), host));
    }

    if (auto r = cfg.save(root); !r.has_value()) {
        return std::unexpected(std::move(r.error()).at("writing the installation config"));
    }
    return report;
}

} // namespace cork::setup
