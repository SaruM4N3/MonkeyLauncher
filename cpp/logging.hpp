#pragma once
#include <sstream>
#include <string>
#include <utility>

namespace ml {

enum class Level { Debug, Info, Warning, Error, Critical };

// Reads MONKEYLAUNCHER_DEBUG / --debug / -v and opens the log file.
void log_init(int argc, char** argv);
void log_write(Level level, const std::string& msg);

// Minimal "{}" formatter (std::format needs GCC 13+; this builds as C++17 so
// the app compiles on Debian 12 / Ubuntu 20.04+). Each "{}" is replaced by
// the next argument, streamed with operator<<.
inline void format_into(std::ostringstream& out, const char* f) { out << f; }
template <class T, class... Rest>
void format_into(std::ostringstream& out, const char* f, const T& v, const Rest&... rest) {
    for (; *f; ++f) {
        if (f[0] == '{' && f[1] == '}') {
            out << v;
            format_into(out, f + 2, rest...);
            return;
        }
        out << *f;
    }
}
template <class... Args>
std::string format(const char* f, const Args&... a) {
    std::ostringstream out;
    format_into(out, f, a...);
    return out.str();
}

template <class... Args>
void log_debug(const char* f, const Args&... a) { log_write(Level::Debug, format(f, a...)); }
template <class... Args>
void log_info(const char* f, const Args&... a) { log_write(Level::Info, format(f, a...)); }
template <class... Args>
void log_warning(const char* f, const Args&... a) { log_write(Level::Warning, format(f, a...)); }
template <class... Args>
void log_error(const char* f, const Args&... a) { log_write(Level::Error, format(f, a...)); }

}  // namespace ml
