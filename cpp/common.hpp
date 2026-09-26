#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace ml {

namespace fs = std::filesystem;

// Python's str.strip() / str.split helpers used all over the config parsing.
std::string strip(const std::string& s);
std::string to_lower(std::string s);
std::vector<std::string> split_lines(const std::string& text);
std::vector<std::string> split_whitespace(const std::string& text);
std::string join(const std::vector<std::string>& parts, const std::string& sep);
bool starts_with(const std::string& s, const std::string& prefix);
std::string read_text_file(const fs::path& p);
bool write_text_file(const fs::path& p, const std::string& text);

// Path.home(): $HOME, falling back to the passwd entry.
fs::path home_dir();

// Directory containing the running executable (INSTALL_LIB).
fs::path exe_dir();

}  // namespace ml
