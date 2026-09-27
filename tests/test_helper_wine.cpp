// Проверки PE-хелпера против настоящего Wine. MSVC для них не нужен — этим
// они и ценны: их можно гонять на каждый коммит, в отличие от приёмки, которой
// нужны многогигабайтные пакеты Microsoft.
//
// Пропускается (код 77), если Wine или хелпер не собраны: обычная сборка
// проекта их не производит.

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "base/fs.hpp"
#include "check.h"
#include "proto/protocol.hpp"

namespace fs = cork::fs;
using fs::stdfs::path;
using namespace cork::proto;

namespace {

constexpr int kSkip = 77;

struct Env {
    path wine;
    path helper;
    path child;
    path work;     // рабочий каталог проверок
    path prefix;   // WINEPREFIX
};

std::string env_or(const char *name, const std::string &fallback) {
    const char *v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? std::string(v) : fallback;
}

// Запуск программы с ожиданием и ограничением по времени. Таймаут здесь не
// роскошь: смысл половины проверок в том, что процессы не остаются жить, и
// зависший тест должен падать, а не висеть в CI до самого лимита задания.
struct RunResult {
    bool timed_out = false;
    int exit_code = -1;
};

RunResult run(const std::vector<std::string> &argv, const std::vector<std::string> &extra_env,
              int timeout_ms) {
    std::vector<char *> cargv;
    for (const auto &a : argv) {
        cargv.push_back(const_cast<char *>(a.c_str()));
    }
    cargv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        return {false, -1};
    }
    if (pid == 0) {
        for (const auto &kv : extra_env) {
            ::putenv(const_cast<char *>(kv.c_str()));
        }
        ::setpgid(0, 0);
        ::execv(cargv[0], cargv.data());
        ::_exit(127);
    }

    // Опрос вместо sigtimedwait: проще и здесь достаточно, шага в 20 мс хватает
    // на проверках, которые длятся доли секунды.
    const int step_ms = 20;
    for (int waited = 0; waited < timeout_ms; waited += step_ms) {
        int status = 0;
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            return {false, WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status)};
        }
        ::usleep(static_cast<useconds_t>(step_ms) * 1000);
    }
    ::kill(-pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
    return {true, -1};
}

// Пишет request-файл и запускает хелпер. Возвращает код возврата процесса wine
// и разобранный статус, если он появился.
struct HelperRun {
    RunResult proc;
    bool has_status = false;
    Status status;
};

HelperRun run_helper(const Env &e, Request req, const std::string &tag, int timeout_ms = 30000) {
    const path req_file = e.work / (tag + ".req");
    const path status_file = e.work / (tag + ".status");
    fs::stdfs::remove(status_file);

    req.status_path = status_file.string();
    const auto bytes = encode(req);
    (void)fs::write_atomic(req_file,
                           std::span<const std::byte>(
                               reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()));

    HelperRun out;
    out.proc = run({e.wine.string(), e.helper.string(), req_file.string()},
                   {"WINEPREFIX=" + e.prefix.string(), "WINEDEBUG=-all"}, timeout_ms);

    auto content = fs::read_file(status_file);
    if (content.has_value() && !content->empty()) {
        Status s;
        if (decode(reinterpret_cast<const unsigned char *>(content->data()), content->size(), s) ==
            0) {
            out.has_status = true;
            out.status = s;
        }
    }
    return out;
}

} // namespace

int main() {
    const path source_dir(CORK_SOURCE_DIR);

    Env e;
    e.wine = env_or("CORK_TEST_WINE",
                    (source_dir / "build/wine-staging/opt/cork-wine/bin/wine").string());
    e.helper = env_or("CORK_TEST_HELPER",
                      (source_dir / "build/helper/cork-helper.exe").string());
    e.child = env_or("CORK_TEST_CHILD",
                     (source_dir / "build/helper/cork-test-child.exe").string());

    if (!fs::is_regular_file(e.wine) || !fs::is_regular_file(e.helper) ||
        !fs::is_regular_file(e.child)) {
        std::fprintf(stderr,
                     "skip: need a built Wine and helper\n"
                     "  wine:   %s\n  helper: %s\n  child:  %s\n"
                     "  build them with tools/wine/build.sh && tools/wine/build-helper.sh\n",
                     e.wine.c_str(), e.helper.c_str(), e.child.c_str());
        return kSkip;
    }

    // Рабочие файлы кладутся в build/, а не в /tmp: на этой машине /tmp — это
    // tmpfs, и префикс Wine туда попросту не поместится.
    e.work = source_dir / "build/helper-test";
    e.prefix = e.work / "prefix";
    fs::stdfs::remove_all(e.work);
    (void)fs::mkdir_p(e.work);

    // --- 1. обычное завершение ---
    {
        Request r;
        r.exe = e.child.string();
        r.args = {"exit", "0"};
        const auto got = run_helper(e, r, "exit0");
        CHECK(!got.proc.timed_out);
        CHECK(got.has_status);
        if (got.has_status) {
            CHECK(got.status.kind == StatusKind::ChildExited);
            CHECK_EQ(std::to_string(got.status.code), std::string("0"));
            // Job должен создаваться: без него нет гарантии уборки дерева.
            CHECK((got.status.flags & kJobCreated) != 0);
        }
    }

    // --- 2. код возврата больше байта ---
    // Ради этого всё и затевалось: mt.exe отдаёт 0x41020001, Unix отдаёт
    // родителю восемь бит, и без хелпера значение до нас не доезжает.
    {
        Request r;
        r.exe = e.child.string();
        r.args = {"exit", "0x41020001"};
        const auto got = run_helper(e, r, "bigexit");
        CHECK(!got.proc.timed_out);
        CHECK(got.has_status);
        if (got.has_status) {
            CHECK(got.status.kind == StatusKind::ChildExited);
            CHECK_EQ(std::to_string(got.status.code), std::to_string(0x41020001u));
            // А сам процесс wine при этом отдаёт лишь младший байт — ровно та
            // потеря, которую хелпер и обходит.
            CHECK_EQ(std::to_string(got.proc.exit_code), std::string("1"));
        }
    }

    // --- 3. нечего запускать ---
    {
        Request r;
        r.exe = (e.work / "no-such-tool.exe").string();
        r.args = {"whatever"};
        const auto got = run_helper(e, r, "nospawn");
        CHECK(!got.proc.timed_out);
        CHECK(got.has_status);
        if (got.has_status) {
            CHECK(got.status.kind == StatusKind::SpawnFailed);
            CHECK(got.status.code != 0);
        }
    }

    // --- 4. трансляция путей и кавычки ---
    // Аргумент, помеченный как путь, должен доехать до программы уже в
    // DOS-форме; непомеченный — как есть. Заодно проверяется, что аргумент с
    // пробелом не разваливается на два.
    {
        const path some_dir = e.work / "путь с пробелом";
        (void)fs::mkdir_p(some_dir);

        Request r;
        r.exe = e.child.string();
        r.args = {"args", some_dir.string(), "/nologo", "plain arg"};
        r.path_refs = {{1, 0}};

        const path out_file = e.work / "args.out";
        // Вывод хелпера наследуется напрямую, поэтому перехватываем его
        // перенаправлением потока на уровне процесса.
        const int saved = ::dup(STDOUT_FILENO);
        const int fd = ::open(out_file.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        ::dup2(fd, STDOUT_FILENO);
        const auto got = run_helper(e, r, "paths");
        ::fflush(stdout);
        ::dup2(saved, STDOUT_FILENO);
        ::close(fd);
        ::close(saved);

        CHECK(!got.proc.timed_out);
        auto out = fs::read_file(out_file);
        CHECK(out.has_value());
        if (out) {
            // Путь переведён: появилась буква диска и обратные слэши.
            const bool translated =
                out->find(":\\") != std::string::npos &&
                out->find(some_dir.string()) == std::string::npos;
            if (!translated) {
                std::fprintf(stderr, "вывод потомка:\n%s\n", out->c_str());
            }
            CHECK(translated);
            // Непомеченные аргументы не тронуты, и аргумент с пробелом цел.
            CHECK(out->find("/nologo") != std::string::npos);
            CHECK(out->find("plain arg") != std::string::npos);
        }
    }

    // --- 5. эксперимент 1: job удерживает отсоединённого внука ---
    // Потомок порождает внука с CREATE_NO_WINDOW (именно на этом флаге Wine
    // зовёт setsid() и уводит процесс из Unix-группы) и сразу выходит. Хелпер
    // выходит следом, его единственный хэндл на job закрывается, и
    // KILL_ON_JOB_CLOSE обязан забрать внука с собой.
    //
    // Проверяется через `wineserver -w`: он ждёт завершения всех процессов
    // префикса. Если job сработал, ожидание возвращается сразу; если нет —
    // висит все тридцать секунд, которые спит внук.
    {
        Request r;
        r.exe = e.child.string();
        r.args = {"spawn-detached", "30000"};
        const auto got = run_helper(e, r, "job");
        CHECK(!got.proc.timed_out);
        CHECK(got.has_status);
        if (got.has_status) {
            CHECK((got.status.flags & kJobCreated) != 0);
        }

        const path wineserver = e.wine.parent_path() / "wineserver";
        const auto wait = run({wineserver.string(), "-w"},
                              {"WINEPREFIX=" + e.prefix.string(), "WINEDEBUG=-all"}, 8000);
        if (wait.timed_out) {
            std::fprintf(stderr,
                         "внук пережил закрытие job: KILL_ON_JOB_CLOSE не сработал\n");
        }
        CHECK(!wait.timed_out);
    }

    return cork::test::finish("test_helper_wine");
}
