#include "updater.hpp"

#include <nlohmann/json.hpp>
#include <stdlib.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <thread>

#include "http.hpp"
#include "logging.hpp"
#include "proc.hpp"
#include "version.hpp"

namespace ml {

// INSTALL_LIB is wherever the executable lives — ~/.local/lib/monkeylauncher
// for a source install, /usr/lib/monkeylauncher for a .deb/Arch package, or
// the repo's build/ dir for a dev checkout run in place.
static fs::path install_lib() { return exe_dir(); }
static fs::path install_bin() { return home_dir() / ".local" / "bin"; }
static fs::path icon_dest()   { return home_dir() / ".local" / "share" / "icons" / "monkeylauncher.png"; }

bool is_source_install() {
    const fs::path lib = install_lib(), home = home_dir();
    auto [h, l] = std::mismatch(home.begin(), home.end(), lib.begin(), lib.end());
    return h == home.end();
}

bool is_dev_checkout() {
    std::error_code ec;
    return fs::exists(install_lib().parent_path() / ".git", ec);
}

static std::vector<int> parse_version(const std::string& v) {
    std::vector<int> parts;
    size_t i = 0;
    while (i < v.size() && (v[i] == 'v' || v[i] == 'V')) ++i;
    std::string cur;
    auto flush = [&] {
        std::string digits;
        for (char c : cur) if (std::isdigit(static_cast<unsigned char>(c))) digits += c;
        parts.push_back(digits.empty() ? 0 : std::stoi(digits));
        cur.clear();
    };
    for (; i < v.size(); ++i) {
        if (v[i] == '.') flush(); else cur += v[i];
    }
    flush();
    return parts;
}

bool is_newer(const std::string& latest, const std::string& current) {
    return parse_version(latest) > parse_version(current);   // tuple comparison
}
bool is_newer(const std::string& latest) { return is_newer(latest, current_version()); }

static std::string json_string(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : "";
}

static std::string lstrip_v(std::string s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == 'v' || s[i] == 'V')) ++i;
    return s.substr(i);
}

ReleaseInfo check_latest_release() {
    const std::string url = std::string("https://api.github.com/repos/") + REPO + "/releases/latest";
    HttpResponse resp = http_get(url, 10, {"Accept: application/vnd.github+json"});
    if (resp.status >= 400) throw std::runtime_error("HTTP Error " + std::to_string(resp.status));
    nlohmann::json j = nlohmann::json::parse(resp.body);
    return {lstrip_v(json_string(j, "tag_name")), json_string(j, "body"),
            json_string(j, "html_url"), json_string(j, "tarball_url")};
}

// Copies over a possibly-running executable safely: write next to it, then
// rename over (a plain overwrite fails with "Text file busy").
static void replace_file(const fs::path& src, const fs::path& dst) {
    const fs::path tmp = dst.string() + ".new";
    fs::copy_file(src, tmp, fs::copy_options::overwrite_existing);
    fs::permissions(tmp, fs::status(src).permissions());
    std::error_code ec;
    if (fs::is_directory(dst, ec)) fs::remove_all(dst, ec);   // e.g. the old Python package dir
    fs::rename(tmp, dst);
}

void perform_source_update(const std::string& tarball_url) {
    char tmpl[] = "/tmp/monkeylauncher-update-XXXXXX";
    if (!mkdtemp(tmpl)) throw std::runtime_error("could not create a temporary directory");
    const fs::path tmp = tmpl;
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code ec; fs::remove_all(p, ec); } } cleanup{tmp};

    const fs::path archive = tmp / "release.tar.gz";
    long status = http_download(tarball_url, archive, 30);
    if (status >= 400) throw std::runtime_error("HTTP Error " + std::to_string(status));

    if (run({"tar", "-xzf", archive.string(), "-C", tmp.string()}) != 0)
        throw std::runtime_error("Could not extract the release archive");

    fs::path root;
    for (const auto& e : fs::directory_iterator(tmp))
        if (e.is_directory()) { root = e.path(); break; }
    if (root.empty()) throw std::runtime_error("Downloaded release archive was empty");

    // The release ships sources, so build them here.
    log_info("Building update in {}", root.string());
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    if (run({"make", "-j" + std::to_string(cores), "-C", root.string()}) != 0)
        throw std::runtime_error("Build failed — see the terminal output for the compiler errors");

    const fs::path lib = install_lib();
    fs::create_directories(lib);
    replace_file(root / "build" / "monkeylauncher", lib / "monkeylauncher");

    std::error_code ec;
    if (fs::exists(root / "VERSION", ec)) replace_file(root / "VERSION", lib / "VERSION");

    if (fs::exists(root / "src" / "MonkeyLauncherCLI.sh", ec)) {
        const fs::path cli = install_bin() / "MonkeyLauncherCLI";
        replace_file(root / "src" / "MonkeyLauncherCLI.sh", cli);
        fs::permissions(cli, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                                 fs::perms::others_read | fs::perms::others_exec);
    }
    if (fs::exists(root / "src" / "logo.png", ec)) {
        fs::create_directories(icon_dest().parent_path());
        replace_file(root / "src" / "logo.png", icon_dest());
    }
    log_info("Updated MonkeyLauncher files in {}", lib.string());
}

}  // namespace ml
