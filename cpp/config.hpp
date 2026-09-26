#pragma once
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
const fs::path& steam_root();
const fs::path& wineprefix_path();

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
