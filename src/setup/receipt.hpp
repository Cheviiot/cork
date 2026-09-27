#pragma once

// Receipt — что на самом деле произошло с этим поколением.
//
// Зачем он нужен, видно из того, во что вырождается проверка без него.
// Готовность, определяемая наличием каталогов и файла bin/wine, довольна
// деревом с нулём символьных ссылок вместо двух сотен — то есть с
// развалившимся Wine Mono, без которого не стартует MSBuild. Наличие каталога
// не говорит ни о том, что в нём, ни о том, чем он туда попал.
//
// Receipt отвечает на это тем, что записывает не намерение, а результат: из
// какого манифеста, какие пейлоады по каким хешам, сколько файлов откуда
// распаковано, какие патчи с какими хешами до и после, какой дайджест у
// получившегося дерева. Отсюда же берётся ответ «в каком состоянии установка»,
// и состояние не выводится из файловой системы, а читается из журнала.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "base/error.hpp"
#include "setup/digest.hpp"

namespace cork::setup {

inline constexpr const char *kReceiptFileName = "receipt.json";
inline constexpr const char *kReceiptSchemaName = "cork/receipt";
inline constexpr int kReceiptSchemaVersion = 1;

// Состояния идут строго по возрастанию: каждое следующее требует предыдущего.
// Пропуск невозможен по построению — переход добавляется только тем кодом,
// который этот этап выполнил и проверил.
enum class State {
    Resolved = 1,   // выбор пакетов сделан, дайджест выбора записан
    Fetched = 2,    // каждый пейлоад лежит в хранилище и проверен по sha256
    Staged = 3,     // всё распаковано и перенесено, дерево собрано
    Verified = 4,   // дайджест дерева сошёлся, обязательные проверки прошли
    Published = 5,  // поколение переименовано на место и видно как current
    Buildable = 6,  // пробник скомпилирован, скомпонован и проверен
};

const char *state_name(State);
Result<State> state_from_name(std::string_view);

struct StateEntry {
    State state = State::Resolved;
    std::string at;       // ISO 8601 UTC
    std::string version;  // версия cork, выполнившая переход
};

struct ManifestSource {
    std::string channel_url;
    std::string manifest_url;
    std::string manifest_sha256;
    std::string product_version;
};

struct SelectionRecord {
    std::vector<std::string> package_ids;
    // sha256 от отсортированного перечисления выбора. Позволяет сказать «выбор
    // тот же» без сравнения шестисот строк.
    std::string digest;
};

struct PayloadRecord {
    std::string package_id;
    std::string file_name;
    std::string sha256;
    std::int64_t size = 0;
};

struct UnpackRecord {
    std::string package_id;
    std::string source;  // имя пейлоада
    std::string kind;    // "zip" или "msi"
    std::uint64_t files = 0;
};

struct PatchRecord {
    std::string patch;  // имя файла патча
    std::string path;   // что пропатчено, относительно корня поколения
    std::string pre_sha256;
    std::string post_sha256;
};

// Состояние своей сборки Wine внутри поколения. Симлинки и Mono вынесены
// отдельными числами не для красоты: именно они разваливались молча, и вопрос
// «сколько их должно быть» должен иметь письменный ответ.
struct WineRecord {
    std::string id;
    std::uint64_t symlinks = 0;
    std::uint64_t mono_files = 0;
};

struct Receipt {
    ManifestSource source;
    SelectionRecord selection;
    std::vector<PayloadRecord> payloads;
    std::vector<UnpackRecord> unpacked;
    // Аппликатора патчей к дереву MSVC ещё нет; массив заложен сразу, чтобы
    // его появление не потребовало новой версии схемы.
    std::vector<PatchRecord> patches;
    TreeDigest tree;
    WineRecord wine;
    std::vector<StateEntry> log;

    // Наибольшее достигнутое состояние, или ошибка, если журнал пуст.
    [[nodiscard]] Result<State> state() const;
    [[nodiscard]] bool at_least(State) const;

    // Добавляет переход. Повтор того же состояния разрешён (повторная
    // проверка), откат назад — нет: это означало бы, что кто-то переписывает
    // историю, а не продолжает её.
    [[nodiscard]] Result<void> advance(State, std::string_view version);

    [[nodiscard]] std::string to_json() const;
    static Result<Receipt> from_json(std::string_view);

    [[nodiscard]] Result<void> save(const std::filesystem::path &generation_root) const;
    static Result<Receipt> load(const std::filesystem::path &generation_root);
};

// Дайджест выбора: sha256 от отсортированных идентификаторов пакетов.
std::string selection_digest(std::vector<std::string> package_ids);

// Текущее время в ISO 8601 UTC с точностью до секунды.
std::string now_iso8601();

} // namespace cork::setup
