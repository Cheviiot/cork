#include "i18n/messages.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <utility>

namespace cork::i18n {
namespace {

using Table = std::array<std::string_view, static_cast<std::size_t>(Msg::Count)>;

std::size_t index(Msg m) { return static_cast<std::size_t>(m); }

const Table &english() {
    static const Table table = {
#define CORK_MESSAGE_TEXT(name, text) std::string_view(text),
        CORK_MESSAGES(CORK_MESSAGE_TEXT)
#undef CORK_MESSAGE_TEXT
    };
    return table;
}

// Русский каталог.
//
// Про множественное число: в русском их три, а формата, который бы их выбрал,
// у нас нет и заводить его ради полудюжины строк не стоит. Поэтому строки со
// счётчиками переписаны так, чтобы согласование не требовалось: «проверок: 12»
// вместо «12 проверок» — оборот сухой, зато верный при любом числе. Это
// решение, а не недосмотр: механизм plural forms обошёлся бы дороже всего
// каталога.
const Table &russian() {
    static const Table table = [] {
        Table t{};
        const auto set = [&t](Msg m, std::string_view s) { t[index(m)] = s; };

        set(Msg::Tagline, "кросс-компиляция настоящим MSVC на Linux через Wine");
        set(Msg::UsageHeader, "Использование:");
        set(Msg::OptionsHeader, "Ключи:");
        set(Msg::RunToolDirectly, "запустить инструмент напрямую (cl, link, lib, ...)");
        set(Msg::HelpFooter,
            "Всё лежит под $CORK_HOME (по умолчанию ~/.cork). Поколение собирается\n"
            "в staging/, проверяется там и только потом переименовывается на место,\n"
            "так что прерванная установка не станет той, на которую показывает\n"
            "toolchains/current.");
        set(Msg::HelpHint, "`cork help <команда>` покажет ключи одной команды.");
        set(Msg::Repeatable, "можно повторять");

        set(Msg::CmdSetup,
            "установить всё нужное для сборки: скачать, установить, подготовить");
        set(Msg::CmdDownload, "скачать пакеты Microsoft и распаковать их в сборочный каталог");
        set(Msg::CmdInstall, "проверить сборочный каталог и опубликовать как текущий тулчейн");
        set(Msg::CmdDoctor, "проверить, что установка действительно способна собирать");
        set(Msg::CmdRun, "выполнить сборку или собранное в отдельном префиксе Wine");
        set(Msg::CmdSession, "посмотреть или удалить сборочные сессии");
        set(Msg::CmdTemplate, "сделать переносимый шаблон префикса для новых сессий");
        set(Msg::CmdGc, "удалить брошенные сборочные каталоги и сессии");
        set(Msg::CmdEnv, "напечатать команды оболочки, кладущие инструменты в PATH");
        set(Msg::CmdCompletion, "напечатать скрипт автодополнения для bash, zsh или fish");
        set(Msg::CmdVersion, "напечатать версию");
        set(Msg::CmdHelp, "показать эту справку или справку по одной команде");

        set(Msg::OptRoot, "работать в этом каталоге вместо $CORK_HOME");
        set(Msg::OptHelp, "показать эту справку");
        set(Msg::OptAcceptLicence, "принять лицензию Microsoft на скачиваемые инструменты");
        set(Msg::OptMsvcVersion,
            "поколение компилятора; повторите, чтобы поставить несколько сразу");
        set(Msg::OptSdkVersion, "версия Windows SDK");
        set(Msg::OptArchitecture, "цель установки; повторите для нескольких");
        set(Msg::OptNoSdk, "не устанавливать Windows SDK");
        set(Msg::OptIgnore, "пропустить этот пакет; повторите для нескольких");
        set(Msg::OptMajor, "мажорная версия Visual Studio, из манифеста которой читать");
        set(Msg::OptPreview, "использовать канал предварительных версий");
        set(Msg::OptManifest, "читать манифест из файла, а не из сети");
        set(Msg::OptStore, "хранить скачанные пейлоады здесь");
        set(Msg::OptJobs, "сколько пейлоадов качать одновременно");
        set(Msg::OptOnlyDownload, "наполнить хранилище и остановиться, без распаковки");
        set(Msg::OptPrintDepsTree, "напечатать дерево зависимостей выбора и остановиться");
        set(Msg::OptListWorkloads, "перечислить рабочие нагрузки из манифеста и остановиться");
        set(Msg::OptListComponents, "перечислить компоненты из манифеста и остановиться");
        set(Msg::OptQuickDigest, "хешировать дерево только по форме, не читая содержимое файлов");
        set(Msg::OptDeep, "дополнительно сверить каждый файл с записанным дайджестом");
        set(Msg::OptBuild, "дополнительно собрать и скомпоновать пробник на каждую цель");
        set(Msg::OptGeneration, "проверить это поколение вместо текущего");
        set(Msg::OptSessionKey, "использовать этот ключ сессии вместо вычисленного");
        set(Msg::OptKeep, "оставить сессию на месте после выхода");
        set(Msg::OptForceSession, "удалить сессию, даже если она занята");
        set(Msg::OptForceTemplate, "заменить существующий шаблон");
        set(Msg::OptOlderThanHours, "порог возраста в часах");
        set(Msg::OptGenerations, "сколько опубликованных поколений оставить, остальные удалить");
        set(Msg::OptArch, "архитектура цели");
        set(Msg::OptShell, "bash, zsh или fish");

        set(Msg::FetchingChannel, "Читаю {}\n");
        set(Msg::FetchingManifest, "Читаю манифест установщика\n");
        set(Msg::LoadedManifest, "Загружен манифест установщика для {}\n");
        set(Msg::SelectedPackages, "Выбрано пакетов: {}, скачать: {}, на диске: {}\n");
        set(Msg::DownloadingInto, "Скачиваю файлов: {} в {}\n");
        set(Msg::AlreadyDownloadedLine, "  [{}/{}] {} (уже скачано)\n");
        set(Msg::DownloadedInto, "Скачано в {}\n");
        set(Msg::UnpackedInto, "Распаковано пейлоадов: {} (файлов из установщиков: {}) в {}\n");
        set(Msg::NextInstall, "Дальше: cork install {}\n");
        set(Msg::RuntimeInstalled,
            "Установлен рантайм Wine {}: файлов {}, символьных ссылок {}\n");

        set(Msg::Relocating, "Раскладываю распакованные компоненты\n");
        set(Msg::CaseAliases,
            "Добавлено ссылок строчными именами (для clang-cl): {}\n");
        set(Msg::UsingMsvc, "Использую MSVC {}\n");
        set(Msg::UsingSdk, "Использую Windows SDK {}\n");
        
        set(Msg::NoSdkYet,
            "Windows SDK пока не установлен; заработает только компиляция без заголовков\n");
        set(Msg::WineComposition, "Runtime Wine {}: символьных ссылок {}, файлов Mono {}\n");
        set(Msg::ComputingDigest, "Считаю дайджест дерева ({})\n");
        set(Msg::DigestStructure, "только структура");
        set(Msg::DigestContents, "структура и содержимое");
        set(Msg::TreeSummary, "Дерево: файлов {}, символьных ссылок {}, {}\n");
        set(Msg::InstalledWrappers, "Установлены обёртки инструментов для {}\n");
        set(Msg::PublishedAt, "\nОпубликовано {}\n");
        set(Msg::AddToPath, "Добавьте в PATH, чтобы пользоваться:\n");

        set(Msg::BuildingTemplate, "Собираю шаблон префикса из {}\n");
        set(Msg::TemplateAt, "Шаблон в {}\n");
        set(Msg::TemplateStats, "  ссылок профиля заменено: {}, ссылок устройств удалено: {}, "
                                "путей в реестре исправлено: {}\n");
        set(Msg::TemplateShared,
            "  Указывают на рантайм вместо копии: {} файлов ({})\n");
        set(Msg::TemplateNote,
            "Новые сборочные сессии будут клонировать его вместо запуска wineboot.\n");
        set(Msg::NoSessions, "Сборочных сессий нет.\n");
        set(Msg::SessionInUse, "занята");
        set(Msg::RemovedSession, "Удалена сессия {}\n");
        set(Msg::SessionForDirectory, "Сессия этого каталога: {}\n");
        set(Msg::GcRemoved, "Удалено брошенных сборочных каталогов: {}, освобождено {}\n");
        set(Msg::GcSessions, "Удалено брошенных сборочных сессий: {}\n");
        set(Msg::GcGenerations, "Удалено опубликованных поколений: {}, освобождено {}\n");
        set(Msg::GenerationCount, "Опубликованных поколений: {}\n");
        set(Msg::CurrentMarker, "текущее");

        set(Msg::ChecksSummary, "\nПроверок: {}, неуспешных: {}, предупреждений: {}\n");
        set(Msg::CannotBuild,
            "Эта установка не способна собирать. Повторите `cork setup --accept-license`.\n");
        set(Msg::DoctorHint,
            "`cork doctor --deep` сверит всё дерево с его дайджестом, а\n"
            "`cork doctor --build` соберёт и скомпонует пробник на каждую цель.\n");
        return t;
    }();
    return table;
}

const std::unordered_map<std::string_view, const Table *> &catalogs() {
    static const std::unordered_map<std::string_view, const Table *> map = {
        {"en", &english()},
        {"ru", &russian()},
    };
    return map;
}

// Из «ru_RU.UTF-8» получается «ru». Модификаторы вроде «@latin» и кодировка
// отбрасываются: каталог у нас на язык, а не на локаль целиком.
std::string language_of(std::string_view locale) {
    const std::size_t end = locale.find_first_of("_.@");
    std::string tag(end == std::string_view::npos ? locale : locale.substr(0, end));
    std::transform(tag.begin(), tag.end(), tag.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return tag;
}

std::string_view detect_language() {
    // CORK_LANG раньше локали: он задаётся ради этой программы, а LANG — ради
    // всей системы, и пользователю нужен способ разойтись с ней, не трогая её.
    for (const char *name : {"CORK_LANG", "LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char *value = std::getenv(name);
        if (value == nullptr || *value == '\0') {
            continue;
        }
        // «C» и «POSIX» — это явно заказанный английский, а не отсутствие
        // выбора: продолжать перебор после них значило бы переопределить его.
        const std::string tag = language_of(value);
        if (tag == "c" || tag == "posix") {
            return "en";
        }
        const auto &map = catalogs();
        const auto it = map.find(tag);
        if (it != map.end()) {
            return it->first;
        }
        return "en";
    }
    return "en";
}

std::string_view &current_language() {
    static std::string_view language = detect_language();
    return language;
}

} // namespace

std::string_view original(Msg m) { return english()[index(m)]; }

std::string_view tr(Msg m) {
    const auto &map = catalogs();
    const auto it = map.find(current_language());
    if (it != map.end()) {
        // Непереведённая строка отдаётся по-английски, а не пустой: пропуск в
        // каталоге обязан выглядеть как непереведённый текст, а не как
        // пропавшая строка. Полноту при этом стережёт тест, а не пользователь.
        if (const std::string_view s = (*it->second)[index(m)]; !s.empty()) {
            return s;
        }
    }
    return original(m);
}

std::string_view language() { return current_language(); }

void set_language(std::string_view tag) {
    const std::string normalised = language_of(tag);
    const auto &map = catalogs();
    const auto it = map.find(normalised);
    current_language() = it == map.end() ? std::string_view("en") : it->first;
}

const std::vector<std::string_view> &languages() {
    static const std::vector<std::string_view> list = [] {
        std::vector<std::string_view> names;
        for (const auto &entry : catalogs()) {
            names.push_back(entry.first);
        }
        std::sort(names.begin(), names.end());
        return names;
    }();
    return list;
}

std::size_t translated_count(std::string_view tag) {
    const auto &map = catalogs();
    const auto it = map.find(tag);
    if (it == map.end()) {
        return 0;
    }
    return static_cast<std::size_t>(
        std::count_if(it->second->begin(), it->second->end(),
                      [](std::string_view s) { return !s.empty(); }));
}

} // namespace cork::i18n
