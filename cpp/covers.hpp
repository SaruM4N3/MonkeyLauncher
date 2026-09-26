#pragma once
#include <optional>
#include <string>

#include "common.hpp"

namespace ml {

fs::path cover_cache_path(const std::string& exe);
// Best-guess display name for a game when no manual override is set.
std::string default_game_name(const std::string& exe);
// Downloads (or returns the cached) cover from the Steam store; nullopt if
// nothing was found or the lookup failed (logged, never thrown).
std::optional<fs::path> fetch_cover(const std::string& exe);

}  // namespace ml
