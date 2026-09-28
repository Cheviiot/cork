#include "setup/layout.hpp"

#include <algorithm>
#include <vector>

namespace cork::setup {
namespace {

namespace stdfs = std::filesystem;

std::string join(const std::vector<std::string> &parts, char sep) {
    std::string out;
    for (const auto &p : parts) {
        if (!out.empty()) {
            out += sep;
        }
        out += p;
    }
    return out;
}

} // namespace

std::string to_wine_path(const stdfs::path &p) {
    std::string s = p.string();
    std::replace(s.begin(), s.end(), '/', '\\');
    return "z:" + s;
}

TargetPaths derive_target_paths(std::string_view msvc_version, std::string_view sdk_version,
                                std::string_view host_arch, std::string_view target_arch,
                                std::string_view dotnet_host) {
    TargetPaths t;
    t.bin = std::string("vc/tools/msvc/") + std::string(msvc_version) + "/bin/Host" +
            std::string(host_arch) + "/" + std::string(target_arch);
    if (!sdk_version.empty()) {
        t.sdk_bin = std::string("kits/10/bin/") + std::string(sdk_version) + "/" +
                    std::string(host_arch);
    }
    t.msbuild_bin = std::string("MSBuild/Current/Bin/") + std::string(dotnet_host);
    return t;
}

ToolEnvironment derive_environment(const Config &cfg, const stdfs::path &root,
                                   std::string_view target_arch) {
    ToolEnvironment env;

    const stdfs::path msvc_dir =
        root / "vc" / "tools" / "msvc" / cfg.msvc_version;
    const std::string msvc_win = to_wine_path(msvc_dir);

    std::vector<std::string> include = {
        msvc_win + "\\atlmfc\\include",
        msvc_win + "\\include",
    };
    std::vector<std::string> lib = {
        msvc_win + "\\atlmfc\\lib\\" + std::string(target_arch),
        msvc_win + "\\lib\\" + std::string(target_arch),
    };

    // SDK появляется не сразу: срез, на котором собирается программа без
    // заголовков, живёт вообще без него, и пустые пути в INCLUDE лучше, чем
    // пути в несуществующие каталоги.
    if (!cfg.sdk_version.empty()) {
        const std::string sdk_win = to_wine_path(root / "kits" / "10");
        const std::string inc = sdk_win + "\\include\\" + cfg.sdk_version;
        const std::string libdir = sdk_win + "\\lib\\" + cfg.sdk_version;
        include.push_back(inc + "\\shared");
        include.push_back(inc + "\\ucrt");
        include.push_back(inc + "\\um");
        include.push_back(inc + "\\winrt");
        include.push_back(inc + "\\km");
        lib.push_back(libdir + "\\ucrt\\" + std::string(target_arch));
        lib.push_back(libdir + "\\um\\" + std::string(target_arch));
        lib.push_back(libdir + "\\km\\" + std::string(target_arch));
    }

    env.include = join(include, ';');
    env.lib = join(lib, ';');
    env.lib_path = env.lib;

    // WINEPATH ищет DLL. Третьим идёт каталог инструментов ХОСТОВОЙ
    // архитектуры, а не целевой: mspdbcore.dll и её соседи лежат именно там,
    // и без этого компилятор для x86 не находит собственные библиотеки.
    // Каталоги инструментов идут без буквы диска: Wine принимает такую форму
    // для поиска DLL, и именно она использовалась раньше. Полная DOS-нотация
    // здесь не нужна, а лишняя «z:» — повод для расхождения.
    const auto &target = cfg.targets.at(std::string(target_arch));
    const auto no_drive = [](const stdfs::path &p) {
        std::string s = p.string();
        std::replace(s.begin(), s.end(), '/', '\\');
        return s;
    };
    std::vector<std::string> wine_path = {
        no_drive(root / target.bin),
    };
    if (!target.sdk_bin.empty()) {
        wine_path.push_back(no_drive(root / target.sdk_bin));
    }
    // Третьим — каталог инструментов ХОСТОВОЙ архитектуры, уже в полной
    // нотации: там лежат mspdbcore.dll и её соседи, без которых компилятор
    // для x86 не находит собственные библиотеки.
    env.host_bin = root / (std::string("vc/tools/msvc/") + cfg.msvc_version + "/bin/Host" +
                           cfg.host_arch + "/" + cfg.host_arch);
    wine_path.push_back(to_wine_path(env.host_bin));
    // Последними — отладочные библиотеки времени выполнения, двумя частями.
    //
    // Без них программа, собранная с /MDd, не стартует вовсе, и это не
    // мелочь: именно так собирается конфигурация Debug по умолчанию. Wine
    // отдаёт как builtin только выпускные vcruntime140 и ucrtbase, а
    // отладочные лежат врозь: msvcp140d.dll и vcruntime140_1d.dll приходят из
    // редиста MSVC, ucrtbased.dll — из SDK. Каталог SDK при этом содержит
    // ровно один файл, так что затенить им нечего.
    //
    // Последними, потому что компилятору они не нужны, а подмешивать
    // отладочные библиотеки в поиск раньше выпускных незачем.
    if (!target.debug_crt.empty()) {
        wine_path.push_back(no_drive(root / target.debug_crt));
    }
    if (!cfg.sdk_version.empty()) {
        wine_path.push_back(no_drive(root / "kits" / "10" / "bin" / cfg.sdk_version /
                                     std::string(target_arch) / "ucrt"));
    }
    env.wine_path = join(wine_path, ';');

    // Настоящий vcruntime из MSVC вместо встроенного в Wine: иначе
    // собранное связывается с реализацией, которой у Microsoft нет.
    env.wine_dll_overrides = "vcruntime140=n;vcruntime140_1=n";
    return env;
}

} // namespace cork::setup
