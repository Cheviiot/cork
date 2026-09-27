// Разбор манифеста, версии и индекс. Здесь же регрессия на дефект P1:
// ограничение версии обязано учитываться при выборе варианта.

#include <string>

#include "check.h"
#include "manifest/index.hpp"
#include "manifest/parse.hpp"
#include "manifest/version.hpp"

using namespace cork::manifest;

namespace {

// Уменьшенный до сути кусок настоящего манифеста: два варианта одного пакета
// разных версий, зависимости в обеих формах записи, локализация.
const char *kManifest = R"json({
  "info": { "productDisplayVersion": "18.8.0" },
  "packages": [
    {
      "id": "Microsoft.VisualCpp.Tools",
      "type": "Vsix",
      "version": "1.0",
      "chip": "x64",
      "payloads": [
        { "fileName": "a\\b\\tools.vsix", "url": "https://example/tools-1.vsix",
          "sha256": "AABB", "size": 100 }
      ],
      "installSizes": { "targetDrive": 500, "systemDrive": 20 }
    },
    {
      "id": "Microsoft.VisualCpp.Tools",
      "type": "Vsix",
      "version": "2.0",
      "chip": "x64",
      "payloads": [
        { "fileName": "tools.vsix", "url": "https://example/tools-2.vsix",
          "sha256": "ccdd", "size": 200 }
      ]
    },
    {
      "id": "Microsoft.VisualStudio.Workload.VCTools",
      "type": "Workload",
      "version": "18.8.35201.0",
      "localizedResources": [
        { "language": "en-US", "title": "Desktop C++", "description": "d", "license": "L" },
        { "language": "ru-RU", "title": "Разработка C++", "description": "о", "license": "L" }
      ],
      "dependencies": {
        "Microsoft.VisualCpp.Tools": "2.0",
        "Microsoft.VisualCpp.Optional": { "version": "1.0", "type": "Optional" },
        "Microsoft.Renamed": { "version": "[1.0,2.0)", "id": "Microsoft.Actual" }
      }
    },
    {
      "id": "Win11SDK_10.0.26100",
      "type": "Component",
      "version": "10.0.26100.16",
      "machineArch": "neutral"
    }
  ]
})json";

} // namespace

int main() {
    // --- разбор версий ---
    {
        const Version v = Version::parse("14.44.35207.1");
        CHECK_EQ(v.to_string(), std::string("14.44.35207.1"));
        // Недостающие части — нули, поэтому 14.44 и 14.44.0.0 равны.
        CHECK(Version::parse("14.44") == Version::parse("14.44.0.0"));
        CHECK(Version::parse("14.44") < Version::parse("14.45"));
        CHECK(Version::parse("2.0") > Version::parse("1.9.9.9"));
        // Суффикс не участвует в сравнении, но сохраняется.
        const Version pre = Version::parse("17.0.0-preview");
        CHECK_EQ(pre.suffix, std::string("-preview"));
        CHECK(pre == Version::parse("17.0.0"));
        // Мусор не должен давать случайное число.
        CHECK(Version::parse("") == Version::parse("0"));
    }

    // --- диапазоны ---
    {
        CHECK(VersionRange::parse("").is_any());
        CHECK(VersionRange::parse("").contains(Version::parse("0.1")));

        // Голая версия означает «не ниже».
        const auto bare = VersionRange::parse("1.0");
        CHECK(bare.contains(Version::parse("1.0")));
        CHECK(bare.contains(Version::parse("2.0")));
        CHECK(!bare.contains(Version::parse("0.9")));

        const auto exact = VersionRange::parse("[1.0]");
        CHECK(exact.contains(Version::parse("1.0")));
        CHECK(!exact.contains(Version::parse("1.1")));

        const auto half_open = VersionRange::parse("[1.0,2.0)");
        CHECK(half_open.contains(Version::parse("1.0")));
        CHECK(half_open.contains(Version::parse("1.9.9")));
        CHECK(!half_open.contains(Version::parse("2.0")));
        CHECK(!half_open.contains(Version::parse("0.9")));

        const auto open_low = VersionRange::parse("(1.0,2.0]");
        CHECK(!open_low.contains(Version::parse("1.0")));
        CHECK(open_low.contains(Version::parse("2.0")));

        const auto at_least = VersionRange::parse("[1.0,)");
        CHECK(at_least.contains(Version::parse("9.9")));
        CHECK(!at_least.contains(Version::parse("0.1")));

        const auto at_most = VersionRange::parse("(,2.0]");
        CHECK(at_most.contains(Version::parse("0.1")));
        CHECK(at_most.contains(Version::parse("2.0")));
        CHECK(!at_most.contains(Version::parse("2.1")));
    }

    // --- разбор документа ---
    Document doc;
    {
        auto parsed = parse_installer_manifest(kManifest);
        CHECK(parsed.has_value());
        if (!parsed) {
            std::fprintf(stderr, "%s\n", parsed.error().to_string().c_str());
            return cork::test::finish("test_manifest");
        }
        doc = std::move(*parsed);

        CHECK_EQ(doc.product_display_version, std::string("18.8.0"));
        CHECK_EQ(std::to_string(doc.packages.size()), std::string("4"));

        const auto &first = doc.packages[0];
        CHECK_EQ(first.id, std::string("Microsoft.VisualCpp.Tools"));
        CHECK_EQ(std::to_string(first.payloads.size()), std::string("1"));
        // fileName приходит с путём внутри пакета; на диске нужно имя файла.
        CHECK_EQ(first.payloads[0].base_name(), std::string("tools.vsix"));
        CHECK_EQ(std::to_string(first.payloads[0].size), std::string("100"));
        // installSizes суммируется по всем дискам.
        CHECK_EQ(std::to_string(first.install_size), std::string("520"));

        // Зависимости: обе формы записи и переопределение id.
        const auto &workload = doc.packages[2];
        CHECK_EQ(std::to_string(workload.dependencies.size()), std::string("3"));
        bool saw_optional = false;
        bool saw_renamed = false;
        for (const auto &d : workload.dependencies) {
            if (d.id == "Microsoft.VisualCpp.Optional") {
                saw_optional = d.kind == Dependency::Kind::Optional;
            }
            // Объектная форма с полем "id" переопределяет ключ.
            if (d.id == "Microsoft.Actual") {
                saw_renamed = d.version.contains(Version::parse("1.5")) &&
                              !d.version.contains(Version::parse("2.0"));
            }
        }
        CHECK(saw_optional);
        CHECK(saw_renamed);

        // Локализация: точное совпадение языка предпочитается.
        const auto *ru = workload.localized_for("ru-RU");
        CHECK(ru != nullptr);
        if (ru != nullptr) {
            CHECK_EQ(ru->title, std::string("Разработка C++"));
        }
        const auto *en = workload.localized_for("en");
        CHECK(en != nullptr);
        if (en != nullptr) {
            CHECK_EQ(en->title, std::string("Desktop C++"));
        }
    }

    // --- индекс и ограничение версии (регрессия P1) ---
    {
        const Index idx = Index::build(doc, "x64", "en");

        CHECK(idx.contains("microsoft.visualcpp.tools"));
        CHECK(idx.contains("Microsoft.VisualCpp.Tools"));  // регистр не важен
        CHECK(!idx.contains("Nope"));

        // Поиск обязан учитывать запрошенную версию, а не отдавать первый
        // подходящий по архитектуре вариант.
        Constraints want_two;
        want_two.version = VersionRange::parse("2.0");
        auto two = idx.find("Microsoft.VisualCpp.Tools", want_two);
        CHECK(two.has_value());
        if (two) {
            CHECK_EQ((*two)->version_text, std::string("2.0"));
        }

        Constraints want_exact_one;
        want_exact_one.version = VersionRange::parse("[1.0]");
        auto one = idx.find("Microsoft.VisualCpp.Tools", want_exact_one);
        CHECK(one.has_value());
        if (one) {
            CHECK_EQ((*one)->version_text, std::string("1.0"));
        }

        // Недостижимое ограничение — отказ, а не «сойдёт что-нибудь».
        Constraints impossible;
        impossible.version = VersionRange::parse("[9.0]");
        auto none = idx.find("Microsoft.VisualCpp.Tools", impossible);
        CHECK(!none.has_value());
        if (!none) {
            // В сообщении должно быть видно, что есть на самом деле.
            const std::string msg = none.error().to_string();
            CHECK(msg.find("1.0") != std::string::npos);
            CHECK(msg.find("2.0") != std::string::npos);
        }

        // Отсутствующий пакет и неподходящий вариант — разные исходы, и это
        // должно быть видно по тексту отказа, а не только по его наличию.
        auto missing = idx.find("No.Such.Package", Constraints{});
        CHECK(!missing.has_value());
        if (!missing) {
            CHECK(missing.error().to_string().find("no package") != std::string::npos);
        }

        // neutral подходит под любую запрошенную архитектуру.
        Constraints arm;
        arm.machine_arch = "arm64";
        CHECK(idx.find("Win11SDK_10.0.26100", arm).has_value());

        CHECK_EQ(std::to_string(idx.by_type("Workload").size()), std::string("1"));
        CHECK_EQ(std::to_string(idx.ids_with_prefix("win11sdk").size()), std::string("1"));
    }

    return cork::test::finish("test_manifest");
}
