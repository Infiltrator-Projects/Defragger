// SPDX-License-Identifier: GPL-3.0-or-later
#include "helper_policy.hpp"
#include "json.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace defragger {
namespace {

constexpr std::uint64_t kProtocolVersion = 1U;

class Fd {
public:
    explicit Fd(int value = -1) noexcept : value_(value) {}
    ~Fd() { if (value_ >= 0) (void)close(value_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const noexcept { return value_; }
    int release() noexcept {
        const int value = value_;
        value_ = -1;
        return value;
    }
private:
    int value_;
};

Json id_value(const Json& message) {
    const Json* id = message.find("id");
    return id == nullptr ? Json(nullptr) : *id;
}

std::int64_t request_id(const Json& message) {
    const Json* id = message.find("id");
    return id == nullptr ? 0 : id->integer_or(0);
}

std::vector<std::string> string_array(const Json& value) {
    if (!value.is_array())
        throw std::runtime_error("argv must be a list of strings");
    std::vector<std::string> result;
    result.reserve(value.array().size());
    for (const auto& item : value.array()) {
        if (!item.is_string())
            throw std::runtime_error("argv must be a list of strings");
        result.emplace_back(item.string());
    }
    return result;
}

class Helper {
public:
    explicit Helper(std::uint32_t invoking_uid)
        : invoking_uid_(invoking_uid) {}

    ~Helper() {
        try { stop_active_and_wait(); } catch (...) {}
    }

    int run() {
        emit(Json::Object{
            {"type", Json("ready")},
            {"protocol", Json::unsigned_integer(kProtocolVersion)},
            {"pid", Json::unsigned_integer(
                static_cast<std::uint64_t>(getpid()))}});

        char* line = nullptr;
        std::size_t capacity = 0U;
        while (getline(&line, &capacity, stdin) >= 0) {
            try {
                Json message = Json::parse(line);
                if (!message.is_object())
                    throw std::runtime_error("request must be an object");
                const std::string action =
                    message.find("action") != nullptr
                        ? message.at("action").string_or() : "";
                if (action == "run") {
                    handle_run(message);
                } else if (action == "stop") {
                    handle_stop(message);
                } else if (action == "ping") {
                    emit(Json::Object{
                        {"type", Json("pong")},
                        {"id", id_value(message)}});
                } else if (action == "quit") {
                    stop_active_and_wait();
                    emit(Json::Object{{"type", Json("bye")}});
                    std::free(line);
                    return 0;
                } else {
                    fail(id_value(message), "unknown helper action");
                }
            } catch (const std::exception& error) {
                fail(Json(nullptr),
                     std::string("invalid request: ") + error.what());
            }
        }
        std::free(line);
        stop_active_and_wait();
        return 0;
    }

private:
    std::uint32_t invoking_uid_;
    std::mutex emit_mutex_;
    std::mutex active_mutex_;
    std::thread worker_;
    pid_t active_pid_ = -1;
    std::int64_t active_id_ = 0;
    bool active_has_output_ = false;
    bool pending_stop_ = false;

    void emit(Json::Object object) {
        const std::string encoded = Json(std::move(object)).dump();
        std::lock_guard<std::mutex> lock(emit_mutex_);
        std::fwrite(encoded.data(), 1U, encoded.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }

    void fail(Json id, const std::string& message) {
        emit(Json::Object{
            {"type", Json("error")},
            {"id", std::move(id)},
            {"message", Json(message)}});
    }

    void handle_run(const Json& message) {
        const std::int64_t id = request_id(message);
        const std::string program =
            message.find("program") != nullptr
                ? message.at("program").string_or() : "";
        const Json* raw_arguments = message.find("argv");
        if (raw_arguments == nullptr) {
            fail(Json::integer(id), "argv must be a list of strings");
            return;
        }

        std::vector<std::string> arguments;
        try {
            arguments = string_array(*raw_arguments);
        } catch (const std::exception& error) {
            fail(Json::integer(id), error.what());
            return;
        }

        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            if (active_pid_ > 0) {
                fail(Json::integer(id),
                     "another privileged operation is already active");
                return;
            }
        }
        if (worker_.joinable()) worker_.join();

        worker_ = std::thread(
            [this, id, program, arguments = std::move(arguments)] {
                run_request(id, program, arguments);
            });
    }

    void run_request(std::int64_t id, const std::string& program,
                     const std::vector<std::string>& arguments) noexcept {
        pid_t child = -1;
        try {
            const HelperCommand allowed =
                helper_command(program, arguments, invoking_uid_);
            if (access(allowed.executable.c_str(), X_OK) != 0) {
                throw std::runtime_error(
                    "helper command is unavailable: " + allowed.executable);
            }

            int output_pipe[2]{-1, -1};
            if (pipe(output_pipe) != 0)
                throw std::runtime_error(
                    std::string("creating helper pipe failed: ") +
                    std::strerror(errno));
            Fd read_end(output_pipe[0]);
            Fd write_end(output_pipe[1]);

            child = fork();
            if (child < 0)
                throw std::runtime_error(
                    std::string("fork failed: ") + std::strerror(errno));
            if (child == 0) {
                (void)setsid();
                if (dup2(write_end.get(), STDOUT_FILENO) < 0 ||
                    dup2(write_end.get(), STDERR_FILENO) < 0) {
                    _exit(126);
                }
                (void)close(read_end.get());
                (void)close(write_end.get());
                (void)setenv("LC_ALL", "C", 1);
                (void)setenv("LANG", "C", 1);

                std::vector<std::string> command;
                command.reserve(allowed.arguments.size() + 1U);
                command.push_back(allowed.executable);
                command.insert(command.end(), allowed.arguments.begin(),
                               allowed.arguments.end());
                std::vector<char*> raw;
                raw.reserve(command.size() + 1U);
                for (auto& item : command) raw.push_back(item.data());
                raw.push_back(nullptr);
                execv(raw[0], raw.data());
                _exit(errno == ENOENT ? 127 : 126);
            }

            (void)close(write_end.release());
            {
                std::lock_guard<std::mutex> lock(active_mutex_);
                active_pid_ = child;
                active_id_ = id;
                active_has_output_ = false;
                pending_stop_ = false;
            }
            emit(Json::Object{
                {"type", Json("started")},
                {"id", Json::integer(id)},
                {"pid", Json::unsigned_integer(
                    static_cast<std::uint64_t>(child))},
                {"pgid", Json::unsigned_integer(
                    static_cast<std::uint64_t>(child))}});

            FILE* stream = fdopen(read_end.release(), "r");
            if (stream == nullptr)
                throw std::runtime_error(
                    std::string("fdopen failed: ") + std::strerror(errno));
            std::unique_ptr<FILE, int(*)(FILE*)> input(stream, std::fclose);

            static const std::regex progress_expression(
                R"(^\s*(\d+(?:\.\d+)?)\s+percent completed\s*$)",
                std::regex::ECMAScript | std::regex::icase);
            char* line = nullptr;
            std::size_t capacity = 0U;
            double last_progress = -1.0;
            auto last_progress_time =
                std::chrono::steady_clock::time_point::min();

            while (getline(&line, &capacity, stream) >= 0) {
                std::string clean(line);
                while (!clean.empty() &&
                       (clean.back() == '\n' || clean.back() == '\r')) {
                    clean.pop_back();
                }

                bool deliver_queued_stop = false;
                {
                    std::lock_guard<std::mutex> lock(active_mutex_);
                    if (!active_has_output_) {
                        active_has_output_ = true;
                        deliver_queued_stop = pending_stop_;
                        pending_stop_ = false;
                    }
                }

                std::smatch match;
                if (std::regex_match(clean, match, progress_expression)) {
                    double percent = std::stod(match[1].str());
                    percent = std::clamp(percent, 0.0, 100.0);
                    const auto now = std::chrono::steady_clock::now();
                    const bool timed =
                        last_progress_time ==
                            std::chrono::steady_clock::time_point::min() ||
                        now - last_progress_time >=
                            std::chrono::milliseconds(250);
                    if (last_progress < 0.0 ||
                        std::fabs(percent - last_progress) >= 0.05 ||
                        timed || percent >= 100.0) {
                        emit(Json::Object{
                            {"type", Json("progress")},
                            {"id", Json::integer(id)},
                            {"percent", Json::real(percent)}});
                        last_progress = percent;
                        last_progress_time = now;
                    }
                } else {
                    emit(Json::Object{
                        {"type", Json("output")},
                        {"id", Json::integer(id)},
                        {"line", Json(clean)}});
                }

                if (deliver_queued_stop)
                    deliver_stop(child, Json(nullptr), id,
                                 "queued SIGINT delivered after engine initialisation");
            }
            std::free(line);

            int status = 0;
            for (;;) {
                const pid_t waited = waitpid(child, &status, 0);
                if (waited == child) break;
                if (waited < 0 && errno == EINTR) continue;
                throw std::runtime_error(
                    std::string("waitpid failed: ") + std::strerror(errno));
            }
            const int return_code = WIFEXITED(status)
                ? WEXITSTATUS(status)
                : WIFSIGNALED(status)
                    ? 128 + WTERMSIG(status) : 127;
            emit(Json::Object{
                {"type", Json("finished")},
                {"id", Json::integer(id)},
                {"returncode", Json::integer(return_code)}});
        } catch (const std::exception& error) {
            if (child > 0) {
                if (kill(-child, SIGINT) == 0 || errno == ESRCH) {
                    int status = 0;
                    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
                }
            }
            fail(Json::integer(id), error.what());
            emit(Json::Object{
                {"type", Json("finished")},
                {"id", Json::integer(id)},
                {"returncode", Json::integer(127)}});
        }

        std::lock_guard<std::mutex> lock(active_mutex_);
        active_pid_ = -1;
        active_id_ = 0;
        active_has_output_ = false;
        pending_stop_ = false;
    }

    void deliver_stop(pid_t pid, Json id, std::int64_t active_id,
                      const char* success_message = "SIGINT delivered") {
        if (kill(-pid, SIGINT) == 0) {
            emit(Json::Object{
                {"type", Json("stop-result")},
                {"id", std::move(id)},
                {"active_id", Json::integer(active_id)},
                {"delivered", Json(true)},
                {"message", Json(success_message)}});
        } else {
            const int failure = errno;
            emit(Json::Object{
                {"type", Json("stop-result")},
                {"id", std::move(id)},
                {"active_id", Json::integer(active_id)},
                {"delivered", Json(false)},
                {"message", Json(
                    failure == ESRCH ? "operation already exited"
                                     : std::strerror(failure))}});
        }
    }

    void handle_stop(const Json& message) {
        pid_t pid = -1;
        std::int64_t active_id = 0;
        bool has_output = false;
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            pid = active_pid_;
            active_id = active_id_;
            has_output = active_has_output_;
            if (pid > 0 && !has_output) pending_stop_ = true;
        }

        const Json id = id_value(message);
        if (pid <= 0) {
            emit(Json::Object{
                {"type", Json("stop-result")},
                {"id", id},
                {"active_id", Json::integer(active_id)},
                {"delivered", Json(false)},
                {"message", Json("no active operation")}});
            return;
        }
        if (!has_output) {
            emit(Json::Object{
                {"type", Json("stop-result")},
                {"id", id},
                {"active_id", Json::integer(active_id)},
                {"delivered", Json(true)},
                {"message", Json(
                    "safe stop queued until engine initialisation")}});
            return;
        }
        deliver_stop(pid, id, active_id);
    }

    void stop_active_and_wait() {
        pid_t signalled = -1;
        for (;;) {
            pid_t pid = -1;
            {
                std::lock_guard<std::mutex> lock(active_mutex_);
                pid = active_pid_;
            }
            if (pid > 0 && pid != signalled) {
                if (kill(-pid, SIGINT) == 0 || errno == ESRCH)
                    signalled = pid;
            }
            if (!worker_.joinable()) return;
            worker_.join();
            return;
        }
    }
};

} // namespace
} // namespace defragger

int main() {
    if (geteuid() != 0) {
        std::fputs(
            "Defragmenter privileged helper must run as root\n", stderr);
        return 1;
    }
    try {
        defragger::Helper helper(
            defragger::invoking_uid_from_environment());
        return helper.run();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Defragmenter privileged helper: %s\n",
                     error.what());
        return 1;
    }
}
