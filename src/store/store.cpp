#include "store/store.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <mutex>
#include <optional>
#include <thread>

#include <fmt/format.h>

#include "base/fs.hpp"
#include "base/lock.hpp"
#include "base/sha256.hpp"

namespace cork::store {
namespace {

namespace stdfs = std::filesystem;

// Столько попыток на файл. Транзитные отказы сети — оборванное соединение,
// подвисший сервер — укладываются в это число с запасом; постоянные не
// укладываются ни в какое, и растягивать ожидание бессмысленно.
constexpr int kMaxAttempts = 5;

// 1, 2, 4, 8 секунд, дальше не растёт: смысл в том, чтобы дать сети время
// прийти в себя, а не в том, чтобы ждать минутами.
std::chrono::seconds backoff_for(int attempt) {
    const int seconds = 1 << std::min(attempt - 1, 3);
    return std::chrono::seconds(seconds);
}

bool is_hex(std::string_view s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

} // namespace

bool BlobId::valid() const { return sha256.size() == 64 && is_hex(sha256); }

ArtifactStore::ArtifactStore(stdfs::path root) : root_(std::move(root)) {}

stdfs::path ArtifactStore::blob_path(const BlobId &id) const {
    const std::string hex = to_lower(id.sha256);
    // Два первых знака уходят в подкаталог: иначе в одном каталоге окажутся
    // десятки тысяч файлов.
    return root_ / "blobs" / "sha256" / hex.substr(0, 2) / hex;
}

bool ArtifactStore::has(const BlobId &id) const {
    return id.valid() && fs::is_regular_file(blob_path(id));
}

Result<stdfs::path> ArtifactStore::path_of(const BlobId &id) const {
    if (!id.valid()) {
        return err_internal("not a sha256 digest: \"" + id.sha256 + "\"");
    }
    const stdfs::path p = blob_path(id);
    if (!fs::is_regular_file(p)) {
        return err_not_found("blob " + id.sha256 + " is not in the store");
    }
    return p;
}

Result<stdfs::path> ArtifactStore::acquire(const FetchRequest &req, Progress &progress) {
    if (!req.id.valid()) {
        return err_internal("fetch request for \"" + req.label + "\" has no sha256");
    }

    const stdfs::path final_path = blob_path(req.id);
    if (fs::is_regular_file(final_path)) {
        progress.on_done(req, true);
        return final_path;
    }

    // Блокировка именно на блоб: параллельные задачи, которым нужен один и
    // тот же файл, ждут друг друга, а не качают его дважды.
    auto lock = Lock::acquire(root_ / "locks", "blob-" + to_lower(req.id.sha256),
                              Lock::Mode::Exclusive, -1);
    if (!lock.has_value()) {
        return std::unexpected(std::move(lock.error()).at("acquiring " + req.label));
    }

    // Пока ждали блокировку, файл мог появиться.
    if (fs::is_regular_file(final_path)) {
        progress.on_done(req, true);
        return final_path;
    }

    if (auto r = fs::mkdir_p(root_ / "tmp"); !r.has_value()) {
        return std::unexpected(std::move(r.error()));
    }
    if (auto r = fs::mkdir_p(final_path.parent_path()); !r.has_value()) {
        return std::unexpected(std::move(r.error()));
    }

    // Незавершённое лежит в tmp и называется по хешу: следующий запуск
    // подхватит его и продолжит с того же байта.
    const stdfs::path staging = root_ / "tmp" / to_lower(req.id.sha256);

    progress.on_start(req);

    net::Options o;
    o.progress = [&](std::int64_t done, std::int64_t total) {
        progress.on_bytes(req, done, total > 0 ? total : req.expected_size);
        return progress.keep_going();
    };

    // Повторы обязательны: за одну загрузку MSVC проходит семь сотен файлов и
    // несколько гигабайт, и одна заминка сети не должна рушить всё целиком.
    // Докачка делает повтор дешёвым — он продолжает с того же байта, а не
    // начинает заново.
    Error last_error{Error::Code::Network, "no attempt was made"};
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (attempt > 0) {
            if (!progress.keep_going()) {
                return err_interrupted("cancelled while retrying " + req.label);
            }
            progress.on_retry(req, attempt, last_error);
            std::this_thread::sleep_for(backoff_for(attempt));
        }

        auto downloaded = net::download_file(req.url, staging, o);
        if (!downloaded.has_value()) {
            last_error = std::move(downloaded.error());
            // Отмена пользователем повторам не подлежит.
            if (last_error.code == Error::Code::Interrupted) {
                return std::unexpected(std::move(last_error).at("downloading " + req.label));
            }
            continue;
        }

        auto got = Sha256::hex_of_file(staging);
        if (!got.has_value()) {
            return std::unexpected(std::move(got.error()).at("verifying " + req.label));
        }
        if (hex_equal(*got, req.id.sha256)) {
            last_error = Error{Error::Code::Internal, ""};
            break;
        }

        // Несовпадение хеша тоже повторяется: чаще всего это оборванная или
        // подпорченная передача, а не неверный хеш в манифесте. Испорченное
        // при этом удаляется, иначе следующая попытка «продолжит» заведомо
        // неверный файл.
        std::error_code ec;
        stdfs::remove(staging, ec);
        last_error = Error{Error::Code::Verification,
                           "checksum mismatch for " + req.label + ": expected " +
                               to_lower(req.id.sha256) + ", got " + *got};
    }

    if (!last_error.message.empty()) {
        return std::unexpected(std::move(last_error).at(
            fmt::format("downloading {} ({} attempts)", req.label, kMaxAttempts)));
    }

    std::error_code ec;
    stdfs::rename(staging, final_path, ec);
    if (ec) {
        return err_io("moving " + req.label + " into the store: " + ec.message());
    }

    progress.on_done(req, false);
    return final_path;
}

Result<stdfs::path> ArtifactStore::put_file(const stdfs::path &source) {
    auto hex = Sha256::hex_of_file(source);
    if (!hex.has_value()) {
        return std::unexpected(std::move(hex.error()).at("hashing " + source.string()));
    }

    const BlobId id{*hex};
    const stdfs::path final_path = blob_path(id);
    if (fs::is_regular_file(final_path)) {
        return final_path;
    }

    if (auto r = fs::mkdir_p(final_path.parent_path()); !r.has_value()) {
        return std::unexpected(std::move(r.error()));
    }

    auto content = fs::read_file(source);
    if (!content.has_value()) {
        return std::unexpected(std::move(content.error()));
    }
    if (auto r = fs::write_atomic(final_path, std::string_view(*content)); !r.has_value()) {
        return std::unexpected(std::move(r.error()));
    }
    return final_path;
}

Result<void> fetch_all(ArtifactStore &store, const std::vector<FetchRequest> &requests,
                       int concurrency, Progress &progress) {
    if (requests.empty()) {
        return {};
    }
    const int workers = std::max(1, std::min<int>(concurrency, static_cast<int>(requests.size())));

    std::atomic<std::size_t> next{0};
    std::atomic<bool> stop{false};
    std::mutex error_mutex;
    std::optional<Error> first_error;

    const auto worker = [&] {
        for (;;) {
            if (stop.load()) {
                return;
            }
            const std::size_t i = next.fetch_add(1);
            if (i >= requests.size()) {
                return;
            }
            auto r = store.acquire(requests[i], progress);
            if (!r.has_value()) {
                std::lock_guard<std::mutex> guard(error_mutex);
                // Сохраняется первая ошибка: остальные, скорее всего, её
                // следствия, и вываливать их все значит спрятать причину.
                if (!first_error.has_value()) {
                    first_error = std::move(r.error());
                }
                stop.store(true);
                return;
            }
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(workers));
    for (int i = 0; i < workers; ++i) {
        pool.emplace_back(worker);
    }
    for (auto &t : pool) {
        t.join();
    }

    if (first_error.has_value()) {
        return std::unexpected(std::move(*first_error));
    }
    return {};
}

} // namespace cork::store
