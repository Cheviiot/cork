#pragma once

// Локализация «хрома» командной строки.
//
// Граница проведена по потоку вывода, и это сознательно: stdout переводится,
// stderr — нет. Она грубее, чем деление «текст для человека» против
// «диагностика», зато её видно в коде без рассуждений, и она не расползается.
// На stdout идёт то, что читают, пока пользуются программой: справка, ход
// работы, итог. На stderr — то, что копируют в отчёт и ищут в сети; переведённое
// сообщение об ошибке не находится ни там, ни там, и помочь по нему не может
// никто, кроме говорящих на том же языке.
//
// Все строки перечислены в одном макросе, и в этом весь смысл устройства:
// каталог любого языка — список ровно по этому перечню, поэтому неполный
// перевод обнаруживается тестом, а не пользователем, у которого посреди
// русского текста вылезла английская строка. Английский текст живёт прямо в
// перечне и служит и значением, и запасным вариантом.
//
// Каталоги вшиты в бинарник, а не лежат файлами рядом. Внешний файл — ещё одна
// вещь, которая теряется при копировании программы, и ещё один путь отказа в
// самом начале запуска.

#include <cstddef>
#include <string_view>
#include <vector>

#include <fmt/format.h>

namespace cork::i18n {

// Перечень всех переводимых строк: имя и английский текст.
#define CORK_MESSAGES(X)                                                                       \
    /* справка */                                                                              \
    X(Tagline, "cross compile with the real MSVC toolchain on Linux via Wine")                 \
    X(UsageHeader, "Usage:")                                                                   \
    X(OptionsHeader, "Options:")                                                               \
    X(RunToolDirectly, "run a tool directly (cl, link, lib, ...)")                             \
    X(HelpFooter,                                                                              \
      "Everything lives under $CORK_HOME (~/.cork by default). A generation is built\n"        \
      "in staging/, verified there and only then renamed into place, so an interrupted\n"      \
      "install never becomes the one that toolchains/current points at.")                      \
    X(HelpHint, "Run `cork help <command>` for the options of one command.")                   \
    X(Repeatable, "repeatable")                                                                \
                                                                                               \
    /* строки описания команд */                                                               \
    X(CmdDownload, "fetch Microsoft's packages and unpack them into a build directory")        \
    X(CmdInstall, "check a build directory and publish it as the current toolchain")           \
    X(CmdDoctor, "check that the installation can actually build")                             \
    X(CmdRun, "run a build, or something you built, in its own Wine prefix")                              \
    X(CmdSession, "inspect or remove build sessions")                                          \
    X(CmdTemplate, "make a portable prefix template for new sessions to clone")                \
    X(CmdGc, "remove abandoned build directories and sessions")                                \
    X(CmdEnv, "print shell commands that put the tools on PATH")                               \
    X(CmdCompletion, "print a completion script for bash, zsh or fish")                        \
    X(CmdVersion, "print the version")                                                         \
    X(CmdHelp, "show this message, or help for one command")                                   \
                                                                                               \
    /* строки описания ключей */                                                               \
    X(OptRoot, "work under this directory instead of $CORK_HOME")                              \
    X(OptHelp, "show this help")                                                               \
    X(OptAcceptLicence, "accept Microsoft's licence for the tools it downloads")               \
    X(OptMsvcVersion, "compiler generation; repeat to install several side by side")           \
    X(OptSdkVersion, "Windows SDK version")                                                    \
    X(OptArchitecture, "target to install; repeat for several")                                \
    X(OptNoSdk, "do not install the Windows SDK")                                              \
    X(OptIgnore, "skip this package; repeat for several")                                      \
    X(OptMajor, "Visual Studio major version to read the manifest from")                       \
    X(OptPreview, "use the preview channel")                                                   \
    X(OptManifest, "read the manifest from a file instead of the network")                     \
    X(OptStore, "keep downloaded payloads here")                                               \
    X(OptJobs, "how many payloads to fetch at once")                                           \
    X(OptOnlyDownload, "fill the store and stop, without unpacking")                           \
    X(OptPrintDepsTree, "print the dependency tree of the selection and stop")                 \
    X(OptListWorkloads, "list the workloads the manifest offers and stop")                     \
    X(OptListComponents, "list the components the manifest offers and stop")                   \
    X(OptQuickDigest, "hash the tree by shape only, without reading file contents")            \
    X(OptDeep, "also verify every file against the recorded digest")                           \
    X(OptBuild, "also compile and link a probe for every target")                              \
    X(OptGeneration, "check this generation instead of the current one")                       \
    X(OptSessionKey, "use this session key instead of deriving one")                           \
    X(OptKeep, "leave the session in place afterwards")                                        \
    X(OptForceSession, "remove even a session that is in use")                                 \
    X(OptForceTemplate, "replace an existing template")                                        \
    X(OptOlderThanHours, "age threshold, in hours")                                            \
    X(OptGenerations, "keep this many published generations, remove the rest")                 \
    X(OptArch, "target architecture")                                                          \
    X(OptShell, "bash, zsh or fish")                                                           \
                                                                                               \
    /* ход скачивания */                                                                       \
    X(FetchingChannel, "Fetching {}\n")                                                        \
    X(FetchingManifest, "Fetching the installer manifest\n")                                   \
    X(LoadedManifest, "Loaded installer manifest for {}\n")                                    \
    X(SelectedPackages, "Selected {} packages, {} to download, {} on disk\n")                  \
    X(DownloadingInto, "Downloading {} files into {}\n")                                       \
    X(AlreadyDownloadedLine, "  [{}/{}] {} (already downloaded)\n")                            \
    X(DownloadedInto, "Downloaded into {}\n")                                                  \
    X(UnpackedInto, "Unpacked {} payloads ({} files from installers) into {}\n")               \
    X(NextInstall, "Next: cork install {}\n")                                                  \
                                                                                               \
    /* ход установки */                                                                        \
    X(Relocating, "Relocating unpacked components\n")                                          \
    X(UsingMsvc, "Using MSVC {}\n")                                                            \
    X(UsingSdk, "Using Windows SDK {}\n")                                                      \
    X(NoSdkYet, "No Windows SDK installed yet; only headerless compilation will work\n")       \
    X(WineComposition, "Wine runtime {}: {} symlinks, {} Mono files\n")                        \
    X(ComputingDigest, "Computing the tree digest ({})\n")                                     \
    X(DigestStructure, "structure only")                                                       \
    X(DigestContents, "structure and contents")                                                \
    X(TreeSummary, "Tree: {} files, {} symlinks, {}\n")                                        \
    X(InstalledWrappers, "Installed tool wrappers for {}\n")                                   \
    X(PublishedAt, "\nPublished {}\n")                                                         \
    X(AddToPath, "Add to PATH to use them:\n")                                                 \
                                                                                               \
    /* сессии и шаблон префикса */                                                             \
    X(BuildingTemplate, "Building a prefix template from {}\n")                                \
    X(TemplateAt, "Template at {}\n")                                                          \
    X(TemplateStats,                                                                           \
      "  {} profile links replaced, {} device links removed, {} registry paths fixed\n")       \
    X(TemplateNote, "New build sessions will clone this instead of running wineboot.\n")       \
    X(TemplateShared, "  {} files ({}) pointed at the runtime instead of copied\n")  \
    X(NoSessions, "No build sessions.\n")                                                      \
    X(SessionInUse, "in use")                                                                  \
    X(RemovedSession, "Removed session {}\n")                                                  \
    X(SessionForDirectory, "Session for this directory: {}\n")                                 \
    X(GcRemoved, "Removed {} abandoned build directories, freed {}\n")                         \
    X(GcSessions, "Removed {} abandoned build sessions\n")                                     \
    X(GcGenerations, "Removed {} published generations, freed {}\n")                           \
    X(GenerationCount, "{} published generations\n")                                           \
    X(CurrentMarker, "current")                                                                \
                                                                                               \
    /* итог doctor */                                                                          \
    X(ChecksSummary, "\n{} checks, {} failed, {} warnings\n")                                  \
    X(CannotBuild,                                                                             \
      "This installation cannot build. Re-run `cork download` and `cork install`.\n")          \
    X(DoctorHint,                                                                              \
      "Run `cork doctor --deep` to verify the whole tree against its digest, or\n"             \
      "`cork doctor --build` to compile and link a probe for every target.\n")

enum class Msg : std::size_t {
#define CORK_MESSAGE_ENUM(name, text) name,
    CORK_MESSAGES(CORK_MESSAGE_ENUM)
#undef CORK_MESSAGE_ENUM
        Count
};

// Текст на текущем языке. Если перевода нет — английский из перечня.
std::string_view tr(Msg);

// Английский оригинал, независимо от выбранного языка. Нужен проверке полноты
// и тем местам, где сравнивают с эталоном.
std::string_view original(Msg);

// Язык этого запуска: «en», «ru». Определяется из окружения при первом
// обращении: CORK_LANG, затем LC_ALL, LC_MESSAGES, LANG.
std::string_view language();

// Смена языка. Нужна тестам; неизвестный язык откатывает на «en».
void set_language(std::string_view);

// Языки, для которых есть каталог, включая английский.
const std::vector<std::string_view> &languages();

// Сколько строк переведено в каталоге этого языка. Английский по определению
// полон; для остальных это то, что проверяет тест полноты.
std::size_t translated_count(std::string_view language);

// Печать локализованной строки. Формат известен только в рантайме, поэтому
// fmt::runtime; проверить его на этапе компиляции нельзя, зато тест сверяет,
// что у перевода те же подстановки, что у оригинала.
template <typename... A> void say(Msg m, A &&...a) {
    fmt::print(fmt::runtime(tr(m)), std::forward<A>(a)...);
}

template <typename... A> std::string text(Msg m, A &&...a) {
    return fmt::format(fmt::runtime(tr(m)), std::forward<A>(a)...);
}

} // namespace cork::i18n
