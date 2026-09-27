#pragma once

// Сессия сборки: свой префикс Wine на всё время, пока идёт сборка.
//
// Зачем вообще отдельный префикс. Префикс — это изменяемое состояние: реестр,
// системный каталог, службы. Две параллельные сборки в одном префиксе мешают
// друг другу, а брошенный процесс Wine из предыдущей сборки переживает её и
// достаётся следующей. Отдельный префикс на сборку убирает и то, и другое.
//
// Почему это не бесплатно и почему всё-таки дёшево. Префикс весит больше
// полугигабайта, и копировать его на каждую сборку было бы немыслимо. Но на
// btrfs и xfs копия делается разделением экстентов и стоит почти ничего —
// см. base/clone.hpp. На файловой системе без reflink цена настоящая, и об
// этом сказано один раз, с предложением общего префикса.
//
// Когда сборка кончилась. Точного ответа не существует: сборочная система не
// сообщает об этом никому. Поэтому есть три способа, и все три нужны:
//
//   * `cork run -- <команда>` владеет временем жизни явно и сносит префикс на
//     выходе, включая Ctrl-C. Это единственный надёжный способ;
//   * `cork session stop|kill` — руками, когда сборку запускали иначе;
//   * сборка мусора по возрасту при создании новой сессии — чтобы брошенные
//     префиксы не копились на диске месяцами.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "base/error.hpp"
#include "base/lock.hpp"
#include "setup/generation.hpp"

namespace cork::exec {

enum class PrefixMode {
    // Свой префикс на сессию, полученный клонированием шаблона.
    PerSession,
    // Один общий префикс на всех. Спасение для файловых систем без reflink,
    // но параллельные сборки снова мешают друг другу.
    Shared,
};

PrefixMode prefix_mode_from_env();

// Ключ сессии — то, что отличает одну сборку от другой.
//
// Выводится из корня сборочного дерева: первый каталог вверх по пути, в
// котором лежит build.ninja, CMakeCache.txt, *.sln или .git. Так все вызовы
// компилятора одной сборки попадают в одну сессию, а разные проекты не
// делят префикс. Переопределяется CORK_SESSION.
std::string session_key(const std::filesystem::path &cwd);

struct Session {
    std::string key;
    std::filesystem::path dir;     // sessions/<key>
    std::filesystem::path prefix;  // префикс Wine этой сессии
    bool created = false;          // префикс создан этим вызовом, а не найден
};

// Открывает сессию, создавая префикс при первом обращении. Держит на ней
// разделяемую блокировку, пока живёт Lock: по ней сборка мусора и `session
// stop` понимают, что сессия занята.
[[nodiscard]] Result<Session> open_session(const setup::Root &, const std::string &key,
                                           const std::filesystem::path &wine_runtime,
                                           const std::string &wine_id, Lock &hold);

// Сносит сессию. Если ею кто-то занят, отказ — кроме force, который сначала
// гасит процессы Wine этого префикса.
[[nodiscard]] Result<void> close_session(const setup::Root &, const std::string &key,
                                         const std::filesystem::path &wine_runtime, bool force);

struct SessionInfo {
    std::string key;
    std::filesystem::path prefix;
    std::uint64_t bytes = 0;
    std::chrono::system_clock::time_point last_used;
    bool busy = false;
};

[[nodiscard]] Result<std::vector<SessionInfo>> list_sessions(const setup::Root &);

// Убирает сессии старше age, которыми никто не занят. Возвращает, сколько
// снесено.
[[nodiscard]] Result<std::uint64_t> collect_sessions(const setup::Root &,
                                                     const std::filesystem::path &wine_runtime,
                                                     std::chrono::seconds age);

} // namespace cork::exec
