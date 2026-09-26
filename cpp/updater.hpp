#pragma once
#include <string>

#include "common.hpp"

namespace ml {

inline constexpr const char* REPO = "SaruM4N3/MonkeyLauncher";

struct ReleaseInfo {
    std::string version;
    std::string body;
    std::string html_url;
    std::string tarball_url;
};

// True for an install.sh (~/.local) install, false for a distro package
// (.deb/Arch, both under /usr) — those get updated via the package manager.
bool is_source_install();
// True when running straight out of a git checkout (build/ inside the repo)
// rather than an installed copy — one-click update is skipped there.
bool is_dev_checkout();
bool is_newer(const std::string& latest, const std::string& current);
bool is_newer(const std::string& latest);

// Throws std::runtime_error on network/parse failure.
ReleaseInfo check_latest_release();
// Downloads the release tarball, builds it, and installs over the current
// source install in place. Throws std::runtime_error on failure.
void perform_source_update(const std::string& tarball_url);

}  // namespace ml
