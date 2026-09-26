#pragma once
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common.hpp"

namespace ml {

// Insertion-ordered key/value store, mirroring the Python dict the config
// files were read into (write order follows insertion order).
class Config {
public:
    bool has(const std::string& key) const;
    std::string get(const std::string& key, const std::string& def = "") const;
    void set(const std::string& key, const std::string& value);
    void erase(const std::string& key);
    const std::vector<std::pair<std::string, std::string>>& items() const { return items_; }
    void clear() { items_.clear(); }

private:
    std::vector<std::pair<std::string, std::string>> items_;
};

// ── Paths ────────────────────────────────────────────────────────────────────
const fs::path& config_dir();
const fs::path& config_file();
const fs::path& gamedirs_file();
const fs::path& games_dir();
const fs::path& saves_base();
const fs::path& log_dir();
const fs::path& log_file();
const fs::path& covers_dir();
// Steam install dir: the STEAM_ROOT saved in the config (a folder the user
// picked) if it is valid, else auto-detected. Both references stay valid
// across reload_steam_paths().
const fs::path& steam_root();
const fs::path& wineprefix_path();
// True if steam_root() really contains a steamapps/ directory (i.e. Steam
// was found, as opposed to falling back to the default location).
bool steam_root_found();
// Re-runs the detection (after the config's STEAM_ROOT changed).
void reload_steam_paths();
// Accepts a folder containing steamapps/, or the steamapps/ folder itself
// (returning its parent). nullopt if it is neither.
std::optional<fs::path> normalize_steam_dir(const fs::path& chosen);
// Remembers a user-chosen Steam dir in the config and re-detects.
void save_steam_root(const fs::path& dir);
// Forgets the user-chosen Steam dir (back to auto-detection).
void clear_steam_root();
// True if steam_root() comes from a folder the user picked.
bool steam_root_is_custom();

// ── Game-dirs helpers ────────────────────────────────────────────────────────
std::vector<std::string> read_gamedirs();
void write_gamedirs(const std::vector<std::string>& dirs);

// ── Config helpers ───────────────────────────────────────────────────────────
Config read_config(const fs::path& path);
void write_config(const fs::path& path, const Config& cfg);

std::string game_key(const std::string& label);
fs::path game_config_path(const std::string& label);
fs::path game_save_path(const std::string& label);

// ── Game directory scanning ──────────────────────────────────────────────────
std::vector<std::string> get_exe_list(const fs::path& gamedir);
std::vector<std::string> get_all_exe_list(const fs::path& gamedir);

// All *.exe files under `base`, as paths relative to it, sorted like
// Python's sorted(Path.rglob('*.exe')) (component-wise).
std::vector<fs::path> rglob_exe(const fs::path& base);
// Same for entries with exactly this name (Path.rglob(name)).
std::vector<fs::path> rglob_named(const fs::path& base, const std::string& name);

}  // namespace ml
