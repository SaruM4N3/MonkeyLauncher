#include "main_window.hpp"

#include <signal.h>

#include <algorithm>
#include <thread>

#include "covers.hpp"
#include "dialogs.hpp"
#include "logging.hpp"
#include "steam.hpp"
#include "ui.hpp"
#include "version.hpp"

namespace ml {

std::pair<std::vector<std::string>, std::vector<std::string>>
parse_launch_tokens(const std::string& text, Env& env) {
    std::vector<std::string> prefix, suffix;
    if (text.empty()) return {prefix, suffix};
    const auto tokens = shlex_split(text);
    std::vector<std::string> before, after;
    auto it = std::find(tokens.begin(), tokens.end(), "%command%");
    if (it != tokens.end()) {
        before.assign(tokens.begin(), it);
        after.assign(it + 1, tokens.end());
    } else {
        after = tokens;
    }
    auto handle = [&](const std::vector<std::string>& toks, std::vector<std::string>& bucket) {
        for (const auto& tok : toks) {
            size_t eq = tok.find('=');
            if (eq != std::string::npos && !starts_with(tok, "-"))
                env[tok.substr(0, eq)] = tok.substr(eq + 1);
            else
                bucket.push_back(tok);
        }
    };
    handle(before, prefix);
    handle(after, suffix);
    return {prefix, suffix};
}

static std::string ustr(const Gtk::TreeModel::Row& r, const Gtk::TreeModelColumn<Glib::ustring>& c) {
    return r.get_value(c).raw();
}

// ── Main window ──────────────────────────────────────────────────────────────
MonkeyLauncher::MonkeyLauncher(const Glib::RefPtr<Gtk::Application>& app)
    : Gtk::ApplicationWindow(app) {
    set_title("MonkeyLauncher");
    set_default_size(700, 520);
    set_icon_name("applications-games");

    cfg_         = read_config(config_file());
    proton_dirs_ = get_proton_dirs();
    gamedirs_    = read_gamedirs();

    {
        std::string names;
        for (size_t i = 0; i < proton_dirs_.size(); ++i)
            names += (i ? ", '" : "'") + proton_dirs_[i].filename().string() + "'";
        log_debug("Detected {} Proton installation(s): [{}]", proton_dirs_.size(), names);
    }

    build_ui();
    if (cfg_.get("VIEW") == "grid") {
        // Gtk::Stack resets its visible child back to the first one added
        // as soon as the window's show_all() runs, undoing any
        // set_visible_child() made beforehand — so this has to be
        // deferred until after the window is shown.
        run_on_main([this] { restore_grid_view(); });
    }
    load_games();
    check_installed_deps();
    load_installer_redist();
    auto_check_updates();
}

// ── UI construction ──────────────────────────────────────────────────────────
// A flat, icon+label radio button used as a header-bar page switch — same
// idea as Gtk::StackSwitcher, but laid out ourselves so Library can sit on
// the left and Help/Settings on the right, with the update button centered
// between them.
Gtk::RadioButton* MonkeyLauncher::nav_button(const char* icon_name, const char* label,
                                             Gtk::RadioButton* group) {
    auto* btn = Gtk::manage(new Gtk::RadioButton);
    if (group) btn->join_group(*group);
    btn->set_mode(false);   // plain button look, no radio dot
    btn->get_style_context()->add_class("flat");
    auto* box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    pack(*box, *Gtk::manage(new Gtk::Image(icon_name, Gtk::ICON_SIZE_BUTTON)));
    pack(*box, *Gtk::manage(new Gtk::Label(label)));
    btn->add(*box);
    return btn;
}

void MonkeyLauncher::build_ui() {
    auto* hb = Gtk::manage(new Gtk::HeaderBar);
    hb->set_show_close_button(true);
    set_titlebar(*hb);

    stack_ = Gtk::manage(new Gtk::Stack);
    stack_->set_transition_type(Gtk::STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);

    library_nav_btn_  = nav_button("applications-games-symbolic", "Library", nullptr);
    help_nav_btn_     = nav_button("help-about-symbolic", "Help", library_nav_btn_);
    settings_nav_btn_ = nav_button("preferences-system-symbolic", "Settings", library_nav_btn_);
    library_nav_btn_->set_active(true);
    library_nav_btn_->signal_toggled().connect([this] {
        if (library_nav_btn_->get_active()) stack_->set_visible_child("library");
    });
    help_nav_btn_->signal_toggled().connect([this] {
        if (help_nav_btn_->get_active()) stack_->set_visible_child("help");
    });
    settings_nav_btn_->signal_toggled().connect([this] {
        if (settings_nav_btn_->get_active()) stack_->set_visible_child("settings");
    });

    hb->pack_start(*library_nav_btn_);
    hb->pack_end(*settings_nav_btn_);
    hb->pack_end(*help_nav_btn_);

    update_now_btn_ = Gtk::manage(new Gtk::Button);
    update_now_btn_->set_no_show_all(true);
    update_now_btn_->get_style_context()->add_class("suggested-action");
    auto update_css = Gtk::CssProvider::create();
    update_css->load_from_data("button { background: #2ec27e; color: #fff; }");
    update_now_btn_->get_style_context()->add_provider(update_css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    update_now_btn_->signal_clicked().connect([this] { on_update_now_clicked(); });
    auto* update_box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    pack(*update_box, *Gtk::manage(new Gtk::Image("software-update-available-symbolic",
                                                  Gtk::ICON_SIZE_BUTTON)));
    update_now_label_ = Gtk::manage(new Gtk::Label("Update now"));
    pack(*update_box, *update_now_label_);
    update_now_btn_->add(*update_box);
    hb->set_custom_title(*update_now_btn_);

    add(*stack_);

    build_library_page();
    build_help_page();
    build_settings_page();
}

void MonkeyLauncher::build_library_page() {
    auto* lib_box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));

    // Search bar + path toggle side by side
    auto* search_bar = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    search_bar->set_margin_top(8);
    search_bar->set_margin_bottom(8);
    search_bar->set_margin_start(8);
    search_bar->set_margin_end(8);
    search_entry_ = Gtk::manage(new Gtk::SearchEntry);
    search_entry_->set_hexpand(true);
    search_entry_->set_placeholder_text("Search games…");
    search_entry_->signal_search_changed().connect([this] { on_search_changed(); });
    pack(*search_bar, *search_entry_, true, true);

    path_toggle_ = Gtk::manage(new Gtk::ToggleButton("Show full path"));
    path_toggle_->set_active(false);
    path_toggle_->set_tooltip_text("Toggle between filename only and full path");
    path_toggle_->signal_toggled().connect([this] { on_path_toggle(); });
    pack(*search_bar, *path_toggle_);

    view_toggle_ = Gtk::manage(new Gtk::ToggleButton("Grid"));
    view_toggle_->set_tooltip_text("Toggle between list and grid view");
    view_toggle_->signal_toggled().connect([this] { on_view_toggle(); });
    pack(*search_bar, *view_toggle_);

    show_hidden_toggle_ = Gtk::manage(new Gtk::ToggleButton("Show hidden"));
    show_hidden_toggle_->set_active(false);
    show_hidden_toggle_->set_tooltip_text(
        "Show deactivated games ('Remove from list'-ed) so they can be reactivated");
    show_hidden_toggle_->signal_toggled().connect([this] { on_show_hidden_toggle(); });
    pack(*search_bar, *show_hidden_toggle_);

    pack(*lib_box, *search_bar);
    pack(*lib_box, *Gtk::manage(new Gtk::Separator));

    auto* scroll = Gtk::manage(new Gtk::ScrolledWindow);
    scroll->set_vexpand(true);
    scroll->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);

    store_  = Gtk::TreeStore::create(cols_);
    filter_ = Gtk::TreeModelFilter::create(store_);
    filter_->set_visible_func(sigc::mem_fun(*this, &MonkeyLauncher::game_filter));
    tv_ = Gtk::manage(new Gtk::TreeView(filter_));
    tv_->set_headers_visible(false);
    auto* game_col = Gtk::manage(new Gtk::TreeViewColumn("Game"));
    auto* icon_renderer = Gtk::manage(new Gtk::CellRendererPixbuf);
    game_col->pack_start(*icon_renderer, false);
    game_col->add_attribute(*icon_renderer, "pixbuf", cols_.icon);
    // Dims deactivated ('Remove from list'-ed) games instead of hiding them
    // outright, so they can be found again and reactivated.
    auto dim = [this](Gtk::CellRenderer* cell, const Gtk::TreeModel::iterator& it) {
        cell->property_sensitive() = !(*it).get_value(cols_.hidden);
    };
    game_col->set_cell_data_func(*icon_renderer, dim);
    auto* game_renderer = Gtk::manage(new Gtk::CellRendererText);
    game_col->pack_start(*game_renderer, true);
    game_col->add_attribute(*game_renderer, "text", cols_.display);
    game_col->set_cell_data_func(*game_renderer, dim);
    tv_->append_column(*game_col);
    tv_->signal_row_activated().connect(
        [this](const Gtk::TreeModel::Path& p, Gtk::TreeViewColumn* c) { on_game_activated(p, c); });
    tv_->signal_button_press_event().connect(
        [this](GdkEventButton* e) { return on_tree_button_press(e); }, false);
    tv_->get_selection()->signal_changed().connect([this] { on_selection_changed(); });
    scroll->add(*tv_);

    // ── Grid (cover-art) view ────────────────────────────────────────────────
    auto* grid_scroll = Gtk::manage(new Gtk::ScrolledWindow);
    grid_scroll->set_vexpand(true);
    grid_scroll->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);

    grid_store_  = Gtk::ListStore::create(grid_cols_);
    grid_filter_ = Gtk::TreeModelFilter::create(grid_store_);
    grid_filter_->set_visible_func(sigc::mem_fun(*this, &MonkeyLauncher::grid_filter_fn));
    icon_view_ = Gtk::manage(new Gtk::IconView(Glib::RefPtr<Gtk::TreeModel>(grid_filter_)));
    auto* pixbuf_renderer = Gtk::manage(new Gtk::CellRendererPixbuf);
    icon_view_->pack_start(*pixbuf_renderer, false);
    icon_view_->add_attribute(*pixbuf_renderer, "pixbuf", grid_cols_.cover);
    auto grid_dim = [this](Gtk::CellRenderer* cell) {
        return [this, cell](const Gtk::TreeModel::const_iterator& it) {
            cell->property_sensitive() = !(*it).get_value(grid_cols_.hidden);
        };
    };
    icon_view_->set_cell_data_func(*pixbuf_renderer, grid_dim(pixbuf_renderer));
    auto* text_renderer = Gtk::manage(new Gtk::CellRendererText);
    text_renderer->set_alignment(0.5f, 0.0f);
    // Fixed single-line width regardless of what's shown — short names
    // center within it, and long strings (e.g. "Show full path" in this
    // view) ellipsize in the middle instead of wrapping into a tall,
    // uneven cell that throws off the rest of the grid.
    text_renderer->property_ellipsize() = Pango::ELLIPSIZE_MIDDLE;
    text_renderer->property_width_chars() = 15;
    icon_view_->pack_start(*text_renderer, true);
    icon_view_->add_attribute(*text_renderer, "text", grid_cols_.display);
    icon_view_->set_cell_data_func(*text_renderer, grid_dim(text_renderer));
    icon_view_->set_item_width(130);
    icon_view_->signal_item_activated().connect([this](const Gtk::TreeModel::Path&) { on_launch(); });
    icon_view_->signal_selection_changed().connect([this] { on_selection_changed(); });
    icon_view_->signal_button_press_event().connect(
        [this](GdkEventButton* e) { return on_grid_button_press(e); }, false);
    grid_scroll->add(*icon_view_);

    view_stack_ = Gtk::manage(new Gtk::Stack);
    view_stack_->add(*scroll, "list");
    view_stack_->add(*grid_scroll, "grid");

    // Overlay the view stack with a centered spinner shown during scanning
    auto* overlay = Gtk::manage(new Gtk::Overlay);
    overlay->set_vexpand(true);
    overlay->add(*view_stack_);
    spinner_ = Gtk::manage(new Gtk::Spinner);
    spinner_->set_halign(Gtk::ALIGN_CENTER);
    spinner_->set_valign(Gtk::ALIGN_CENTER);
    spinner_->set_size_request(48, 48);
    overlay->add_overlay(*spinner_);
    pack(*lib_box, *overlay, true, true);

    auto* add_dir_btn = Gtk::manage(new Gtk::Button("+ Add game directory"));
    add_dir_btn->signal_clicked().connect([this] { on_add_game_dir(); });
    pack(*lib_box, *add_dir_btn);

    pack(*lib_box, *Gtk::manage(new Gtk::Separator));

    auto* bottom = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 8));
    set_margin_all(*bottom, 10);
    pack(*lib_box, *bottom);

    mangohud_btn_ = Gtk::manage(new Gtk::ToggleButton("MangoHud"));
    mangohud_btn_->signal_toggled().connect([this] { mangohud_ = mangohud_btn_->get_active(); });
    pack(*bottom, *mangohud_btn_);

    game_settings_btn_ = Gtk::manage(new Gtk::Button("Game Settings"));
    game_settings_btn_->set_sensitive(false);
    game_settings_btn_->signal_clicked().connect([this] { on_game_settings(); });
    pack(*bottom, *game_settings_btn_);

    pack(*bottom, *Gtk::manage(new Gtk::Box), true, true);   // spacer

    launch_btn_ = Gtk::manage(new Gtk::Button("Launch"));
    launch_btn_->get_style_context()->add_class("suggested-action");
    launch_btn_->set_sensitive(false);
    launch_btn_->signal_clicked().connect([this] { on_launch(); });
    pack(*bottom, *launch_btn_);

    stack_->add(*lib_box, "library", "Library");
}

void MonkeyLauncher::build_help_page() {
    auto help_css = Gtk::CssProvider::create();
    help_css->load_from_data(R"(
            .help-card {
                background-color: alpha(@theme_fg_color, 0.05);
                border-radius: 12px;
            }
        )");
    Gtk::StyleContext::add_provider_for_screen(
        Gdk::Screen::get_default(), help_css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    auto* help_scroll = Gtk::manage(new Gtk::ScrolledWindow);
    help_scroll->set_vexpand(true);
    help_scroll->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    auto* help_box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 20));
    set_margin_all(*help_box, 24);

    auto* page_title = make_label("", 0);
    page_title->set_markup(R"(<span size="x-large" weight="bold">Help</span>)");
    pack(*help_box, *page_title);
    auto* page_subtitle = make_label("Everything you need to know about using MonkeyLauncher.", 0);
    page_subtitle->get_style_context()->add_class("dim-label");
    pack(*help_box, *page_subtitle, false, false, 4);

    auto add_help_section = [help_box](std::string title, const std::vector<std::string>& bullets) {
        auto* card = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 10));
        set_margin_all(*card, 16);
        card->get_style_context()->add_class("help-card");

        std::transform(title.begin(), title.end(), title.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        auto* title_lbl = make_label("", 0);
        title_lbl->set_markup(R"(<span size="large" weight="bold">)" + title + ":</span>");
        pack(*card, *title_lbl);

        auto* list_box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 6));
        for (const auto& item : bullets) {
            auto* row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 8));
            auto* bullet_lbl = make_label("•", 0);
            bullet_lbl->set_valign(Gtk::ALIGN_START);
            pack(*row, *bullet_lbl);
            auto* item_lbl = make_label("", 0);
            item_lbl->set_line_wrap(true);
            item_lbl->set_valign(Gtk::ALIGN_START);
            item_lbl->set_markup(item);
            pack(*row, *item_lbl, true, true);
            pack(*list_box, *row);
        }
        pack(*card, *list_box);
        pack(*help_box, *card);
    };

    add_help_section("Library", {
        "Click <b>+ Add game directory</b> to point MonkeyLauncher at a folder "
        "containing your games — it scans recursively for executables.",
        "Double-click a game (or select it and click <b>Launch</b>) to start it "
        "through Proton.",
        "Use the search bar to filter games.",
        "<b>Show full path</b> shows the underlying exe paths instead of the "
        "display name.",
        "<b>Grid</b> switches to a cover-art grid view — the list/grid choice "
        "is remembered across restarts.",
        "<b>Show hidden</b> reveals games you've removed from the list.",
        "Right-click a game for <b>Game Settings</b> or to remove it from the "
        "list; right-click a directory to rescan or remove it from the library.",
        "<b>MangoHud</b> toggles the performance overlay for the next launch.",
    });

    add_help_section("Game Settings", {
        "Open via the <b>Game Settings</b> button or right-click menu on a game.",
        "<b>General</b> — rename the game and set custom cover art.",
        "<b>Launch</b> — choose <b>OnlineFix</b> (default) to run through the "
        "shared Proton prefix with the WINEDLLOVERRIDES below it applied: check "
        "a DLL to force it native/builtin, or add your own.",
        "Choose <b>Offline</b> to bypass DLL overrides and the selected "
        "Proton/shared prefix, running <tt>umu-run &lt;exe&gt;</tt> with no "
        "custom environment (same as running the exe bare from a terminal) — "
        "useful for games that don't need the online-fix tricks and run worse "
        "under the shared prefix.",
        "Launch Options (further down the same tab) apply in both modes.",
        "<b>Save Directory</b> — point at the save path inside the Proton "
        "prefix so MonkeyLauncher can symlink it to a stable location.",
    });

    add_help_section("Launch options", {
        "<tt>KEY=VALUE</tt> tokens (e.g. <tt>GAMEMODE=1 DRI_PRIME=1</tt>) are "
        "applied as environment variables for the launch.",
        "Anything else is treated as a command-line token, Steam-style: put "
        "<tt>%command%</tt> where the game's launch command should go, with "
        "wrapper programs before it, e.g. <tt>gamemoderun %command%</tt>.",
        "If you leave out <tt>%command%</tt>, plain tokens (e.g. "
        "<tt>-windowed -novid</tt>) are appended after the game instead.",
        "You can mix all three freely, e.g. "
        "<tt>PROTON_LOG=1 gamemoderun %command% -windowed</tt>.",
    });

    add_help_section("Settings tab", {
        "<b>Proton</b> — pick which installed Proton/UMU build is used to "
        "launch games.",
        "<b>Global launch options</b> (same syntax as a game's Launch Options) "
        "apply to every game; per-game options are layered on top and win on "
        "conflicting env vars.",
        "<b>Dependencies</b> — run bundled Windows installers/redistributables "
        "(e.g. DirectX, VC++) through Proton, and install common winetricks "
        "packages, before playing.",
        "<b>Advanced</b> — <b>Check for Updates</b> against the latest GitHub "
        "release (source installs can update in place from here; package "
        "installs are pointed at their package manager instead); jump to the "
        "config/logs folders; or reset all MonkeyLauncher config (game "
        "directories, Proton choice, per-game settings) — save files are kept.",
        "<b>Support</b> — star the project on GitHub, or report a bug.",
    });

    help_scroll->add(*help_box);
    stack_->add(*help_scroll, "help", "Help");
}

static void open_path(const std::string& target) {
    try {
        spawn_detached({"xdg-open", target});
    } catch (const std::exception& e) {
        log_warning("Could not open {}: {}", target, e.what());
    }
}

void MonkeyLauncher::build_settings_page() {
    // Steam-settings-style: sections left, content right
    auto* settings_row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL));

    settings_stack_ = Gtk::manage(new Gtk::Stack);
    settings_stack_->set_transition_type(Gtk::STACK_TRANSITION_TYPE_CROSSFADE);

    auto* settings_sidebar = Gtk::manage(new Gtk::StackSidebar);
    settings_sidebar->set_stack(*settings_stack_);
    pack(*settings_row, *settings_sidebar);
    pack(*settings_row, *Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_VERTICAL)));
    pack(*settings_row, *settings_stack_, true, true);

    // ── Proton ───────────────────────────────────────────────────────────────
    // Pinned to the top at its natural size — Gtk::Stack sizes every page
    // to match the tallest one (Installer/Dependencies have long lists),
    // and without this the version row stretches/centers into that
    // extra space instead of staying put.
    auto* proton_box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 8));
    set_margin_all(*proton_box, 16);
    proton_box->set_valign(Gtk::ALIGN_START);

    auto* version_row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 8));
    pack(*version_row, *Gtk::manage(new Gtk::Label("Version:")));
    proton_combo_ = Gtk::manage(new Gtk::ComboBoxText);
    proton_combo_->set_hexpand(true);
    populate_proton_combo();
    proton_combo_->signal_changed().connect([this] { on_proton_changed(); });
    pack(*version_row, *proton_combo_, true, true);
    pack(*proton_box, *version_row);

    auto* global_opts_row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 8));
    pack(*global_opts_row, *Gtk::manage(new Gtk::Label("Global launch options:")));
    global_launch_opts_entry_ = Gtk::manage(new Gtk::Entry);
    global_launch_opts_entry_->set_hexpand(true);
    global_launch_opts_entry_->set_placeholder_text("e.g. gamemoderun %command%");
    global_launch_opts_entry_->set_text(cfg_.get("GLOBAL_LAUNCH_ENV"));
    global_launch_opts_entry_->signal_activate().connect([this] { on_global_launch_opts_changed(); });
    global_launch_opts_entry_->signal_focus_out_event().connect(
        [this](GdkEventFocus*) { return on_global_launch_opts_changed(); }, false);
    pack(*global_opts_row, *global_launch_opts_entry_, true, true);
    pack(*proton_box, *global_opts_row);
    auto* note = make_label("", 0);
    note->set_line_wrap(true);
    note->set_markup("<small>Applied to every game, same syntax as a game's Launch "
                     "Options (see Help). Per-game options are applied on top and "
                     "win on conflicting env vars.</small>");
    pack(*proton_box, *note);

    settings_stack_->add(*proton_box, "proton", "Proton");

    // ── Dependencies (installers + winetricks — both install things into the
    // prefix before playing) ──────────────────────────────────────────────────
    auto* deps_outer = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 8));
    set_margin_all(*deps_outer, 16);

    pack(*deps_outer, *make_label("Installers", 0));
    auto* inst_top = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 8));
    installer_source_lbl_ = make_label("", 0);
    installer_source_lbl_->set_hexpand(true);
    installer_source_lbl_->set_line_wrap(true);
    auto* inst_browse_btn = Gtk::manage(new Gtk::Button("Browse directory…"));
    inst_browse_btn->signal_clicked().connect([this] { on_installer_browse(); });
    pack(*inst_top, *installer_source_lbl_, true, true);
    pack(*inst_top, *inst_browse_btn);
    pack(*deps_outer, *inst_top);

    auto* inst_scroll = Gtk::manage(new Gtk::ScrolledWindow);
    inst_scroll->set_vexpand(true);
    inst_scroll->set_min_content_height(120);
    inst_scroll->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    installer_list_ = Gtk::manage(new Gtk::ListBox);
    installer_list_->set_selection_mode(Gtk::SELECTION_NONE);
    inst_scroll->add(*installer_list_);
    pack(*deps_outer, *inst_scroll, true, true);

    installer_run_btn_ = Gtk::manage(new Gtk::Button("Run selected"));
    installer_run_btn_->get_style_context()->add_class("suggested-action");
    installer_run_btn_->set_sensitive(false);
    installer_run_btn_->signal_clicked().connect([this] { on_run_installer(); });
    pack(*deps_outer, *installer_run_btn_);

    pack(*deps_outer, *Gtk::manage(new Gtk::Separator), false, false, 8);
    pack(*deps_outer, *make_label("Winetricks packages", 0));

    auto* deps_scroll = Gtk::manage(new Gtk::ScrolledWindow);
    deps_scroll->set_vexpand(true);
    deps_scroll->set_min_content_height(120);
    deps_scroll->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    auto* deps_list = Gtk::manage(new Gtk::ListBox);
    deps_list->set_selection_mode(Gtk::SELECTION_NONE);
    for (const auto& [verb, desc] : WINETRICKS_PACKAGES) {
        auto* row  = Gtk::manage(new Gtk::ListBoxRow);
        auto* hbox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 8));
        set_margin_all(*hbox, 6);
        auto* cb = Gtk::manage(new Gtk::CheckButton);
        pack(*hbox, *cb);
        pack(*hbox, *make_label(verb + "  —  " + desc, 0), true, true);
        auto* spinner = Gtk::manage(new Gtk::Spinner);
        spinner->set_size_request(16, 16);
        spinner->set_no_show_all(true);
        auto* status_lbl = Gtk::manage(new Gtk::Label);
        status_lbl->set_no_show_all(true);
        hbox->pack_end(*status_lbl, false, false, 0);
        hbox->pack_end(*spinner, false, false, 0);
        row->add(*hbox);
        deps_list->add(*row);
        winetricks_rows_.push_back({verb, cb, spinner, status_lbl});
    }
    deps_scroll->add(*deps_list);
    pack(*deps_outer, *deps_scroll, true, true);

    install_deps_btn_ = Gtk::manage(new Gtk::Button("Install selected"));
    install_deps_btn_->get_style_context()->add_class("suggested-action");
    install_deps_btn_->signal_clicked().connect([this] { on_install_deps(); });
    pack(*deps_outer, *install_deps_btn_);

    settings_stack_->add(*deps_outer, "deps", "Dependencies");

    // ── Advanced ─────────────────────────────────────────────────────────────
    auto* advanced_box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 8));
    set_margin_all(*advanced_box, 16);

    auto* update_row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 8));
    version_lbl_ = make_label("Version " + current_version(), 0);
    version_lbl_->set_hexpand(true);
    pack(*update_row, *version_lbl_, true, true);
    update_btn_ = Gtk::manage(new Gtk::Button("Check for Updates"));
    update_btn_->signal_clicked().connect([this] { on_check_updates(); });
    pack(*update_row, *update_btn_);
    pack(*advanced_box, *update_row);

    pack(*advanced_box, *Gtk::manage(new Gtk::Separator));

    auto* folders_row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 8));
    auto* open_config_btn = Gtk::manage(new Gtk::Button("Open config folder"));
    open_config_btn->signal_clicked().connect([] { open_path(config_dir().string()); });
    pack(*folders_row, *open_config_btn);
    auto* open_logs_btn = Gtk::manage(new Gtk::Button("Open logs folder"));
    open_logs_btn->signal_clicked().connect([] { open_path(log_dir().string()); });
    pack(*folders_row, *open_logs_btn);
    pack(*advanced_box, *folders_row);

    pack(*advanced_box, *Gtk::manage(new Gtk::Separator));

    auto* reset_note = make_label(
        "Resetting clears all game directories, Proton preference, "
        "and per-game settings.\nSave files are NOT deleted.", 0);
    reset_note->set_line_wrap(true);
    pack(*advanced_box, *reset_note);
    auto* reset_btn = Gtk::manage(new Gtk::Button("Reset all config…"));
    reset_btn->get_style_context()->add_class("destructive-action");
    reset_btn->signal_clicked().connect([this] { on_reset(); });
    pack(*advanced_box, *reset_btn);
    settings_stack_->add(*advanced_box, "advanced", "Advanced");

    // ── Support ──────────────────────────────────────────────────────────────
    auto* support_box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 12));
    set_margin_all(*support_box, 16);
    support_box->set_valign(Gtk::ALIGN_START);

    auto* thanks_lbl = make_label("", 0);
    thanks_lbl->set_line_wrap(true);
    thanks_lbl->set_markup(
        "<b>Thanks for using MonkeyLauncher!</b>\n"
        "It's a hobby project, built and maintained in my spare time — if it's "
        "been useful to you, a star on GitHub genuinely helps and costs nothing.");
    pack(*support_box, *thanks_lbl);

    auto* star_btn = Gtk::manage(new Gtk::Button("⭐ Star on GitHub"));
    star_btn->get_style_context()->add_class("suggested-action");
    star_btn->signal_clicked().connect(
        [] { open_path(std::string("https://github.com/") + REPO); });
    pack(*support_box, *star_btn);

    pack(*support_box, *Gtk::manage(new Gtk::Separator), false, false, 8);

    auto* bug_lbl = make_label("Found a bug, or a game that won't launch right? Let me know.", 0);
    bug_lbl->set_line_wrap(true);
    pack(*support_box, *bug_lbl);

    auto* bug_btn = Gtk::manage(new Gtk::Button("Report a Bug"));
    bug_btn->signal_clicked().connect(
        [] { open_path(std::string("https://github.com/") + REPO + "/issues/new"); });
    pack(*support_box, *bug_btn);

    settings_stack_->add(*support_box, "support", "Support");

    stack_->add(*settings_row, "settings", "Settings");
}

std::string MonkeyLauncher::display_name(const std::string& exe, const Config& gcfg) const {
    if (show_fullpath_) return exe;
    const std::string name = gcfg.get("NAME");
    return name.empty() ? default_game_name(exe) : name;
}

void MonkeyLauncher::refresh_display_names() {
    for (auto& top : store_->children()) {
        for (auto& child : top.children()) {
            const std::string exe = ustr(child, cols_.exe);
            if (!exe.empty()) {
                Config gcfg = read_config(game_config_path(exe));
                child.set_value(cols_.display, Glib::ustring(display_name(exe, gcfg)));
            }
        }
    }
    for (auto& row : grid_store_->children()) {
        const std::string exe = ustr(row, grid_cols_.exe);
        Config gcfg = read_config(game_config_path(exe));
        row.set_value(grid_cols_.display, Glib::ustring(display_name(exe, gcfg)));
    }
}

void MonkeyLauncher::populate_proton_combo() {
    proton_combo_->remove_all();
    const std::string saved = cfg_.get("PROTONPATH");
    int active = 0;
    for (size_t i = 0; i < proton_dirs_.size(); ++i) {
        proton_combo_->append(proton_dirs_[i].filename().string());
        if (!saved.empty() && proton_dirs_[i].string() == saved) active = static_cast<int>(i);
    }
    proton_combo_->set_active(active);
}

bool MonkeyLauncher::game_filter(const Gtk::TreeModel::const_iterator& it) {
    const Glib::ustring query = search_entry_->get_text().lowercase();
    // Dir row: visible if at least one child is both active (or
    // show-hidden is on) and matches the search
    if (it->get_value(cols_.exe).empty()) {
        for (const auto& child : it->children()) {
            if ((show_hidden_ || !child.get_value(cols_.hidden)) &&
                (query.empty() || child.get_value(cols_.display).lowercase().find(query) != Glib::ustring::npos))
                return true;
        }
        return false;
    }
    if (it->get_value(cols_.hidden) && !show_hidden_) return false;
    return query.empty() || it->get_value(cols_.display).lowercase().find(query) != Glib::ustring::npos;
}

bool MonkeyLauncher::grid_filter_fn(const Gtk::TreeModel::const_iterator& it) {
    if (it->get_value(grid_cols_.hidden) && !show_hidden_) return false;
    const Glib::ustring query = search_entry_->get_text().lowercase();
    return query.empty() || it->get_value(grid_cols_.display).lowercase().find(query) != Glib::ustring::npos;
}

// ── Cover art ────────────────────────────────────────────────────────────────
// Covers are always center-cropped to a portrait aspect ratio before
// scaling, whether they came from a 2:3 library cover or a landscape
// header/hero fallback — otherwise items end up different heights and
// rows/columns stop lining up.
MonkeyLauncher::Pix MonkeyLauncher::placeholder_pixbuf(int size, double aspect) {
    const std::string key = format("ph:{}:{}", size, aspect);
    if (auto it = placeholder_cache_.find(key); it != placeholder_cache_.end()) return it->second;
    const int height = std::max(1, static_cast<int>(size / aspect));
    Pix canvas = Gdk::Pixbuf::create(Gdk::COLORSPACE_RGB, true, 8, size, height);
    canvas->fill(0x2b2b2bff);
    try {
        const int icon_size = std::max(8, static_cast<int>(std::min(size, height) * 0.6));
        Pix icon = Gtk::IconTheme::get_default()->load_icon(
            "applications-games", icon_size, Gtk::ICON_LOOKUP_FORCE_SIZE);
        const int x = (size - icon->get_width()) / 2;
        const int y = (height - icon->get_height()) / 2;
        icon->composite(canvas, x, y, icon->get_width(), icon->get_height(),
                        x, y, 1.0, 1.0, Gdk::INTERP_BILINEAR, 255);
    } catch (const Glib::Error&) {
    }
    placeholder_cache_[key] = canvas;
    return canvas;
}

// Loads a cover image cropped to a fixed aspect ratio and scaled to an
// exact size, so every item in a grid/list is pixel-identical regardless
// of the source image's own proportions.
MonkeyLauncher::Pix MonkeyLauncher::load_cover_pixbuf(const fs::path& path, int size, double aspect) {
    Pix pixbuf;
    try {
        pixbuf = Gdk::Pixbuf::create_from_file(path.string());
    } catch (const Glib::Error& e) {
        log_debug("Could not load cover image {}: {}", path.string(), e.what().raw());
        return {};
    }
    int w = pixbuf->get_width(), h = pixbuf->get_height();
    if (w <= 0 || h <= 0) return {};
    if (static_cast<double>(w) / h > aspect) {
        const int crop_w = std::max(1, static_cast<int>(h * aspect));
        pixbuf = Gdk::Pixbuf::create_subpixbuf(pixbuf, (w - crop_w) / 2, 0, crop_w, h);
        w = crop_w;
    } else {
        const int crop_h = std::max(1, static_cast<int>(w / aspect));
        pixbuf = Gdk::Pixbuf::create_subpixbuf(pixbuf, 0, (h - crop_h) / 2, w, crop_h);
        h = crop_h;
    }
    return pixbuf->scale_simple(size, std::max(1, static_cast<int>(size / aspect)), Gdk::INTERP_BILINEAR);
}

MonkeyLauncher::Pix MonkeyLauncher::load_grid_pixbuf(const fs::path& path, int size) {
    return load_cover_pixbuf(path, size);
}

// Small square-ish icon for the list view, left of the game name.
MonkeyLauncher::Pix MonkeyLauncher::list_icon_pixbuf_for(const std::string& exe, int size) {
    const fs::path cache_path = cover_cache_path(exe);
    std::error_code ec;
    if (fs::exists(cache_path, ec)) {
        Pix pixbuf = load_cover_pixbuf(cache_path, size, 1.0);
        if (pixbuf) return apply_mode_badge(pixbuf, exe);
    }
    return apply_mode_badge(placeholder_pixbuf(size, 1.0), exe);
}

MonkeyLauncher::Pix MonkeyLauncher::mode_badge_icon(bool offline, int size) {
    const std::string key = format("badge:{}:{}", offline, size);
    if (auto it = placeholder_cache_.find(key); it != placeholder_cache_.end()) return it->second;
    const char* icon_name = offline ? "network-offline-symbolic" : "network-transmit-receive-symbolic";
    Pix icon;
    try {
        icon = Gtk::IconTheme::get_default()->load_icon(icon_name, size, Gtk::ICON_LOOKUP_FORCE_SIZE);
    } catch (const Glib::Error&) {
    }
    placeholder_cache_[key] = icon;
    return icon;
}

// Stamps a small badge on the bottom-right corner of a cover/icon pixbuf
// showing whether the game launches through OnlineFix (the shared Proton
// prefix + WINEDLLOVERRIDES) or Offline (umu's own default Proton/prefix,
// no overrides) — see the Launch tab in Game Settings.
MonkeyLauncher::Pix MonkeyLauncher::apply_mode_badge(Pix pixbuf, const std::string& exe) {
    const bool offline = read_config(game_config_path(exe)).get("OFFLINE") == "1";
    const int size = std::max(10, static_cast<int>(std::min(pixbuf->get_width(), pixbuf->get_height()) * 0.28));
    Pix badge = mode_badge_icon(offline, size);
    if (!badge) return pixbuf;
    pixbuf = pixbuf->copy();
    if (!pixbuf->get_has_alpha()) pixbuf = pixbuf->add_alpha(false, 0, 0, 0);
    const int x = pixbuf->get_width() - badge->get_width() - 2;
    const int y = pixbuf->get_height() - badge->get_height() - 2;
    badge->composite(pixbuf, x, y, badge->get_width(), badge->get_height(),
                     x, y, 1.0, 1.0, Gdk::INTERP_BILINEAR, 255);
    return pixbuf;
}

MonkeyLauncher::Pix MonkeyLauncher::folder_icon_pixbuf(int size) {
    if (!placeholder_cache_.count("folder")) {
        Pix pixbuf;
        try {
            pixbuf = Gtk::IconTheme::get_default()->load_icon("folder", size, Gtk::ICON_LOOKUP_FORCE_SIZE);
        } catch (const Glib::Error&) {
            pixbuf = placeholder_pixbuf(size, 1.0);
        }
        placeholder_cache_["folder"] = pixbuf;
    }
    return placeholder_cache_["folder"];
}

// Loads an already-cached cover for a fresh grid row, if there is one —
// otherwise the placeholder, pending a fetch. Without this, every reload
// (startup, add/remove dir, rescan) would show placeholders even for games
// whose cover was already downloaded in a previous session, since
// start_cover_fetch skips cache hits.
MonkeyLauncher::Pix MonkeyLauncher::grid_pixbuf_for(const std::string& exe) {
    const fs::path cache_path = cover_cache_path(exe);
    std::error_code ec;
    if (fs::exists(cache_path, ec)) {
        Pix pixbuf = load_grid_pixbuf(cache_path);
        if (pixbuf) return apply_mode_badge(pixbuf, exe);
    }
    return apply_mode_badge(placeholder_pixbuf(), exe);
}

void MonkeyLauncher::start_cover_fetch() {
    if (cover_fetch_running_) return;
    std::vector<std::pair<std::string, std::string>> pending;
    std::error_code ec;
    for (auto& row : grid_store_->children()) {
        const std::string exe = ustr(row, grid_cols_.exe);
        if (!fs::exists(cover_cache_path(exe), ec)) pending.emplace_back(exe, ustr(row, grid_cols_.gamedir));
    }
    if (pending.empty()) return;
    cover_fetch_running_ = true;
    log_debug("Fetching {} missing cover(s) from the Steam store", pending.size());

    std::thread([this, pending] {
        for (const auto& [exe, gamedir] : pending) {
            if (auto path = fetch_cover(exe))
                run_on_main([this, exe, gamedir, p = *path] { apply_cover(exe, gamedir, p); });
        }
        cover_fetch_running_ = false;
        log_debug("Cover fetch pass finished");
    }).detach();
}

void MonkeyLauncher::apply_cover(const std::string& exe, const std::string& gamedir, const fs::path& path) {
    Pix pixbuf = load_grid_pixbuf(path);
    if (pixbuf) {
        pixbuf = apply_mode_badge(pixbuf, exe);
        for (auto& row : grid_store_->children()) {
            if (ustr(row, grid_cols_.exe) == exe && ustr(row, grid_cols_.gamedir) == gamedir) {
                row.set_value(grid_cols_.cover, pixbuf);
                break;
            }
        }
    }

    Pix list_icon = list_icon_pixbuf_for(exe);
    for (auto& top : store_->children())
        for (auto& child : top.children())
            if (ustr(child, cols_.exe) == exe && ustr(child, cols_.gamedir) == gamedir)
                child.set_value(cols_.icon, list_icon);
}

// ── Data loading ─────────────────────────────────────────────────────────────
void MonkeyLauncher::load_games() {
    store_->clear();
    launch_btn_->set_sensitive(false);
    game_settings_btn_->set_sensitive(false);
    spinner_->start();
    {
        std::string dirs;
        for (size_t i = 0; i < gamedirs_.size(); ++i) dirs += (i ? ", '" : "'") + gamedirs_[i] + "'";
        log_debug("Scanning {} game director(y/ies): [{}]", gamedirs_.size(), dirs);
    }

    std::thread([this, gamedirs = gamedirs_] {
        std::vector<std::pair<std::string, std::vector<GameEntry>>> results;
        for (const auto& gamedir : gamedirs) {
            std::error_code ec;
            if (!fs::is_directory(gamedir, ec)) {
                log_warning("Game directory no longer exists, skipping: {}", gamedir);
                continue;
            }
            std::vector<GameEntry> exes;
            for (const auto& exe : get_exe_list(gamedir))
                exes.push_back({exe, read_config(game_config_path(exe)), gamedir});
            if (!exes.empty()) results.emplace_back(gamedir, std::move(exes));
        }
        run_on_main([this, results = std::move(results)] { apply_games(results); });
    }).detach();
}

void MonkeyLauncher::apply_games(
    const std::vector<std::pair<std::string, std::vector<GameEntry>>>& results) {
    size_t total = 0;
    for (const auto& r : results) total += r.second.size();
    log_info("Found {} game(s) across {} director(y/ies)", total, results.size());
    grid_store_->clear();
    for (const auto& [gamedir, exes] : results) {
        auto parent = store_->append();
        parent->set_value(cols_.display, Glib::ustring(fs::path(gamedir).filename().string()));
        parent->set_value(cols_.exe, Glib::ustring(""));
        parent->set_value(cols_.gamedir, Glib::ustring(gamedir));
        parent->set_value(cols_.hidden, false);
        parent->set_value(cols_.icon, folder_icon_pixbuf());
        for (const auto& g : exes) {
            const std::string display = display_name(g.exe, g.cfg);
            const bool hidden = g.cfg.get("HIDDEN") == "1";
            auto child = store_->append(parent->children());
            child->set_value(cols_.display, Glib::ustring(display));
            child->set_value(cols_.exe, Glib::ustring(g.exe));
            child->set_value(cols_.gamedir, Glib::ustring(g.gamedir));
            child->set_value(cols_.hidden, hidden);
            child->set_value(cols_.icon, list_icon_pixbuf_for(g.exe));

            auto row = grid_store_->append();
            row->set_value(grid_cols_.cover, grid_pixbuf_for(g.exe));
            row->set_value(grid_cols_.display, Glib::ustring(display));
            row->set_value(grid_cols_.exe, Glib::ustring(g.exe));
            row->set_value(grid_cols_.gamedir, Glib::ustring(g.gamedir));
            row->set_value(grid_cols_.hidden, hidden);
        }
    }
    tv_->expand_all();
    spinner_->stop();
    start_cover_fetch();
}

// (exe_relpath, gamedir) for the selected game row, or ("", "") if none.
std::pair<std::string, std::string> MonkeyLauncher::selected_game() {
    if (view_stack_->get_visible_child_name() == "grid") {
        auto items = icon_view_->get_selected_items();
        if (items.empty()) return {};
        auto it = grid_filter_->get_iter(items[0]);
        return {ustr(*it, grid_cols_.exe), ustr(*it, grid_cols_.gamedir)};
    }
    auto it = tv_->get_selection()->get_selected();
    if (it && !it->get_value(cols_.exe).empty())
        return {ustr(*it, cols_.exe), ustr(*it, cols_.gamedir)};
    return {};
}

std::optional<fs::path> MonkeyLauncher::selected_proton() {
    const int idx = proton_combo_->get_active_row_number();
    if (idx >= 0 && idx < static_cast<int>(proton_dirs_.size())) return proton_dirs_[idx];
    return std::nullopt;
}

// ── Signals ──────────────────────────────────────────────────────────────────
void MonkeyLauncher::on_search_changed() {
    filter_->refilter();
    tv_->expand_all();
    grid_filter_->refilter();
}

void MonkeyLauncher::restore_grid_view() { view_toggle_->set_active(true); }

void MonkeyLauncher::on_view_toggle() {
    if (view_toggle_->get_active()) {
        view_stack_->set_visible_child("grid");
        view_toggle_->set_label("List");
        start_cover_fetch();
    } else {
        view_stack_->set_visible_child("list");
        view_toggle_->set_label("Grid");
    }
    on_selection_changed();
    cfg_.set("VIEW", view_toggle_->get_active() ? "grid" : "list");
    write_config(config_file(), cfg_);
}

void MonkeyLauncher::on_show_hidden_toggle() {
    show_hidden_ = show_hidden_toggle_->get_active();
    filter_->refilter();
    tv_->expand_all();
    grid_filter_->refilter();
}

void MonkeyLauncher::popup_at(std::unique_ptr<Gtk::Menu> menu, GdkEventButton* event) {
    menu_ = std::move(menu);
    menu_->show_all();
    menu_->popup_at_pointer(reinterpret_cast<GdkEvent*>(event));
}

static Gtk::MenuItem* menu_item(Gtk::Menu& menu, const Glib::ustring& label, std::function<void()> fn) {
    auto* item = Gtk::manage(new Gtk::MenuItem(label));
    item->signal_activate().connect(std::move(fn));
    menu.append(*item);
    return item;
}

bool MonkeyLauncher::on_grid_button_press(GdkEventButton* event) {
    if (event->button != 3) return false;
    Gtk::TreeModel::Path path;
    Gtk::CellRenderer* cell = nullptr;
    if (!icon_view_->get_item_at_pos(static_cast<int>(event->x), static_cast<int>(event->y), path, cell))
        return false;
    icon_view_->select_path(path);
    auto it = grid_filter_->get_iter(path);
    const std::string exe = ustr(*it, grid_cols_.exe);
    const bool is_hidden = (*it).get_value(grid_cols_.hidden);

    auto menu = std::make_unique<Gtk::Menu>();
    menu_item(*menu, "Game Settings", [this] { on_game_settings(); });
    if (is_hidden) menu_item(*menu, "Activate", [this, exe] { on_activate_game(exe); });
    else           menu_item(*menu, "Remove from list", [this, exe] { on_hide_game(exe); });
    popup_at(std::move(menu), event);
    return false;
}

void MonkeyLauncher::on_selection_changed() {
    const std::string exe = selected_game().first;
    game_settings_btn_->set_sensitive(!exe.empty());
    if (running_pid_ == 0) launch_btn_->set_sensitive(!exe.empty());
}

void MonkeyLauncher::on_path_toggle() {
    show_fullpath_ = path_toggle_->get_active();
    path_toggle_->set_label(show_fullpath_ ? "Show name only" : "Show full path");
    refresh_display_names();
}

void MonkeyLauncher::on_game_activated(const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn*) {
    auto it = tv_->get_selection()->get_selected();
    if (it && it->get_value(cols_.exe).empty()) {   // dir row — toggle expand
        if (tv_->row_expanded(path)) tv_->collapse_row(path);
        else                         tv_->expand_row(path, false);
    } else {
        on_launch();
    }
}

bool MonkeyLauncher::on_tree_button_press(GdkEventButton* event) {
    if (event->button != 3) return false;
    Gtk::TreeModel::Path path;
    Gtk::TreeViewColumn* col = nullptr;
    int cx, cy;
    if (!tv_->get_path_at_pos(static_cast<int>(event->x), static_cast<int>(event->y), path, col, cx, cy))
        return false;
    tv_->get_selection()->select(path);
    auto it = tv_->get_selection()->get_selected();
    if (!it) return false;

    auto menu = std::make_unique<Gtk::Menu>();
    if (it->get_value(cols_.exe).empty()) {   // dir row
        const std::string gamedir = ustr(*it, cols_.gamedir);
        menu_item(*menu, "Rescan directory", [this, gamedir] { on_rescan_game_dir(gamedir); });
        menu_item(*menu, "Remove '" + fs::path(gamedir).filename().string() + "' from library",
                  [this, gamedir] { on_remove_game_dir(gamedir); });
    } else {                                  // game row
        const std::string exe = ustr(*it, cols_.exe);
        const bool is_hidden = it->get_value(cols_.hidden);
        menu_item(*menu, "Game Settings", [this] { on_game_settings(); });
        if (is_hidden) menu_item(*menu, "Activate", [this, exe] { on_activate_game(exe); });
        else           menu_item(*menu, "Remove from list", [this, exe] { on_hide_game(exe); });
    }
    popup_at(std::move(menu), event);
    return false;
}

void MonkeyLauncher::on_hide_game(const std::string& exe) {
    Config gcfg = read_config(game_config_path(exe));
    gcfg.set("HIDDEN", "1");
    write_config(game_config_path(exe), gcfg);
    load_games();
}

void MonkeyLauncher::on_activate_game(const std::string& exe) {
    Config gcfg = read_config(game_config_path(exe));
    gcfg.erase("HIDDEN");
    write_config(game_config_path(exe), gcfg);
    load_games();
}

void MonkeyLauncher::on_launch() {
    if (running_pid_ != 0) {
        log_info("Stopping game (pid={})", running_pid_);
        kill(running_pid_, SIGTERM);
        return;
    }

    const auto [exe, gamedir] = selected_game();
    if (exe.empty()) {
        log_warning("Launch attempted without a selected game");
        show_error(this, "Select a game first.");
        return;
    }

    const std::string game_path  = (fs::path(gamedir) / exe).string();
    const Config game_cfg        = read_config(game_config_path(exe));
    const std::string savedir    = game_cfg.get("SAVEDIR");
    const std::string launch_env = game_cfg.get("LAUNCH_ENV");
    const bool offline           = game_cfg.get("OFFLINE") == "1";

    std::error_code ec;
    if (!savedir.empty() && fs::is_directory(fs::path(savedir).parent_path(), ec)) {
        try {
            setup_save_symlink(savedir, game_save_path(exe));
        } catch (const std::exception& e) {
            log_warning("Save symlink warning for {}: {}", exe, e.what());
        }
    }

    Env env = current_env();
    std::vector<std::string> cmd;
    std::optional<fs::path> proton;
    try {
        if (offline) {
            // Offline game: no forced WINE/WINEPREFIX/GAMEID/PROTONPATH/
            // WINEDLLOVERRIDES — umu manages its own default Proton build,
            // prefix, and runtime, same as running it bare from a terminal.
            // Launch options (global and per-game) still apply; only the
            // saved WINEDLLOVERRIDES token (OnlineFix-specific) is dropped.
            std::vector<std::string> kept;
            for (const auto& tok : split_whitespace(launch_env))
                if (!starts_with(tok, "WINEDLLOVERRIDES=")) kept.push_back(tok);
            auto [g_prefix, g_suffix] = parse_launch_tokens(cfg_.get("GLOBAL_LAUNCH_ENV"), env);
            auto [p_prefix, p_suffix] = parse_launch_tokens(join(kept, " "), env);
            cmd = g_prefix;
            cmd.insert(cmd.end(), p_prefix.begin(), p_prefix.end());
            cmd.push_back("umu-run");
            cmd.push_back(game_path);
            cmd.insert(cmd.end(), p_suffix.begin(), p_suffix.end());
            cmd.insert(cmd.end(), g_suffix.begin(), g_suffix.end());
        } else {
            proton = selected_proton();
            if (!proton) {
                log_warning("Launch attempted without a selected Proton version");
                show_error(this, "Select a Proton version first.");
                return;
            }
            env["WINE"]             = (*proton / "files" / "bin" / "wine64").string();
            env["WINESERVER"]       = (*proton / "files" / "bin" / "wineserver").string();
            env["WINEPREFIX"]       = wineprefix_path().string() + "/";
            env["WINEDLLOVERRIDES"] = "OnlineFix64=n;SteamOverlay64=n;winmm=n,b;dnet=n;steam_api64=n";
            env["GAMEID"]           = "480";
            env["PROTONPATH"]       = proton->string();
            env["DXVK_STATE_CACHE"] = "1";
            env["MANGOHUD"]         = mangohud_ ? "1" : "0";
            // Global overrides applied first, per-game overrides applied after so
            // they win on conflicting env vars. KEY=VALUE tokens become env vars;
            // everything else is a command-line token, Steam-%command%-style:
            // tokens before "%command%" wrap/prefix the launch command (e.g.
            // "gamemoderun %command%"), tokens after it (or all of them, if
            // %command% is omitted) are appended as extra args to the game.
            // Global wrapping goes outermost, per-game innermost.
            auto [g_prefix, g_suffix] = parse_launch_tokens(cfg_.get("GLOBAL_LAUNCH_ENV"), env);
            auto [p_prefix, p_suffix] = parse_launch_tokens(launch_env, env);
            cmd = g_prefix;
            cmd.insert(cmd.end(), p_prefix.begin(), p_prefix.end());
            cmd.push_back("umu-run");
            cmd.push_back(game_path);
            cmd.insert(cmd.end(), p_suffix.begin(), p_suffix.end());
            cmd.insert(cmd.end(), g_suffix.begin(), g_suffix.end());
        }
    } catch (const std::exception& e) {
        log_error("Invalid launch options: {}", e.what());
        show_error(this, std::string("Invalid launch options: ") + e.what());
        return;
    }

    // Run with cwd set to the game's own folder — some games load assets
    // via relative paths and misbehave (or outright crash) if launched
    // from an unrelated working directory, which Steam/Windows normally
    // avoid by always starting a game "in" its own install folder.
    const fs::path game_cwd = fs::path(game_path).parent_path();

    log_info("Launching {}{}", exe,
             offline ? " (offline, umu default Proton)" : " via " + proton->filename().string());
    log_debug("Launch command: {} (cwd={})", join(cmd, " "), game_cwd.string());
    log_debug("Offline: {}, per-game overrides: {}, savedir: {}, mangohud: {}",
              offline ? "True" : "False", launch_env.empty() ? "(none)" : launch_env,
              savedir.empty() ? "(none)" : savedir, mangohud_ ? "True" : "False");

    pid_t pid;
    try {
        pid = spawn(cmd, &env, game_cwd);
    } catch (const std::exception& e) {
        log_error("Could not start the game: {}", e.what());
        show_error(this, std::string("Could not start the game:\n") + e.what());
        return;
    }
    running_pid_ = pid;

    launch_btn_->set_label("Stop");
    launch_btn_->get_style_context()->remove_class("suggested-action");
    launch_btn_->get_style_context()->add_class("destructive-action");
    launch_btn_->set_sensitive(true);

    Glib::signal_child_watch().connect(
        sigc::mem_fun(*this, &MonkeyLauncher::on_game_exit), pid);
}

void MonkeyLauncher::on_game_exit(GPid pid, int status) {
    log_info("Game process exited (pid={}, status={})", pid, status);
    running_pid_ = 0;
    launch_btn_->set_label("Launch");
    launch_btn_->get_style_context()->remove_class("destructive-action");
    launch_btn_->get_style_context()->add_class("suggested-action");
    launch_btn_->set_sensitive(!selected_game().first.empty());
}

void MonkeyLauncher::on_game_settings() {
    const auto [exe, gamedir] = selected_game();
    if (exe.empty()) return;
    GameSettingsDialog dialog(*this, exe);
    if (dialog.run() == Gtk::RESPONSE_OK) {
        Config result = dialog.get_result();
        if (read_config(game_config_path(exe)).get("HIDDEN") == "1") result.set("HIDDEN", "1");
        write_config(game_config_path(exe), result);
        refresh_game_row(exe, gamedir);
    }
}

// Re-reads a single game's config/cover after Game Settings closes, so a
// manually set name or cover shows up immediately.
void MonkeyLauncher::refresh_game_row(const std::string& exe, const std::string& gamedir) {
    Config gcfg = read_config(game_config_path(exe));
    const std::string display = display_name(exe, gcfg);

    Pix list_icon = list_icon_pixbuf_for(exe);
    for (auto& top : store_->children()) {
        for (auto& child : top.children()) {
            if (ustr(child, cols_.exe) == exe) {
                child.set_value(cols_.display, Glib::ustring(display));
                child.set_value(cols_.icon, list_icon);
            }
        }
    }

    Pix pixbuf = grid_pixbuf_for(exe);
    for (auto& row : grid_store_->children()) {
        if (ustr(row, grid_cols_.exe) == exe && ustr(row, grid_cols_.gamedir) == gamedir) {
            row.set_value(grid_cols_.cover, pixbuf);
            row.set_value(grid_cols_.display, Glib::ustring(display));
            break;
        }
    }

    start_cover_fetch();
}

void MonkeyLauncher::on_add_game_dir() {
    Gtk::FileChooserDialog dialog(*this, "Select game directory", Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
    dialog.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
    dialog.add_button("Select", Gtk::RESPONSE_OK);
    dialog.set_current_folder(gamedirs_.empty() ? home_dir().string() : gamedirs_.back());
    if (dialog.run() == Gtk::RESPONSE_OK) {
        const std::string chosen = dialog.get_filename();
        if (std::find(gamedirs_.begin(), gamedirs_.end(), chosen) == gamedirs_.end()) {
            gamedirs_.push_back(chosen);
            write_gamedirs(gamedirs_);
            load_games();
            load_installer_redist();
        }
    }
}

void MonkeyLauncher::on_remove_game_dir(const std::string& gamedir) {
    gamedirs_.erase(std::remove(gamedirs_.begin(), gamedirs_.end(), gamedir), gamedirs_.end());
    write_gamedirs(gamedirs_);
    load_games();
    load_installer_redist();
}

void MonkeyLauncher::on_rescan_game_dir(const std::string& gamedir) {
    spinner_->start();
    std::thread([this, gamedir] {
        std::vector<std::pair<std::string, Config>> exes;
        std::error_code ec;
        if (fs::is_directory(gamedir, ec))
            for (const auto& exe : get_exe_list(gamedir))
                exes.emplace_back(exe, read_config(game_config_path(exe)));
        run_on_main([this, gamedir, exes = std::move(exes)] { apply_rescan(gamedir, exes); });
    }).detach();
}

void MonkeyLauncher::apply_rescan(const std::string& gamedir,
                                  const std::vector<std::pair<std::string, Config>>& exes) {
    for (auto it = store_->children().begin(); it != store_->children().end(); ++it) {
        if (ustr(*it, cols_.gamedir) != gamedir) continue;
        while (!it->children().empty()) store_->erase(it->children().begin());
        for (const auto& [exe, gcfg] : exes) {
            auto child = store_->append(it->children());
            child->set_value(cols_.display, Glib::ustring(display_name(exe, gcfg)));
            child->set_value(cols_.exe, Glib::ustring(exe));
            child->set_value(cols_.gamedir, Glib::ustring(gamedir));
            child->set_value(cols_.hidden, gcfg.get("HIDDEN") == "1");
            child->set_value(cols_.icon, list_icon_pixbuf_for(exe));
        }
        Gtk::TreeModel::Path filtered = filter_->convert_child_path_to_path(store_->get_path(it));
        if (!filtered.empty()) tv_->expand_row(filtered, false);
        break;
    }

    for (auto it = grid_store_->children().begin(); it != grid_store_->children().end();) {
        if (ustr(*it, grid_cols_.gamedir) == gamedir) it = grid_store_->erase(it);
        else ++it;
    }
    for (const auto& [exe, gcfg] : exes) {
        auto row = grid_store_->append();
        row->set_value(grid_cols_.cover, grid_pixbuf_for(exe));
        row->set_value(grid_cols_.display, Glib::ustring(display_name(exe, gcfg)));
        row->set_value(grid_cols_.exe, Glib::ustring(exe));
        row->set_value(grid_cols_.gamedir, Glib::ustring(gamedir));
        row->set_value(grid_cols_.hidden, gcfg.get("HIDDEN") == "1");
    }

    spinner_->stop();
    start_cover_fetch();
}

void MonkeyLauncher::on_proton_changed() {
    const int idx = proton_combo_->get_active_row_number();
    if (idx >= 0 && idx < static_cast<int>(proton_dirs_.size())) {
        cfg_.set("PROTONPATH", proton_dirs_[idx].string());
        write_config(config_file(), cfg_);
    }
    check_installed_deps();
}

bool MonkeyLauncher::on_global_launch_opts_changed() {
    cfg_.set("GLOBAL_LAUNCH_ENV", strip(global_launch_opts_entry_->get_text().raw()));
    write_config(config_file(), cfg_);
    return false;
}

void MonkeyLauncher::check_installed_deps() {
    for (auto& row : winetricks_rows_) {
        row.spinner->stop();
        row.spinner->hide();
        row.label->hide();
    }
    install_deps_btn_->set_sensitive(false);
    install_deps_btn_->set_label("Checking installed…");

    std::thread([this] {
        std::set<std::string> installed;
        try {
            RunResult r = run_capture({"protontricks", "--no-bwrap", "480", "list-installed"}, 30);
            if (!r.timed_out)
                for (const auto& line : split_lines(r.out))
                    if (!strip(line).empty()) installed.insert(strip(line));
        } catch (const std::exception&) {
            installed.clear();
        }
        run_on_main([this, installed = std::move(installed)] { apply_installed_deps(installed); });
    }).detach();
}

void MonkeyLauncher::apply_installed_deps(const std::set<std::string>& installed) {
    for (auto& row : winetricks_rows_)
        if (installed.count(row.key)) set_status(row, "ok");
    if (install_deps_btn_->get_label() == "Checking installed…") {
        install_deps_btn_->set_label("Install selected");
        install_deps_btn_->set_sensitive(true);
    }
}

void MonkeyLauncher::on_install_deps() {
    if (!selected_proton()) {
        show_error(this, "No Proton version selected.");
        return;
    }
    std::vector<std::string> verbs;
    for (const auto& row : winetricks_rows_)
        if (row.cb->get_active()) verbs.push_back(row.key);
    if (verbs.empty()) {
        show_error(this, "Select at least one package to install.");
        return;
    }
    install_deps_btn_->set_sensitive(false);
    install_deps_btn_->set_label("Please wait…");
    run_winetricks(verbs);
}

void MonkeyLauncher::run_winetricks(const std::vector<std::string>& verbs) {
    log_info("Installing winetricks packages: {}", join(verbs, ", "));

    std::thread([this, verbs] {
        for (const auto& verb : verbs) {
            run_on_main([this, verb] {
                if (auto* r = find_row(winetricks_rows_, verb)) set_status(*r, "running");
            });
            log_debug("protontricks --no-bwrap 480 {}", verb);
            int code;
            try {
                code = run({"protontricks", "--no-bwrap", "480", verb});
            } catch (const std::exception& e) {
                log_error("Could not run protontricks: {}", e.what());
                code = -1;
            }
            const bool ok = code == 0;
            if (ok) log_info("Installed {}", verb);
            else    log_error("Failed to install {} (exit {})", verb, code);
            run_on_main([this, verb, ok] {
                if (auto* r = find_row(winetricks_rows_, verb)) {
                    set_status(*r, ok ? "ok" : "error");
                    if (ok) r->cb->set_active(false);
                }
            });
        }
        run_on_main([this] {
            install_deps_btn_->set_label("Install selected");
            install_deps_btn_->set_sensitive(true);
        });
    }).detach();
}

MonkeyLauncher::StatusRow* MonkeyLauncher::find_row(std::vector<StatusRow>& rows, const std::string& key) {
    for (auto& r : rows)
        if (r.key == key) return &r;
    return nullptr;
}

void MonkeyLauncher::set_status(StatusRow& row, const std::string& status) {
    if (status == "running") {
        row.label->hide();
        row.spinner->show();
        row.spinner->start();
    } else if (status == "ok") {
        row.spinner->stop();
        row.spinner->hide();
        row.label->set_markup(R"(<span color="#57a773" size="x-large">✓</span>)");
        row.label->show();
    } else if (status == "error") {
        row.spinner->stop();
        row.spinner->hide();
        row.label->set_markup(R"(<span color="#e05c5c" size="x-large">✗</span>)");
        row.label->show();
    }
}

void MonkeyLauncher::populate_installer(const std::vector<std::pair<std::string, std::string>>& roots) {
    // Rows own the widgets, so drop our pointers to them first.
    installer_rows_.clear();
    for (auto* child : installer_list_->get_children()) installer_list_->remove(*child);

    for (const auto& [label, base_str] : roots) {
        const fs::path base(base_str);
        std::map<std::string, std::vector<fs::path>> groups;
        for (const auto& rel : rglob_exe(base)) {
            std::string folder = rel.parent_path().string();
            if (folder.empty()) folder = ".";
            groups[folder].push_back(base / rel);
        }
        if (groups.empty()) continue;

        auto* hdr = Gtk::manage(new Gtk::ListBoxRow);
        hdr->set_selectable(false);
        hdr->set_activatable(false);
        auto* hdr_lbl = make_label(label, 0);
        hdr_lbl->set_margin_start(6);
        hdr_lbl->set_margin_top(4);
        hdr_lbl->set_margin_bottom(2);
        hdr_lbl->get_style_context()->add_class("dim-label");
        hdr->add(*hdr_lbl);
        installer_list_->add(*hdr);

        if (auto root_it = groups.find("."); root_it != groups.end()) {
            for (const auto& exe : root_it->second) add_installer_row(exe);
            groups.erase(root_it);
        }
        for (const auto& [folder, exes] : groups) {
            auto* sfhdr = Gtk::manage(new Gtk::ListBoxRow);
            sfhdr->set_selectable(false);
            sfhdr->set_activatable(false);
            auto* lbl = make_label(folder, 0);
            lbl->set_margin_start(14);
            lbl->set_margin_top(2);
            lbl->set_margin_bottom(1);
            sfhdr->add(*lbl);
            installer_list_->add(*sfhdr);
            for (const auto& exe : exes) add_installer_row(exe, true);
        }
    }

    installer_list_->show_all();
    update_installer_run_btn();
}

void MonkeyLauncher::add_installer_row(const fs::path& exe, bool indent) {
    auto* row  = Gtk::manage(new Gtk::ListBoxRow);
    auto* hbox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 8));
    set_margin_all(*hbox, 6);
    hbox->set_margin_start(indent ? 22 : 10);
    auto* cb = Gtk::manage(new Gtk::CheckButton);
    cb->signal_toggled().connect([this] { update_installer_run_btn(); });
    pack(*hbox, *cb);
    pack(*hbox, *make_label(exe.filename().string(), 0), true, true);
    auto* spinner = Gtk::manage(new Gtk::Spinner);
    spinner->set_size_request(16, 16);
    spinner->set_no_show_all(true);
    auto* status_lbl = Gtk::manage(new Gtk::Label);
    status_lbl->set_no_show_all(true);
    hbox->pack_end(*status_lbl, false, false, 0);
    hbox->pack_end(*spinner, false, false, 0);
    row->add(*hbox);
    installer_list_->add(*row);
    installer_rows_.push_back({exe.string(), cb, spinner, status_lbl});
}

void MonkeyLauncher::load_installer_redist() {
    std::vector<std::pair<std::string, std::string>> roots;
    for (const auto& gd : gamedirs_) {
        for (const auto& rel : rglob_named(gd, "_CommonRedist"))
            roots.emplace_back(fs::path(gd).filename().string() + " › " + rel.string(),
                               (fs::path(gd) / rel).string());
    }
    if (!roots.empty()) {
        installer_source_lbl_->set_markup("<b>_CommonRedist</b> from game directories");
        populate_installer(roots);
    } else {
        installer_source_lbl_->set_markup(
            "<i>No _CommonRedist found — use Browse to pick a folder</i>");
        populate_installer({});
    }
}

void MonkeyLauncher::on_installer_browse() {
    Gtk::FileChooserDialog fc(*this, "Select directory", Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
    fc.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
    fc.add_button("Select", Gtk::RESPONSE_OK);
    fc.set_current_folder(gamedirs_.empty() ? home_dir().string() : gamedirs_.front());
    if (fc.run() == Gtk::RESPONSE_OK) {
        const std::string chosen = fc.get_filename();
        installer_source_lbl_->set_markup("<i>" + Glib::Markup::escape_text(chosen) + "</i>");
        populate_installer({{fs::path(chosen).filename().string(), chosen}});
    }
}

void MonkeyLauncher::update_installer_run_btn() {
    const bool any_checked = std::any_of(installer_rows_.begin(), installer_rows_.end(),
                                         [](const StatusRow& r) { return r.cb->get_active(); });
    if (installer_run_btn_->get_label() != "Please wait…") installer_run_btn_->set_sensitive(any_checked);
}

void MonkeyLauncher::on_run_installer() {
    auto proton = selected_proton();
    if (!proton) {
        show_error(this, "No Proton version selected.");
        return;
    }

    std::vector<std::string> checked;
    for (const auto& r : installer_rows_)
        if (r.cb->get_active()) checked.push_back(r.key);
    if (checked.empty()) return;

    installer_run_btn_->set_sensitive(false);
    installer_run_btn_->set_label("Please wait…");

    Env env = current_env();
    env["WINEPREFIX"] = wineprefix_path().string() + "/";
    env["PROTONPATH"] = proton->string();
    env["GAMEID"]     = "0";

    std::thread([this, checked, env] {
        for (const auto& exe_path : checked) {
            run_on_main([this, exe_path] {
                if (auto* r = find_row(installer_rows_, exe_path)) set_status(*r, "running");
            });
            log_info("Running installer: {}", exe_path);
            int code;
            try {
                code = run({"umu-run", exe_path}, &env, fs::path(exe_path).parent_path());
            } catch (const std::exception& e) {
                log_error("Could not run umu-run: {}", e.what());
                code = -1;
            }
            const bool ok = code == 0;
            if (ok) log_info("Installer finished: {}", exe_path);
            else    log_error("Installer failed: {} (exit {})", exe_path, code);
            run_on_main([this, exe_path, ok] {
                if (auto* r = find_row(installer_rows_, exe_path)) {
                    set_status(*r, ok ? "ok" : "error");
                    if (ok) r->cb->set_active(false);
                }
            });
        }
        run_on_main([this] {
            installer_run_btn_->set_label("Run selected");
            update_installer_run_btn();
        });
    }).detach();
}

// ── Updates ──────────────────────────────────────────────────────────────────
// Silent version check run at launch — unlike on_check_updates, never shows
// a dialog on its own; it just reveals the green 'Update now' button on the
// Library page when a newer release exists.
void MonkeyLauncher::auto_check_updates() {
    log_debug("Checking GitHub for the latest release (startup check)");
    std::thread([this] {
        try {
            ReleaseInfo info = check_latest_release();
            run_on_main([this, info] { on_auto_update_checked(info); });
        } catch (const std::exception& e) {
            log_debug("Startup update check failed (ignored): {}", e.what());
        }
    }).detach();
}

void MonkeyLauncher::on_auto_update_checked(const ReleaseInfo& info) {
    if (is_newer(info.version)) {
        log_info("Update available: {} → {}", current_version(), info.version);
        update_info_ = info;
        update_now_btn_->show();
    } else {
        log_debug("Up to date (current {}, latest {})", current_version(), info.version);
    }
}

void MonkeyLauncher::on_update_now_clicked() {
    if (update_info_) prompt_update(*update_info_);
}

void MonkeyLauncher::on_check_updates() {
    update_btn_->set_sensitive(false);
    update_btn_->set_label("Checking…");
    log_debug("Checking GitHub for the latest release");

    std::thread([this] {
        try {
            ReleaseInfo info = check_latest_release();
            run_on_main([this, info] { show_update_result(info, ""); });
        } catch (const std::exception& e) {
            run_on_main([this, msg = std::string(e.what())] { show_update_result(std::nullopt, msg); });
        }
    }).detach();
}

void MonkeyLauncher::show_update_result(const std::optional<ReleaseInfo>& info, const std::string& error) {
    update_btn_->set_sensitive(true);
    update_btn_->set_label("Check for Updates");
    if (!error.empty()) {
        log_error("Update check failed: {}", error);
        show_error(this, "Could not check for updates:\n" + error);
        return;
    }
    if (!is_newer(info->version)) {
        log_info("Up to date (current {}, latest {})", current_version(), info->version);
        update_info_.reset();
        update_now_btn_->hide();
        run_message(this, Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK, "You're up to date",
                    "MonkeyLauncher " + current_version() + " is the latest version.");
        return;
    }

    log_info("Update available: {} → {}", current_version(), info->version);
    update_info_ = info;
    update_now_btn_->show();
    prompt_update(*info);
}

void MonkeyLauncher::prompt_update(const ReleaseInfo& info) {
    const bool can_auto_update = is_source_install() && !is_dev_checkout();
    Gtk::MessageDialog d(*this, "Update available: " + info.version, false,
                         Gtk::MESSAGE_INFO, Gtk::BUTTONS_NONE);
    Glib::ustring body = strip(info.body);
    if (body.size() > 2000) body = body.substr(0, 2000);
    d.set_secondary_text(body.empty() ? Glib::ustring("(no changelog provided)") : body);
    d.add_button("Later", Gtk::RESPONSE_CANCEL);
    d.add_button(can_auto_update ? "Update Now" : "Open Releases Page", Gtk::RESPONSE_OK);
    const int response = d.run();
    d.hide();
    if (response != Gtk::RESPONSE_OK) return;
    if (can_auto_update) run_update(info);
    else                 open_path(info.html_url);
}

void MonkeyLauncher::run_update(const ReleaseInfo& info) {
    update_btn_->set_sensitive(false);
    update_btn_->set_label("Updating…");
    update_now_btn_->set_sensitive(false);
    update_now_label_->set_text("Updating…");
    log_info("Downloading and installing update {}", info.version);

    std::thread([this, url = info.tarball_url] {
        std::string error;
        try {
            perform_source_update(url);
        } catch (const std::exception& e) {
            error = e.what();
            if (error.empty()) error = "unknown error";
        }
        run_on_main([this, error] { update_done(error); });
    }).detach();
}

void MonkeyLauncher::update_done(const std::string& error) {
    update_btn_->set_sensitive(true);
    update_btn_->set_label("Check for Updates");
    update_now_btn_->set_sensitive(true);
    update_now_label_->set_text("Update now");
    if (!error.empty()) {
        log_error("Update failed: {}", error);
        show_error(this, "Update failed:\n" + error);
        return;
    }
    update_info_.reset();
    update_now_btn_->hide();
    log_info("Update installed — restart required");
    run_message(this, Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK, "Update installed",
                "Restart MonkeyLauncher to use the new version.");
}

void MonkeyLauncher::on_reset() {
    Gtk::MessageDialog dialog(*this, "Reset all config?", false, Gtk::MESSAGE_WARNING, Gtk::BUTTONS_YES_NO);
    dialog.set_secondary_text(
        "This will clear all game directories, Proton preference, "
        "and per-game settings. Save files are NOT deleted.");
    if (dialog.run() == Gtk::RESPONSE_YES) {
        log_warning("Resetting all config (games, dirs, Proton preference) at user's request");
        std::error_code ec;
        fs::remove(config_file(), ec);
        fs::remove(gamedirs_file(), ec);
        fs::remove_all(games_dir(), ec);
        cfg_.clear();
        gamedirs_.clear();
        store_->clear();
        global_launch_opts_entry_->set_text("");
    }
}

}  // namespace ml
