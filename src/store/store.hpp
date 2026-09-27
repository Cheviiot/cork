#pragma once

// Хранилище скачанных артефактов, адресуемое содержимым.
//
// Три решения, и все они про то, чтобы «файл есть» означало «файл
// проверен».
//
// Кэш адресуется хешем, а не именем пакета. Раньше это был
// cache/<packageKey>/<file>, и наличие проверялось пересчётом sha256 при
// каждом запуске — то есть чтением гигабайтов ради ответа «да». Здесь блоб
// лежит как blobs/sha256/<aa>/<hex> и попадает туда только после проверки, а
// has() — это stat. Заодно один и тот же пейлоад, встречающийся в нескольких
// пакетах, хранится один раз.
//
// Незавершённое всегда в tmp/, а не рядом с целью: прерывание оставляет файл,
// который невозможно принять за готовый.
//
// Блокировка берётся на блоб, а не на каталог назначения. Кэш общий для всех
// установок сразу, и две параллельные качают в него одни и те же файлы;
// блокировка на «свой» каталог от этого не спасает.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "base/error.hpp"
#include "net/http.hpp"

namespace cork::store {

struct BlobId {
    std::string sha256;

    [[nodiscard]] bool valid() const;
    friend bool operator<(const BlobId &a, const BlobId &b) { return a.sha256 < b.sha256; }
};

struct FetchRequest {
    BlobId id;
    std::string url;
    std::int64_t expected_size = 0;
    std::string label;  // что показать пользователю
};

// Отчёт о ходе работ для CLI. Реализации живут в cli/, сюда приходит только
// интерфейс: ни один слой ниже cli ничего не печатает сам.
//
// ВАЖНО: все методы зовутся из рабочих потоков fetch_all, а не из того,
// который его вызвал, и могут выполняться одновременно. Реализация обязана
// синхронизировать своё состояние сама. Обычный счётчик здесь даёт гонку,
// которую почти не видно: значения сходятся в подавляющем большинстве
// прогонов, и тест проходит — пока однажды не перестанет.
class Progress {
public:
    virtual ~Progress() = default;
    virtual void on_start(const FetchRequest &) {}
    virtual void on_bytes(const FetchRequest &, std::int64_t /*done*/, std::int64_t /*total*/) {}
    virtual void on_done(const FetchRequest &, bool /*from_cache*/) {}
    // Повтор после сбоя: пользователь должен видеть, что загрузка не встала,
    // а перезапускается, и по какой причине.
    virtual void on_retry(const FetchRequest &, int /*attempt*/, const Error &) {}
    // Возврат false просит прервать всё скачивание.
    virtual bool keep_going() { return true; }
};

class ArtifactStore {
public:
    explicit ArtifactStore(std::filesystem::path root);

    [[nodiscard]] const std::filesystem::path &root() const { return root_; }

    // Только про проверенные блобы: непроверенного в хранилище не бывает.
    [[nodiscard]] bool has(const BlobId &) const;
    [[nodiscard]] Result<std::filesystem::path> path_of(const BlobId &) const;

    // Скачивает, если нужно, проверяет хеш и вносит переименованием.
    // Повторный вызов на уже имеющемся блобе не трогает сеть.
    Result<std::filesystem::path> acquire(const FetchRequest &, Progress &);

    // Вносит уже имеющийся локальный файл. Нужно для --manifest с локальным
    // путём и для артефактов, собранных нами самими.
    Result<std::filesystem::path> put_file(const std::filesystem::path &source);

private:
    [[nodiscard]] std::filesystem::path blob_path(const BlobId &) const;

    std::filesystem::path root_;
};

// Параллельное скачивание. Блокировка на блоб не даёт двум задачам качать
// один и тот же файл, поэтому дубликаты в списке безвредны.
Result<void> fetch_all(ArtifactStore &, const std::vector<FetchRequest> &, int concurrency,
                       Progress &);

} // namespace cork::store
