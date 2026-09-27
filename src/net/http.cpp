#include "net/http.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <curl/curl.h>

#include "base/fs.hpp"

namespace cork::net {
namespace {

namespace stdfs = std::filesystem;

struct StringSink {
    std::string data;
};

std::size_t write_to_string(char *ptr, std::size_t size, std::size_t nmemb, void *userdata) {
    auto *sink = static_cast<StringSink *>(userdata);
    sink->data.append(ptr, size * nmemb);
    return size * nmemb;
}

struct FileSink {
    std::FILE *f = nullptr;
    std::int64_t written = 0;
};

std::size_t write_to_file(char *ptr, std::size_t size, std::size_t nmemb, void *userdata) {
    auto *sink = static_cast<FileSink *>(userdata);
    const std::size_t n = std::fwrite(ptr, size, nmemb, sink->f);
    sink->written += static_cast<std::int64_t>(n * size);
    return n * size;
}

struct HeaderState {
    std::int64_t content_range_start = -1;
    bool saw_content_range = false;
};

std::size_t collect_header(char *buffer, std::size_t size, std::size_t nitems, void *userdata) {
    auto *state = static_cast<HeaderState *>(userdata);
    const std::string_view line(buffer, size * nitems);

    constexpr std::string_view kName = "content-range:";
    if (line.size() > kName.size()) {
        std::string lowered(line.substr(0, kName.size()));
        for (auto &c : lowered) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (lowered == kName) {
            state->saw_content_range = true;
            std::int64_t start = 0;
            if (parse_content_range_start(line.substr(kName.size()), start)) {
                state->content_range_start = start;
            }
        }
    }
    return size * nitems;
}

struct ProgressState {
    ProgressFn fn;
    std::int64_t base = 0;  // уже лежало в .part до этого запуска
};

int progress_callback(void *userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto *state = static_cast<ProgressState *>(userdata);
    if (!state->fn) {
        return 0;
    }
    const std::int64_t total = dltotal > 0 ? state->base + static_cast<std::int64_t>(dltotal) : 0;
    // Возврат ненулевого значения просит curl прервать передачу.
    return state->fn(state->base + static_cast<std::int64_t>(dlnow), total) ? 0 : 1;
}

void apply_common(CURL *curl, const Options &opts) {
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "cork/0.1");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,
                     static_cast<long>(opts.connect_timeout_seconds));
    if (opts.timeout_seconds > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(opts.timeout_seconds));
    }
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, opts.low_speed_bytes);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, opts.low_speed_seconds);
}

std::string url_of(std::string_view url) { return std::string(url); }

} // namespace

bool parse_content_range_start(std::string_view header, std::int64_t &start) {
    // "bytes 123-456/789" либо " bytes 123-456/*"
    while (!header.empty() && (header.front() == ' ' || header.front() == '\t')) {
        header.remove_prefix(1);
    }
    constexpr std::string_view kUnit = "bytes ";
    if (header.rfind(kUnit, 0) != 0) {
        return false;
    }
    header.remove_prefix(kUnit.size());

    std::int64_t value = 0;
    std::size_t digits = 0;
    while (digits < header.size() && header[digits] >= '0' && header[digits] <= '9') {
        value = value * 10 + (header[digits] - '0');
        ++digits;
    }
    if (digits == 0) {
        return false;
    }
    // За числом обязан идти дефис: "bytes 123" без него — не диапазон.
    if (digits >= header.size() || header[digits] != '-') {
        return false;
    }
    start = value;
    return true;
}

void global_init() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

Result<std::string> get(std::string_view url, const Options &opts) {
    global_init();
    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        return err_network("initialising curl");
    }
    struct Cleanup {
        CURL *c;
        ~Cleanup() { curl_easy_cleanup(c); }
    } cleanup{curl};

    StringSink sink;
    ProgressState progress{opts.progress, 0};

    const std::string u = url_of(url);
    curl_easy_setopt(curl, CURLOPT_URL, u.c_str());
    apply_common(curl, opts);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, opts.progress ? 0L : 1L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress);

    const CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_ABORTED_BY_CALLBACK) {
        return err_interrupted("GET " + u + ": cancelled");
    }
    if (rc != CURLE_OK) {
        return err_network("GET " + u + ": " + curl_easy_strerror(rc));
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (status < 200 || status >= 300) {
        // Первые байты тела попадают в сообщение: без них «HTTP 404» на
        // относительном пути в --manifest выглядит как «invalid character '<'»
        // где-то в разборе JSON, и причина неочевидна.
        std::string preview = sink.data.substr(0, 200);
        for (auto &c : preview) {
            if (c == '\n' || c == '\r') {
                c = ' ';
            }
        }
        return err_network("GET " + u + ": HTTP " + std::to_string(status) +
                           (preview.empty() ? "" : "; body starts with: " + preview));
    }
    return sink.data;
}

Result<DownloadResult> download_file(std::string_view url, const stdfs::path &dest,
                                     const Options &opts) {
    global_init();

    const stdfs::path part = dest.string() + ".part";
    std::int64_t offset = 0;
    {
        std::error_code ec;
        const auto size = stdfs::file_size(part, ec);
        if (!ec) {
            offset = static_cast<std::int64_t>(size);
        }
    }

    if (auto r = fs::mkdir_p(dest.parent_path()); !r.has_value()) {
        return std::unexpected(std::move(r.error()).at("preparing download directory"));
    }

    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        return err_network("initialising curl");
    }
    struct Cleanup {
        CURL *c;
        ~Cleanup() { curl_easy_cleanup(c); }
    } cleanup{curl};

    HeaderState headers;
    ProgressState progress{opts.progress, offset};

    const std::string u = url_of(url);
    curl_easy_setopt(curl, CURLOPT_URL, u.c_str());
    apply_common(curl, opts);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, collect_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headers);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, opts.progress ? 0L : 1L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress);
    if (offset > 0) {
        curl_easy_setopt(curl, CURLOPT_RANGE, (std::to_string(offset) + "-").c_str());
    }

    // Файл открывается только после того, как станет ясно, дописываем мы или
    // начинаем заново: решение зависит от кода ответа и Content-Range.
    FileSink sink;
    bool opened_for_append = false;

    // curl зовёт writefunction уже после заголовков, поэтому открытие можно
    // отложить в саму функцию записи — но так получается запутанно. Проще
    // выполнить запрос в два шага: сначала узнать код ответа заголовками,
    // затем решить. Для этого используется CURLOPT_HEADERFUNCTION выше и
    // открытие по первому вызову записи.
    struct LazyOpen {
        FileSink *sink;
        HeaderState *headers;
        const stdfs::path *part;
        std::int64_t offset;
        bool *opened_for_append;
        std::string error;
        bool decided = false;
    } lazy{&sink, &headers, &part, offset, &opened_for_append, {}, false};

    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &lazy);
    curl_easy_setopt(
        curl, CURLOPT_WRITEFUNCTION,
        +[](char *ptr, std::size_t size, std::size_t nmemb, void *userdata) -> std::size_t {
            auto *l = static_cast<LazyOpen *>(userdata);
            if (!l->decided) {
                l->decided = true;
                // Решение принимается ровно один раз, на первом куске тела.
                const bool append = l->offset > 0 && l->headers->saw_content_range &&
                                    l->headers->content_range_start == l->offset;
                if (l->offset > 0 && !append) {
                    // Сервер прислал не то, о чём просили: либо вовсе
                    // проигнорировал Range, либо начал тело в другом месте.
                    // Дописывать такое — молча испортить файл.
                    l->sink->f = std::fopen(l->part->c_str(), "wb");
                } else {
                    l->sink->f = std::fopen(l->part->c_str(), append ? "ab" : "wb");
                    *l->opened_for_append = append;
                }
                if (l->sink->f == nullptr) {
                    l->error = std::strerror(errno);
                    return 0;
                }
            }
            return write_to_file(ptr, size, nmemb, l->sink);
        });

    const CURLcode rc = curl_easy_perform(curl);

    if (sink.f != nullptr) {
        std::fclose(sink.f);
        sink.f = nullptr;
    }

    if (rc == CURLE_ABORTED_BY_CALLBACK) {
        // .part намеренно остаётся: следующий запуск продолжит с него.
        return err_interrupted("downloading " + u + ": cancelled");
    }
    if (rc != CURLE_OK) {
        if (!lazy.error.empty()) {
            return err_io("writing " + part.string() + ": " + lazy.error);
        }
        return err_network("downloading " + u + ": " + curl_easy_strerror(rc));
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    if (status == 416) {
        // Наш .part уже не короче настоящего файла: он устарел или испорчен.
        // Выбрасываем, чтобы следующая попытка пошла с нуля.
        std::error_code ec;
        stdfs::remove(part, ec);
        return err_protocol("downloading " + u +
                            ": server says the requested range is not satisfiable; "
                            "discarded the partial file, retry will start over");
    }
    if (status < 200 || status >= 300) {
        return err_network("downloading " + u + ": HTTP " + std::to_string(status));
    }

    std::error_code ec;
    stdfs::rename(part, dest, ec);
    if (ec) {
        return err_io("renaming " + part.string() + " to " + dest.string() + ": " + ec.message());
    }

    DownloadResult out;
    out.bytes_transferred = sink.written;
    out.resumed = opened_for_append;
    return out;
}

} // namespace cork::net
