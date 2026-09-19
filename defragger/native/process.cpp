// SPDX-License-Identifier: GPL-3.0-or-later
#include "process.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace defragger {
namespace {

class Fd {
public:
    explicit Fd(int value = -1) noexcept : value_(value) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value_(other.release()) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    int get() const noexcept { return value_; }
    int release() noexcept {
        const int value = value_;
        value_ = -1;
        return value;
    }
    void reset(int value = -1) noexcept {
        if (value_ >= 0) (void)close(value_);
        value_ = value;
    }
private:
    int value_;
};

std::runtime_error system_error(const char* action) {
    return std::runtime_error(
        std::string(action) + ": " + std::strerror(errno));
}

void read_stream(int fd, std::string& output, std::size_t limit,
                 bool& truncated) noexcept {
    std::array<char, 8192> buffer{};
    for (;;) {
        const ssize_t count = read(fd, buffer.data(), buffer.size());
        if (count == 0) return;
        if (count < 0) {
            if (errno == EINTR) continue;
            return;
        }
        const auto amount = static_cast<std::size_t>(count);
        const std::size_t available =
            output.size() < limit ? limit - output.size() : 0U;
        const std::size_t kept = std::min(available, amount);
        if (kept != 0U) output.append(buffer.data(), kept);
        if (kept < amount) truncated = true;
    }
}

} // namespace

CommandResult run_capture(const std::vector<std::string>& command,
                          std::size_t output_limit) {
    if (command.empty() || command.front().empty())
        throw std::invalid_argument("cannot run an empty command");

    int stdout_pipe[2]{-1, -1};
    int stderr_pipe[2]{-1, -1};
    if (pipe(stdout_pipe) != 0) throw system_error("pipe stdout");
    Fd stdout_read(stdout_pipe[0]);
    Fd stdout_write(stdout_pipe[1]);
    if (pipe(stderr_pipe) != 0) throw system_error("pipe stderr");
    Fd stderr_read(stderr_pipe[0]);
    Fd stderr_write(stderr_pipe[1]);

    const pid_t child = fork();
    if (child < 0) throw system_error("fork");
    if (child == 0) {
        if (dup2(stdout_write.get(), STDOUT_FILENO) < 0 ||
            dup2(stderr_write.get(), STDERR_FILENO) < 0) {
            _exit(126);
        }
        stdout_read.reset();
        stdout_write.reset();
        stderr_read.reset();
        stderr_write.reset();
        (void)setenv("LC_ALL", "C", 1);
        (void)setenv("LANG", "C", 1);

        std::vector<char*> arguments;
        arguments.reserve(command.size() + 1U);
        for (const auto& item : command)
            arguments.push_back(const_cast<char*>(item.c_str()));
        arguments.push_back(nullptr);
        execv(arguments.front(), arguments.data());
        _exit(errno == ENOENT ? 127 : 126);
    }

    stdout_write.reset();
    stderr_write.reset();

    CommandResult result;
    bool stdout_truncated = false;
    bool stderr_truncated = false;
    std::thread stdout_thread(
        read_stream, stdout_read.get(), std::ref(result.standard_output),
        output_limit, std::ref(stdout_truncated));
    std::thread stderr_thread(
        read_stream, stderr_read.get(), std::ref(result.standard_error),
        output_limit, std::ref(stderr_truncated));

    int status = 0;
    for (;;) {
        const pid_t waited = waitpid(child, &status, 0);
        if (waited == child) break;
        if (waited < 0 && errno == EINTR) continue;
        stdout_thread.join();
        stderr_thread.join();
        throw system_error("waitpid");
    }
    stdout_thread.join();
    stderr_thread.join();
    result.output_truncated = stdout_truncated || stderr_truncated;

    if (WIFEXITED(status))
        result.return_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        result.return_code = 128 + WTERMSIG(status);
    else
        result.return_code = 126;

    if (result.output_truncated)
        throw std::runtime_error("child process output exceeded safety limit");
    return result;
}

} // namespace defragger
