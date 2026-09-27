// cork-test-child.exe — подопытная Windows-программа для проверок хелпера.
//
// Отдельная программа, а не режим самопроверки внутри самого хелпера: хелпер
// вшивается в поставляемый бинарник, и тестовые ветки в нём — лишний код у
// пользователя. Здесь же можно завести что угодно, включая заведомо
// неправильное поведение.
//
// Команды:
//   exit <код>          завершиться с указанным кодом (принимается и 0x...)
//   sleep <мс>          подождать
//   spawn-detached <мс> породить внука с CREATE_NO_WINDOW и сразу выйти
//   echo <текст...>     напечатать в stdout
//   args                напечатать свои аргументы по одному в строке
//   cat                 прочитать stdin и напечатать в stdout

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

unsigned long parse_code(const char *s) { return std::strtoul(s, nullptr, 0); }

int cmd_spawn_detached(const char *ms) {
    // CREATE_NO_WINDOW — ровно тот флаг, из-за которого Wine вызывает setsid()
    // и уводит процесс из Unix-группы. Внук должен пережить смерть родителя,
    // если его не держит job, и умереть вместе с job, если держит.
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);

    std::wstring cmdline = L"\"";
    cmdline += self;
    cmdline += L"\" sleep ";
    cmdline += std::to_wstring(std::strtoul(ms, nullptr, 10));

    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
    buf.push_back(L'\0');

    if (!CreateProcessW(self, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi)) {
        std::fprintf(stderr, "spawn-detached: CreateProcessW failed: %lu\n", GetLastError());
        return 1;
    }
    std::printf("grandchild %lu\n", pi.dwProcessId);
    std::fflush(stdout);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

int cmd_cat() {
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, stdin)) > 0) {
        std::fwrite(buf, 1, n, stdout);
    }
    std::fflush(stdout);
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: child <command> [args...]\n");
        return 2;
    }
    const std::string cmd(argv[1]);

    if (cmd == "exit" && argc >= 3) {
        // Именно ExitProcess, а не return: нужен полный 32-битный код, а не то,
        // что от него оставит CRT.
        ExitProcess(static_cast<UINT>(parse_code(argv[2])));
    }
    if (cmd == "sleep" && argc >= 3) {
        Sleep(static_cast<DWORD>(std::strtoul(argv[2], nullptr, 10)));
        return 0;
    }
    if (cmd == "spawn-detached" && argc >= 3) {
        return cmd_spawn_detached(argv[2]);
    }
    if (cmd == "echo") {
        for (int i = 2; i < argc; ++i) {
            std::printf("%s%s", argv[i], i + 1 < argc ? " " : "\n");
        }
        std::fflush(stdout);
        return 0;
    }
    if (cmd == "args") {
        for (int i = 1; i < argc; ++i) {
            std::printf("%s\n", argv[i]);
        }
        std::fflush(stdout);
        return 0;
    }
    if (cmd == "cat") {
        return cmd_cat();
    }

    std::fprintf(stderr, "child: unknown command %s\n", cmd.c_str());
    return 2;
}
