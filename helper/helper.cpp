// cork-helper.exe — нативная Windows-половина обёртки инструментов.
//
// Собирается wineg++ из дерева Wine, поставляемого вместе с cork, и
// вшивается в бинарник cork. Существует потому, что три вещи доступны
// только изнутри Windows-процесса:
//
//   1. Настоящий 32-битный код возврата. Unix отдаёт родителю восемь бит, а
//      mt.exe законно завершается с 0x41020001 («манифест не изменился»),
//      которое CMake ожидает увидеть как 0xbb. К моменту, когда код доходит до
//      Unix-стороны, он уже усечён до 0x01, и восстановить его неоткуда.
//   2. Трансляция путей по настоящей таблице дисков префикса
//      (wine_get_dos_file_name), а не подстановкой «z:» строкой. Путь внутри
//      префикса при этом корректно становится C:\..., а не Z:\...\drive_c\...
//   3. Job Object, из которого нельзя выйти через setsid(). Именно так Wine
//      уводит процессы, созданные с CREATE_NO_WINDOW, из Unix-группы
//      процессов — и именно поэтому прежняя реализация вынуждена была
//      заводить cgroup v2.
//
// Запускается как: wine cork-helper.exe <unix-путь-к-request-файлу>

#include <windows.h>

#include <string>
#include <vector>

#include "proto/protocol.hpp"

namespace {

using cork::proto::Request;
using cork::proto::Status;
using cork::proto::StatusKind;

// --- трансляция путей --------------------------------------------------------

// Обёртка над wine_get_dos_file_name с освобождением памяти. Wine выделяет
// результат в куче процесса и требует HeapFree — держать это в одном месте
// дешевле, чем помнить на каждом вызове.
class DosPath {
public:
    explicit DosPath(const std::string &unix_path) {
        raw_ = wine_get_dos_file_name(unix_path.c_str());
    }
    ~DosPath() {
        if (raw_ != nullptr) {
            HeapFree(GetProcessHeap(), 0, raw_);
        }
    }
    DosPath(const DosPath &) = delete;
    DosPath &operator=(const DosPath &) = delete;

    bool ok() const { return raw_ != nullptr; }
    const wchar_t *get() const { return raw_; }
    std::wstring str() const { return raw_ != nullptr ? std::wstring(raw_) : std::wstring(); }

private:
    WCHAR *raw_ = nullptr;
};

std::wstring to_wide(const std::string &s) {
    if (s.empty()) {
        return {};
    }
    const int need = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                         nullptr, 0);
    if (need <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), need);
    return out;
}

// Путь несуществующего файла в существующем каталоге транслируется, а путь в
// несуществующем каталоге — нет. Поэтому при неудаче пробуем перевести
// родительский каталог и приклеить имя обратно: именно так ведёт себя вывод
// компилятора: /Fo указывает на ещё не созданный .obj, и без этого приёма он
// остался бы непереведённым.
std::wstring translate_path(const std::string &unix_path) {
    DosPath direct(unix_path);
    if (direct.ok()) {
        return direct.str();
    }

    const std::size_t slash = unix_path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) {
        return {};
    }
    const std::string parent = unix_path.substr(0, slash);
    DosPath dir(parent);
    if (!dir.ok()) {
        return {};
    }
    std::wstring out = dir.str();
    if (!out.empty() && out.back() != L'\\') {
        out += L'\\';
    }
    out += to_wide(unix_path.substr(slash + 1));
    return out;
}

// --- сборка командной строки -------------------------------------------------

// Правила CommandLineToArgvW целиком, включая обратные слэши перед кавычкой.
// Наивная схема «обернуть в кавычки, экранировать кавычку» ломается на пути,
// кончающемся обратным слэшем, — а это любой каталог вида C:\dir\:
// закрывающая кавычка оказывается экранированной, и аргумент склеивается со
// следующим.
void append_quoted(std::wstring &out, const std::wstring &arg) {
    const bool needs_quotes =
        arg.empty() || arg.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!needs_quotes) {
        out += arg;
        return;
    }

    out += L'"';
    for (auto it = arg.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            // Перед закрывающей кавычкой каждый обратный слэш надо удвоить,
            // иначе он съест саму кавычку.
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        out += *it;
    }
    out += L'"';
}

// --- чтение и запись ---------------------------------------------------------

bool read_whole_file(const std::wstring &dos_path, std::vector<unsigned char> &out) {
    HANDLE h = CreateFileW(dos_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 || size.QuadPart > (16 << 20)) {
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    const bool ok = out.empty() ||
                    (ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) &&
                     read == out.size());
    CloseHandle(h);
    return ok;
}

// Статус пишется и сбрасывается на диск до выхода. Его отсутствие нативная
// сторона трактует как внутреннюю ошибку, а не как успех: молчание здесь
// означает, что хелпер не добрался до конца, и это надо видеть.
bool write_status(const std::wstring &dos_path, const Status &s) {
    if (dos_path.empty()) {
        return false;
    }
    const auto bytes = cork::proto::encode(s);
    HANDLE h = CreateFileW(dos_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                              nullptr) &&
                    written == bytes.size();
    FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
}

// --- Job Object --------------------------------------------------------------

// Перевод одного аргумента: ключ остаётся как есть, путь после offset
// переводится. offset == npos означает «это не путь».
std::wstring translate_argument(const std::string &arg, std::size_t offset) {
    if (offset == std::wstring::npos) {
        return to_wide(arg);
    }
    const std::string prefix = arg.substr(0, offset);
    const std::string path = arg.substr(offset);

    // Относительный путь не переводится: потомок наследует наш рабочий
    // каталог, и Wine разрешит его сам и правильно. Перевод в абсолютную
    // DOS-форму тут ничего не добавил бы, а зависимость от того, где
    // оказался хелпер, добавил бы.
    if (path.empty() || path[0] != '/') {
        return to_wide(arg);
    }
    const std::wstring translated = translate_path(path);
    // Непереводимый путь передаётся как есть: аргумент мог быть опознан как
    // путь ошибочно, и портить его молча нельзя.
    return to_wide(prefix) + (translated.empty() ? to_wide(path) : translated);
}

// Переписывает response-файл с переведёнными путями и возвращает аргумент
// "@<новый файл>" для командной строки.
//
// Новый файл кладётся рядом со status-файлом, то есть в каталог запуска, и
// исчезает вместе с ним. Писать рядом с исходным нельзя: он может лежать в
// дереве проекта, доступном только на чтение, да и мусорить там незачем.
bool rewrite_response_file(const cork::proto::ResponseFile &rf,
                           const std::string &status_unix_path, std::wstring &out_arg) {
    std::vector<std::size_t> offset(rf.args.size(), std::wstring::npos);
    for (const auto &ref : rf.path_refs) {
        offset[ref.index] = ref.offset;
    }

    std::wstring body;
    for (std::size_t i = 0; i < rf.args.size(); ++i) {
        if (i != 0) {
            body += L'\n';
        }
        // Кавычки те же, что у командной строки: файл читает тот же разборщик.
        append_quoted(body, translate_argument(rf.args[i], offset[i]));
    }
    body += L'\n';

    // Имя нового файла — по номеру аргумента: в одной команде их может быть
    // несколько, и перезаписать один другим было бы тихой поломкой.
    std::string unix_path = status_unix_path + ".rsp" + std::to_string(rf.arg_index);
    const std::wstring dos = translate_path(unix_path);
    if (dos.empty()) {
        return false;
    }

    HANDLE h = CreateFileW(dos.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    // UTF-16LE с меткой порядка байтов: так его читает cl без догадок о
    // кодировке, и пути с кириллицей переживают запись без потерь.
    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    bool ok = WriteFile(h, &bom, sizeof bom, &written, nullptr) != 0 && written == sizeof bom;
    if (ok && !body.empty()) {
        const DWORD bytes = static_cast<DWORD>(body.size() * sizeof(wchar_t));
        ok = WriteFile(h, body.data(), bytes, &written, nullptr) != 0 && written == bytes;
    }
    CloseHandle(h);
    if (!ok) {
        return false;
    }
    out_arg = L"@" + dos;
    return true;
}

HANDLE job_handle = nullptr;

// Три решения, и каждое существенно.
//
// Нет JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK. Флаг выглядит безобидно, но
// разрешает любому потомку тихо выйти из job — то есть своими руками отменяет
// гарантию, ради которой job и заводился.
//
// В job помещается сам хелпер, а не ребёнок после CreateProcessW. Иначе между
// созданием процесса и его назначением остаётся окно, в котором ребёнок
// успевает породить внука вне job. Членство наследуется, поэтому «сначала
// себя» закрывает окно полностью.
//
// KILL_ON_JOB_CLOSE работает как страховочный трос: единственный хэндл держит
// хелпер, и если его убили с Unix-стороны, job уничтожается вместе со всем
// деревом.
bool create_job() {
    job_handle = CreateJobObjectW(nullptr, nullptr);
    if (job_handle == nullptr) {
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if (!SetInformationJobObject(job_handle, JobObjectExtendedLimitInformation, &info,
                                 sizeof info)) {
        CloseHandle(job_handle);
        job_handle = nullptr;
        return false;
    }
    if (!AssignProcessToJobObject(job_handle, GetCurrentProcess())) {
        CloseHandle(job_handle);
        job_handle = nullptr;
        return false;
    }
    return true;
}

BOOL WINAPI console_handler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
            if (job_handle != nullptr) {
                TerminateJobObject(job_handle, 130);
            }
            return TRUE;
        default:
            return FALSE;
    }
}

// Коды возврата самого хелпера умещаются в байт и служат подсказкой: истина
// всегда в status-файле.
constexpr int kExitBadRequest = 0x7e;
constexpr int kExitSpawnFailed = 0x7f;

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        return kExitBadRequest;
    }

    const std::string request_unix_path(argv[1]);
    const std::wstring request_dos = translate_path(request_unix_path);

    std::vector<unsigned char> raw;
    if (request_dos.empty() || !read_whole_file(request_dos, raw)) {
        return kExitBadRequest;
    }

    Request req;
    const std::uint32_t reason = cork::proto::decode(raw.data(), raw.size(), req);
    if (reason != 0) {
        // Путь к status-файлу известен только из самого запроса, поэтому
        // сообщить о неразобранном запросе через него нельзя — остаётся код
        // возврата.
        return kExitBadRequest;
    }

    const std::wstring status_dos = translate_path(req.status_path);
    Status status;

    const std::wstring exe_dos = translate_path(req.exe);
    if (exe_dos.empty()) {
        status.kind = StatusKind::SpawnFailed;
        status.code = ERROR_FILE_NOT_FOUND;
        write_status(status_dos, status);
        return kExitSpawnFailed;
    }

    const bool want_job = (req.flags & cork::proto::kNoJob) == 0;
    if (want_job && create_job()) {
        status.flags |= cork::proto::kJobCreated;
    }
    SetConsoleCtrlHandler(console_handler, TRUE);

    // Какие аргументы являются путями и с какого места, решила нативная
    // сторона; здесь только выполняется перевод.
    std::vector<std::size_t> path_offset(req.args.size(), std::wstring::npos);
    if ((req.flags & cork::proto::kTranslatePaths) != 0) {
        for (const auto &ref : req.path_refs) {
            path_offset[ref.index] = ref.offset;
        }
    }

    // Response-файлы переписываются до сборки командной строки: аргумент
    // "@old.rsp" превращается в "@new.rsp", и дальше он уже обычный аргумент.
    std::vector<std::wstring> rewritten(req.args.size());
    if ((req.flags & cork::proto::kTranslateResponseFiles) != 0) {
        for (const auto &rf : req.response_files) {
            std::wstring replacement;
            if (!rewrite_response_file(rf, req.status_path, replacement)) {
                status.kind = cork::proto::StatusKind::BadRequest;
                status.code = cork::proto::kReasonResponseWriteFailed;
                write_status(status_dos, status);
                return 1;
            }
            rewritten[rf.arg_index] = replacement;
        }
    }

    std::wstring cmdline;
    append_quoted(cmdline, exe_dos);
    for (std::size_t i = 0; i < req.args.size(); ++i) {
        cmdline += L' ';
        if (!rewritten[i].empty()) {
            append_quoted(cmdline, rewritten[i]);
            continue;
        }
        append_quoted(cmdline, translate_argument(req.args[i], path_offset[i]));
    }

    std::wstring cwd_dos;
    if (!req.cwd.empty()) {
        cwd_dos = translate_path(req.cwd);
    }

    // Окружение потомка выставляется здесь, а не наследуется от нас: часть
    // переменных ломает сам хелпер. См. Request::child_env.
    for (const auto &kv : req.child_env) {
        const std::size_t eq = kv.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        SetEnvironmentVariableW(to_wide(kv.substr(0, eq)).c_str(),
                                to_wide(kv.substr(eq + 1)).c_str());
    }

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutable_cmdline(cmdline.begin(), cmdline.end());
    mutable_cmdline.push_back(L'\0');

    if (!CreateProcessW(exe_dos.c_str(), mutable_cmdline.data(), nullptr, nullptr, TRUE, 0,
                        nullptr, cwd_dos.empty() ? nullptr : cwd_dos.c_str(), &si, &pi)) {
        status.kind = StatusKind::SpawnFailed;
        status.code = GetLastError();
        write_status(status_dos, status);
        return kExitSpawnFailed;
    }

    status.child_pid = pi.dwProcessId;
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(pi.hProcess, &exit_code)) {
        exit_code = GetLastError();
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    status.kind = StatusKind::ChildExited;
    status.code = exit_code;
    write_status(status_dos, status);

    // Отображение кодов (в том числе mt.exe 0x41020001 -> 0xbb) делает нативная
    // сторона: она знает, каким именем её позвали, а здесь это означало бы
    // сниффинг basename, который в прежней реализации уже однажды не совпадал
    // с реальным путём.
    return static_cast<int>(exit_code & 0xff);
}
