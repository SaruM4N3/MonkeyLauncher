#include "logging.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>

#include "config.hpp"

namespace ml {

namespace {

constexpr std::uintmax_t MAX_BYTES    = 2'000'000;
constexpr int            BACKUP_COUNT = 2;

std::mutex g_mutex;
bool       g_debug      = false;
bool       g_use_color  = false;
bool       g_file_ready = false;
std::ofstream g_file;

const char* level_name(Level l) {
    switch (l) {
        case Level::Debug:    return "DEBUG";
        case Level::Info:     return "INFO";
        case Level::Warning:  return "WARNING";
        case Level::Error:    return "ERROR";
        case Level::Critical: return "CRITICAL";
    }
    return "";
}

const char* level_color(Level l) {
    switch (l) {
        case Level::Debug:    return "\033[2;37m";      // dim gray
        case Level::Info:     return "\033[0;36m";      // cyan
        case Level::Warning:  return "\033[1;33m";      // yellow
        case Level::Error:    return "\033[0;31m";      // red
        case Level::Critical: return "\033[1;41;37m";   // bold white on red
    }
    return "";
}

std::string format_line(Level l, const std::string& msg) {
    char ts[16];
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    std::strftime(ts, sizeof ts, "%H:%M:%S", &tm);
    char head[64];
    std::snprintf(head, sizeof head, "%s %-8s ", ts, level_name(l));
    return std::string(head) + msg;
}

// Same scheme as Python's RotatingFileHandler: monkeylauncher.log rolls to
// .log.1, .log.1 to .log.2, and the oldest is dropped.
void rotate_if_needed(size_t incoming) {
    std::error_code ec;
    auto size = fs::file_size(log_file(), ec);
    if (ec || size + incoming < MAX_BYTES) return;
    g_file.close();
    for (int i = BACKUP_COUNT - 1; i >= 1; --i) {
        fs::path from = log_file().string() + "." + std::to_string(i);
        fs::path to   = log_file().string() + "." + std::to_string(i + 1);
        if (fs::exists(from, ec)) fs::rename(from, to, ec);
    }
    fs::rename(log_file(), log_file().string() + ".1", ec);
    g_file.open(log_file(), std::ios::app);
}

}  // namespace

void log_init(int argc, char** argv) {
    const char* env = std::getenv("MONKEYLAUNCHER_DEBUG");
    g_debug = env && std::strcmp(env, "1") == 0;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--debug") || !std::strcmp(argv[i], "-v")) g_debug = true;
    g_use_color = isatty(fileno(stderr));

    std::error_code ec;
    fs::create_directories(log_dir(), ec);
    g_file.open(log_file(), std::ios::app);
    g_file_ready = g_file.is_open();
    if (!g_file_ready)
        log_warning("Could not open log file {}", log_file().string());
}

void log_write(Level level, const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::string line = format_line(level, msg);

    if (g_debug || level >= Level::Info) {
        if (g_use_color)
            std::fprintf(stderr, "%s%s\033[0m\n", level_color(level), line.c_str());
        else
            std::fprintf(stderr, "%s\n", line.c_str());
    }
    if (g_file_ready) {
        rotate_if_needed(line.size() + 1);
        g_file << line << '\n';
        g_file.flush();
    }
}

}  // namespace ml
