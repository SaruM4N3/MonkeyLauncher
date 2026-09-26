#pragma once
#include <gtkmm.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "config.hpp"
#include "ui.hpp"

namespace ml {

extern const std::vector<std::pair<std::string, std::string>> DEFAULT_DLL_OVERRIDES;
extern const std::vector<std::pair<std::string, std::string>> WINETRICKS_PACKAGES;

class GameSettingsDialog : public Gtk::Dialog {
public:
    GameSettingsDialog(Gtk::Window& parent, const std::string& label);
    Config get_result() const;

private:
    struct DllRow {
        Gtk::Box*         box;
        Gtk::CheckButton* cb;
        Gtk::Entry*       dll;
        Gtk::Entry*       mode;
    };

    void on_offline_toggled();
    void add_dll_row(const std::string& dll_name, const std::string& mode, bool checked);
    void on_browse_save();
    void on_open_saves();
    void refresh_cover_preview();
    void on_browse_cover();
    void on_clear_cover();

    std::string label_;
    Config      cfg_;
    std::vector<DllRow> dll_rows_;

    Gtk::Stack*       settings_stack_;
    Gtk::Image*       cover_preview_;
    Gtk::Entry*       name_entry_;
    Gtk::RadioButton* onlinefix_radio_;
    Gtk::RadioButton* offline_radio_;
    Gtk::Box*         dll_box_;
    Gtk::Entry*       launch_opts_entry_;
    Gtk::Entry*       save_entry_;
};

class InstallDepsDialog : public Gtk::Dialog {
public:
    InstallDepsDialog(Gtk::Window& parent, const std::string& gamedir,
                      const std::vector<fs::path>& proton_dirs);
    std::optional<fs::path> get_proton_path() const;
    std::vector<std::string> get_selected_verbs() const;

private:
    void on_run_local_exe();

    std::string gamedir_;
    std::vector<fs::path> proton_dirs_;
    Gtk::ComboBoxText* proton_combo_;
    Gtk::Button*       install_btn_;
    std::vector<std::pair<std::string, Gtk::CheckButton*>> checks_;
};

}  // namespace ml
