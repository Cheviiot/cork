// Регрессии на подтверждённые дефекты выбора пакетов. Все они об одном:
// молчание вместо отказа. Неполный граф, подменённая версия или установка без
// MSBuild не должны считаться успехом.

#include <string>

#include "check.h"
#include "manifest/parse.hpp"
#include "resolve/resolve.hpp"


using namespace cork::resolve;

namespace {

// Фикстура собрана так, чтобы в ней был и полный рабочий набор, и каждая из
// ловушек: пакет с недостающей обязательной зависимостью, версионированный
// компонент MSVC и id «Win10SDK» без версии.
const char *kManifest = R"json({
  "info": { "productDisplayVersion": "18.8.0" },
  "packages": [
    { "id": "Microsoft.VisualStudio.Workload.VCTools", "type": "Workload", "version": "18.8",
      "dependencies": {
        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64": "18.8",
        "Win11SDK_10.0.26100": { "version": "10.0.26100", "type": "Recommended" }
      } },
    { "id": "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "type": "Component",
      "version": "18.8",
      "dependencies": { "Microsoft.VisualCpp.Tools.Core": "18.8" } },
    { "id": "Microsoft.VisualCpp.Tools.Core", "type": "Vsix", "version": "18.8" },

    { "id": "Microsoft.VisualStudio.Component.VC.14.40.17.10.x86.x64", "type": "Component",
      "version": "17.10",
      "dependencies": { "Microsoft.VisualCpp.Tools.Core": "17.10" } },

    { "id": "Microsoft.Build", "type": "Vsix", "version": "18.8" },
    { "id": "Microsoft.Build.Dependencies", "type": "Vsix", "version": "18.8" },
    { "id": "Microsoft.VisualCpp.DIA.SDK", "type": "Vsix", "version": "18.8" },
    { "id": "Microsoft.VisualStudio.VC.vcvars", "type": "Vsix", "version": "18.8" },
    { "id": "Microsoft.VisualStudio.PackageGroup.VsDevCmd", "type": "Group", "version": "18.8" },
    { "id": "Microsoft.VisualCpp.ASAN.X86", "type": "Vsix", "version": "18.8" },
    { "id": "Microsoft.VisualStudio.Component.VC.ATL", "type": "Component", "version": "18.8" },

    { "id": "Win11SDK_10.0.26100", "type": "Component", "version": "10.0.26100.16" },
    { "id": "Win10SDK_10.0.19041", "type": "Component", "version": "10.0.19041.0" },
    { "id": "Win10SDK_10.0.22621", "type": "Component", "version": "10.0.22621.0" },

    { "id": "Win10SDK", "type": "Component", "version": "0.0" },

    { "id": "Broken.Package", "type": "Vsix", "version": "1.0",
      "dependencies": { "Does.Not.Exist": "1.0" } },
    { "id": "Broken.Optional", "type": "Vsix", "version": "1.0",
      "dependencies": { "Also.Missing": { "version": "1.0", "type": "Optional" } } }
  ]
})json";

cork::manifest::Document parse() {
    auto doc = cork::manifest::parse_installer_manifest(kManifest);
    if (!doc.has_value()) {
        std::fprintf(stderr, "фикстура не разобралась: %s\n", doc.error().to_string().c_str());
        std::abort();
    }
    return std::move(*doc);
}

bool has_package(const Plan &p, std::string_view id) {
    for (const auto *pkg : p.packages) {
        if (pkg->id == id) {
            return true;
        }
    }
    return false;
}

} // namespace

int main() {
    const cork::manifest::Document doc = parse();
    const cork::manifest::Index idx = cork::manifest::Index::build(doc, "x64", "en");

    // --- базовый выбор по умолчанию ---
    {
        Options o;
        o.host_arch = "x64";
        auto plan = resolve_selection(idx, o);
        CHECK(plan.has_value());
        if (plan) {
            CHECK(has_package(*plan, "Microsoft.VisualStudio.Workload.VCTools"));
            CHECK(has_package(*plan, "Microsoft.VisualStudio.Component.VC.Tools.x86.x64"));
            CHECK(has_package(*plan, "Microsoft.VisualCpp.Tools.Core"));
            CHECK(has_package(*plan, "Microsoft.Build"));
            CHECK(!plan->digest.empty());
        }
    }

    // --- P1: отсутствующая обязательная зависимость это отказ ---
    // Замыкание зависимостей, возвращающее пустоту вместо отказа, отдаёт
    // отдавал неполный граф как успешный результат.
    {
        Options o;
        o.host_arch = "x64";
        o.packages = {"Broken.Package"};
        auto plan = resolve_selection(idx, o);
        CHECK(!plan.has_value());
        if (!plan) {
            const std::string msg = plan.error().to_string();
            CHECK(msg.find("Does.Not.Exist") != std::string::npos);
            CHECK(msg.find("Broken.Package") != std::string::npos);
        }
    }

    // Необязательная недостающая зависимость отказом не является.
    {
        Options o;
        o.host_arch = "x64";
        o.packages = {"Broken.Optional"};
        o.include_optional = true;
        auto plan = resolve_selection(idx, o);
        CHECK(plan.has_value());
    }

    // --- P1: явная версия MSVC не подменяется ---
    // «Didn't find exact version packages for 16.11, assuming this is provided
    // by the default/latest version» — так это выглядело, и пользователь
    // получал не то, что просил.
    {
        Options o;
        o.host_arch = "x64";
        o.msvc_version = "16.11";  // в фикстуре её нет
        auto plan = resolve_selection(idx, o);
        CHECK(!plan.has_value());
        if (!plan) {
            CHECK(plan.error().to_string().find("16.11") != std::string::npos);
        }
    }

    // --- P1/P2: заданная версия MSVC не отменяет остальной состав ---
    // Фолбэк with_default исключал случай заданной версии, поэтому
    // WithWorkload, WithDIA, WithMSBuild и WithDevCmd оставались невыставленными,
    // и установка выходила без MSBuild.exe — молча.
    {
        Options o;
        o.host_arch = "x64";
        o.msvc_version = "17.10";  // в фикстуре есть
        auto plan = resolve_selection(idx, o);
        CHECK(plan.has_value());
        if (plan) {
            CHECK(has_package(*plan, "Microsoft.VisualStudio.Component.VC.14.40.17.10.x86.x64"));
            CHECK(has_package(*plan, "Microsoft.Build"));
            CHECK(has_package(*plan, "Microsoft.Build.Dependencies"));
            CHECK(has_package(*plan, "Microsoft.VisualCpp.DIA.SDK"));
            CHECK(has_package(*plan, "Microsoft.VisualStudio.VC.vcvars"));
            CHECK(has_package(*plan, "Microsoft.VisualStudio.PackageGroup.VsDevCmd"));
            CHECK(has_package(*plan, "Microsoft.VisualStudio.Workload.VCTools"));
        }
    }

    // --- P2: id ровно «Win10SDK» не роняет процесс ---
    // Срез по фиксированному смещению без проверки длины ронял бы процесс.
    {
        Options o;
        o.host_arch = "x64";
        o.sdk_version = "10.0.26100";
        auto plan = resolve_selection(idx, o);
        CHECK(plan.has_value());
        if (plan) {
            CHECK(has_package(*plan, "Win11SDK_10.0.26100"));
        }
    }

    // Несуществующая версия SDK — отказ со списком того, что есть.
    {
        Options o;
        o.host_arch = "x64";
        o.sdk_version = "10.0.99999";
        auto plan = resolve_selection(idx, o);
        CHECK(!plan.has_value());
        if (!plan) {
            const std::string msg = plan.error().to_string();
            CHECK(msg.find("10.0.26100") != std::string::npos);
            CHECK(msg.find("10.0.19041") != std::string::npos);
        }
    }

    // --- исключение обязательной зависимости через --ignore ---
    // Это почти наверняка ошибка пользователя, и молчать о ней нельзя.
    {
        Options o;
        o.host_arch = "x64";
        o.packages = {"Microsoft.VisualStudio.Component.VC.Tools.x86.x64"};
        o.ignore = {"Microsoft.VisualCpp.Tools.Core"};
        auto plan = resolve_selection(idx, o);
        CHECK(!plan.has_value());
        if (!plan) {
            CHECK(plan.error().to_string().find("--ignore") != std::string::npos);
        }
    }

    // --- неизвестный пакет, названный пользователем ---
    {
        Options o;
        o.host_arch = "x64";
        o.packages = {"Totally.Made.Up"};
        auto plan = resolve_selection(idx, o);
        CHECK(!plan.has_value());
    }

    // --- детерминированность ---
    // Дайджест входит в имя поколения установки: одинаковый выбор обязан
    // давать одинаковое имя при каждом запуске.
    {
        Options o;
        o.host_arch = "x64";
        auto a = resolve_selection(idx, o);
        auto b = resolve_selection(idx, o);
        CHECK(a.has_value() && b.has_value());
        if (a && b) {
            CHECK_EQ(a->digest, b->digest);
            CHECK_EQ(std::to_string(a->packages.size()), std::to_string(b->packages.size()));
        }
    }

    // --- отключение SDK ---
    {
        Options o;
        o.host_arch = "x64";
        o.with_sdk = false;
        auto plan = resolve_selection(idx, o);
        CHECK(plan.has_value());
        if (plan) {
            CHECK(!has_package(*plan, "Win11SDK_10.0.26100"));
            CHECK(!has_package(*plan, "Win10SDK_10.0.19041"));
        }
    }

    // --- дерево зависимостей показывает то же, что и выбор ---
    {
        Options o;
        o.host_arch = "x64";
        o.packages = {"Microsoft.VisualStudio.Workload.VCTools"};
        const std::string tree = render_tree(idx, o);
        CHECK(tree.find("Microsoft.VisualStudio.Workload.VCTools") != std::string::npos);
        CHECK(tree.find("Microsoft.VisualCpp.Tools.Core") != std::string::npos);
        CHECK(tree.find("[recommended]") != std::string::npos);
    }

    return cork::test::finish("test_resolve");
}
