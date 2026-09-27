// Сценарии повреждений.
//
// Смысл этого теста в одном предложении: сегодняшний случай «Wine Mono
// развалено, а doctor печатает All checks passed» должен стать невозможным по
// устройству. Проверяется это на синтетическом дереве, поэтому ни настоящий
// MSVC, ни Wine, ни сеть здесь не нужны — тест идёт в CI на каждый коммит.
//
// Каждый сценарий сначала убеждается, что здоровое дерево признано здоровым, и
// только потом ломает ровно одну вещь: иначе «упало» ничего не доказывает.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include <fmt/format.h>

#include "base/fs.hpp"
#include "base/sha256.hpp"
#include "check.h"
#include "setup/config.hpp"
#include "setup/digest.hpp"
#include "setup/doctor.hpp"
#include "setup/generation.hpp"
#include "setup/receipt.hpp"

using namespace cork;
using namespace cork::setup;

namespace stdfs = std::filesystem;

namespace {

constexpr const char *kMsvc = "14.51.36231";
constexpr const char *kSdk = "10.0.26100.0";
constexpr const char *kWineId = "11.18-mono11.3.0";

class Sandbox {
public:
    Sandbox() {
        const char *base = std::getenv("CORK_TEST_TMP");
        stdfs::path parent = base != nullptr && *base != '\0' ? stdfs::path(base)
                                                              : stdfs::current_path() / ".testtmp";
        path_ = parent / fmt::format("doctor-{}-{}", ::getpid(), ++counter_);
        std::error_code ec;
        stdfs::remove_all(path_, ec);
        stdfs::create_directories(path_, ec);
    }
    ~Sandbox() {
        std::error_code ec;
        stdfs::remove_all(path_, ec);
    }
    [[nodiscard]] const stdfs::path &path() const { return path_; }

private:
    static inline int counter_ = 0;
    stdfs::path path_;
};

void write_file(const stdfs::path &p, std::string_view content) {
    std::error_code ec;
    stdfs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

// Дерево Wine: несколько обычных файлов, символьные ссылки и «сборка Mono».
// Настоящее весит под гигабайт, но проверяются в нём ровно две величины —
// сколько ссылок и сколько файлов Mono, — а они воспроизводятся и на десяти
// файлах.
void make_wine_runtime(const stdfs::path &runtime) {
    write_file(runtime / "bin" / "wine", "#!/bin/sh\n");
    write_file(runtime / "lib" / "wine" / "x86_64-windows" / "kernel32.dll", "MZ");
    for (int i = 0; i < 4; ++i) {
        write_file(runtime / "share" / "wine" / "mono" / "lib" / "mono" / "4.5" /
                       fmt::format("System.{}.dll", i),
                   "mono assembly");
    }
    std::error_code ec;
    for (int i = 0; i < 3; ++i) {
        stdfs::create_symlink("kernel32.dll",
                              runtime / "lib" / "wine" / "x86_64-windows" /
                                  fmt::format("alias{}.dll", i),
                              ec);
    }
}

struct Installation {
    Root root;
    stdfs::path generation;
};

// Собирает и публикует поколение настоящим путём: через каталог сборки,
// дайджест и переименование. Проверять доктора на дереве, собранном в обход
// публикации, значило бы проверять не то, что бывает в жизни.
Installation make_installation(const Sandbox &box) {
    Installation inst;
    inst.root = Root{box.path() / "home"};
    CHECK(inst.root.ensure().has_value());
    make_wine_runtime(inst.root.runtime(kWineId));

    auto staging = Staging::create(inst.root);
    CHECK(staging.has_value());
    const stdfs::path dir = staging->path();

    const std::string helper_body = "MZ fake pe helper";
    write_file(dir / "bin" / "cork-helper.exe", helper_body);
    const std::string tools = fmt::format("vc/tools/msvc/{}/bin/Hostx64", kMsvc);
    write_file(dir / tools / "x64" / "cl.exe", "cl");
    write_file(dir / tools / "x64" / "link.exe", "link");
    write_file(dir / "kits" / "10" / "include" / kSdk / "ucrt" / "stdio.h", "#pragma once\n");
    write_file(dir / "kits" / "10" / "lib" / kSdk / "um" / "x64" / "kernel32.lib", "lib");

    Config cfg;
    cfg.generation = fmt::format("{}-{}", kMsvc, kSdk);
    cfg.created_by_version = "test";
    cfg.host_arch = "x64";
    cfg.dotnet_host = "amd64";
    cfg.msvc_version = kMsvc;
    cfg.sdk_version = kSdk;
    cfg.wine_id = kWineId;
    cfg.helper_path = "bin/cork-helper.exe";
    cfg.helper_sha256 = Sha256::hex_of(helper_body);
    cfg.targets["x64"] = TargetPaths{tools + "/x64", "", "MSBuild/Current/Bin/amd64", ""};
    CHECK(cfg.save(dir).has_value());

    Receipt receipt;
    CHECK(receipt.advance(State::Resolved, "test").has_value());
    CHECK(receipt.advance(State::Fetched, "test").has_value());
    CHECK(receipt.advance(State::Staged, "test").has_value());

    auto wine = inspect_wine(inst.root.runtime(kWineId));
    CHECK(wine.has_value());
    receipt.wine.id = kWineId;
    receipt.wine.symlinks = wine->symlinks;
    receipt.wine.mono_files = wine->mono_files;

    DigestOptions dopts;
    dopts.mode = DigestMode::Content;
    dopts.exclude = {kReceiptFileName};
    auto digest = digest_tree(dir, dopts);
    CHECK(digest.has_value());
    receipt.tree = *digest;

    CHECK(receipt.advance(State::Verified, "test").has_value());
    CHECK(receipt.advance(State::Published, "test").has_value());
    CHECK(receipt.save(dir).has_value());

    auto published = staging->publish(cfg.generation, digest->digest);
    CHECK(published.has_value());
    inst.generation = *published;
    return inst;
}

Report check(const Installation &inst, CheckLevel level = CheckLevel::Quick) {
    DoctorOptions opts;
    opts.level = level;
    auto report = diagnose(inst.root, opts);
    CHECK(report.has_value());
    return report.has_value() ? *report : Report{};
}

// Есть ли среди отказов тот, что назван так. Проверяется именно имя: доктор,
// который падает не на том, на чём сломано, бесполезен почти так же, как
// доктор, который не падает вовсе.
bool failed_on(const Report &r, std::string_view name) {
    for (const auto &c : r.checks) {
        if (c.severity == Severity::Failure && c.name == name) {
            return true;
        }
    }
    return false;
}

void test_healthy_installation_passes() {
    Sandbox box;
    auto inst = make_installation(box);
    auto report = check(inst);
    CHECK(report.healthy());
    CHECK(report.count(Severity::Failure) == 0);
    // И на глубоком уровне тоже: дайджест записан по этому же дереву.
    auto deep = check(inst, CheckLevel::Deep);
    CHECK(deep.healthy());
    CHECK(!failed_on(deep, "tree digest"));
}

void test_missing_receipt() {
    Sandbox box;
    auto inst = make_installation(box);
    std::error_code ec;
    stdfs::remove(inst.generation / kReceiptFileName, ec);
    auto report = check(inst);
    CHECK(!report.healthy());
    CHECK(failed_on(report, "receipt"));
}

void test_receipt_from_a_different_schema() {
    Sandbox box;
    auto inst = make_installation(box);
    write_file(inst.generation / kReceiptFileName,
               R"({"schema":"cork/receipt","schema_version":99})");
    auto report = check(inst);
    CHECK(!report.healthy());
    CHECK(failed_on(report, "receipt"));
}

void test_generation_that_was_never_published() {
    Sandbox box;
    auto inst = make_installation(box);
    // Журнал обрывается на staged: ровно так выглядел бы каталог, если бы
    // кто-то переименовал его на место руками, минуя проверки.
    auto receipt = Receipt::load(inst.generation);
    CHECK(receipt.has_value());
    while (!receipt->log.empty() && receipt->log.back().state > State::Staged) {
        receipt->log.pop_back();
    }
    CHECK(receipt->save(inst.generation).has_value());
    auto report = check(inst);
    CHECK(!report.healthy());
    CHECK(failed_on(report, "state"));
}

void test_missing_configuration() {
    Sandbox box;
    auto inst = make_installation(box);
    std::error_code ec;
    stdfs::remove(inst.generation / kConfigFileName, ec);
    auto report = check(inst);
    CHECK(!report.healthy());
    CHECK(failed_on(report, "configuration"));
}

void test_substituted_helper() {
    Sandbox box;
    auto inst = make_installation(box);
    // Размер тот же, содержимое другое: проверка по наличию файла или по его
    // размеру такого не заметит.
    write_file(inst.generation / "bin" / "cork-helper.exe", "MZ other pe helper");
    auto report = check(inst);
    CHECK(!report.healthy());
    CHECK(failed_on(report, "pe helper hash"));
}

void test_missing_compiler() {
    Sandbox box;
    auto inst = make_installation(box);
    std::error_code ec;
    stdfs::remove(inst.generation / fmt::format("vc/tools/msvc/{}/bin/Hostx64/x64/cl.exe", kMsvc),
                  ec);
    auto report = check(inst);
    CHECK(!report.healthy());
    CHECK(failed_on(report, "cl.exe (x64)"));
}

void test_missing_sdk_headers() {
    Sandbox box;
    auto inst = make_installation(box);
    std::error_code ec;
    stdfs::remove(inst.generation / "kits" / "10" / "include" / kSdk / "ucrt" / "stdio.h", ec);
    auto report = check(inst);
    CHECK(!report.healthy());
    CHECK(failed_on(report, "sdk headers"));
}

void test_wine_symlinks_replaced_by_files() {
    // Самая частая порча целиком: распаковщик записал символьные ссылки
    // обычными файлами с путём внутри, и в дереве Wine их стало ноль. Все
    // каталоги при этом на месте, размеры правдоподобные — проверка по форме
    // такое дерево принимает.
    Sandbox box;
    auto inst = make_installation(box);
    const stdfs::path dir = inst.root.runtime(kWineId) / "lib" / "wine" / "x86_64-windows";
    std::error_code ec;
    for (int i = 0; i < 3; ++i) {
        const stdfs::path link = dir / fmt::format("alias{}.dll", i);
        stdfs::remove(link, ec);
        write_file(link, "kernel32.dll");
    }
    auto report = check(inst);
    CHECK(!report.healthy());
    CHECK(failed_on(report, "wine symlinks"));
}

void test_gutted_mono() {
    // Тот же случай, но с другой стороны: файлы Mono исчезли. Без них
    // MSBuild.exe не стартует, и узнать об этом надо здесь, а не посреди
    // сборки чужого проекта.
    Sandbox box;
    auto inst = make_installation(box);
    std::error_code ec;
    stdfs::remove_all(inst.root.runtime(kWineId) / "share" / "wine" / "mono", ec);
    auto report = check(inst);
    CHECK(!report.healthy());
    CHECK(failed_on(report, "wine mono"));
}

void test_missing_wine_runtime() {
    Sandbox box;
    auto inst = make_installation(box);
    std::error_code ec;
    stdfs::remove_all(inst.root.runtime(kWineId), ec);
    auto report = check(inst);
    CHECK(!report.healthy());
    CHECK(failed_on(report, "wine binary"));
}

void test_deep_check_notices_a_changed_file() {
    Sandbox box;
    auto inst = make_installation(box);
    // Тот же размер, другое содержимое: быстрая проверка такого не видит и не
    // должна — на то и есть глубокая.
    write_file(inst.generation / fmt::format("vc/tools/msvc/{}/bin/Hostx64/x64/cl.exe", kMsvc),
               "CL");
    auto quick = check(inst);
    CHECK(quick.healthy());
    auto deep = check(inst, CheckLevel::Deep);
    CHECK(!deep.healthy());
    CHECK(failed_on(deep, "tree digest"));
}

void test_dangling_current() {
    Sandbox box;
    auto inst = make_installation(box);
    std::error_code ec;
    stdfs::remove_all(inst.generation, ec);
    auto report = check(inst);
    CHECK(!report.healthy());
    CHECK(failed_on(report, "current generation"));
}

void test_nothing_installed() {
    Sandbox box;
    Root root{box.path() / "home"};
    CHECK(root.ensure().has_value());
    DoctorOptions opts;
    auto report = diagnose(root, opts);
    CHECK(report.has_value());
    CHECK(!report->healthy());
    CHECK(failed_on(*report, "current generation"));
}

} // namespace

int main() {
    test_healthy_installation_passes();
    test_missing_receipt();
    test_receipt_from_a_different_schema();
    test_generation_that_was_never_published();
    test_missing_configuration();
    test_substituted_helper();
    test_missing_compiler();
    test_missing_sdk_headers();
    test_wine_symlinks_replaced_by_files();
    test_gutted_mono();
    test_missing_wine_runtime();
    test_deep_check_notices_a_changed_file();
    test_dangling_current();
    test_nothing_installed();
    return cork::test::finish("test_doctor");
}
