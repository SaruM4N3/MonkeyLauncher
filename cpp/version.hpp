#pragma once
#include <string>

namespace ml {

// Reads the VERSION file installed alongside the executable
// (INSTALL_LIB/VERSION for both source and package installs), falling back
// to the repo root for a dev checkout run straight out of build/.
const std::string& current_version();

}  // namespace ml
