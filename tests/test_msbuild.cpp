// Окружение и аргументы MSBuild.
//
// Всё здесь — про то, что MSBuild ищет компилятор и SDK через реестр, которого
// под Wine нет. Переменные, которые заменяют этот поиск, добыты отладкой
// конкретных отказов, и проверяются они так же конкретно: не «набор непуст», а
// «то, без чего сборка падает, на месте».

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include <fmt/format.h>

#include "check.h"
#include "exec/msbuild.hpp"
#include "setup/config.hpp"

using namespace cork;
using namespace cork::exec;

namespace stdfs = std::filesystem;

namespace {

std::vector<std::string> a(std::initializer_list<const char *> items) {
    return std::vector<std::string>(items.begin(), items.end());
}

setup::Config make_config() {
    setup::Config cfg;
    cfg.msvc_version = "14.51.36231";
    cfg.sdk_version = "10.0.26100.0";
    cfg.host_arch = "x64";
    cfg.targets["x64"] = setup::TargetPaths{"vc/tools/msvc/14.51.36231/bin/Hostx64/x64", "",
                                            "MSBuild/Current/Bin/amd64", ""};
    cfg.targets["x86"] = setup::TargetPaths{"vc/tools/msvc/14.51.36231/bin/Hostx64/x86", "",
                                            "MSBuild/Current/Bin", ""};
    return cfg;
}

void test_platform_names() {
    // Единственная цель, у которой имя платформы не совпадает с именем
    // архитектуры. Забыть про неё — собрать x64 там, где просили x86.
    const std::string x86 = msbuild_platform("x86");
    const std::string x64 = msbuild_platform("x64");
    const std::string arm64 = msbuild_platform("arm64");
    const std::string arm = msbuild_platform("arm");
    CHECK_EQ(x86, std::string("Win32"));
    CHECK_EQ(x64, std::string("x64"));
    CHECK_EQ(arm64, std::string("ARM64"));
    CHECK_EQ(arm, std::string("ARM"));
}

void test_environment_covers_the_registry_lookup() {
    const setup::Config cfg = make_config();
    const auto env = msbuild_env(cfg, "/gen", "x64");

    // Без этого весь остальной набор бессмыслен: поиск остаётся в реестре.
    CHECK(env.at("DisableRegistryUse") == "true");

    // Пустая VCToolsVersion заменяется на буквальный плейсхолдер, который
    // потом доезжает до сравнения версий и роняет сборку.
    CHECK_EQ(env.at("VCToolsVersion"), cfg.msvc_version);

    // А это то, что делает устаревший PlatformToolset совместимым с новым
    // компилятором: без него проверка поколений отвергает пару.
    CHECK(env.at("CheckMSVCComponents") == "false");

    // Часы драйверной сборки: дату ставят по местным, проверяют по UTC.
    CHECK(env.at("TZ") == "UTC");

    CHECK(env.count("WindowsSdkDir") == 1);
    CHECK(env.count("UniversalCRTSdkDir_10") == 1);
    CHECK_EQ(env.at("WindowsTargetPlatformVersion"), cfg.sdk_version);
    CHECK_EQ(env.at("Platform"), std::string("x64"));

    // Пути в DOS-нотации и с завершающим разделителем: файлы свойств
    // склеивают их с именем подкаталога напрямую.
    CHECK(env.at("VsInstallRoot").rfind("z:", 0) == 0);
    CHECK(env.at("VsInstallRoot").back() == '\\');
}

void test_environment_follows_the_target() {
    const setup::Config cfg = make_config();
    const auto x86_env = msbuild_env(cfg, "/gen", "x86");
    CHECK_EQ(x86_env.at("Platform"), std::string("Win32"));

    // Хостовая архитектура инструментов берётся из пути MSBuild цели, а не
    // угадывается: для x86 там каталог без amd64.
    const auto x64_env = msbuild_env(cfg, "/gen", "x64");
    CHECK(x64_env.count("PreferredToolArchitecture") == 1);
    CHECK(x86_env.count("PreferredToolArchitecture") == 0);
}

void test_abi_compatible_names_resolve() {
    // Поколение 14.x бинарно совместимо само с собой, поэтому проект на v142
    // можно собрать установленным v145, и имя обязано разрешиться.
    const setup::Config cfg = make_config();
    const auto env = msbuild_env(cfg, "/gen", "x64");
    for (const char *n : {"140", "141", "142", "143"}) {
        CHECK(env.count(std::string("VCInstallDir_") + n) == 1);
        CHECK(env.count(std::string("VCToolsInstallDir_") + n) == 1);
    }
}

void test_ancient_names_are_not_faked() {
    // v90, v100, v110 и v120 — это 9.0, 10.0, 11.0 и 12.0, другие ABI.
    // Выдать за них 14.x значит получить двоичный файл, несовместимый с
    // библиотеками той эпохи, причём молча. Пусть лучше MSBuild скажет, что
    // набора нет.
    const setup::Config cfg = make_config();
    const auto env = msbuild_env(cfg, "/gen", "x64");
    for (const char *n : {"90", "100", "110", "120"}) {
        CHECK(env.count(std::string("VCToolsInstallDir_") + n) == 0);
    }
}

void test_a_real_toolset_beats_the_stand_in() {
    // Когда набор установлен по-настоящему, имя должно вести именно в него, а
    // не в основной. Иначе установка второго поколения не имела бы смысла:
    // проект на v142 всё равно собирался бы v145.
    setup::Config cfg = make_config();
    cfg.toolsets = {{"145", "14.51.36231"}, {"142", "14.29.30133"}};
    const auto env = msbuild_env(cfg, "/gen", "x64");

    CHECK(env.at("VCToolsInstallDir_142").find("14.29.30133") != std::string::npos);
    CHECK(env.at("VCToolsInstallDir_145").find("14.51.36231") != std::string::npos);
    // А имя без своего набора по-прежнему ведёт в основной.
    CHECK(env.at("VCToolsInstallDir_141").find("14.51.36231") != std::string::npos);
}

void test_forced_properties() {
    const setup::Config cfg = make_config();

    const auto plain = msbuild_forced_args(cfg, a({"project.vcxproj"}));
    bool has_sdk = false;
    bool has_node_reuse = false;
    for (const auto &x : plain) {
        has_sdk = has_sdk || x.find("WindowsTargetPlatformVersion=") != std::string::npos;
        has_node_reuse = has_node_reuse || x == "/nodeReuse:false";
    }
    CHECK(has_sdk);
    CHECK(has_node_reuse);
}

void test_user_switches_win() {
    const setup::Config cfg = make_config();

    // Явно заданное пользователем не перебивается. Версия SDK — свойство,
    // которое проект может и захотеть закрепить сам.
    for (const auto &given : {a({"/p:WindowsTargetPlatformVersion=10.0.19041.0"}),
                              a({"-property:windowstargetplatformversion=10.0.19041.0"})}) {
        const auto forced = msbuild_forced_args(cfg, given);
        for (const auto &x : forced) {
            CHECK(x.find("WindowsTargetPlatformVersion") == std::string::npos);
        }
    }

    // То же для повторного использования узлов: кто просит его осознанно,
    // получает его.
    for (const auto &given : {a({"/nodeReuse:true"}), a({"-nr:true"}), a({"/NODEREUSE:false"})}) {
        const auto forced = msbuild_forced_args(cfg, given);
        for (const auto &x : forced) {
            CHECK(x != "/nodeReuse:false");
        }
    }
}

void test_no_sdk_no_forced_version() {
    // Установка без SDK возможна, и навязывать пустую версию нельзя: это
    // свойство сломало бы сборку вернее, чем его отсутствие.
    setup::Config cfg = make_config();
    cfg.sdk_version.clear();
    for (const auto &x : msbuild_forced_args(cfg, a({"project.vcxproj"}))) {
        CHECK(x.find("WindowsTargetPlatformVersion") == std::string::npos);
    }
}

void test_toolset_suffixes_from_disk() {
    const char *base = std::getenv("CORK_TEST_TMP");
    stdfs::path dir = base != nullptr && *base != '\0' ? stdfs::path(base)
                                                       : stdfs::current_path() / ".testtmp";
    dir /= fmt::format("msbuild-{}", ::getpid());
    std::error_code ec;
    stdfs::remove_all(dir, ec);

    // Две разные нумерации: имя компилятора и версия схемы целей MSBuild.
    // Спрашивают обе, и обе должны найтись.
    stdfs::create_directories(dir / "VC" / "Auxiliary" / "Build", ec);
    std::ofstream(dir / "VC" / "Auxiliary" / "Build" /
                  "Microsoft.VCToolsVersion.v145.default.props")
        << "x";
    stdfs::create_directories(dir / "MSBuild" / "Microsoft" / "VC" / "v180", ec);
    // Нечисловое имя не набор инструментов и попасть в список не должно.
    stdfs::create_directories(dir / "MSBuild" / "Microsoft" / "VC" / "v140_xp", ec);

    const auto found = toolset_suffixes(dir);
    const auto has = [&found](std::string_view n) {
        return std::find(found.begin(), found.end(), n) != found.end();
    };
    CHECK(has("145"));
    CHECK(has("180"));
    CHECK(has("142"));  // ABI-совместимое, всегда в списке
    CHECK(!has("120"));  // другой ABI — подменять нельзя
    CHECK(!has("140_xp"));

    // Повторов нет: одно и то же имя из двух источников — одна запись.
    auto sorted = found;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());

    stdfs::remove_all(dir, ec);
}

} // namespace

int main() {
    test_platform_names();
    test_environment_covers_the_registry_lookup();
    test_environment_follows_the_target();
    test_abi_compatible_names_resolve();
    test_ancient_names_are_not_faked();
    test_a_real_toolset_beats_the_stand_in();
    test_forced_properties();
    test_user_switches_win();
    test_no_sdk_no_forced_version();
    test_toolset_suffixes_from_disk();
    return cork::test::finish("test_msbuild");
}
