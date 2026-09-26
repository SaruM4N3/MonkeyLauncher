#pragma once
#include <optional>
#include <string>
#include <vector>

#include "common.hpp"

namespace ml {

std::vector<fs::path> get_steam_libs();
std::vector<fs::path> get_proton_dirs();
std::optional<fs::path> get_spacewar_dir();
std::optional<fs::path> find_spacewar_exe();
bool check_steam_running();
bool check_app480_installed();

void setup_save_symlink(const fs::path& prefix_savedir, const fs::path& ml_savedir);
void run_through_proton(const std::string& exe_path, const fs::path& proton_path);
void sync_steamclient_files();
bool bootstrap_proton_prefix(const fs::path& proton_path);

}  // namespace ml
