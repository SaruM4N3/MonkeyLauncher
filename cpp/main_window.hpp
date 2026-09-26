#pragma once
#include <gtkmm.h>

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "config.hpp"
#include "proc.hpp"
#include "updater.hpp"

namespace ml {

// Parse a launch-options string (global or per-game) Steam-%command%-style:
// KEY=VALUE tokens are applied to `env` in place, "%command%" marks where
// the game's launch command goes, and every other token before/after it
// becomes a prefix/suffix arg. Returns (prefix, suffix).
std::pair<std::vector<std::string>, std::vector<std::string>>
parse_launch_tokens(const std::string& text, Env& env);

class MonkeyLauncher : public Gtk::ApplicationWindow {
public:
    explicit MonkeyLauncher(const Glib::RefPtr<Gtk::Application>& app);

private:
    typedef Glib::RefPtr<Gdk::Pixbuf> Pix;

    struct GameEntry {
        std::string exe;
        Config      cfg;
        std::string gamedir;
    };
    struct StatusRow {                    // installer / winetricks row widgets
        std::string       key;
        Gtk::CheckButton* cb;
        Gtk::Spinner*     spinner;
        Gtk::Label*       label;
    };

    // TreeStore: display, exe_relpath (empty for dir rows), gamedir,
    // hidden (dims the row; dir rows are always false), icon (folder icon
    // for dir rows, cover-derived icon for games)
    struct TreeCols : Gtk::TreeModel::ColumnRecord {
        Gtk::TreeModelColumn<Glib::ustring> display, exe, gamedir;
        Gtk::TreeModelColumn<bool>          hidden;
        Gtk::TreeModelColumn<Pix>           icon;
        TreeCols() { add(display); add(exe); add(gamedir); add(hidden); add(icon); }
    };
    // Flat ListStore (no directory grouping): cover, display, exe, gamedir, hidden
    struct GridCols : Gtk::TreeModel::ColumnRecord {
        Gtk::TreeModelColumn<Pix>           cover;
        Gtk::TreeModelColumn<Glib::ustring> display, exe, gamedir;
        Gtk::TreeModelColumn<bool>          hidden;
        GridCols() { add(cover); add(display); add(exe); add(gamedir); add(hidden); }
    };

    // ── UI construction ──
    Gtk::RadioButton* nav_button(const char* icon_name, const char* label, Gtk::RadioButton* group);
    void build_ui();
    void build_library_page();
    void build_help_page();
    void build_settings_page();

    std::string display_name(const std::string& exe, const Config& gcfg) const;
    void refresh_display_names();
    void populate_proton_combo();
    bool game_filter(const Gtk::TreeModel::const_iterator& it);
    bool grid_filter_fn(const Gtk::TreeModel::const_iterator& it);

    // ── Cover art ──
    static constexpr double COVER_ASPECT = 2.0 / 3.0;
    Pix placeholder_pixbuf(int size = 110, double aspect = COVER_ASPECT);
    Pix load_cover_pixbuf(const fs::path& path, int size = 110, double aspect = COVER_ASPECT);
    Pix load_grid_pixbuf(const fs::path& path, int size = 110);
    Pix list_icon_pixbuf_for(const std::string& exe, int size = 24);
    Pix mode_badge_icon(bool offline, int size);
    Pix apply_mode_badge(Pix pixbuf, const std::string& exe);
    Pix folder_icon_pixbuf(int size = 24);
    Pix grid_pixbuf_for(const std::string& exe);
    void start_cover_fetch();
    void apply_cover(const std::string& exe, const std::string& gamedir, const fs::path& path);

    // ── Data loading ──
    void load_games();
    void apply_games(const std::vector<std::pair<std::string, std::vector<GameEntry>>>& results);
    std::pair<std::string, std::string> selected_game();   // (exe, gamedir); empty exe if none
    std::optional<fs::path> selected_proton();

    // ── Signals ──
    void on_search_changed();
    void restore_grid_view();
    void on_view_toggle();
    void on_show_hidden_toggle();
    bool on_grid_button_press(GdkEventButton* event);
    void on_selection_changed();
    void on_path_toggle();
    void on_game_activated(const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn*);
    bool on_tree_button_press(GdkEventButton* event);
    void popup_menu(GdkEventButton* event);
    void on_hide_game(const std::string& exe);
    void on_activate_game(const std::string& exe);
    void on_launch();
    void on_game_exit(GPid pid, int status);
    void on_game_settings();
    void refresh_game_row(const std::string& exe, const std::string& gamedir);
    void on_add_game_dir();
    void on_remove_game_dir(const std::string& gamedir);
    void on_rescan_game_dir(const std::string& gamedir);
    void apply_rescan(const std::string& gamedir, const std::vector<std::pair<std::string, Config>>& exes);
    void on_proton_changed();
    void refresh_steam_labels();
    void refresh_after_steam_change();
    void on_change_steam_root();
    void on_change_steam_library();
    bool on_global_launch_opts_changed();
    void check_installed_deps();
    void apply_installed_deps(const std::set<std::string>& installed);
    void on_install_deps();
    void run_winetricks(const std::vector<std::string>& verbs);
    void set_status(StatusRow& row, const std::string& status);
    StatusRow* find_row(std::vector<StatusRow>& rows, const std::string& key);
    void populate_installer(const std::vector<std::pair<std::string, std::string>>& roots);
    void add_installer_row(const fs::path& exe, bool indent = false);
    void load_installer_redist();
    void on_installer_browse();
    void update_installer_run_btn();
    void on_run_installer();

    // ── Updates ──
    void auto_check_updates();
    void on_auto_update_checked(const ReleaseInfo& info);
    void on_update_now_clicked();
    void on_check_updates();
    void show_update_result(const std::optional<ReleaseInfo>& info, const std::string& error);
    void prompt_update(const ReleaseInfo& info);
    void run_update(const ReleaseInfo& info);
    void update_done(const std::string& error);
    void on_reset();

    void popup_at(std::unique_ptr<Gtk::Menu> menu, GdkEventButton* event);

    Config                    cfg_;
    std::vector<fs::path>     proton_dirs_;
    std::vector<std::string>  gamedirs_;
    bool                      mangohud_      = false;
    bool                      show_fullpath_ = false;
    bool                      show_hidden_   = false;
    pid_t                     running_pid_   = 0;
    std::atomic<bool>         cover_fetch_running_{false};
    std::map<std::string, Pix> placeholder_cache_;
    std::optional<ReleaseInfo> update_info_;

    TreeCols  cols_;
    GridCols  grid_cols_;

    Gtk::Stack*        stack_;
    Gtk::Stack*        view_stack_;
    Gtk::Stack*        settings_stack_;
    Gtk::RadioButton  *library_nav_btn_, *help_nav_btn_, *settings_nav_btn_;
    Gtk::Button*       update_now_btn_;
    Gtk::Label*        update_now_label_;
    Gtk::SearchEntry*  search_entry_;
    Gtk::ToggleButton *path_toggle_, *view_toggle_, *show_hidden_toggle_, *mangohud_btn_;
    Glib::RefPtr<Gtk::TreeStore>       store_;
    Glib::RefPtr<Gtk::TreeModelFilter> filter_;
    Gtk::TreeView*     tv_;
    Glib::RefPtr<Gtk::ListStore>       grid_store_;
    Glib::RefPtr<Gtk::TreeModelFilter> grid_filter_;
    Gtk::IconView*     icon_view_;
    Gtk::Spinner*      spinner_;
    Gtk::Button       *game_settings_btn_, *launch_btn_;

    Gtk::Label*        steam_root_lbl_;
    Gtk::Label*        steam_lib_lbl_;
    Gtk::ComboBoxText* proton_combo_;
    Gtk::Entry*        global_launch_opts_entry_;
    Gtk::Label*        installer_source_lbl_;
    Gtk::ListBox*      installer_list_;
    Gtk::Button*       installer_run_btn_;
    std::vector<StatusRow> installer_rows_;
    std::vector<StatusRow> winetricks_rows_;
    Gtk::Button*       install_deps_btn_;
    Gtk::Label*        version_lbl_;
    Gtk::Button*       update_btn_;

    std::unique_ptr<Gtk::Menu> menu_;
};

}  // namespace ml
