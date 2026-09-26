#include "dialogs.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>

#include "covers.hpp"
#include "logging.hpp"
#include "proc.hpp"
#include "steam.hpp"

namespace ml {

const std::vector<std::pair<std::string, std::string>> DEFAULT_DLL_OVERRIDES = {
    {"OnlineFix64",    "n"},
    {"SteamOverlay64", "n"},
    {"winmm",          "n,b"},
    {"dnet",           "n"},
    {"steam_api64",    "n"},
};

const std::vector<std::pair<std::string, std::string>> WINETRICKS_PACKAGES = {
    {"vcrun2022",      "Visual C++ 2015-2022 Redistributable"},
    {"vcrun2019",      "Visual C++ 2015-2019 Redistributable"},
    {"vcrun2013",      "Visual C++ 2013 Redistributable"},
    {"vcrun2010",      "Visual C++ 2010 Redistributable"},
    {"dotnet48",       ".NET Framework 4.8"},
    {"dotnet6",        ".NET 6 Runtime"},
    {"dotnet7",        ".NET 7 Runtime"},
    {"dotnet8",        ".NET 8 Runtime"},
    {"d3dx9",          "DirectX 9 (d3dx9)"},
    {"d3dcompiler_47", "D3D Shader Compiler 47"},
    {"d3dx11_43",      "DirectX 11 (d3dx11)"},
    {"openal",         "OpenAL audio library"},
    {"faudio",         "FAudio (XAudio2)"},
    {"xact",           "XACT audio engine"},
    {"xna40",          "XNA Framework 4.0"},
    {"physx",          "NVIDIA PhysX"},
    {"mfc140",         "MFC 14.0"},
};

// ── GameSettingsDialog ───────────────────────────────────────────────────────
GameSettingsDialog::GameSettingsDialog(Gtk::Window& parent, const std::string& label)
    : Gtk::Dialog("Settings — " + label, parent, false), label_(label) {
    set_default_size(680, 520);
    cfg_ = read_config(game_config_path(label));

    add_button("Cancel", Gtk::RESPONSE_CANCEL);
    add_button("Save",   Gtk::RESPONSE_OK);
    set_default_response(Gtk::RESPONSE_OK);

    // Parse existing LAUNCH_ENV: pull WINEDLLOVERRIDES out for the
    // Compatibility tab, but keep every other token — env vars and
    // plain launch flags alike — verbatim for the Launch Options field.
    std::vector<std::pair<std::string, std::string>> existing_dll;   // ordered
    bool has_per_game_dll = false;
    std::vector<std::string> extra_tokens;
    static const std::string DLL_PREFIX = "WINEDLLOVERRIDES=";
    for (const auto& tok : split_whitespace(cfg_.get("LAUNCH_ENV"))) {
        if (starts_with(tok, DLL_PREFIX)) {
            has_per_game_dll = true;
            const std::string rest = tok.substr(DLL_PREFIX.size());
            size_t start = 0;
            while (true) {
                size_t semi = rest.find(';', start);
                const std::string part = rest.substr(start, semi == std::string::npos ? semi : semi - start);
                size_t eq = part.find('=');
                if (eq != std::string::npos) {
                    const std::string dll = strip(part.substr(0, eq)), mode = strip(part.substr(eq + 1));
                    bool replaced = false;
                    for (auto& kv : existing_dll)
                        if (kv.first == dll) { kv.second = mode; replaced = true; }
                    if (!replaced) existing_dll.emplace_back(dll, mode);
                }
                if (semi == std::string::npos) break;
                start = semi + 1;
            }
        } else {
            extra_tokens.push_back(tok);
        }
    }

    // ── Steam-settings-style layout: sections on the left, content on the right
    auto* content_row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL));
    pack(*get_content_area(), *content_row, true, true);

    settings_stack_ = Gtk::manage(new Gtk::Stack);
    settings_stack_->set_transition_type(Gtk::STACK_TRANSITION_TYPE_CROSSFADE);

    auto* sidebar = Gtk::manage(new Gtk::StackSidebar);
    sidebar->set_stack(*settings_stack_);
    pack(*content_row, *sidebar);
    pack(*content_row, *Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_VERTICAL)));
    pack(*content_row, *settings_stack_, true, true);

    // ── Display (name + cover) ──────────────────────────────────────────────
    auto* display_box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 16));
    set_margin_all(*display_box, 16);
    display_box->set_halign(Gtk::ALIGN_START);
    display_box->set_valign(Gtk::ALIGN_START);

    auto* cover_col = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 6));
    cover_preview_ = Gtk::manage(new Gtk::Image);
    refresh_cover_preview();
    pack(*cover_col, *cover_preview_);

    auto* cover_btn_row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    auto* cover_set_btn = Gtk::manage(new Gtk::Button("Set cover…"));
    cover_set_btn->signal_clicked().connect([this] { on_browse_cover(); });
    auto* cover_clear_btn = Gtk::manage(new Gtk::Button("Clear cover"));
    cover_clear_btn->signal_clicked().connect([this] { on_clear_cover(); });
    pack(*cover_btn_row, *cover_set_btn, true, true);
    pack(*cover_btn_row, *cover_clear_btn, true, true);
    pack(*cover_col, *cover_btn_row);
    pack(*display_box, *cover_col);

    auto* fields_col = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 6));
    fields_col->set_valign(Gtk::ALIGN_START);
    auto* name_row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 8));
    pack(*name_row, *Gtk::manage(new Gtk::Label("Name:")));
    name_entry_ = Gtk::manage(new Gtk::Entry);
    name_entry_->set_hexpand(true);
    name_entry_->set_placeholder_text(default_game_name(label));
    name_entry_->set_text(cfg_.get("NAME"));
    pack(*name_row, *name_entry_, true, true);
    pack(*fields_col, *name_row);
    pack(*display_box, *fields_col, true, true);

    settings_stack_->add(*display_box, "display", "General");

    // ── Launch (compatibility mode + WINEDLLOVERRIDES + launch options) ──────
    auto* compat_scroll = Gtk::manage(new Gtk::ScrolledWindow);
    compat_scroll->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    auto* compat_outer = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 4));
    set_margin_all(*compat_outer, 16);

    pack(*compat_outer, *make_label("Compatibility mode", 0));
    auto* mode_row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 12));
    onlinefix_radio_ = Gtk::manage(new Gtk::RadioButton("OnlineFix"));
    onlinefix_radio_->set_tooltip_text(
        "Runs through the shared Proton prefix with the WINEDLLOVERRIDES below "
        "applied, so the cracked online-fix DLLs load in place of the real "
        "Steamworks API.");
    offline_radio_ = Gtk::manage(new Gtk::RadioButton("Offline"));
    offline_radio_->join_group(*onlinefix_radio_);
    offline_radio_->set_tooltip_text(
        "Skips WINEDLLOVERRIDES and the selected Proton/shared prefix entirely — "
        "umu manages its own default Proton build and prefix instead, exactly "
        "like running the exe bare from a terminal. Launch options still apply. "
        "Useful for games that don't need the online-fix compatibility tricks "
        "and actually run worse under the shared Proton prefix.");
    if (cfg_.get("OFFLINE") == "1") offline_radio_->set_active(true);
    offline_radio_->signal_toggled().connect([this] { on_offline_toggled(); });
    pack(*mode_row, *onlinefix_radio_);
    pack(*mode_row, *offline_radio_);
    pack(*compat_outer, *mode_row);
    pack(*compat_outer, *Gtk::manage(new Gtk::Separator), false, false, 4);

    pack(*compat_outer, *make_label("WINEDLLOVERRIDES", 0), false, false, 4);
    dll_box_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 4));

    std::set<std::string> shown;
    for (const auto& [dll, default_mode] : DEFAULT_DLL_OVERRIDES) {
        bool checked;
        std::string mode;
        if (has_per_game_dll) {
            auto it = std::find_if(existing_dll.begin(), existing_dll.end(),
                                   [&](const auto& kv) { return kv.first == dll; });
            checked = it != existing_dll.end();
            mode    = checked ? it->second : default_mode;
        } else {
            checked = true;
            mode    = default_mode;
        }
        add_dll_row(dll, mode, checked);
        shown.insert(dll);
    }
    for (const auto& [dll, mode] : existing_dll)
        if (!shown.count(dll)) add_dll_row(dll, mode, true);

    auto* add_btn = Gtk::manage(new Gtk::Button("+ Add override"));
    add_btn->get_style_context()->add_class("flat");
    add_btn->signal_clicked().connect([this] {
        add_dll_row("", "", true);
        dll_box_->show_all();
    });
    pack(*dll_box_, *add_btn);
    pack(*compat_outer, *dll_box_);

    pack(*compat_outer, *Gtk::manage(new Gtk::Separator), false, false, 8);
    pack(*compat_outer, *make_label("Launch options", 0), false, false, 4);
    auto* opts_row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 8));
    launch_opts_entry_ = Gtk::manage(new Gtk::Entry);
    launch_opts_entry_->set_hexpand(true);
    launch_opts_entry_->set_placeholder_text("e.g. GAMEMODE=1 DRI_PRIME=1");
    launch_opts_entry_->set_text(join(extra_tokens, " "));
    pack(*opts_row, *launch_opts_entry_, true, true);
    pack(*compat_outer, *opts_row);

    compat_scroll->add(*compat_outer);
    settings_stack_->add(*compat_scroll, "launch", "Launch");
    on_offline_toggled();

    // ── Save directory ──────────────────────────────────────────────────────
    auto* save_box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 8));
    set_margin_all(*save_box, 16);

    auto* savebox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    save_entry_ = Gtk::manage(new Gtk::Entry);
    save_entry_->set_hexpand(true);
    save_entry_->set_placeholder_text("Path inside Proton prefix");
    save_entry_->set_text(cfg_.get("SAVEDIR"));
    auto* browse_btn = Gtk::manage(new Gtk::Button("Browse…"));
    browse_btn->signal_clicked().connect([this] { on_browse_save(); });
    pack(*savebox, *save_entry_, true, true);
    pack(*savebox, *browse_btn);
    pack(*save_box, *savebox);

    auto* note = make_label("", 0);
    note->set_line_wrap(true);
    note->set_markup("<small>Saves stored in: <i>" +
                     Glib::Markup::escape_text(game_save_path(label).string()) + "</i></small>");
    pack(*save_box, *note);
    auto* open_btn = Gtk::manage(new Gtk::Button("Open save folder"));
    open_btn->signal_clicked().connect([this] { on_open_saves(); });
    pack(*save_box, *open_btn);

    settings_stack_->add(*save_box, "save", "Save Directory");

    show_all();
}

void GameSettingsDialog::on_offline_toggled() {
    // Launch options stay usable in both modes — only the WINEDLLOVERRIDES
    // rows are OnlineFix-specific.
    dll_box_->set_sensitive(!offline_radio_->get_active());
}

void GameSettingsDialog::add_dll_row(const std::string& dll_name, const std::string& mode, bool checked) {
    auto* row_box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    auto* cb = Gtk::manage(new Gtk::CheckButton);
    cb->set_active(checked);
    auto* dll_e = Gtk::manage(new Gtk::Entry);
    dll_e->set_width_chars(16);
    dll_e->set_placeholder_text("dll name");
    dll_e->set_text(dll_name);
    auto* sep = Gtk::manage(new Gtk::Label("="));
    auto* mode_e = Gtk::manage(new Gtk::Entry);
    mode_e->set_width_chars(6);
    mode_e->set_placeholder_text("n,b…");
    mode_e->set_text(mode);
    auto* rm_btn = Gtk::manage(new Gtk::Button("×"));
    rm_btn->get_style_context()->add_class("flat");
    pack(*row_box, *cb);
    pack(*row_box, *dll_e, true, true);
    pack(*row_box, *sep);
    pack(*row_box, *mode_e);
    pack(*row_box, *rm_btn);
    dll_rows_.push_back({row_box, cb, dll_e, mode_e});

    rm_btn->signal_clicked().connect([this, row_box] {
        dll_box_->remove(*row_box);
        dll_rows_.erase(std::remove_if(dll_rows_.begin(), dll_rows_.end(),
                                       [&](const DllRow& r) { return r.box == row_box; }),
                        dll_rows_.end());
    });

    // Insert before the "+ Add override" button
    const auto children = dll_box_->get_children();
    const int pos = children.empty() ? 0 : static_cast<int>(children.size()) - 1;
    pack(*dll_box_, *row_box);
    dll_box_->reorder_child(*row_box, pos);
}

void GameSettingsDialog::on_browse_save() {
    std::error_code ec;
    fs::path start = wineprefix_path() / "pfx" / "drive_c" / "users" / "steamuser";
    if (!fs::is_directory(start, ec)) start = wineprefix_path() / "pfx";
    Gtk::FileChooserDialog dialog(*this, "Select save directory in Proton prefix",
                                  Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
    dialog.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
    dialog.add_button("Select", Gtk::RESPONSE_OK);
    dialog.set_current_folder(start.string());
    if (dialog.run() == Gtk::RESPONSE_OK) save_entry_->set_text(dialog.get_filename());
}

void GameSettingsDialog::on_open_saves() {
    const fs::path ml_save = game_save_path(label_);
    std::error_code ec;
    fs::create_directories(ml_save, ec);
    try {
        spawn_detached({"xdg-open", ml_save.string()});
    } catch (const std::exception& e) {
        log_warning("Could not open {}: {}", ml_save.string(), e.what());
    }
}

void GameSettingsDialog::refresh_cover_preview() {
    const fs::path cache_path = cover_cache_path(label_);
    std::error_code ec;
    if (fs::exists(cache_path, ec)) {
        try {
            auto pixbuf = Gdk::Pixbuf::create_from_file(cache_path.string(), 160, 240, true);
            cover_preview_->set(pixbuf);
            return;
        } catch (const Glib::Error&) {
        }
    }
    cover_preview_->set_pixel_size(160);
    cover_preview_->set_from_icon_name("applications-games", Gtk::ICON_SIZE_DIALOG);
}

void GameSettingsDialog::on_browse_cover() {
    Gtk::FileChooserDialog dialog(*this, "Select cover image", Gtk::FILE_CHOOSER_ACTION_OPEN);
    dialog.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
    dialog.add_button("Select", Gtk::RESPONSE_OK);
    auto img_filter = Gtk::FileFilter::create();
    img_filter->set_name("Images");
    for (const char* pattern : {"*.png", "*.jpg", "*.jpeg", "*.webp", "*.bmp"})
        img_filter->add_pattern(pattern);
    dialog.add_filter(img_filter);
    if (dialog.run() == Gtk::RESPONSE_OK) {
        const fs::path src = dialog.get_filename();
        const fs::path cache_path = cover_cache_path(label_);
        std::error_code ec;
        fs::create_directories(cache_path.parent_path(), ec);
        fs::copy_file(src, cache_path, fs::copy_options::overwrite_existing, ec);
        if (ec) show_error(this, "Could not copy the cover image: " + ec.message());
        refresh_cover_preview();
    }
}

void GameSettingsDialog::on_clear_cover() {
    std::error_code ec;
    fs::remove(cover_cache_path(label_), ec);
    refresh_cover_preview();
}

Config GameSettingsDialog::get_result() const {
    std::vector<std::string> dll_parts;
    for (const auto& r : dll_rows_) {
        if (!r.cb->get_active()) continue;
        const std::string dll  = strip(r.dll->get_text().raw());
        const std::string mode = strip(r.mode->get_text().raw());
        if (!dll.empty() && !mode.empty()) dll_parts.push_back(dll + "=" + mode);
    }

    // Always record the WINEDLLOVERRIDES token, even empty — otherwise
    // "explicitly no overrides" (every row unchecked) is indistinguishable
    // from "never customized" once saved, and reopening resets every row
    // back to checked.
    std::vector<std::string> env_parts = {"WINEDLLOVERRIDES=" + join(dll_parts, ";")};
    const std::string extra = strip(launch_opts_entry_->get_text().raw());
    if (!extra.empty()) env_parts.push_back(extra);

    Config out;
    out.set("NAME",       strip(name_entry_->get_text().raw()));
    out.set("LAUNCH_ENV", join(env_parts, " "));
    out.set("SAVEDIR",    strip(save_entry_->get_text().raw()));
    out.set("OFFLINE",    offline_radio_->get_active() ? "1" : "");
    return out;
}

// ── InstallDepsDialog ────────────────────────────────────────────────────────
InstallDepsDialog::InstallDepsDialog(Gtk::Window& parent, const std::string& gamedir,
                                     const std::vector<fs::path>& proton_dirs)
    : Gtk::Dialog("Install Dependencies", parent, false), gamedir_(gamedir), proton_dirs_(proton_dirs) {
    set_default_size(580, 520);

    add_button("Cancel", Gtk::RESPONSE_CANCEL);
    install_btn_ = add_button("Install", Gtk::RESPONSE_OK);
    install_btn_->get_style_context()->add_class("suggested-action");

    Gtk::Box* box = get_content_area();
    box->set_spacing(0);

    // Proton picker
    auto* proton_box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 8));
    set_margin_all(*proton_box, 12);
    pack(*proton_box, *Gtk::manage(new Gtk::Label("Install into:")));
    proton_combo_ = Gtk::manage(new Gtk::ComboBoxText);
    for (const auto& d : proton_dirs_) proton_combo_->append(d.filename().string());
    proton_combo_->set_active(0);
    pack(*proton_box, *proton_combo_, true, true);
    pack(*box, *proton_box);

    pack(*box, *Gtk::manage(new Gtk::Separator));

    // Run local exe option
    auto* local_btn = Gtk::manage(new Gtk::Button("Run .exe from game directory…"));
    set_margin_all(*local_btn, 12);
    local_btn->signal_clicked().connect([this] { on_run_local_exe(); });
    pack(*box, *local_btn);

    pack(*box, *Gtk::manage(new Gtk::Separator));

    // Winetricks package list
    auto* label = make_label("Winetricks packages:", 0);
    label->set_margin_start(12);
    label->set_margin_top(8);
    pack(*box, *label);

    auto* scroll = Gtk::manage(new Gtk::ScrolledWindow);
    set_margin_all(*scroll, 12);
    scroll->set_vexpand(true);
    scroll->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    auto* listbox = Gtk::manage(new Gtk::ListBox);
    listbox->set_selection_mode(Gtk::SELECTION_NONE);
    for (const auto& [verb, desc] : WINETRICKS_PACKAGES) {
        auto* row  = Gtk::manage(new Gtk::ListBoxRow);
        auto* hbox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 8));
        set_margin_all(*hbox, 6);
        auto* cb = Gtk::manage(new Gtk::CheckButton);
        pack(*hbox, *cb);
        pack(*hbox, *make_label(verb + "  —  " + desc, 0), true, true);
        row->add(*hbox);
        listbox->add(*row);
        checks_.emplace_back(verb, cb);
    }
    scroll->add(*listbox);
    pack(*box, *scroll, true, true);

    show_all();
}

std::optional<fs::path> InstallDepsDialog::get_proton_path() const {
    int idx = proton_combo_->get_active_row_number();
    if (idx >= 0 && idx < static_cast<int>(proton_dirs_.size())) return proton_dirs_[idx];
    return std::nullopt;
}

std::vector<std::string> InstallDepsDialog::get_selected_verbs() const {
    std::vector<std::string> out;
    for (const auto& [verb, cb] : checks_)
        if (cb->get_active()) out.push_back(verb);
    return out;
}

void InstallDepsDialog::on_run_local_exe() {
    const auto exes = get_all_exe_list(gamedir_);
    if (exes.empty()) {
        show_error(this, "No .exe files found in game directory.");
        return;
    }
    Gtk::Dialog dialog("Select installer", *this, false);
    dialog.set_default_size(500, 400);
    dialog.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
    dialog.add_button("Run", Gtk::RESPONSE_OK);
    Gtk::ScrolledWindow scroll;
    set_margin_all(scroll, 12);
    scroll.set_vexpand(true);

    Gtk::TreeModel::ColumnRecord cols;
    Gtk::TreeModelColumn<Glib::ustring> col_name;
    cols.add(col_name);
    auto store = Gtk::ListStore::create(cols);
    for (const auto& e : exes) store->append()->set_value(0, Glib::ustring(e));
    Gtk::TreeView tv(store);
    tv.append_column("Executable", col_name);
    tv.set_headers_visible(false);
    scroll.add(tv);
    dialog.get_content_area()->add(scroll);
    dialog.show_all();

    if (dialog.run() == Gtk::RESPONSE_OK) {
        auto it = tv.get_selection()->get_selected();
        if (it) {
            const std::string exe_label = it->get_value(col_name).raw();
            const std::string exe_path  = (fs::path(gamedir_) / exe_label).string();
            auto proton = get_proton_path();
            dialog.hide();
            hide();
            if (proton) run_through_proton(exe_path, *proton);
        }
    }
}

}  // namespace ml
