#include "exec/msbuild.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <system_error>

#include <fmt/format.h>

#include "base/fs.hpp"

namespace cork::exec {
namespace {

namespace stdfs = std::filesystem;

// Имена PlatformToolset, для которых заполняются VCInstallDir_<N>. Тот же
// список, что у алиасов в setup/relocate.cpp, и по той же причине: поколение
// 14.x бинарно совместимо само с собой, а v90…v120 — это другие ABI, и
// выдавать за них новый компилятор нельзя.
constexpr const char *kAbiCompatibleToolsets[] = {"140", "141", "142", "143"};

// v<цифры> и ничего больше: варианты вроде v140_xp сюда не подходят, и это
// правильно — они означают другой набор, а не другое написание того же.
bool numeric_toolset_dir(std::string_view name, std::string &number) {
    if (name.size() < 2 || name[0] != 'v') {
        return false;
    }
    for (std::size_t i = 1; i < name.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(name[i])) == 0) {
            return false;
        }
    }
    number = std::string(name.substr(1));
    return true;
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char &c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// Уже ли задано это глобальное свойство в командной строке. Формы «/p:Имя=»
// и «-property:Имя=», регистр не важен ни в ключе, ни в имени.
bool has_global_property(const std::vector<std::string> &args, std::string_view name) {
    const std::string needle = to_lower(name) + "=";
    for (const auto &a : args) {
        if (a.size() < 3 || (a[0] != '/' && a[0] != '-')) {
            continue;
        }
        const std::string lower = to_lower(a);
        for (const char *prefix : {"p:", "property:"}) {
            const std::size_t at = 1 + std::string_view(prefix).size();
            if (lower.compare(1, std::string_view(prefix).size(), prefix) == 0 &&
                lower.compare(at, needle.size(), needle) == 0) {
                return true;
            }
        }
    }
    return false;
}

bool has_node_reuse_switch(const std::vector<std::string> &args) {
    for (const auto &a : args) {
        if (a.size() < 4 || (a[0] != '/' && a[0] != '-')) {
            continue;
        }
        const std::string lower = to_lower(a);
        if (lower.compare(1, 10, "nodereuse:") == 0 || lower.compare(1, 3, "nr:") == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

std::string msbuild_platform(const std::string &arch) {
    // Единственная цель, у которой имя платформы не совпадает с нашим именем
    // архитектуры. Забыть про неё значит собрать x64 там, где просили x86.
    if (arch == "x86") {
        return "Win32";
    }
    if (arch == "arm") {
        return "ARM";
    }
    if (arch == "arm64") {
        return "ARM64";
    }
    return "x64";
}

std::vector<std::string> toolset_suffixes(const stdfs::path &generation_root) {
    std::set<std::string> seen;
    std::vector<std::string> out;
    const auto record = [&](const std::string &n) {
        if (seen.insert(n).second) {
            out.push_back(n);
        }
    };

    std::error_code ec;

    // Первый источник: имена, которыми Microsoft помечает установленный
    // компилятор — Microsoft.VCToolsVersion.v<N>.default.props.
    const stdfs::path aux = generation_root / "VC" / "Auxiliary" / "Build";
    for (const auto &entry : stdfs::directory_iterator(aux, ec)) {
        if (ec) {
            break;
        }
        const std::string name = entry.path().filename().string();
        constexpr std::string_view prefix = "Microsoft.VCToolsVersion.";
        constexpr std::string_view suffix = ".default.props";
        if (name.size() <= prefix.size() + suffix.size() || name.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }
        if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
            continue;
        }
        std::string number;
        if (numeric_toolset_dir(name.substr(prefix.size(), name.size() - prefix.size() - suffix.size()),
                                number)) {
            record(number);
        }
    }

    // Второй источник: версии схемы целей MSBuild. Это другая нумерация —
    // v150, v160, v170, v180 идут с самим MSBuild и не зависят от того, какой
    // компилятор установлен, — но спрашивают её тем же способом.
    const stdfs::path vc = generation_root / "MSBuild" / "Microsoft" / "VC";
    for (const auto &entry : stdfs::directory_iterator(vc, ec)) {
        if (ec) {
            break;
        }
        std::string number;
        if (numeric_toolset_dir(entry.path().filename().string(), number)) {
            record(number);
        }
    }

    // Третий: все исторические имена. Проект, привязанный к любому из них,
    // должен разрешиться в тот единственный компилятор, который стоит.
    for (const char *n : kAbiCompatibleToolsets) {
        record(n);
    }
    return out;
}

std::map<std::string, std::string> msbuild_env(const setup::Config &cfg,
                                               const stdfs::path &root,
                                               const std::string &target_arch) {
    const std::string base = setup::to_wine_path(root) + "\\";
    const std::string sdk = setup::to_wine_path(root / "kits" / "10") + "\\";
    const std::string msvc_base = setup::to_wine_path(root / "VC") + "\\";
    const std::string msvc_dir =
        setup::to_wine_path(root / "vc" / "tools" / "msvc" / cfg.msvc_version) + "\\";

    std::map<std::string, std::string> env{
        // Сборка драйверов ставит в INF дату по местным часам (StampInf), а
        // проверяет её по UTC (Inf2Cat). Восточнее Гринвича эти двое почти
        // весь день расходятся на календарный день, и только что проставленная
        // дата объявляется будущей. Одни часы для обоих снимают расхождение.
        {"TZ", "UTC"},

        // Перевод поиска с реестра на переменные окружения. Всё, что ниже,
        // имеет смысл только вместе с этим.
        {"DisableRegistryUse", "true"},

        // VCToolsVersion обязана быть настоящей версией. Пустую
        // Microsoft.Cpp.VCTools.props заменяет буквальной строкой
        // «VCToolsVersion_is_not_defined», и эта строка доезжает до сравнений
        // версий, которые ничем не защищены, — там она и роняет сборку.
        {"VCToolsVersion", cfg.msvc_version},

        // А это то, что делает безопасным сочетание настоящей версии с
        // устаревшим PlatformToolset: без него проверка соответствия
        // поколений отвергает пару «проект на v142, компилятор новее»,
        // то есть ровно тот случай, ради которого заведены алиасы. Всё
        // остальное, что эта проверка охраняет, — предупреждения о наличии
        // MFC, ATL и Spectre, на саму сборку не влияющие.
        {"CheckMSVCComponents", "false"},

        {"VsInstallRoot", base},
        {"VSInstallDir", base},
        {"SDKReferenceDirectoryRoot", base},
        {"SDKExtensionDirectoryRoot", base},
        {"MSBUILDSDKREFERENCEDIRECTORY", base},
        {"MSBUILDMULTIPLATFORMSDKREFERENCEDIRECTORY", base},

        {"WindowsSdkDir", sdk},
        {"WindowsSdkDir_10", sdk},
        {"UniversalCRTSdkDir", sdk},
        {"UniversalCRTSdkDir_10", sdk},
        {"UCRTContentRoot", sdk},
        {"NETFXKitsDir", sdk},
        {"NETFXSDKDir", sdk},
        {"WindowsTargetPlatformVersion", cfg.sdk_version},

        {"Platform", msbuild_platform(target_arch)},
    };

    // VCInstallDir_<N> и VCToolsInstallDir_<N> спрашиваются по двум разным
    // нумерациям: одна — короткое имя PlatformToolset из .vcxproj, другая —
    // версия схемы целей MSBuild. Заполняем все, какие могут спросить.
    //
    // Если набор с таким именем установлен по-настоящему, имя ведёт именно в
    // него: проект, привязанный к v142, должен собираться настоящим v142.
    // Остальные имена ведут в основной набор — это подмена, и она допустима
    // только внутри поколения 14.x, где Microsoft держит бинарную
    // совместимость. Отбор имён для подмены делает kAbiCompatibleToolsets.
    std::vector<std::string> names = toolset_suffixes(root);
    // Имена установленных наборов добавляются безусловно: мы знаем, что они
    // есть, даже если обход дерева их не нашёл.
    for (const auto &[short_name, version] : cfg.toolsets) {
        if (std::find(names.begin(), names.end(), short_name) == names.end()) {
            names.push_back(short_name);
        }
    }

    for (const auto &n : names) {
        env["VCInstallDir_" + n] = msvc_base;
        if (const auto real = cfg.toolsets.find(n); real != cfg.toolsets.end()) {
            env["VCToolsInstallDir_" + n] =
                setup::to_wine_path(root / "vc" / "tools" / "msvc" / real->second) + "\\";
        } else {
            env["VCToolsInstallDir_" + n] = msvc_dir;
        }
    }

    if (auto target = cfg.targets.find(target_arch); target != cfg.targets.end()) {
        if (target->second.msbuild_bin.find("amd64") != std::string::npos) {
            env["PreferredToolArchitecture"] = "x64";
        }
    }
    return env;
}

std::vector<std::string> msbuild_forced_args(const setup::Config &cfg,
                                             const std::vector<std::string> &args) {
    std::vector<std::string> out;

    // Единственное свойство, которому не хватает переменной окружения.
    // Остальные — вход для поиска файла свойств, и наше значение видно всегда.
    // А WindowsTargetPlatformVersion сам записан в PropertyGroup большинства
    // старых .vcxproj, и явное присваивание в проекте всегда сильнее
    // унаследованной переменной. Глобальное свойство из командной строки —
    // единственное, что проект перебить не может; оно здесь и нужно, потому
    // что SDK у нас установлен ровно один, и проект должен собираться против
    // него, а не падать из-за версии, вписанной когда-то в другой системе.
    if (!cfg.sdk_version.empty() && !has_global_property(args, "WindowsTargetPlatformVersion")) {
        out.push_back("/p:WindowsTargetPlatformVersion=" + cfg.sdk_version);
    }

    // Узлы повторного использования MSBuild ведут себя не как обычные дети:
    // они намеренно переживают породивший их msbuild.exe и ждут следующего
    // вызова. Здесь это особенно плохо. Во-первых, такой узел остаётся в
    // префиксе сессии и будет убит её сносом — в лучшем случае. Во-вторых,
    // прерванная сборка способна оставить узел с полуоткрытым каналом, и он
    // не умирает, а зависает навсегда, отвечая на каждый следующий вызов
    // ошибкой, никак не связанной с происходящим.
    //
    // Поэтому по умолчанию выключено: каждый вызов получает чистый процесс.
    // Цена — сотни миллисекунд на запуск узла. Тот, кому повторное
    // использование нужно осознанно, задаёт свой /nodeReuse и получает его.
    if (!has_node_reuse_switch(args)) {
        out.emplace_back("/nodeReuse:false");
    }
    return out;
}

} // namespace cork::exec
