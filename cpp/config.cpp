#include "config.hpp"

#include <algorithm>
#include <regex>
#include <set>

namespace ml {

bool Config::has(const std::string& key) const {
    return std::any_of(items_.begin(), items_.end(),
                       [&](const auto& kv) { return kv.first == key; });
}

std::string Config::get(const std::string& key, const std::string& def) const {
    for (const auto& kv : items_)
        if (kv.first == key) return kv.second;
    return def;
}

void Config::set(const std::string& key, const std::string& value) {
    for (auto& kv : items_) {
        if (kv.first == key) {
            kv.second = value;
            return;
        }
    }
    items_.emplace_back(key, value);
}

void Config::erase(const std::string& key) {
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [&](const auto& kv) { return kv.first == key; }),
                 items_.end());
}

// ── Paths ────────────────────────────────────────────────────────────────────
const fs::path& config_dir() {
    static const fs::path p = home_dir() / ".config" / "MonkeyLauncher";
    return p;
}
const fs::path& config_file()   { static const fs::path p = config_dir() / "config";   return p; }
const fs::path& gamedirs_file() { static const fs::path p = config_dir() / "gamedirs"; return p; }
const fs::path& games_dir()     { static const fs::path p = config_dir() / "games";    return p; }
const fs::path& saves_base()    { static const fs::path p = config_dir() / "saves";    return p; }
const fs::path& log_dir()       { static const fs::path p = config_dir() / "logs";     return p; }
const fs::path& log_file()      { static const fs::path p = log_dir() / "monkeylauncher.log"; return p; }
const fs::path& covers_dir()    { static const fs::path p = config_dir() / "covers";   return p; }

// Locates the real Steam install dir. This varies by distro/packaging:
// Arch puts it straight at ~/.local/share/Steam, while the official Debian/
// Ubuntu package installs to ~/.steam/debian-installation instead. Steam
// itself maintains ~/.steam/root and ~/.steam/steam as symlinks to wherever
// it actually lives (the same symlink Proton resolves for
// STEAM_COMPAT_CLIENT_INSTALL_PATH), so prefer those over guessing.
static fs::path detect_steam_root() {
    const fs::path home = home_dir();
    const std::vector<fs::path> candidates = {
        home / ".steam" / "root",
        home / ".steam" / "steam",
        home / ".local" / "share" / "Steam",
        home / ".steam" / "debian-installation",
        home / ".var" / "app" / "com.valvesoftware.Steam" / ".local" / "share" / "Steam",
        home / ".var" / "app" / "com.valvesoftware.Steam" / "data" / "Steam",
        home / "snap" / "steam" / "common" / ".local" / "share" / "Steam",
    };
    std::error_code ec;
    for (const auto& c : candidates)
        if (fs::is_directory(c / "steamapps", ec)) return c;
    return home / ".local" / "share" / "Steam";
}

const fs::path& steam_root() {
    static const fs::path p = detect_steam_root();
    return p;
}

const fs::path& wineprefix_path() {
    static const fs::path p = steam_root() / "steamapps" / "compatdata" / "480";
    return p;
}

static const std::set<std::string> EXCLUDE_DIRS = {"_CommonRedist", "Binaries"};
static const std::regex EXCLUDE_NAMES("CrashHandler", std::regex::icase);

// ── Game-dirs helpers ────────────────────────────────────────────────────────
std::vector<std::string> read_gamedirs() {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::exists(gamedirs_file(), ec)) return out;
    for (const auto& line : split_lines(read_text_file(gamedirs_file())))
        if (!strip(line).empty()) out.push_back(line);
    return out;
}

void write_gamedirs(const std::vector<std::string>& dirs) {
    std::error_code ec;
    fs::create_directories(config_dir(), ec);
    write_text_file(gamedirs_file(), join(dirs, "\n") + (dirs.empty() ? "" : "\n"));
}

// ── Config helpers ───────────────────────────────────────────────────────────
Config read_config(const fs::path& path) {
    Config cfg;
    std::error_code ec;
    if (!fs::exists(path, ec)) return cfg;
    for (const auto& line : split_lines(read_text_file(path))) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        cfg.set(strip(line.substr(0, eq)), strip(line.substr(eq + 1)));
    }
    return cfg;
}

void write_config(const fs::path& path, const Config& cfg) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::vector<std::string> lines;
    for (const auto& [k, v] : cfg.items())
        if (!v.empty()) lines.push_back(k + "=" + v);
    write_text_file(path, join(lines, "\n") + "\n");
}

std::string game_key(const std::string& label) {
    std::string out;
    for (char c : label) {
        if (c == '/') out += "__";
        else if (c == ' ') out += '_';
        else out += c;
    }
    return out;
}

fs::path game_config_path(const std::string& label) { return games_dir() / game_key(label); }
fs::path game_save_path(const std::string& label)   { return saves_base() / game_key(label); }

// ── Game directory scanning ──────────────────────────────────────────────────
std::vector<fs::path> rglob_exe(const fs::path& base) {
    std::vector<fs::path> result;
    std::error_code ec;
    if (!fs::is_directory(base, ec)) return result;
    fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".exe") == 0)
            result.push_back(it->path().lexically_relative(base));
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<fs::path> rglob_named(const fs::path& base, const std::string& name) {
    std::vector<fs::path> result;
    std::error_code ec;
    if (!fs::is_directory(base, ec)) return result;
    fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec))
        if (it->path().filename().string() == name)
            result.push_back(it->path().lexically_relative(base));
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::string> get_exe_list(const fs::path& gamedir) {
    std::vector<std::string> result;
    for (const auto& rel : rglob_exe(gamedir)) {
        bool excluded = false;
        for (const auto& part : rel)
            if (EXCLUDE_DIRS.count(part.string())) excluded = true;
        if (excluded) continue;
        if (std::regex_search(rel.filename().string(), EXCLUDE_NAMES)) continue;
        result.push_back(rel.string());
    }
    return result;
}

std::vector<std::string> get_all_exe_list(const fs::path& gamedir) {
    std::vector<std::string> result;
    for (const auto& rel : rglob_exe(gamedir)) result.push_back(rel.string());
    return result;
}

}  // namespace ml
