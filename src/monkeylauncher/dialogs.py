import os
import shutil
import subprocess
from pathlib import Path

import gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, GLib, GdkPixbuf

from .config import WINEPREFIX_PATH, game_config_path, game_save_path, get_all_exe_list, read_config
from .covers import cover_cache_path, default_game_name
from .steam import run_through_proton

DEFAULT_DLL_OVERRIDES = [
    ('OnlineFix64',    'n'),
    ('SteamOverlay64', 'n'),
    ('winmm',          'n,b'),
    ('dnet',           'n'),
    ('steam_api64',    'n'),
]

WINETRICKS_PACKAGES = [
    ("vcrun2022",      "Visual C++ 2015-2022 Redistributable"),
    ("vcrun2019",      "Visual C++ 2015-2019 Redistributable"),
    ("vcrun2013",      "Visual C++ 2013 Redistributable"),
    ("vcrun2010",      "Visual C++ 2010 Redistributable"),
    ("dotnet48",       ".NET Framework 4.8"),
    ("dotnet6",        ".NET 6 Runtime"),
    ("dotnet7",        ".NET 7 Runtime"),
    ("dotnet8",        ".NET 8 Runtime"),
    ("d3dx9",          "DirectX 9 (d3dx9)"),
    ("d3dcompiler_47", "D3D Shader Compiler 47"),
    ("d3dx11_43",      "DirectX 11 (d3dx11)"),
    ("openal",         "OpenAL audio library"),
    ("faudio",         "FAudio (XAudio2)"),
    ("xact",           "XACT audio engine"),
    ("xna40",          "XNA Framework 4.0"),
    ("physx",          "NVIDIA PhysX"),
    ("mfc140",         "MFC 14.0"),
]

def show_error(parent, msg):
    d = Gtk.MessageDialog(transient_for=parent, flags=0,
                          message_type=Gtk.MessageType.ERROR,
                          buttons=Gtk.ButtonsType.OK, text=msg)
    d.run(); d.destroy()

# ── Dialogs ────────────────────────────────────────────────────────────────────
class GameSettingsDialog(Gtk.Dialog):
    def __init__(self, parent, label):
        super().__init__(title=f"Settings — {label}", transient_for=parent, flags=0)
        self.set_default_size(680, 520)
        self.label = label
        self.cfg = read_config(game_config_path(label))
        self.dll_rows = []   # list of (checkbox, dll_entry, mode_entry)

        self.add_button("Cancel", Gtk.ResponseType.CANCEL)
        self.add_button("Save",   Gtk.ResponseType.OK)
        self.set_default_response(Gtk.ResponseType.OK)

        # Parse existing LAUNCH_ENV: pull WINEDLLOVERRIDES out for the
        # Compatibility tab, but keep every other token — env vars and
        # plain launch flags alike — verbatim for the Launch Options field.
        existing_dll = {}
        has_per_game_dll = False
        extra_tokens = []
        for tok in self.cfg.get('LAUNCH_ENV', '').split():
            if tok.startswith('WINEDLLOVERRIDES='):
                has_per_game_dll = True
                for part in tok[len('WINEDLLOVERRIDES='):].split(';'):
                    if '=' in part:
                        dll, mode = part.split('=', 1)
                        existing_dll[dll.strip()] = mode.strip()
            else:
                extra_tokens.append(tok)

        # ── Steam-settings-style layout: sections on the left, content on
        # the right ──────────────────────────────────────────────────────────
        content_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL)
        self.get_content_area().pack_start(content_row, True, True, 0)

        self.settings_stack = Gtk.Stack()
        self.settings_stack.set_transition_type(Gtk.StackTransitionType.CROSSFADE)

        sidebar = Gtk.StackSidebar()
        sidebar.set_stack(self.settings_stack)
        content_row.pack_start(sidebar, False, False, 0)
        content_row.pack_start(Gtk.Separator(orientation=Gtk.Orientation.VERTICAL), False, False, 0)
        content_row.pack_start(self.settings_stack, True, True, 0)

        # ── Display (name + cover) ────────────────────────────────────────────
        display_box = Gtk.Box(spacing=16, margin=16)
        display_box.set_halign(Gtk.Align.START)
        display_box.set_valign(Gtk.Align.START)

        cover_col = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        self.cover_preview = Gtk.Image()
        self._refresh_cover_preview()
        cover_col.pack_start(self.cover_preview, False, False, 0)

        cover_btn_row = Gtk.Box(spacing=6)
        cover_set_btn = Gtk.Button(label="Set cover…")
        cover_set_btn.connect('clicked', self.on_browse_cover)
        cover_clear_btn = Gtk.Button(label="Clear cover")
        cover_clear_btn.connect('clicked', self.on_clear_cover)
        cover_btn_row.pack_start(cover_set_btn,   True, True, 0)
        cover_btn_row.pack_start(cover_clear_btn, True, True, 0)
        cover_col.pack_start(cover_btn_row, False, False, 0)
        display_box.pack_start(cover_col, False, False, 0)

        fields_col = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        fields_col.set_valign(Gtk.Align.START)
        name_row = Gtk.Box(spacing=8)
        name_row.pack_start(Gtk.Label(label="Name:"), False, False, 0)
        self.name_entry = Gtk.Entry(hexpand=True, placeholder_text=default_game_name(label))
        self.name_entry.set_text(self.cfg.get('NAME', ''))
        name_row.pack_start(self.name_entry, True, True, 0)
        fields_col.pack_start(name_row, False, False, 0)
        display_box.pack_start(fields_col, True, True, 0)

        self.settings_stack.add_titled(display_box, 'display', 'General')

        # ── Launch (compatibility mode + WINEDLLOVERRIDES + launch options) ────
        compat_scroll = Gtk.ScrolledWindow()
        compat_scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        compat_outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, margin=16, spacing=4)

        compat_outer.pack_start(
            Gtk.Label(label="Compatibility mode", xalign=0), False, False, 0)
        mode_row = Gtk.Box(spacing=12)
        self.onlinefix_radio = Gtk.RadioButton.new_with_label_from_widget(None, "OnlineFix")
        self.onlinefix_radio.set_tooltip_text(
            "Runs through the shared Proton prefix with the WINEDLLOVERRIDES below "
            "applied, so the cracked online-fix DLLs load in place of the real "
            "Steamworks API.")
        self.offline_radio = Gtk.RadioButton.new_with_label_from_widget(
            self.onlinefix_radio, "Offline")
        self.offline_radio.set_tooltip_text(
            "Skips WINEDLLOVERRIDES and the selected Proton/shared prefix entirely — "
            "umu manages its own default Proton build and prefix instead, exactly "
            "like running the exe bare from a terminal. Launch options still apply. "
            "Useful for games that don't need the online-fix compatibility tricks "
            "and actually run worse under the shared Proton prefix.")
        if self.cfg.get('OFFLINE') == '1':
            self.offline_radio.set_active(True)
        self.offline_radio.connect('toggled', self.on_offline_toggled)
        mode_row.pack_start(self.onlinefix_radio, False, False, 0)
        mode_row.pack_start(self.offline_radio,   False, False, 0)
        compat_outer.pack_start(mode_row, False, False, 0)
        compat_outer.pack_start(Gtk.Separator(), False, False, 4)

        compat_outer.pack_start(
            Gtk.Label(label="WINEDLLOVERRIDES", xalign=0), False, False, 4)
        self._dll_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)

        shown = set()
        for dll, default_mode in DEFAULT_DLL_OVERRIDES:
            if has_per_game_dll:
                checked = dll in existing_dll
                mode    = existing_dll.get(dll, default_mode)
            else:
                checked, mode = True, default_mode
            self._add_dll_row(dll, mode, checked)
            shown.add(dll)
        for dll, mode in existing_dll.items():
            if dll not in shown:
                self._add_dll_row(dll, mode, True)

        add_btn = Gtk.Button(label="+ Add override")
        add_btn.get_style_context().add_class('flat')
        add_btn.connect('clicked', lambda _: (self._add_dll_row('', '', True),
                                              self._dll_box.show_all()))
        self._dll_box.pack_start(add_btn, False, False, 0)
        compat_outer.pack_start(self._dll_box, False, False, 0)

        compat_outer.pack_start(Gtk.Separator(), False, False, 8)
        compat_outer.pack_start(
            Gtk.Label(label="Launch options", xalign=0), False, False, 4)
        opts_row = Gtk.Box(spacing=8)
        existing_extra = ' '.join(extra_tokens)
        self.launch_opts_entry = Gtk.Entry(hexpand=True,
                                           placeholder_text="e.g. GAMEMODE=1 DRI_PRIME=1")
        self.launch_opts_entry.set_text(existing_extra)
        opts_row.pack_start(self.launch_opts_entry, True, True, 0)
        compat_outer.pack_start(opts_row, False, False, 0)

        compat_scroll.add(compat_outer)
        self.settings_stack.add_titled(compat_scroll, 'launch', 'Launch')
        self.on_offline_toggled(self.offline_radio)

        # ── Save directory ────────────────────────────────────────────────────
        save_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8, margin=16)

        savebox = Gtk.Box(spacing=6)
        self.save_entry = Gtk.Entry(hexpand=True,
                                    placeholder_text="Path inside Proton prefix")
        self.save_entry.set_text(self.cfg.get('SAVEDIR', ''))
        browse_btn = Gtk.Button(label="Browse…")
        browse_btn.connect('clicked', self.on_browse_save)
        savebox.pack_start(self.save_entry, True, True, 0)
        savebox.pack_start(browse_btn, False, False, 0)
        save_box.pack_start(savebox, False, False, 0)

        ml_save = game_save_path(label)
        note = Gtk.Label(xalign=0, wrap=True)
        note.set_markup(f'<small>Saves stored in: <i>{ml_save}</i></small>')
        save_box.pack_start(note, False, False, 0)
        open_btn = Gtk.Button(label="Open save folder")
        open_btn.connect('clicked', self.on_open_saves)
        save_box.pack_start(open_btn, False, False, 0)

        self.settings_stack.add_titled(save_box, 'save', 'Save Directory')

        self.show_all()

    def on_offline_toggled(self, btn):
        # Launch options stay usable in both modes — only the WINEDLLOVERRIDES
        # rows are OnlineFix-specific.
        self._dll_box.set_sensitive(not self.offline_radio.get_active())

    def _add_dll_row(self, dll_name, mode, checked):
        row_box = Gtk.Box(spacing=6)
        cb = Gtk.CheckButton()
        cb.set_active(checked)
        dll_e = Gtk.Entry(width_chars=16, placeholder_text="dll name")
        dll_e.set_text(dll_name)
        sep = Gtk.Label(label="=")
        mode_e = Gtk.Entry(width_chars=6, placeholder_text="n,b…")
        mode_e.set_text(mode)
        rm_btn = Gtk.Button(label="×")
        rm_btn.get_style_context().add_class('flat')
        row_box.pack_start(cb,     False, False, 0)
        row_box.pack_start(dll_e,  True,  True,  0)
        row_box.pack_start(sep,    False, False, 0)
        row_box.pack_start(mode_e, False, False, 0)
        row_box.pack_start(rm_btn, False, False, 0)
        row_data = (cb, dll_e, mode_e)
        self.dll_rows.append(row_data)

        def on_remove(_btn, rb=row_box, rd=row_data):
            self._dll_box.remove(rb)
            self.dll_rows.remove(rd)
        rm_btn.connect('clicked', on_remove)

        # Insert before the "+ Add override" button
        children = self._dll_box.get_children()
        pos = len(children) - 1 if children else 0
        self._dll_box.pack_start(row_box, False, False, 0)
        self._dll_box.reorder_child(row_box, pos)

    def on_browse_save(self, _):
        start = str(WINEPREFIX_PATH / 'pfx' / 'drive_c' / 'users' / 'steamuser')
        if not os.path.isdir(start):
            start = str(WINEPREFIX_PATH / 'pfx')
        dialog = Gtk.FileChooserDialog(
            title="Select save directory in Proton prefix",
            parent=self,
            action=Gtk.FileChooserAction.SELECT_FOLDER)
        dialog.add_buttons(Gtk.STOCK_CANCEL, Gtk.ResponseType.CANCEL,
                           "Select", Gtk.ResponseType.OK)
        dialog.set_current_folder(start)
        if dialog.run() == Gtk.ResponseType.OK:
            self.save_entry.set_text(dialog.get_filename())
        dialog.destroy()

    def on_open_saves(self, _):
        ml_save = game_save_path(self.label)
        ml_save.mkdir(parents=True, exist_ok=True)
        subprocess.Popen(['xdg-open', str(ml_save)])

    def _refresh_cover_preview(self):
        cache_path = cover_cache_path(self.label)
        if cache_path.exists():
            try:
                pixbuf = GdkPixbuf.Pixbuf.new_from_file_at_scale(
                    str(cache_path), 160, 240, True)
                self.cover_preview.set_from_pixbuf(pixbuf)
                return
            except GLib.Error:
                pass
        self.cover_preview.set_pixel_size(160)
        self.cover_preview.set_from_icon_name('applications-games', Gtk.IconSize.DIALOG)

    def on_browse_cover(self, _):
        dialog = Gtk.FileChooserDialog(
            title="Select cover image",
            parent=self,
            action=Gtk.FileChooserAction.OPEN)
        dialog.add_buttons(Gtk.STOCK_CANCEL, Gtk.ResponseType.CANCEL,
                           "Select", Gtk.ResponseType.OK)
        img_filter = Gtk.FileFilter()
        img_filter.set_name("Images")
        for pattern in ('*.png', '*.jpg', '*.jpeg', '*.webp', '*.bmp'):
            img_filter.add_pattern(pattern)
        dialog.add_filter(img_filter)
        if dialog.run() == Gtk.ResponseType.OK:
            src = Path(dialog.get_filename())
            cache_path = cover_cache_path(self.label)
            cache_path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, cache_path)
            self._refresh_cover_preview()
        dialog.destroy()

    def on_clear_cover(self, _):
        cache_path = cover_cache_path(self.label)
        if cache_path.exists():
            cache_path.unlink()
        self._refresh_cover_preview()

    def get_result(self):
        dll_parts = []
        for cb, dll_e, mode_e in self.dll_rows:
            if cb.get_active():
                dll  = dll_e.get_text().strip()
                mode = mode_e.get_text().strip()
                if dll and mode:
                    dll_parts.append(f'{dll}={mode}')

        # Always record the WINEDLLOVERRIDES token, even empty — otherwise
        # "explicitly no overrides" (every row unchecked) is indistinguishable
        # from "never customized" once saved, and reopening resets every row
        # back to checked.
        env_parts = [f'WINEDLLOVERRIDES={";".join(dll_parts)}']
        extra = self.launch_opts_entry.get_text().strip()
        if extra:
            env_parts.append(extra)
        return {
            'NAME':       self.name_entry.get_text().strip(),
            'LAUNCH_ENV': ' '.join(env_parts),
            'SAVEDIR':    self.save_entry.get_text().strip(),
            'OFFLINE':    '1' if self.offline_radio.get_active() else '',
        }


class InstallDepsDialog(Gtk.Dialog):
    def __init__(self, parent, gamedir, proton_dirs):
        super().__init__(title="Install Dependencies", transient_for=parent, flags=0)
        self.set_default_size(580, 520)
        self.gamedir    = gamedir
        self.proton_dirs = proton_dirs

        self.add_button("Cancel",  Gtk.ResponseType.CANCEL)
        self.install_btn = self.add_button("Install", Gtk.ResponseType.OK)
        self.install_btn.get_style_context().add_class('suggested-action')

        box = self.get_content_area()
        box.set_spacing(0)

        # Proton picker
        proton_box = Gtk.Box(spacing=8, margin=12)
        proton_box.pack_start(Gtk.Label(label="Install into:"), False, False, 0)
        self.proton_combo = Gtk.ComboBoxText()
        for d in proton_dirs:
            self.proton_combo.append_text(d.name)
        self.proton_combo.set_active(0)
        proton_box.pack_start(self.proton_combo, True, True, 0)
        box.pack_start(proton_box, False, False, 0)

        box.pack_start(Gtk.Separator(), False, False, 0)

        # Run local exe option
        local_btn = Gtk.Button(label="Run .exe from game directory…", margin=12)
        local_btn.connect('clicked', self.on_run_local_exe)
        box.pack_start(local_btn, False, False, 0)

        box.pack_start(Gtk.Separator(), False, False, 0)

        # Winetricks package list
        label = Gtk.Label(label="Winetricks packages:", xalign=0,
                          margin_start=12, margin_top=8)
        label.get_style_context()
        box.pack_start(label, False, False, 0)

        scroll = Gtk.ScrolledWindow(margin=12, vexpand=True)
        scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        listbox = Gtk.ListBox()
        listbox.set_selection_mode(Gtk.SelectionMode.NONE)
        self.checks = {}
        for verb, desc in WINETRICKS_PACKAGES:
            row = Gtk.ListBoxRow()
            hbox = Gtk.Box(spacing=8, margin=6)
            cb = Gtk.CheckButton()
            hbox.pack_start(cb, False, False, 0)
            hbox.pack_start(Gtk.Label(label=f"{verb}  —  {desc}", xalign=0), True, True, 0)
            row.add(hbox)
            listbox.add(row)
            self.checks[verb] = cb
        scroll.add(listbox)
        box.pack_start(scroll, True, True, 0)

        self.show_all()

    def get_proton_path(self):
        idx = self.proton_combo.get_active()
        return self.proton_dirs[idx] if idx >= 0 else None

    def get_selected_verbs(self):
        return [v for v, cb in self.checks.items() if cb.get_active()]

    def on_run_local_exe(self, _):
        exes = get_all_exe_list(self.gamedir)
        if not exes:
            show_error(self, "No .exe files found in game directory.")
            return
        dialog = Gtk.Dialog(title="Select installer", transient_for=self, flags=0)
        dialog.set_default_size(500, 400)
        dialog.add_buttons(Gtk.STOCK_CANCEL, Gtk.ResponseType.CANCEL,
                           "Run", Gtk.ResponseType.OK)
        scroll = Gtk.ScrolledWindow(margin=12, vexpand=True)
        store = Gtk.ListStore(str)
        for e in exes:
            store.append([e])
        tv = Gtk.TreeView(model=store)
        tv.append_column(Gtk.TreeViewColumn("Executable",
                         Gtk.CellRendererText(), text=0))
        tv.set_headers_visible(False)
        scroll.add(tv)
        dialog.get_content_area().add(scroll)
        dialog.show_all()
        if dialog.run() == Gtk.ResponseType.OK:
            sel = tv.get_selection()
            model, it = sel.get_selected()
            if it:
                exe_label = model[it][0]
                exe_path  = str(Path(self.gamedir) / exe_label)
                proton    = self.get_proton_path()
                dialog.destroy()
                self.destroy()
                run_through_proton(exe_path, proton)
                return
        dialog.destroy()
