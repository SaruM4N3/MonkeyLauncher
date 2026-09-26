#include "proc.hpp"

#include <fcntl.h>
#include <glibmm/main.h>
#include <glibmm/spawn.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

extern char** environ;

namespace ml {

Env current_env() {
    Env env;
    for (char** e = environ; e && *e; ++e) {
        const char* eq = std::strchr(*e, '=');
        if (eq) env[std::string(*e, eq - *e)] = eq + 1;
    }
    return env;
}

namespace {

int decode_status(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return -WTERMSIG(status);
    return -1;
}

struct CArgs {
    std::vector<std::string> storage;
    std::vector<char*> ptrs;
    explicit CArgs(std::vector<std::string> s) : storage(std::move(s)) {
        for (auto& x : storage) ptrs.push_back(x.data());
        ptrs.push_back(nullptr);
    }
    char** data() { return ptrs.data(); }
};

// fork/exec with an errno pipe so a missing program is reported to the
// caller (like Popen's FileNotFoundError) instead of a silent exit(127).
// stdout_fd >= 0 redirects the child's stdout there.
pid_t do_spawn(const std::vector<std::string>& argv, const Env* env, const fs::path& cwd,
               bool quiet, int stdout_fd) {
    if (argv.empty()) throw std::runtime_error("empty command");

    CArgs cargv(argv);
    std::vector<std::string> envstrs;
    for (const auto& [k, v] : env ? *env : current_env()) envstrs.push_back(k + "=" + v);
    CArgs cenv(std::move(envstrs));
    const std::string cwd_str = cwd.string();

    int errpipe[2];
    if (pipe2(errpipe, O_CLOEXEC) != 0) throw std::runtime_error(std::strerror(errno));

    pid_t pid = fork();
    if (pid < 0) {
        int e = errno;
        close(errpipe[0]);
        close(errpipe[1]);
        throw std::runtime_error(std::strerror(e));
    }
    if (pid == 0) {
        if (!cwd_str.empty() && chdir(cwd_str.c_str()) != 0) {
            int e = errno;
            (void)!write(errpipe[1], &e, sizeof e);
            _exit(127);
        }
        if (stdout_fd >= 0) {
            dup2(stdout_fd, STDOUT_FILENO);
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        } else if (quiet) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }
        execvpe(cargv.ptrs[0], cargv.data(), cenv.data());
        int e = errno;
        (void)!write(errpipe[1], &e, sizeof e);
        _exit(127);
    }

    close(errpipe[1]);
    int child_errno = 0;
    ssize_t n;
    do { n = read(errpipe[0], &child_errno, sizeof child_errno); } while (n < 0 && errno == EINTR);
    close(errpipe[0]);
    if (n == static_cast<ssize_t>(sizeof child_errno)) {
        int status;
        waitpid(pid, &status, 0);
        throw std::runtime_error(std::string(std::strerror(child_errno)) + ": '" + argv[0] + "'");
    }
    return pid;
}

}  // namespace

pid_t spawn(const std::vector<std::string>& argv, const Env* env, const fs::path& cwd, bool quiet) {
    return do_spawn(argv, env, cwd, quiet, -1);
}

bool poll_pid(pid_t pid, int* code) {
    int status;
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) {
        if (code) *code = decode_status(status);
        return true;
    }
    return false;
}

int wait_pid(pid_t pid) {
    int status;
    pid_t r;
    do { r = waitpid(pid, &status, 0); } while (r < 0 && errno == EINTR);
    return r == pid ? decode_status(status) : -1;
}

bool wait_pid_timeout(pid_t pid, double seconds) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (poll_pid(pid)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return poll_pid(pid);
}

int run(const std::vector<std::string>& argv, const Env* env, const fs::path& cwd) {
    return wait_pid(spawn(argv, env, cwd));
}

RunResult run_capture(const std::vector<std::string>& argv, double timeout_seconds) {
    RunResult res;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) throw std::runtime_error(std::strerror(errno));
    pid_t pid;
    try {
        pid = do_spawn(argv, nullptr, {}, false, fds[1]);
    } catch (...) {
        close(fds[0]);
        close(fds[1]);
        throw;
    }
    close(fds[1]);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_seconds);
    char buf[4096];
    for (;;) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) { res.timed_out = true; break; }
        pollfd pfd{fds[0], POLLIN, 0};
        int pr = poll(&pfd, 1, static_cast<int>(left));
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) { res.timed_out = (pr == 0); break; }
        ssize_t n = read(fds[0], buf, sizeof buf);
        if (n <= 0) break;  // EOF
        res.out.append(buf, static_cast<size_t>(n));
    }
    close(fds[0]);
    if (res.timed_out) {
        kill(pid, SIGKILL);
        wait_pid(pid);
        res.code = -SIGKILL;
    } else {
        res.code = wait_pid(pid);
    }
    return res;
}

void spawn_detached(const std::vector<std::string>& argv) {
    pid_t pid = spawn(argv);
    // Reap it once it exits so it doesn't linger as a zombie.
    Glib::signal_child_watch().connect([](GPid p, int) { Glib::spawn_close_pid(p); }, pid);
}

// POSIX shlex.split (no comment handling, like Python's default).
std::vector<std::string> shlex_split(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    bool in_token = false;
    enum { Normal, Single, Double } state = Normal;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        switch (state) {
            case Normal:
                if (std::isspace(static_cast<unsigned char>(c))) {
                    if (in_token) { out.push_back(cur); cur.clear(); in_token = false; }
                } else if (c == '\'') { state = Single; in_token = true; }
                else if (c == '"')  { state = Double; in_token = true; }
                else if (c == '\\') {
                    if (i + 1 >= text.size()) throw std::runtime_error("No escaped character");
                    cur += text[++i];
                    in_token = true;
                } else { cur += c; in_token = true; }
                break;
            case Single:
                if (c == '\'') state = Normal; else cur += c;
                break;
            case Double:
                if (c == '"') state = Normal;
                else if (c == '\\') {
                    if (i + 1 >= text.size()) throw std::runtime_error("No escaped character");
                    char n = text[i + 1];
                    if (n == '"' || n == '\\') { cur += n; ++i; }
                    else cur += c;   // backslash kept literally, as in POSIX shlex
                } else cur += c;
                break;
        }
    }
    if (state != Normal) throw std::runtime_error("No closing quotation");
    if (in_token) out.push_back(cur);
    return out;
}

}  // namespace ml
