#include <atomic>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sys/ioctl.h>
#include <unistd.h>
#include <fmt/format.h>

#include "archive/msi_extract.hpp"
#include "archive/zip.hpp"
#include "base/fs.hpp"
#include "base/lock.hpp"
#include "base/sha256.hpp"
#include "base/version.hpp"
#include "cli/commands.hpp"
#include "i18n/messages.hpp"
#include "manifest/parse.hpp"
#include "net/http.hpp"
#include "resolve/resolve.hpp"
#include "setup/config.hpp"
#include "setup/generation.hpp"
#include "setup/runtime.hpp"
#include "setup/receipt.hpp"
#include "store/store.hpp"

namespace cork::cli {
namespace {

namespace stdfs = std::filesystem;

std::string humanize(std::int64_t bytes) {
    const double b = static_cast<double>(bytes);
    if (bytes > 900LL * 1024 * 1024) {
        return fmt::format("{:.1f} GB", b / (1024.0 * 1024.0 * 1024.0));
    }
    if (bytes > 900LL * 1024) {
        return fmt::format("{:.1f} MB", b / (1024.0 * 1024.0));
    }
    if (bytes > 1024) {
        return fmt::format("{:.1f} KB", b / 1024.0);
    }
    return fmt::format("{} bytes", bytes);
}

// Показ хода работ. Для не-терминала — строка на завершённый файл, иначе
// вывод в журнале CI превращается в километр перерисовок.
// Прогресс скачивания. Всё, что здесь есть, вызывается из рабочих потоков
// fetch_all и одновременно: счётчик поэтому атомарный, а печать под
// мьютексом — иначе строки двух потоков перемежаются посреди слова, и вывод
// становится нечитаемым ровно тогда, когда на него смотрят, то есть при
// разборе неудачной загрузки.
// Прогресс скачивания.
//
// Ведёт себя по-разному в терминале и в конвейере, и это не украшательство.
// В терминале человек смотрит на происходящее сейчас: одна строка, которая
// переписывается на месте, показывает это лучше восьмисот строк, уехавших
// вверх. В журнале сборки, наоборот, переписывание бессмысленно — там нет
// курсора, — и строка на каждый файл становится единственным, по чему потом
// можно понять, на чём всё встало.
//
// Всё вызывается из рабочих потоков fetch_all и одновременно: счётчик поэтому
// атомарный, а вывод под мьютексом.
class ConsoleProgress : public store::Progress {
public:
    explicit ConsoleProgress(std::size_t total)
        : total_(total), interactive_(::isatty(STDOUT_FILENO) != 0) {}

    ~ConsoleProgress() override { finish(); }

    void on_start(const store::FetchRequest &r) override { advance(r.label, false); }

    void on_retry(const store::FetchRequest &r, int attempt, const Error &why) override {
        std::lock_guard<std::mutex> guard(out_);
        // Повтор печатается всегда и отдельной строкой, даже в терминале:
        // это то, что человек должен увидеть, а не то, что промелькнёт в
        // переписываемой строке.
        clear_line();
        fmt::print(stderr, "  retrying {} (attempt {}): {}\n", r.label, attempt + 1, why.message);
        std::fflush(stderr);
    }

    void on_done(const store::FetchRequest &r, bool cached) override {
        if (cached) {
            advance(r.label, true);
        }
    }

    // Убирает недописанную строку, чтобы следующий вывод начался с чистой.
    void finish() {
        std::lock_guard<std::mutex> guard(out_);
        if (interactive_ && dirty_) {
            clear_line();
            std::fflush(stdout);
        }
    }

private:
    void advance(const std::string &label, bool cached) {
        const std::size_t n = index_.fetch_add(1, std::memory_order_relaxed) + 1;
        std::lock_guard<std::mutex> guard(out_);
        if (!interactive_) {
            if (cached) {
                i18n::say(i18n::Msg::AlreadyDownloadedLine, n, total_, label);
                std::fflush(stdout);
            }
            return;
        }
        // Имя обрезается по ширине терминала: строка длиннее неё переносится,
        // и переписывание на месте превращается в бегущую лесенку.
        std::string shown = label;
        const std::size_t room = width() > 24 ? width() - 24 : 24;
        if (shown.size() > room) {
            shown = "..." + shown.substr(shown.size() - room + 3);
        }
        fmt::print("\r\033[K  [{}/{}] {}", n, total_, shown);
        std::fflush(stdout);
        dirty_ = true;
    }

    void clear_line() {
        if (interactive_ && dirty_) {
            fmt::print("\r\033[K");
            dirty_ = false;
        }
    }

    static std::size_t width() {
        struct winsize ws{};
        if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
            return ws.ws_col;
        }
        return 80;
    }

    std::size_t total_;
    bool interactive_;
    std::atomic<std::size_t> index_{0};
    std::mutex out_;
    bool dirty_ = false;
};



std::string host_arch_here() {
#if defined(__aarch64__)
    return "arm64";
#else
    return "x64";
#endif
}

struct Args {
    stdfs::path dest;
    stdfs::path root_dir;
    stdfs::path store_dir;
    int major = 18;
    bool preview = false;
    std::string manifest_file;
    bool accept_license = false;
    std::optional<std::string> msvc_version;
    std::vector<std::string> additional_msvc_versions;
    std::optional<std::string> sdk_version;
    std::vector<std::string> architectures;
    std::vector<std::string> ignore;
    std::vector<std::string> packages;
    bool only_download = false;
    bool no_sdk = false;
    bool print_tree = false;
    bool list_workloads = false;
    bool list_components = false;
    int jobs = 5;
};

// Имя в сообщениях — то, что набрал человек. «cork download: unknown option»
// в ответ на `cork setup` заставляет искать команду, которую не вызывали.
//
// Глобальная переменная, а не параметр: этих сообщений два десятка, и тащить
// имя через все промежуточные функции ради одного слова дороже, чем оно
// стоит. Безопасно потому, что за один запуск выполняется ровно одна команда.
std::string_view g_self = "cork download";

// Переносит содержимое from внутрь into, перезаписывая совпадающие файлы.
//
// Перенос, а не копирование: оба каталога лежат в одном staging, то есть на
// одной файловой системе, и rename стоит правки каталога вместо чтения и
// записи гигабайтов. Ради этого этап и разделён надвое.
//
// Совпадающий путь перезаписывается молча, и это то же самое, что делала
// последовательная распаковка: пейлоад, распакованный позже, затирал файл
// предыдущего. Слияние идёт в том же порядке, поэтому результат тот же.
Result<void> merge_tree(const stdfs::path &from, const stdfs::path &into) {
    std::error_code ec;
    if (!fs::is_dir(from)) {
        return {};
    }
    for (const auto &entry : stdfs::directory_iterator(from, ec)) {
        if (ec) {
            return err_io(fmt::format("listing {}: {}", from.string(), ec.message()));
        }
        const stdfs::path target = into / entry.path().filename();
        const bool is_dir = entry.is_directory(ec) && !entry.is_symlink(ec);
        if (is_dir) {
            if (auto r = fs::mkdir_p(target); !r.has_value()) {
                return r;
            }
            if (auto r = merge_tree(entry.path(), target); !r.has_value()) {
                return r;
            }
            continue;
        }
        // rename не заменяет каталог файлом и наоборот, поэтому мешающее
        // убирается явно. Случай редкий, но молча пройти мимо него нельзя:
        // получилось бы дерево, в котором нет файла, который должен быть.
        if (fs::exists_no_follow(target)) {
            stdfs::remove_all(target, ec);
            ec.clear();
        }
        stdfs::rename(entry.path(), target, ec);
        if (ec) {
            return err_io(fmt::format("moving {} into place: {}", entry.path().string(),
                                      ec.message()));
        }
    }
    return {};
}

bool take_value(const std::vector<std::string> &args, std::size_t &i, std::string &out) {
    if (i + 1 >= args.size()) {
        fmt::print(stderr, "{}: {} needs a value\n", g_self, args[i]);
        return false;
    }
    out = args[++i];
    return true;
}

} // namespace

// Имя в сообщениях — то, что набрал человек. «cork download: unknown option»
// в ответ на `cork setup` заставляет искать команду, которую не вызывали.
int download_command(const std::vector<std::string> &raw_args, bool chained) {
    g_self = chained ? "cork setup" : "cork download";
    Args a;

    for (std::size_t i = 0; i < raw_args.size(); ++i) {
        const std::string &arg = raw_args[i];
        std::string value;
        if (arg == "--root") {
            if (!take_value(raw_args, i, value)) return 2;
            a.root_dir = value;
        } else if (arg == "--store") {
            if (!take_value(raw_args, i, value)) return 2;
            a.store_dir = value;
        } else if (arg == "--major") {
            if (!take_value(raw_args, i, value)) return 2;
            a.major = std::atoi(value.c_str());
        } else if (arg == "--preview") {
            a.preview = true;
        } else if (arg == "--manifest") {
            if (!take_value(raw_args, i, value)) return 2;
            a.manifest_file = value;
        } else if (arg == "--accept-license") {
            a.accept_license = true;
        } else if (arg == "--msvc-version") {
            if (!take_value(raw_args, i, value)) return 2;
            // Ключ повторяемый: первый становится основным поколением, а
            // остальные ставятся рядом. Проект, привязанный к v142, тогда
            // собирается настоящим v142, а не подменённым.
            if (!a.msvc_version.has_value()) {
                a.msvc_version = value;
            } else {
                a.additional_msvc_versions.push_back(value);
            }
        } else if (arg == "--sdk-version") {
            if (!take_value(raw_args, i, value)) return 2;
            a.sdk_version = value;
        } else if (arg == "--architecture") {
            if (!take_value(raw_args, i, value)) return 2;
            // Триплет принимается наравне с коротким именем: см.
            // setup::normalise_target. Непонятное написание уходит дальше как
            // есть — пусть о нём скажет тот, кто знает список целей.
            if (std::string canonical = setup::normalise_target(value); !canonical.empty()) {
                value = std::move(canonical);
            }
            a.architectures.push_back(value);
        } else if (arg == "--ignore") {
            if (!take_value(raw_args, i, value)) return 2;
            a.ignore.push_back(value);
        } else if (arg == "--jobs") {
            if (!take_value(raw_args, i, value)) return 2;
            a.jobs = std::max(1, std::atoi(value.c_str()));
        } else if (arg == "--no-sdk") {
            a.no_sdk = true;
        } else if (arg == "--only-download") {
            a.only_download = true;
        } else if (arg == "--print-deps-tree") {
            a.print_tree = true;
        } else if (arg == "--list-workloads") {
            a.list_workloads = true;
        } else if (arg == "--list-components") {
            a.list_components = true;
        } else if (arg == "-h" || arg == "--help") {
            return cmd_help(0);
        } else if (arg.rfind("--", 0) == 0) {
            fmt::print(stderr, "{}: unknown option {}\n", g_self, arg);
            return 2;
        } else {
            a.packages.push_back(arg);
        }
    }

    setup::Root root = setup::Root::from_environment();
    if (!a.root_dir.empty()) {
        root.base = stdfs::absolute(a.root_dir);
    }
    if (a.store_dir.empty()) {
        a.store_dir = root.store();
    }
    if (auto r = root.ensure(); !r.has_value()) {
        fmt::print(stderr, "{}: {}\n", g_self, r.error().to_string());
        return 1;
    }

    net::global_init();

    // --- манифест ---
    //
    // Откуда он взялся, записывается в receipt: установка, собранная из
    // локального файла, и установка из канала Microsoft — разные вещи, и
    // различить их потом можно только по этой записи.
    std::string manifest_json;
    std::string channel_url;
    std::string manifest_url;
    if (!a.manifest_file.empty()) {
        // Относительный путь приводится к абсолютному до чтения. Склеенный в
        // "file:relative" он даёт листинг корня, и пользователь получает
        // «invalid character '<'» из разбора JSON вместо внятного отказа.
        const stdfs::path p = stdfs::absolute(a.manifest_file);
        auto content = fs::read_file(p);
        if (!content.has_value()) {
            fmt::print(stderr, "{}: {}\n", g_self, content.error().to_string());
            return 1;
        }
        manifest_json = std::move(*content);
        manifest_url = "file://" + p.string();
    } else {
        const char *channel = a.preview ? (a.major < 18 ? "pre" : "insiders")
                                        : (a.major < 18 ? "release" : "stable");
        channel_url = fmt::format("https://aka.ms/vs/{}/{}/channel", a.major, channel);
        i18n::say(i18n::Msg::FetchingChannel, channel_url);
        auto channel_json = net::get(channel_url);
        if (!channel_json.has_value()) {
            fmt::print(stderr, "{}: {}\n", g_self, channel_json.error().to_string());
            return 1;
        }
        auto url = manifest::installer_manifest_url(*channel_json);
        if (!url.has_value()) {
            fmt::print(stderr, "{}: {}\n", g_self, url.error().to_string());
            return 1;
        }
        manifest_url = *url;
        i18n::say(i18n::Msg::FetchingManifest);
        auto body = net::get(*url);
        if (!body.has_value()) {
            fmt::print(stderr, "{}: {}\n", g_self, body.error().to_string());
            return 1;
        }
        manifest_json = std::move(*body);
    }

    auto doc = manifest::parse_installer_manifest(manifest_json);
    if (!doc.has_value()) {
        fmt::print(stderr, "{}: {}\n", g_self, doc.error().to_string());
        return 1;
    }
    i18n::say(i18n::Msg::LoadedManifest, doc->product_display_version);

    resolve::Options opts;
    opts.host_arch = host_arch_here();
    opts.packages = a.packages;
    opts.ignore = a.ignore;
    opts.architectures = a.architectures;
    opts.msvc_version = a.msvc_version;
    opts.additional_msvc_versions = a.additional_msvc_versions;
    opts.sdk_version = a.sdk_version;
    if (a.no_sdk) {
        opts.with_sdk = false;
    }

    const manifest::Index index = manifest::Index::build(*doc, opts.host_arch, opts.language);

    if (a.list_workloads || a.list_components) {
        for (const char *kind : {"Workload", "Component"}) {
            if ((kind == std::string("Workload") && !a.list_workloads) ||
                (kind == std::string("Component") && !a.list_components)) {
                continue;
            }
            const auto list = index.by_type(kind);
            fmt::print("\n{}s ({}):\n", kind, list.size());
            for (const auto *p : list) {
                const auto *lr = p->localized_for(opts.language);
                if (lr != nullptr && !lr->title.empty()) {
                    fmt::print("  {:<65} {}\n", p->id, lr->title);
                } else {
                    fmt::print("  {}\n", p->id);
                }
            }
        }
        return 0;
    }

    if (a.print_tree) {
        // Дерево печатается с той же фильтрацией, что и настоящий выбор, —
        // иначе показанное не совпадало бы со скачиваемым.
        resolve::Options tree_opts = opts;
        if (tree_opts.packages.empty()) {
            tree_opts.packages = {"Microsoft.VisualStudio.Workload.VCTools"};
        }
        fmt::print("{}", resolve::render_tree(index, tree_opts));
        return 0;
    }

    if (!a.accept_license) {
        fmt::print(stderr,
                   "cork download: pass --accept-license to accept Microsoft's Visual "
                   "Studio Build Tools licence.\n"
                   "The terms are the same as obtaining these tools any other way.\n");
        return 1;
    }

    auto plan = resolve::resolve_selection(index, opts);
    if (!plan.has_value()) {
        fmt::print(stderr, "{}: {}\n", g_self, plan.error().to_string());
        return 1;
    }
    i18n::say(i18n::Msg::SelectedPackages, plan->packages.size(),
               humanize(plan->download_size()), humanize(plan->install_size()));

    // --- receipt: с этого места ведётся запись о том, что происходит ---
    setup::Receipt receipt;
    receipt.source.channel_url = channel_url;
    receipt.source.manifest_url = manifest_url;
    receipt.source.manifest_sha256 = "sha256:" + Sha256::hex_of(manifest_json);
    receipt.source.product_version = doc->product_display_version;
    for (const auto *p : plan->packages) {
        receipt.selection.package_ids.push_back(p->id);
    }
    receipt.selection.digest = setup::selection_digest(receipt.selection.package_ids);
    if (auto r = receipt.advance(setup::State::Resolved, kVersion); !r.has_value()) {
        fmt::print(stderr, "{}: {}\n", g_self, r.error().to_string());
        return 1;
    }

    // --- скачивание ---
    std::vector<store::FetchRequest> requests;
    for (const auto *p : plan->packages) {
        for (const auto &payload : p->payloads) {
            if (payload.sha256.empty() || payload.url.empty()) {
                continue;
            }
            store::FetchRequest r;
            r.id = store::BlobId{payload.sha256};
            r.url = payload.url;
            r.expected_size = payload.size;
            r.label = p->id + "/" + payload.base_name();
            requests.push_back(std::move(r));
        }
    }

    store::ArtifactStore artifact_store(a.store_dir);
    ConsoleProgress progress(requests.size());
    i18n::say(i18n::Msg::DownloadingInto, requests.size(), a.store_dir.string());
    if (auto r = store::fetch_all(artifact_store, requests, a.jobs, progress); !r.has_value()) {
        fmt::print(stderr, "{}: {}\n", g_self, r.error().to_string());
        return 1;
    }

    for (const auto *p : plan->packages) {
        for (const auto &payload : p->payloads) {
            if (payload.sha256.empty()) {
                continue;
            }
            receipt.payloads.push_back(setup::PayloadRecord{p->id, payload.base_name(),
                                                            payload.sha256, payload.size});
        }
    }
    if (auto r = receipt.advance(setup::State::Fetched, kVersion); !r.has_value()) {
        fmt::print(stderr, "{}: {}\n", g_self, r.error().to_string());
        return 1;
    }

    if (a.only_download) {
        // Результат остаётся в хранилище и переживает команду: скачивание,
        // исчезающее вместе с временным каталогом, не нужно никому.
        i18n::say(i18n::Msg::DownloadedInto, a.store_dir.string());
        return 0;
    }

    // --- распаковка в каталог сборки ---
    //
    // Поколение собирается в стороне и переименовывается на место только
    // целиком. Прерывание здесь оставляет каталог в staging/, который ничем
    // не притворяется: доктор его не видит, PATH на него не указывает,
    // сборка мусора его уберёт.
    auto staging = setup::Staging::create(root);
    if (!staging.has_value()) {
        fmt::print(stderr, "{}: {}\n", g_self, staging.error().to_string());
        return 1;
    }
    const stdfs::path dest = staging->path();

    // Блокировка на этот каталог сборки, а не на каталог назначения: она
    // говорит сборке мусора «здесь работают», и ядро снимет её само, если нас
    // убьют.
    auto lock = Lock::acquire(root.locks(), "staging-" + dest.filename().string(),
                              Lock::Mode::Exclusive);
    if (!lock.has_value()) {
        fmt::print(stderr, "{}: {}\n", g_self, lock.error().to_string());
        return 1;
    }

    const stdfs::path unpack = dest / "unpack";
    if (auto r = fs::mkdir_p(unpack); !r.has_value()) {
        fmt::print(stderr, "{}: {}\n", g_self, r.error().to_string());
        return 1;
    }

    // Распаковка идёт в два этапа, и второй из них последовательный не по
    // недосмотру.
    //
    // Она упирается в процессор: 220 секунд при загрузке одного ядра из
    // шестнадцати, и это самое долгое, что видит человек за всю установку.
    // Распараллелить её просится само собой, но пейлоады пишут в одно
    // дерево, и семнадцать путей из тринадцати тысяч пишутся дважды —
    // проверено обходом хранилища. При параллельной записи победитель в этих
    // семнадцати случаях определялся бы гонкой, а вместе с ним и дайджест
    // дерева, то есть имя поколения перестало бы быть воспроизводимым.
    //
    // Поэтому каждый пейлоад распаковывается в свой каталог (параллельно), а
    // затем содержимое переносится в общее дерево по порядку плана
    // (последовательно). Порядок слияния тот же, что был у
    // последовательной распаковки, значит результат побайтово тот же.
    // Перенос — это rename в пределах одной файловой системы, то есть правка
    // каталогов, а не копирование данных.
    struct UnpackTask {
        enum class Kind { Zip, Msi } kind = Kind::Zip;
        stdfs::path archive;
        std::string package_id;
        std::string payload_name;
        // Соседние файлы установщика; у zip пуст.
        std::unordered_map<std::string, stdfs::path> side;
    };

    std::vector<UnpackTask> tasks;
    for (const auto *p : plan->packages) {
        if (p->type == "Vsix") {
            for (const auto &payload : p->payloads) {
                if (payload.sha256.empty()) {
                    continue;
                }
                auto blob = artifact_store.path_of(store::BlobId{payload.sha256});
                if (!blob.has_value()) {
                    fmt::print(stderr, "{}: {}\n", g_self, blob.error().to_string());
                    return 1;
                }
                tasks.push_back(
                    UnpackTask{UnpackTask::Kind::Zip, *blob, p->id, payload.base_name(), {}});
            }
            continue;
        }
        // Установщики распаковываются по факту наличия .msi среди пейлоадов, а
        // не по типу пакета и не по имени. Тип здесь не помогает: Windows SDK
        // объявлен как "Exe" (рядом с .msi лежит winsdksetup.exe, который мы
        // не запускаем), а типом "Msi" помечены пакеты самой Visual Studio.
        // Список префиксов id — "Win10SDK", "Win11SDK" — решает это только на
        // вид: вместе с ненужным он теряет нужное, например FileTracker для
        // MSBuild.
        //
        // Соседние файлы установщика — это остальные пейлоады того же пакета:
        // внешние архивы и, изредка, файлы, лежащие несжатыми. В хранилище они
        // разложены по sha256, поэтому «каталога рядом» нет, и поиск идёт по
        // имени пейлоада.
        std::unordered_map<std::string, stdfs::path> side;
        std::vector<const manifest::Payload *> installers;
        for (const auto &payload : p->payloads) {
            if (payload.sha256.empty()) {
                continue;
            }
            auto blob = artifact_store.path_of(store::BlobId{payload.sha256});
            if (!blob.has_value()) {
                fmt::print(stderr, "{}: {}\n", g_self, blob.error().to_string());
                return 1;
            }
            std::string name = payload.base_name();
            side.emplace(name, *blob);
            // Регистр расширения в манифесте не гарантирован ничем, а
            // единственная его проверка — вот эта.
            if (name.size() > 4) {
                std::string tail = name.substr(name.size() - 4);
                for (char &c : tail) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (tail == ".msi") {
                    installers.push_back(&payload);
                }
            }
        }
        for (const auto *payload : installers) {
            tasks.push_back(UnpackTask{UnpackTask::Kind::Msi, side.at(payload->base_name()), p->id,
                                       payload->base_name(), side});
        }
    }

    const stdfs::path parts = dest / "unpack-parts";
    std::error_code parts_ec;
    stdfs::remove_all(parts, parts_ec);
    if (auto r = fs::mkdir_p(parts); !r.has_value()) {
        fmt::print(stderr, "{}: {}\n", g_self, r.error().to_string());
        return 1;
    }

    std::vector<setup::UnpackRecord> records(tasks.size());
    std::vector<std::uint64_t> msi_counts(tasks.size(), 0);
    std::atomic<std::size_t> next_task{0};
    std::atomic<bool> stop_unpacking{false};
    std::mutex unpack_error_mutex;
    std::optional<Error> unpack_error;
    std::string unpack_error_package;

    const auto unpack_worker = [&] {
        for (;;) {
            if (stop_unpacking.load()) {
                return;
            }
            const std::size_t i = next_task.fetch_add(1);
            if (i >= tasks.size()) {
                return;
            }
            const UnpackTask &task = tasks[i];
            const stdfs::path into = parts / std::to_string(i);
            if (auto r = fs::mkdir_p(into); !r.has_value()) {
                std::lock_guard<std::mutex> guard(unpack_error_mutex);
                if (!unpack_error.has_value()) {
                    unpack_error = r.error();
                    unpack_error_package = task.package_id;
                }
                stop_unpacking.store(true);
                return;
            }

            Result<void> failure = {};
            if (task.kind == UnpackTask::Kind::Zip) {
                // Содержимое VSIX лежит под Contents/, остальное — метаданные
                // пакета, которые нам не нужны.
                archive::ZipOptions zo;
                zo.strip_prefix = "Contents/";
                auto stats = archive::extract_zip(task.archive, into, zo);
                if (stats.has_value()) {
                    records[i] = setup::UnpackRecord{task.package_id, task.payload_name, "zip",
                                                     stats->files + stats->symlinks};
                } else {
                    failure = std::unexpected(std::move(stats).error());
                }
            } else {
                archive::MsiExtractOptions mo;
                mo.locate = [&task](std::string_view name) -> Result<stdfs::path> {
                    auto it = task.side.find(std::string(name));
                    if (it == task.side.end()) {
                        return err_not_found(fmt::format(
                            "'{}' is not among the payloads of this package", name));
                    }
                    return it->second;
                };
                auto stats = archive::extract_msi(task.archive, into, mo);
                if (stats.has_value()) {
                    records[i] = setup::UnpackRecord{task.package_id, task.payload_name, "msi",
                                                     stats->files};
                    msi_counts[i] = stats->files;
                } else {
                    failure = std::unexpected(std::move(stats).error());
                }
            }

            if (!failure.has_value()) {
                std::lock_guard<std::mutex> guard(unpack_error_mutex);
                // Первая по порядку задач, а не первая по времени: иначе
                // одна и та же поломка называла бы разные пакеты от прогона
                // к прогону.
                if (!unpack_error.has_value()) {
                    unpack_error = failure.error();
                    unpack_error_package = task.package_id;
                }
                stop_unpacking.store(true);
                return;
            }
        }
    };

    {
        const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
        const std::size_t workers = std::min<std::size_t>(cores, std::max<std::size_t>(tasks.size(), 1));
        std::vector<std::thread> pool;
        pool.reserve(workers);
        for (std::size_t i = 0; i < workers; ++i) {
            pool.emplace_back(unpack_worker);
        }
        for (auto &t : pool) {
            t.join();
        }
    }

    if (unpack_error.has_value()) {
        fmt::print(stderr, "{}: unpacking {}: {}\n", g_self, unpack_error_package,
                   unpack_error->to_string());
        return 1;
    }

    std::size_t unpacked = 0;
    std::size_t msi_files = 0;
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        if (auto r = merge_tree(parts / std::to_string(i), unpack); !r.has_value()) {
            fmt::print(stderr, "{}: {}\n", g_self, r.error().to_string());
            return 1;
        }
        receipt.unpacked.push_back(records[i]);
        msi_files += msi_counts[i];
        ++unpacked;
    }
    stdfs::remove_all(parts, parts_ec);

    i18n::say(i18n::Msg::UnpackedInto, unpacked,
               msi_files, unpack.string());

    if (auto r = receipt.advance(setup::State::Staged, kVersion); !r.has_value()) {
        fmt::print(stderr, "{}: {}\n", g_self, r.error().to_string());
        return 1;
    }
    if (auto r = receipt.save(dest); !r.has_value()) {
        fmt::print(stderr, "{}: {}\n", g_self, r.error().to_string());
        return 1;
    }

    // Рантайм Wine — здесь же, а не отдельной командой. Без него из
    // скачанного нельзя ни собрать, ни проверить ничего, а «скачайте ещё
    // вот это» — тот самый шаг, который пропускают.
    //
    // Отсутствие закрепления не отменяет уже сделанной работы: пакеты
    // скачаны, дерево распаковано, и install по ним пройдёт, как только
    // рантайм появится. Поэтому сообщение на stderr, а не отказ.
    if (auto pins = setup::builtin_runtime_pins(); pins.has_value()) {
        auto installed =
            setup::install_runtime(root, wine_runtime_id(), *pins, artifact_store, progress);
        if (!installed.has_value()) {
            fmt::print(stderr, "{}: {}\n", g_self, installed.error().to_string());
        } else if (!installed->already_present) {
            i18n::say(i18n::Msg::RuntimeInstalled, wine_runtime_id(), installed->files,
                      installed->symlinks);
        }
    } else {
        fmt::print(stderr, "{}: {}\n", g_self, pins.error().to_string());
    }

    // Каталог сборки переживает эту команду: его подхватит install. Без
    // keep() деструктор убрал бы результат получасовой распаковки.
    staging->keep();
    // Подсказка «дальше сделайте вот это» не печатается, когда это «дальше»
    // уже происходит: setup зовёт install сам, и совет набрать его руками
    // сбивал бы с толку ровно того, кто выбрал не набирать команды.
    if (!chained) {
        i18n::say(i18n::Msg::NextInstall, dest.string());
    }
    return 0;
}

int cmd_download(const std::vector<std::string> &args) {
    return download_command(args, /*chained=*/false);
}

// Одна команда на всю установку.
//
// Философия простая: человек ставит Cork, остальное Cork берёт на себя. До
// этого готовая к сборке установка требовала трёх команд подряд —
// `download`, `install`, `template`, — и каждая печатала, какую набрать
// следующей. Список шагов, который надо пройти без ошибок, — это работа,
// переложенная на того, кто пришёл собирать свой проект, а не изучать наш
// порядок.
//
// Отдельные команды остаются: они нужны, когда что-то пошло не так и надо
// повторить один шаг, а не всё. Но нормальный путь — этот.
//
// Чего Cork не берёт на себя: согласия с лицензией Microsoft. Скачивание её
// пакетов требует принять условия, и принять их за человека нельзя — ни
// технически, ни по существу. Это единственное, о чём приходится спросить.
int cmd_setup(const std::vector<std::string> &args) {
    for (const auto &a : args) {
        if (a == "-h" || a == "--help") {
            return cmd_help(0);
        }
    }

    if (const int rc = download_command(args, /*chained=*/true); rc != 0) {
        return rc;
    }

    // Корень передаётся дальше, остальное — нет: у install свои ключи, и
    // чужие он не поймёт. Каталог сборки install находит сам, тот самый,
    // который только что получился.
    std::vector<std::string> install_args;
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--root") {
            install_args.push_back(args[i]);
            install_args.push_back(args[i + 1]);
        }
    }
    return cmd_install(install_args);
}

} // namespace cork::cli
