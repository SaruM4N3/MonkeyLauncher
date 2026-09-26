#pragma once
#include <sys/types.h>

#include <map>
#include <string>
#include <vector>

#include "common.hpp"

namespace ml {

using Env = std::map<std::string, std::string>;

// os.environ.copy()
Env current_env();

// Starts a child without waiting (subprocess.Popen). Throws
// std::runtime_error when the program cannot be executed. `env` == nullptr
// inherits the current environment; `quiet` sends stdout/stderr to /dev/null.
pid_t spawn(const std::vector<std::string>& argv, const Env* env = nullptr,
            const fs::path& cwd = {}, bool quiet = false);

// Non-blocking poll: true (and sets *code) if the child has exited.
bool poll_pid(pid_t pid, int* code = nullptr);
// Blocks until exit; returns the exit code (negative signal number if killed).
int wait_pid(pid_t pid);
// Waits up to `seconds`; true if the child exited in time.
bool wait_pid_timeout(pid_t pid, double seconds);

// spawn + wait_pid (subprocess.run without capturing).
int run(const std::vector<std::string>& argv, const Env* env = nullptr,
        const fs::path& cwd = {});

struct RunResult {
    int         code = -1;
    bool        timed_out = false;
    std::string out;
};
// subprocess.run(capture_output=True, timeout=…): stdout captured, stderr
// discarded. On timeout the child is killed and timed_out is set.
RunResult run_capture(const std::vector<std::string>& argv, double timeout_seconds);

// Popen-and-forget (xdg-open, steam…): the child is reaped in the background.
void spawn_detached(const std::vector<std::string>& argv);

// shlex.split(); throws std::runtime_error on unbalanced quotes.
std::vector<std::string> shlex_split(const std::string& text);

}  // namespace ml
