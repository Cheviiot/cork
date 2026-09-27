#pragma once

// Запуск локального HTTP-сервера из tests/httpd/server.py. Порт читается из
// первой строки его stdout, завершается закрытием stdin. Вынесено в
// заголовок, потому что нужно и загрузчику, и хранилищу.

#include <cstdlib>
#include <string>

#include <csignal>
#include <filesystem>
#include <sys/wait.h>
#include <unistd.h>

namespace cork::test {

class LocalServer {
public:
    bool start(const std::filesystem::path &script, const std::filesystem::path &root) {
        int out_pipe[2];
        int in_pipe[2];
        if (::pipe(out_pipe) != 0 || ::pipe(in_pipe) != 0) {
            return false;
        }
        pid_ = ::fork();
        if (pid_ < 0) {
            return false;
        }
        if (pid_ == 0) {
            ::dup2(in_pipe[0], STDIN_FILENO);
            ::dup2(out_pipe[1], STDOUT_FILENO);
            ::close(in_pipe[1]);
            ::close(out_pipe[0]);
            ::execlp("python3", "python3", script.c_str(), root.c_str(), nullptr);
            ::_exit(127);
        }
        ::close(in_pipe[0]);
        ::close(out_pipe[1]);
        stdin_fd_ = in_pipe[1];

        std::string line;
        char c = 0;
        while (::read(out_pipe[0], &c, 1) == 1 && c != '\n') {
            line.push_back(c);
        }
        ::close(out_pipe[0]);
        if (line.empty()) {
            return false;
        }
        port_ = std::atoi(line.c_str());
        return port_ > 0;
    }

    ~LocalServer() { stop(); }

    LocalServer() = default;
    LocalServer(const LocalServer &) = delete;
    LocalServer &operator=(const LocalServer &) = delete;

    void stop() {
        if (stdin_fd_ >= 0) {
            ::close(stdin_fd_);
            stdin_fd_ = -1;
        }
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            int status = 0;
            ::waitpid(pid_, &status, 0);
            pid_ = -1;
        }
    }

    [[nodiscard]] std::string url(const std::string &suffix) const {
        return "http://127.0.0.1:" + std::to_string(port_) + suffix;
    }

private:
    pid_t pid_ = -1;
    int stdin_fd_ = -1;
    int port_ = 0;
};

} // namespace cork::test
