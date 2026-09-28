#include "cli/spec.hpp"

#include <algorithm>
#include <string>

#include <fmt/format.h>

#include "i18n/messages.hpp"

namespace cork::cli {
namespace {

using i18n::Msg;
using i18n::tr;

// Ключи, общие для всех команд. Повторять их в каждой строке таблицы значило
// бы завести четыре места, которые обязаны совпадать.
const OptionSpec kRoot{"--root", "<dir>", Msg::OptRoot, Completes::Directory};
const OptionSpec kHelp{"--help", "", Msg::OptHelp, Completes::Nothing};

std::vector<OptionSpec> with_common(std::vector<OptionSpec> own) {
    own.push_back(kRoot);
    own.push_back(kHelp);
    return own;
}

// Ширина колонки в символах, а не в байтах. В UTF-8 русская буква занимает
// два байта, и выравнивание по size() разъезжает ровно на переведённой
// справке — то есть там, где его никто не проверяет, пока не увидит.
std::size_t display_width(std::string_view s) {
    std::size_t width = 0;
    for (const char ch : s) {
        // Продолжения многобайтовой последовательности начинаются с 10xxxxxx
        // и на ширину не влияют.
        if ((static_cast<unsigned char>(ch) & 0xC0U) != 0x80U) {
            ++width;
        }
    }
    return width;
}

// fmt выравнивает по кодовым единицам, поэтому дополняем сами.
std::string pad_to(std::string_view s, std::size_t width) {
    std::string out(s);
    for (std::size_t w = display_width(s); w < width; ++w) {
        out.push_back(' ');
    }
    return out;
}

} // namespace

const std::vector<CommandSpec> &command_table() {
    static const std::vector<CommandSpec> table = {
        {"download",
         "--accept-license [options] [package...]",
         Msg::CmdDownload,
         with_common({
             {"--accept-license", "", Msg::OptAcceptLicence},
             {"--msvc-version", "<version>", Msg::OptMsvcVersion, Completes::MsvcVersion, true},
             {"--sdk-version", "<version>", Msg::OptSdkVersion, Completes::SdkVersion},
             {"--architecture", "<arch>", Msg::OptArchitecture, Completes::Architecture, true},
             {"--no-sdk", "", Msg::OptNoSdk},
             {"--ignore", "<package>", Msg::OptIgnore, Completes::Package, true},
             {"--major", "<n>", Msg::OptMajor},
             {"--preview", "", Msg::OptPreview},
             {"--manifest", "<file>", Msg::OptManifest, Completes::File},
             {"--store", "<dir>", Msg::OptStore, Completes::Directory},
             {"--jobs", "<n>", Msg::OptJobs},
             {"--only-download", "", Msg::OptOnlyDownload},
             {"--print-deps-tree", "", Msg::OptPrintDepsTree},
             {"--list-workloads", "", Msg::OptListWorkloads},
             {"--list-components", "", Msg::OptListComponents},
         }),
         Completes::Package},

        {"install",
         "[build-directory]",
         Msg::CmdInstall,
         with_common({
             {"--quick-digest", "", Msg::OptQuickDigest},
         }),
         Completes::Directory},

        {"doctor",
         "[options]",
         Msg::CmdDoctor,
         with_common({
             {"--deep", "", Msg::OptDeep},
             {"--build", "", Msg::OptBuild},
             {"--generation", "<dir>", Msg::OptGeneration, Completes::Directory},
         })},

        {"run",
         "[options] -- <command | program.exe>",
         Msg::CmdRun,
         with_common({
             {"--session", "<key>", Msg::OptSessionKey, Completes::SessionKey},
             {"--keep", "", Msg::OptKeep},
         })},

        {"session",
         "[list|stop|kill] [key]",
         Msg::CmdSession,
         with_common({
             {"--force", "", Msg::OptForceSession},
         }),
         Completes::SessionKey},

        {"template",
         "[prefix]",
         Msg::CmdTemplate,
         with_common({
             {"--force", "", Msg::OptForceTemplate},
         }),
         Completes::Directory},

        {"gc",
         "[options]",
         Msg::CmdGc,
         with_common({
             {"--older-than-hours", "<n>", Msg::OptOlderThanHours},
             {"--generations", "<n>", Msg::OptGenerations},
         })},

        {"env",
         "[--arch <arch>]",
         Msg::CmdEnv,
         with_common({
             {"--arch", "<arch>", Msg::OptArch, Completes::Architecture},
             {"--shell", "<shell>", Msg::OptShell},
         })},

        {"completion", "<shell>", Msg::CmdCompletion, with_common({})},
        {"version", "", Msg::CmdVersion, with_common({})},
        {"help", "[command]", Msg::CmdHelp, with_common({})},
    };
    return table;
}

const CommandSpec *find_command(std::string_view name) {
    const auto &table = command_table();
    const auto it = std::find_if(table.begin(), table.end(),
                                 [name](const CommandSpec &c) { return c.name == name; });
    return it == table.end() ? nullptr : &*it;
}

std::string render_help() {
    // «cork» и имена команд не переводятся: это то, что набирают.
    std::string out = fmt::format("cork - {}\n\n{}\n", tr(Msg::Tagline), tr(Msg::UsageHeader));
    for (const auto &c : command_table()) {
        out += fmt::format("  cork {}  {}\n", pad_to(c.name, 10), tr(c.summary));
    }
    out += fmt::format("\n  cork <tool> [args...]  {}\n", tr(Msg::RunToolDirectly));
    out += fmt::format("\n{}\n\n{}\n", tr(Msg::HelpFooter), tr(Msg::HelpHint));
    return out;
}

std::string render_command_help(const CommandSpec &c) {
    std::string out = fmt::format("cork {} {}\n\n{}\n", c.name, c.usage, tr(c.summary));
    if (c.options.empty()) {
        return out;
    }
    out += fmt::format("\n{}\n", tr(Msg::OptionsHeader));

    // Ширина колонки считается по самому длинному ключу, а не берётся
    // константой: константа расходится с содержимым при первом же добавлении.
    std::size_t width = 0;
    for (const auto &o : c.options) {
        const std::size_t len = o.name.size() + (o.value.empty() ? 0 : o.value.size() + 1);
        width = std::max(width, len);
    }
    for (const auto &o : c.options) {
        std::string left(o.name);
        if (!o.value.empty()) {
            left += " ";
            left += o.value;
        }
        const std::string note =
            o.repeatable ? fmt::format(" ({})", tr(Msg::Repeatable)) : std::string();
        out += fmt::format("  {}  {}{}\n", pad_to(left, width), tr(o.help), note);
    }
    return out;
}

} // namespace cork::cli
