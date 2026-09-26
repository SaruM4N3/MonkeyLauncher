#include <gtkmm.h>

#include <memory>
#include <optional>

#include "config.hpp"
#include "dialogs.hpp"
#include "logging.hpp"
#include "main_window.hpp"
#include "proc.hpp"
#include "steam.hpp"
#include "ui.hpp"

using namespace ml;

// ── Startup checks ───────────────────────────────────────────────────────────
// Shows a Cancel-only dialog while `check` (polled every `interval_ms`) is
// false; closes it as soon as it turns true. Returns false if cancelled.
static bool wait_dialog(Gtk::Window* parent, const Glib::ustring& text, const Glib::ustring& secondary,
                        const std::vector<std::string>& launch_cmd, unsigned interval_ms,
                        bool (*check)()) {
    std::unique_ptr<Gtk::MessageDialog> d;
    if (parent) d = std::make_unique<Gtk::MessageDialog>(*parent, text, false, Gtk::MESSAGE_INFO, Gtk::BUTTONS_CANCEL);
    else        d = std::make_unique<Gtk::MessageDialog>(text, false, Gtk::MESSAGE_INFO, Gtk::BUTTONS_CANCEL);
    d->set_secondary_text(secondary);
    d->show();

    try {
        spawn_detached(launch_cmd);
    } catch (const std::exception& e) {
        log_error("Could not run {}: {}", launch_cmd.front(), e.what());
    }

    Gtk::MessageDialog* dp = d.get();
    sigc::connection poll = Glib::signal_timeout().connect(
        [dp, check] {
            if (check()) {
                dp->response(Gtk::RESPONSE_OK);
                return false;
            }
            return true;
        }, interval_ms);
    const int response = d->run();
    poll.disconnect();   // e.g. after Cancel — must not touch the destroyed dialog
    return response != Gtk::RESPONSE_CANCEL;
}

static bool wait_for_steam(Gtk::Window* parent = nullptr) {
    return wait_dialog(parent, "Starting Steam…", "Waiting for Steam to launch before continuing.",
                       {"steam"}, 1500, check_steam_running);
}

static bool wait_for_spacewar(Gtk::Window* parent = nullptr) {
    return wait_dialog(parent, "Installing Spacewar (App 480)…",
                       "Waiting for the installation to finish before continuing.",
                       {"steam", "steam://install/480"}, 2000, check_app480_installed);
}

// A warning dialog with custom buttons; returns the response id.
static int run_choice(Gtk::Window* parent, const Glib::ustring& text, const Glib::ustring& secondary,
                      const std::vector<std::pair<Glib::ustring, int>>& buttons) {
    std::unique_ptr<Gtk::MessageDialog> d;
    if (parent) d = std::make_unique<Gtk::MessageDialog>(*parent, text, false, Gtk::MESSAGE_WARNING, Gtk::BUTTONS_NONE);
    else        d = std::make_unique<Gtk::MessageDialog>(text, false, Gtk::MESSAGE_WARNING, Gtk::BUTTONS_NONE);
    d->set_secondary_text(secondary);
    for (const auto& [label, id] : buttons) d->add_button(label, id);
    return d->run();
}

// Steam's install folder wasn't auto-detected: let the user point at it.
// Returns false if they give up.
static bool ensure_steam_root(Gtk::Window* parent) {
    while (!steam_root_found()) {
        log_warning("Steam installation not found (looked in the usual places, last tried {})",
                    steam_root().string());
        int response = run_choice(
            parent, "Steam installation not found",
            "MonkeyLauncher couldn't find your Steam folder.\n"
            "Choose the folder that contains \"steamapps\" (usually ~/.local/share/Steam), "
            "or install Steam first.",
            {{"Cancel", Gtk::RESPONSE_CANCEL}, {"Choose folder…", Gtk::RESPONSE_ACCEPT}});
        if (response != Gtk::RESPONSE_ACCEPT) {
            log_warning("User did not provide a Steam folder — aborting startup");
            return false;
        }
        auto chosen = choose_folder(parent, "Select your Steam folder", home_dir());
        if (!chosen) continue;
        auto dir = normalize_steam_dir(*chosen);
        if (!dir) {
            show_error(parent, "That folder doesn't contain a \"steamapps\" directory, so it "
                               "doesn't look like a Steam install.");
            continue;
        }
        log_info("Using Steam folder chosen by the user: {}", dir->string());
        save_steam_root(*dir);
    }
    return true;
}

// App 480 wasn't found in any known Steam library: install it through Steam,
// or point at the library folder that already has it. Returns false if the
// user gives up.
static bool ensure_spacewar(Gtk::Window* parent) {
    while (!check_app480_installed()) {
        log_info("Steam App 480 (Spacewar) is not installed");
        int response = run_choice(
            parent, "Spacewar (App 480) was not found",
            "App 480 is required for the Proton prefix.\n"
            "Install it through Steam, or — if it is already installed in a Steam library "
            "MonkeyLauncher didn't detect — choose that library folder.",
            {{"Cancel", Gtk::RESPONSE_CANCEL},
             {"Choose folder…", Gtk::RESPONSE_ACCEPT},
             {"Install via Steam", Gtk::RESPONSE_YES}});
        if (response == Gtk::RESPONSE_YES) {
            if (!wait_for_spacewar(parent)) {
                log_warning("User cancelled while waiting for App 480 to install");
                return false;
            }
        } else if (response == Gtk::RESPONSE_ACCEPT) {
            auto chosen = choose_folder(parent, "Select the Steam library folder that contains Spacewar",
                                        steam_root());
            if (!chosen) continue;
            auto dir = normalize_steam_dir(*chosen);
            if (!dir || !library_has_spacewar(*dir)) {
                show_error(parent, "Spacewar (App 480) wasn't found in that folder.\n"
                                   "Pick the Steam library folder that contains "
                                   "steamapps/appmanifest_480.acf.");
                continue;
            }
            log_info("Using Steam library chosen by the user: {}", dir->string());
            save_steam_library(*dir);
        } else {
            log_warning("User declined to install App 480 — aborting startup");
            return false;
        }
    }
    return true;
}

// Returns the Proton dir to use for the initial prefix setup: the saved
// favorite if there is one, otherwise prompts the user to pick one (and
// saves that choice, same as the regular favorite-Proton setting).
static std::optional<fs::path> pick_bootstrap_proton(Gtk::Window* parent = nullptr) {
    Config cfg = read_config(config_file());
    const std::string saved = cfg.get("PROTONPATH");
    std::error_code ec;
    if (!saved.empty() && fs::is_directory(saved, ec)) return fs::path(saved);

    const auto proton_dirs = get_proton_dirs();
    if (proton_dirs.empty()) {
        show_error(parent, "No Proton installation found in your Steam libraries.\n"
                           "Install a Proton version via Steam first.");
        return std::nullopt;
    }

    Gtk::Dialog d("Select a Proton version", false);
    if (parent) d.set_transient_for(*parent);
    d.set_default_size(420, -1);
    d.add_button("Cancel", Gtk::RESPONSE_CANCEL);
    d.add_button("OK", Gtk::RESPONSE_OK);
    Gtk::Box* box = d.get_content_area();
    box->set_spacing(8);
    Gtk::Label label("MonkeyLauncher needs a Proton version to set up its shared prefix.");
    label.set_line_wrap(true);
    set_margin_all(label, 12);
    pack(*box, label);
    Gtk::ComboBoxText combo;
    combo.set_margin_start(12);
    combo.set_margin_end(12);
    combo.set_margin_bottom(12);
    for (const auto& pd : proton_dirs) combo.append(pd.filename().string());
    combo.set_active(0);
    pack(*box, combo);
    d.show_all();

    std::optional<fs::path> proton;
    if (d.run() == Gtk::RESPONSE_OK) {
        const int idx = combo.get_active_row_number();
        if (idx >= 0) {
            proton = proton_dirs[idx];
            cfg.set("PROTONPATH", proton->string());
            write_config(config_file(), cfg);
            log_info("Favorite Proton saved: {}", proton->filename().string());
        }
    }
    return proton;
}

static bool run_startup_checks(Gtk::Window* parent = nullptr) {
    log_debug("Running startup checks (Steam found? running? App 480 installed?)");
    if (!ensure_steam_root(parent)) return false;

    if (!check_steam_running()) {
        log_info("Steam is not running");
        int response = run_message(parent, Gtk::MESSAGE_QUESTION, Gtk::BUTTONS_YES_NO,
                                   "Steam is not running", "Launch Steam now and wait for it to start?");
        if (response != Gtk::RESPONSE_YES) {
            log_warning("User declined to launch Steam — aborting startup");
            return false;
        }
        if (!wait_for_steam(parent)) {
            log_warning("User cancelled while waiting for Steam to start");
            return false;
        }
    }

    if (!ensure_spacewar(parent)) return false;

    std::error_code ec;
    if (!fs::is_directory(wineprefix_path() / "pfx", ec)) {
        auto proton = pick_bootstrap_proton(parent);
        if (!proton) {
            log_warning("No Proton version selected — cannot set up the prefix");
            return false;
        }
        if (!bootstrap_proton_prefix(*proton)) {
            show_error(parent, "Could not set up the Proton prefix automatically.\n"
                               "Try a different Proton version, or launch Spacewar once from Steam.");
            return false;
        }
    }

    sync_steamclient_files();

    log_debug("Startup checks passed");
    return true;
}

// ── App entry point ──────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    log_init(argc, argv);

    auto app = Gtk::Application::create("com.monkeylauncher.app");
    app->signal_activate().connect([&app] {
        log_info("Starting MonkeyLauncher GUI");
        if (!run_startup_checks()) {
            log_info("Startup checks failed or were cancelled — exiting");
            app->quit();
            return;
        }
        auto* win = new MonkeyLauncher(app);
        app->add_window(*win);
        win->show_all();
        win->present();
    });
    app->signal_window_removed().connect([](Gtk::Window* w) {
        // Destroy the window once the application has let go of it (deferred:
        // this fires from inside the window's own close handling).
        run_on_main([w] { delete w; });
    });

    // Only argv[0]: flags like --debug/-v are ours (handled in log_init) and
    // GApplication would otherwise reject them as unknown options.
    return app->run(1, argv);
}
