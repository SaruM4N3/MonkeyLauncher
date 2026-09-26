#include "steam.hpp"

#include <signal.h>

#include <algorithm>
#include <chrono>
#include <regex>
#include <thread>

#include "config.hpp"
#include "logging.hpp"
#include "proc.hpp"

namespace ml {

// ── Steam helpers ────────────────────────────────────────────────────────────
std::vector<fs::path> get_steam_libs() {
    std::vector<fs::path> libs;
    const fs::path vdf = steam_root() / "steamapps" / "libraryfolders.vdf";
    std::error_code ec;
    if (!fs::exists(vdf, ec)) return libs;
    const std::string text = read_text_file(vdf);
    static const std::regex re("\"path\"\\s+\"([^\"]+)\"");
    for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it)
        libs.emplace_back((*it)[1].str());
    return libs;
}

std::vector<fs::path> get_proton_dirs() {
    std::vector<fs::path> dirs;
    std::error_code ec;
    for (const auto& lib : get_steam_libs()) {
        const fs::path common = lib / "steamapps" / "common";
        if (!fs::exists(common, ec)) continue;
        std::vector<fs::path> found;
        for (fs::directory_iterator it(common, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            std::error_code ec2;
            if (fs::is_directory(it->path(), ec2) && starts_with(name, "Proton"))
                found.push_back(it->path());
        }
        std::sort(found.begin(), found.end(), [](const fs::path& a, const fs::path& b) {
            return a.filename().string() < b.filename().string();
        });
        dirs.insert(dirs.end(), found.begin(), found.end());
    }
    return dirs;
}

static std::vector<fs::path> libs_or_root() {
    auto libs = get_steam_libs();
    if (libs.empty()) libs.push_back(steam_root());
    return libs;
}

std::optional<fs::path> get_spacewar_dir() {
    std::error_code ec;
    for (const auto& lib : libs_or_root()) {
        const fs::path manifest = lib / "steamapps" / "appmanifest_480.acf";
        if (!fs::exists(manifest, ec)) continue;
        static const std::regex re("\"installdir\"\\s+\"([^\"]+)\"");
        const std::string text = read_text_file(manifest);
        std::smatch m;
        const std::string installdir = std::regex_search(text, m, re) ? m[1].str() : "Spacewar";
        const fs::path d = lib / "steamapps" / "common" / installdir;
        if (fs::is_directory(d, ec)) return d;
    }
    return std::nullopt;
}

std::optional<fs::path> find_spacewar_exe() {
    auto d = get_spacewar_dir();
    if (!d) return std::nullopt;
    auto exes = rglob_exe(*d);
    if (exes.empty()) return std::nullopt;
    return *d / exes.front();
}

bool check_steam_running() {
    try {
        return wait_pid(spawn({"pgrep", "-x", "steam"}, nullptr, {}, /*quiet=*/true)) == 0;
    } catch (...) {
        return false;
    }
}

bool check_app480_installed() {
    std::error_code ec;
    for (const auto& lib : libs_or_root())
        if (fs::exists(lib / "steamapps" / "appmanifest_480.acf", ec)) return true;
    return false;
}

void setup_save_symlink(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::create_directories(dst);
    if (fs::is_directory(src, ec) && !fs::is_symlink(src, ec)) {
        for (const auto& item : fs::directory_iterator(src))
            fs::copy(item.path(), dst / item.path().filename(),
                     fs::copy_options::overwrite_existing | fs::copy_options::recursive);
        fs::remove_all(src);
    }
    if (fs::is_symlink(src, ec)) fs::remove(src);
    if (!fs::exists(src, ec)) fs::create_symlink(dst, src);
}

void run_through_proton(const std::string& exe_path, const fs::path& proton_path) {
    Env env = current_env();
    env["WINE"]             = (proton_path / "files" / "bin" / "wine64").string();
    env["WINESERVER"]       = (proton_path / "files" / "bin" / "wineserver").string();
    env["WINEPREFIX"]       = wineprefix_path().string() + "/";
    env["WINEDLLOVERRIDES"] = "OnlineFix64=n;SteamOverlay64=n;winmm=n,b;dnet=n;steam_api64=n";
    env["PROTONPATH"]       = proton_path.string();
    env["DXVK_STATE_CACHE"] = "1";
    env["GAMEID"]           = "480";
    log_info("Running installer via Proton: {} (proton={})", exe_path,
             proton_path.filename().string());
    pid_t pid = spawn({"umu-run", exe_path}, &env, fs::path(exe_path).parent_path());
    // Popen-and-forget: reap in the background.
    std::thread([pid] { wait_pid(pid); }).detach();
}

// Copies the real Valve steamclient files into the shared prefix's fake
// Steam install (drive_c/Program Files (x86)/Steam/).
//
// Real Steam does this itself via Proton whenever STEAM_COMPAT_CLIENT_INSTALL_PATH
// is set, which is how it normally ends up there. But umu-run, invoked as a
// plain CLI command (`umu-run <exe>`, as we do), resets that variable to an
// empty string internally before it ever reaches Proton — so Proton never
// performs this copy for any prefix set up or run through umu-run. Without
// it, OnlineFix (and other Goldberg-style Steam emulators) can't find the
// "original" steamclient to fall back to for calls they don't implement,
// and fail with "steamclient not found". Safe to call any time; just
// re-copies over whatever's there.
void sync_steamclient_files() {
    const fs::path legacycompat = steam_root() / "legacycompat";
    const fs::path dest = wineprefix_path() / "pfx" / "drive_c" / "Program Files (x86)" / "Steam";
    const std::pair<const char*, const char*> files[] = {
        {"steamclient.dll",           "steamclient.dll"},
        {"steamclient64.dll",         "steamclient64.dll"},
        {"GameOverlayRenderer64.dll", "GameOverlayRenderer64.dll"},
        {"SteamService.exe",          "steam.exe"},
        {"Steam.dll",                 "Steam.dll"},
    };
    std::error_code ec;
    if (!fs::is_directory(wineprefix_path() / "pfx", ec)) return;
    fs::create_directories(dest, ec);
    for (const auto& [src, tgt] : files) {
        const fs::path srcfile = legacycompat / src;
        if (!fs::is_regular_file(srcfile, ec)) continue;
        std::error_code cec;
        fs::copy_file(srcfile, dest / tgt, fs::copy_options::overwrite_existing, cec);
        if (cec) log_warning("Could not copy {} into prefix Steam dir: {}", src, cec.message());
    }
    log_debug("Synced steamclient files into {}", dest.string());
}

// Creates the shared compatdata/480 prefix by launching Spacewar once
// through the given Proton, then killing it as soon as the prefix
// directory shows up. Runs quietly (no dialog) — just a blocking wait.
bool bootstrap_proton_prefix(const fs::path& proton_path) {
    auto exe = find_spacewar_exe();
    if (!exe) {
        log_error("Could not find a Spacewar executable to initialize the Proton prefix.");
        return false;
    }

    log_info("Proton prefix not found — setting it up (first run) using {}…",
             proton_path.filename().string());
    Env env = current_env();
    env["WINE"]       = (proton_path / "files" / "bin" / "wine64").string();
    env["WINESERVER"] = (proton_path / "files" / "bin" / "wineserver").string();
    env["WINEPREFIX"] = wineprefix_path().string() + "/";
    env["PROTONPATH"] = proton_path.string();
    env["GAMEID"]     = "480";
    log_debug("Bootstrapping prefix with {} via {}", exe->string(),
              proton_path.filename().string());
    pid_t pid;
    try {
        pid = spawn({"umu-run", exe->string()}, &env, {}, /*quiet=*/true);
    } catch (const std::exception& e) {
        log_error("Could not start umu-run: {}", e.what());
        return false;
    }

    // wineprefix_path() itself becomes "pfx" (often as a same-directory symlink,
    // created almost immediately) — not a reliable signal that wine has
    // actually finished initializing. system.reg/user.reg only get written
    // once wineboot completes, so wait for those instead; otherwise this
    // terminates Spacewar mid-setup and leaves a half-initialized prefix.
    const fs::path pfx        = wineprefix_path() / "pfx";
    const fs::path system_reg = wineprefix_path() / "system.reg";
    const fs::path user_reg   = wineprefix_path() / "user.reg";
    std::error_code ec;
    auto ready_now = [&] {
        return fs::is_directory(pfx, ec) && fs::exists(system_reg, ec) && fs::exists(user_reg, ec);
    };

    bool exited = false;
    int  waited = 0;
    while (!ready_now() && waited < 180) {
        if (poll_pid(pid)) { exited = true; break; }
        std::this_thread::sleep_for(std::chrono::seconds(1));
        ++waited;
    }

    const bool ready = ready_now();
    if (!exited && !poll_pid(pid)) {
        log_debug("Terminating bootstrap process now that the prefix exists");
        kill(pid, SIGTERM);
        if (!wait_pid_timeout(pid, 5)) {
            kill(pid, SIGKILL);
            wait_pid(pid);
        }
    }

    if (ready) log_info("Proton prefix created at {}", pfx.string());
    else       log_error("Proton prefix setup timed out or failed.");
    return ready;
}

}  // namespace ml
