#!/usr/bin/env python3

import os
import re
import shutil
import subprocess
import threading
from pathlib import Path

import gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, GLib, Pango, Gdk

# ── Paths ─────────────────────────────────────────────────────────────────────
CONFIG_DIR      = Path.home() / '.config' / 'MonkeyLauncher'
CONFIG_FILE     = CONFIG_DIR / 'config'
GAMEDIRS_FILE   = CONFIG_DIR / 'gamedirs'
GAMES_DIR       = CONFIG_DIR / 'games'
SAVES_BASE      = CONFIG_DIR / 'saves'
STEAM_ROOT      = Path.home() / '.local' / 'share' / 'Steam'
WINEPREFIX_PATH = STEAM_ROOT / 'steamapps' / 'compatdata' / '480'

EXCLUDE_DIRS  = {'_CommonRedist', 'Binaries'}
EXCLUDE_NAMES = re.compile(r'CrashHandler', re.IGNORECASE)

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

# ── Game-dirs helpers ──────────────────────────────────────────────────────────
def read_gamedirs():
    if not GAMEDIRS_FILE.exists():
        return []
    return [p for p in GAMEDIRS_FILE.read_text().splitlines() if p.strip()]

def write_gamedirs(dirs):
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    GAMEDIRS_FILE.write_text('\n'.join(dirs) + ('\n' if dirs else ''))

# ── Config helpers ─────────────────────────────────────────────────────────────
def read_config(path):
    cfg = {}
    if Path(path).exists():
        for line in Path(path).read_text().splitlines():
            if '=' in line:
                k, v = line.split('=', 1)
                cfg[k.strip()] = v.strip()
    return cfg

def write_config(path, cfg):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text('\n'.join(f'{k}={v}' for k, v in cfg.items() if v) + '\n')

def game_key(label):
    return label.replace('/', '__').replace(' ', '_')

def game_config_path(label): return GAMES_DIR / game_key(label)
def game_save_path(label):   return SAVES_BASE / game_key(label)

# ── Steam helpers ──────────────────────────────────────────────────────────────
def get_steam_libs():
    vdf = STEAM_ROOT / 'steamapps' / 'libraryfolders.vdf'
    if not vdf.exists():
        return []
    return [Path(m.group(1))
            for m in re.finditer(r'"path"\s+"([^"]+)"', vdf.read_text())]

def get_proton_dirs():
    dirs = []
    for lib in get_steam_libs():
        common = lib / 'steamapps' / 'common'
        if common.exists():
            dirs += sorted(
                (d for d in common.iterdir() if d.is_dir() and d.name.startswith('Proton')),
                key=lambda d: d.name
            )
    return dirs

def get_exe_list(gamedir):
    base = Path(gamedir)
    result = []
    for exe in sorted(base.rglob('*.exe')):
        parts = exe.relative_to(base).parts
        if any(p in EXCLUDE_DIRS for p in parts):
            continue
        if EXCLUDE_NAMES.search(exe.name):
            continue
        result.append(str(exe.relative_to(base)))
    return result

def get_all_exe_list(gamedir):
    base = Path(gamedir)
    return sorted(str(e.relative_to(base)) for e in base.rglob('*.exe'))

def setup_save_symlink(prefix_savedir, ml_savedir):
    src, dst = Path(prefix_savedir), Path(ml_savedir)
    dst.mkdir(parents=True, exist_ok=True)
    if src.is_dir() and not src.is_symlink():
        for item in src.iterdir():
            shutil.copy2(item, dst / item.name)
        shutil.rmtree(src)
    if src.is_symlink():
        src.unlink()
    if not src.exists():
        src.symlink_to(dst)

# ── Dialogs ────────────────────────────────────────────────────────────────────
class GameSettingsDialog(Gtk.Dialog):
    def __init__(self, parent, label):
        super().__init__(title=f"Settings — {label}", transient_for=parent, flags=0)
        self.set_default_size(580, 580)
        self.label = label
        self.cfg = read_config(game_config_path(label))
        self.dll_rows = []   # list of (checkbox, dll_entry, mode_entry)

        self.add_button("Cancel", Gtk.ResponseType.CANCEL)
        self.add_button("Save",   Gtk.ResponseType.OK)
        self.set_default_response(Gtk.ResponseType.OK)

        # Parse existing LAUNCH_ENV
        existing_env = {}
        for pair in self.cfg.get('LAUNCH_ENV', '').split():
            if '=' in pair:
                k, v = pair.split('=', 1)
                existing_env[k] = v

        # Parse per-game WINEDLLOVERRIDES if present
        existing_dll = {}
        has_per_game_dll = 'WINEDLLOVERRIDES' in existing_env
        if has_per_game_dll:
            for part in existing_env.pop('WINEDLLOVERRIDES').split(';'):
                if '=' in part:
                    dll, mode = part.split('=', 1)
                    existing_dll[dll.strip()] = mode.strip()

        scroll = Gtk.ScrolledWindow()
        scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12, margin=16)
        scroll.add(outer)
        self.get_content_area().pack_start(scroll, True, True, 0)

        # ── DLL overrides ─────────────────────────────────────────────────────
        dll_frame = Gtk.Frame()
        dll_frame.set_label("WINEDLLOVERRIDES")
        self._dll_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL,
                                spacing=4, margin=10)

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
        dll_frame.add(self._dll_box)
        outer.pack_start(dll_frame, False, False, 0)

        # ── Launch options ────────────────────────────────────────────────────
        opts_row = Gtk.Box(spacing=8)
        opts_row.pack_start(Gtk.Label(label="Launch options:", xalign=0),
                            False, False, 0)
        existing_extra = ' '.join(f'{k}={v}' for k, v in existing_env.items())
        self.launch_opts_entry = Gtk.Entry(hexpand=True,
                                           placeholder_text="e.g. GAMEMODE=1 DRI_PRIME=1")
        self.launch_opts_entry.set_text(existing_extra)
        opts_row.pack_start(self.launch_opts_entry, True, True, 0)
        outer.pack_start(opts_row, False, False, 0)

        # ── Save directory ────────────────────────────────────────────────────
        save_frame = Gtk.Frame()
        save_frame.set_label("Save directory")
        save_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8, margin=10)

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

        save_frame.add(save_box)
        outer.pack_start(save_frame, False, False, 0)

        self.show_all()

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

    def get_result(self):
        dll_parts = []
        for cb, dll_e, mode_e in self.dll_rows:
            if cb.get_active():
                dll  = dll_e.get_text().strip()
                mode = mode_e.get_text().strip()
                if dll and mode:
                    dll_parts.append(f'{dll}={mode}')

        env_parts = []
        if dll_parts:
            env_parts.append(f'WINEDLLOVERRIDES={";".join(dll_parts)}')
        extra = self.launch_opts_entry.get_text().strip()
        if extra:
            env_parts.append(extra)
        return {
            'LAUNCH_ENV': ' '.join(env_parts),
            'SAVEDIR':    self.save_entry.get_text().strip(),
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


# ── Helpers ────────────────────────────────────────────────────────────────────
def show_error(parent, msg):
    d = Gtk.MessageDialog(transient_for=parent, flags=0,
                          message_type=Gtk.MessageType.ERROR,
                          buttons=Gtk.ButtonsType.OK, text=msg)
    d.run(); d.destroy()

def find_proton_wine(proton_path):
    for name in ('wine64', 'wine'):
        candidate = Path(proton_path) / 'files' / 'bin' / name
        if candidate.is_file():
            return candidate
    return None

def run_through_proton(exe_path, proton_path):
    env = os.environ.copy()
    env.update({
        'WINE':       str(proton / 'files' / 'bin' / 'wine64'),
        'WINESERVER': str(proton / 'files' / 'bin' / 'wineserver'),
        'WINEPREFIX': str(WINEPREFIX_PATH) + '/',
        'WINEDLLOVERRIDES':  'OnlineFix64=n;SteamOverlay64=n;winmm=n,b;dnet=n;steam_api64=n',
        'PROTONPATH': str(proton_path),
        'DXVK_STATE_CACHE':  '1',
        'GAMEID':     '480',
    })
    subprocess.Popen(['umu-run', exe_path], env=env)

# ── Main window ────────────────────────────────────────────────────────────────
class MonkeyLauncher(Gtk.ApplicationWindow):
    def __init__(self, app):
        super().__init__(application=app, title="MonkeyLauncher")
        self.set_default_size(700, 520)
        self.set_icon_name('applications-games')

        self.cfg           = read_config(CONFIG_FILE)
        self.proton_dirs   = get_proton_dirs()
        self.gamedirs      = read_gamedirs()
        self.mangohud      = False
        self.show_fullpath = False
        self._running_proc = None

        self._build_ui()
        self._load_games()
        self._check_installed_deps()
        self._load_installer_redist()

    # ── UI construction ────────────────────────────────────────────────────────
    def _build_ui(self):
        hb = Gtk.HeaderBar(show_close_button=True)
        self.set_titlebar(hb)

        self.stack = Gtk.Stack()
        self.stack.set_transition_type(Gtk.StackTransitionType.SLIDE_LEFT_RIGHT)
        switcher = Gtk.StackSwitcher()
        switcher.set_stack(self.stack)
        hb.set_custom_title(switcher)
        self.add(self.stack)

        # ── Library page ──────────────────────────────────────────────────────
        lib_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)

        # Search bar + path toggle side by side
        search_bar = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL,
                             margin_top=8, margin_bottom=8,
                             margin_start=8, margin_end=8, spacing=6)
        self.search_entry = Gtk.SearchEntry(hexpand=True,
                                            placeholder_text="Search games…")
        self.search_entry.connect('search-changed', self.on_search_changed)
        search_bar.pack_start(self.search_entry, True, True, 0)

        self.path_toggle = Gtk.ToggleButton(label="Show full path")
        self.path_toggle.set_active(False)
        self.path_toggle.set_tooltip_text("Toggle between filename only and full path")
        self.path_toggle.connect('toggled', self.on_path_toggle)
        search_bar.pack_start(self.path_toggle, False, False, 0)

        lib_box.pack_start(search_bar, False, False, 0)
        lib_box.pack_start(Gtk.Separator(), False, False, 0)

        scroll = Gtk.ScrolledWindow(vexpand=True)
        scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)

        # TreeStore: col0=display, col1=exe_relpath (empty for dir rows), col2=gamedir
        self.store = Gtk.TreeStore(str, str, str)
        self.filter = self.store.filter_new()
        self.filter.set_visible_func(self._game_filter)
        self.tv = Gtk.TreeView(model=self.filter, headers_visible=False)
        self.tv.append_column(Gtk.TreeViewColumn("Game", Gtk.CellRendererText(), text=0))
        self.tv.connect('row-activated', self.on_game_activated)
        self.tv.connect('button-press-event', self.on_tree_button_press)
        self.tv.get_selection().connect('changed', self.on_selection_changed)
        scroll.add(self.tv)

        # Overlay the scroll area with a centered spinner shown during scanning
        overlay = Gtk.Overlay(vexpand=True)
        overlay.add(scroll)
        self.spinner = Gtk.Spinner(halign=Gtk.Align.CENTER, valign=Gtk.Align.CENTER,
                                   width_request=48, height_request=48)
        overlay.add_overlay(self.spinner)
        lib_box.pack_start(overlay, True, True, 0)

        add_dir_btn = Gtk.Button(label="+ Add game directory")
        add_dir_btn.connect('clicked', self.on_add_game_dir)
        lib_box.pack_start(add_dir_btn, False, False, 0)

        lib_box.pack_start(Gtk.Separator(), False, False, 0)

        bottom = Gtk.Box(spacing=8, margin=10)
        lib_box.pack_start(bottom, False, False, 0)

        self.mangohud_btn = Gtk.ToggleButton(label="MangoHud")
        self.mangohud_btn.connect('toggled', lambda b: setattr(self, 'mangohud', b.get_active()))
        bottom.pack_start(self.mangohud_btn, False, False, 0)

        self.game_settings_btn = Gtk.Button(label="Game Settings")
        self.game_settings_btn.set_sensitive(False)
        self.game_settings_btn.connect('clicked', self.on_game_settings)
        bottom.pack_start(self.game_settings_btn, False, False, 0)

        bottom.pack_start(Gtk.Box(), True, True, 0)  # spacer

        self.launch_btn = Gtk.Button(label="Launch")
        self.launch_btn.get_style_context().add_class('suggested-action')
        self.launch_btn.set_sensitive(False)
        self.launch_btn.connect('clicked', self.on_launch)
        bottom.pack_start(self.launch_btn, False, False, 0)

        self.stack.add_titled(lib_box, 'library', 'Library')

        # ── Settings page ─────────────────────────────────────────────────────
        settings_scroll = Gtk.ScrolledWindow()
        settings_scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        settings_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        settings_scroll.add(settings_box)

        proton_frame = Gtk.Frame(margin_start=16, margin_end=16,
                                 margin_top=16, margin_bottom=8)
        proton_frame.set_label("Proton")
        proton_inner = Gtk.Box(spacing=8, margin=12)
        proton_inner.pack_start(Gtk.Label(label="Version:"), False, False, 0)
        self.proton_combo = Gtk.ComboBoxText(hexpand=True)
        self._populate_proton_combo()
        self.proton_combo.connect('changed', self.on_proton_changed)
        proton_inner.pack_start(self.proton_combo, True, True, 0)
        proton_frame.add(proton_inner)
        settings_box.pack_start(proton_frame, False, False, 0)

        # Run installer inline section
        inst_frame = Gtk.Frame(margin_start=16, margin_end=16,
                               margin_top=8, margin_bottom=8)
        inst_frame.set_label("Run installer .exe")
        inst_outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8, margin=12)

        inst_top = Gtk.Box(spacing=8)
        self.installer_source_lbl = Gtk.Label(xalign=0, hexpand=True, wrap=True)
        inst_browse_btn = Gtk.Button(label="Browse directory…")
        inst_browse_btn.connect('clicked', self.on_installer_browse)
        inst_top.pack_start(self.installer_source_lbl, True, True, 0)
        inst_top.pack_start(inst_browse_btn, False, False, 0)
        inst_outer.pack_start(inst_top, False, False, 0)

        inst_scroll = Gtk.ScrolledWindow(height_request=180)
        inst_scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        self.installer_checks = {}
        self.installer_status = {}
        self.installer_list = Gtk.ListBox()
        self.installer_list.set_selection_mode(Gtk.SelectionMode.NONE)
        inst_scroll.add(self.installer_list)
        inst_outer.pack_start(inst_scroll, True, True, 0)

        self.installer_run_btn = Gtk.Button(label="Run selected")
        self.installer_run_btn.get_style_context().add_class('suggested-action')
        self.installer_run_btn.set_sensitive(False)
        self.installer_run_btn.connect('clicked', self.on_run_installer)
        inst_outer.pack_start(self.installer_run_btn, False, False, 0)

        inst_frame.add(inst_outer)
        settings_box.pack_start(inst_frame, False, False, 0)

        # Winetricks inline section
        deps_frame = Gtk.Frame(margin_start=16, margin_end=16,
                               margin_top=8, margin_bottom=8)
        deps_frame.set_label("Winetricks dependencies")
        deps_outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8, margin=12)

        deps_scroll = Gtk.ScrolledWindow(height_request=220)
        deps_scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        deps_list = Gtk.ListBox()
        deps_list.set_selection_mode(Gtk.SelectionMode.NONE)
        self.winetricks_checks = {}
        self.winetricks_status = {}  # verb → (spinner, status_label)
        for verb, desc in WINETRICKS_PACKAGES:
            row = Gtk.ListBoxRow()
            hbox = Gtk.Box(spacing=8, margin=6)
            cb = Gtk.CheckButton()
            hbox.pack_start(cb, False, False, 0)
            hbox.pack_start(Gtk.Label(label=f"{verb}  —  {desc}", xalign=0), True, True, 0)
            spinner = Gtk.Spinner(width_request=16, height_request=16, no_show_all=True)
            status_lbl = Gtk.Label(no_show_all=True)
            hbox.pack_end(status_lbl, False, False, 0)
            hbox.pack_end(spinner,    False, False, 0)
            row.add(hbox)
            deps_list.add(row)
            self.winetricks_checks[verb] = cb
            self.winetricks_status[verb] = (spinner, status_lbl)
        deps_scroll.add(deps_list)
        deps_outer.pack_start(deps_scroll, True, True, 0)

        self.install_deps_btn = Gtk.Button(label="Install selected")
        self.install_deps_btn.get_style_context().add_class('suggested-action')
        self.install_deps_btn.connect('clicked', self.on_install_deps)
        deps_outer.pack_start(self.install_deps_btn, False, False, 0)

        deps_frame.add(deps_outer)
        settings_box.pack_start(deps_frame, False, False, 0)

        reset_btn = Gtk.Button(label="Reset all config…",
                               margin_start=16, margin_end=16,
                               margin_top=8, margin_bottom=16)
        reset_btn.get_style_context().add_class('destructive-action')
        reset_btn.connect('clicked', self.on_reset)
        settings_box.pack_end(reset_btn, False, False, 0)

        self.stack.add_titled(settings_scroll, 'settings', 'Settings')

    def _display_name(self, exe, gcfg):
        return exe if self.show_fullpath else Path(exe).name

    def _refresh_display_names(self):
        it = self.store.get_iter_first()
        while it:
            child = self.store.iter_children(it)
            while child:
                exe = self.store[child][1]
                if exe:
                    gcfg = read_config(game_config_path(exe))
                    self.store[child][0] = self._display_name(exe, gcfg)
                child = self.store.iter_next(child)
            it = self.store.iter_next(it)

    def _populate_proton_combo(self):
        self.proton_combo.remove_all()
        saved = self.cfg.get('PROTONPATH', '')
        active = 0
        for i, d in enumerate(self.proton_dirs):
            self.proton_combo.append_text(d.name)
            if saved and str(d) == saved:
                active = i
        self.proton_combo.set_active(active)

    def _game_filter(self, model, it, _):
        query = self.search_entry.get_text().lower()
        if not query:
            return True
        # Dir row: visible if any child game matches
        if not model[it][1]:
            child = model.iter_children(it)
            while child:
                if query in model[child][0].lower():
                    return True
                child = model.iter_next(child)
            return False
        return query in model[it][0].lower()

    # ── Data loading ───────────────────────────────────────────────────────────
    def _load_games(self):
        self.store.clear()
        self.launch_btn.set_sensitive(False)
        self.game_settings_btn.set_sensitive(False)
        self.spinner.start()

        def scan():
            results = []
            for gamedir in list(self.gamedirs):
                if not os.path.isdir(gamedir):
                    continue
                exes = []
                for exe in get_exe_list(gamedir):
                    gcfg = read_config(game_config_path(exe))
                    if gcfg.get('HIDDEN') == '1':
                        continue
                    exes.append((exe, gcfg, gamedir))
                if exes:
                    results.append((gamedir, exes))
            GLib.idle_add(self._apply_games, results)

        threading.Thread(target=scan, daemon=True).start()

    def _apply_games(self, results):
        for gamedir, exes in results:
            parent = self.store.append(None, [Path(gamedir).name, '', gamedir])
            for exe, gcfg, gdir in exes:
                self.store.append(parent, [self._display_name(exe, gcfg), exe, gdir])
        self.tv.expand_all()
        self.spinner.stop()
        return False  # remove from idle queue

    def _selected_game(self):
        """Returns (exe_relpath, gamedir) for the selected game row, or (None, None)."""
        model, it = self.tv.get_selection().get_selected()
        if it and model[it][1]:
            return model[it][1], model[it][2]
        return None, None

    def _selected_proton(self):
        idx = self.proton_combo.get_active()
        if 0 <= idx < len(self.proton_dirs):
            return self.proton_dirs[idx]
        return None

    # ── Signals ────────────────────────────────────────────────────────────────
    def on_search_changed(self, _):
        self.filter.refilter()
        self.tv.expand_all()

    def on_selection_changed(self, _):
        exe, _ = self._selected_game()
        self.game_settings_btn.set_sensitive(exe is not None)
        if self._running_proc is None:
            self.launch_btn.set_sensitive(exe is not None)

    def on_path_toggle(self, btn):
        self.show_fullpath = btn.get_active()
        btn.set_label("Show name only" if self.show_fullpath else "Show full path")
        self._refresh_display_names()

    def on_game_activated(self, tv, path, col):
        model, it = tv.get_selection().get_selected()
        if it and not model[it][1]:  # dir row — toggle expand
            if tv.row_expanded(path):
                tv.collapse_row(path)
            else:
                tv.expand_row(path, False)
        else:
            self.on_launch(None)

    def on_tree_button_press(self, tv, event):
        if event.button != 3:
            return False
        info = tv.get_path_at_pos(int(event.x), int(event.y))
        if not info:
            return False
        path, *_ = info
        tv.get_selection().select_path(path)
        model, it = tv.get_selection().get_selected()
        if not it:
            return False

        menu = Gtk.Menu()
        if not model[it][1]:  # dir row
            gamedir = model[it][2]
            rescan_item = Gtk.MenuItem(label="Rescan directory")
            rescan_item.connect('activate', lambda _: self.on_rescan_game_dir(gamedir))
            menu.append(rescan_item)
            remove_item = Gtk.MenuItem(label=f'Remove \'{Path(gamedir).name}\' from library')
            remove_item.connect('activate', lambda _: self.on_remove_game_dir(gamedir))
            menu.append(remove_item)
        else:                  # game row
            exe = model[it][1]
            settings_item = Gtk.MenuItem(label="Game Settings")
            settings_item.connect('activate', lambda _: self.on_game_settings(None))
            menu.append(settings_item)
            hide_item = Gtk.MenuItem(label="Remove from list")
            hide_item.connect('activate', lambda _: self.on_hide_game(exe))
            menu.append(hide_item)

        menu.show_all()
        menu.popup_at_pointer(event)
        return False

    def on_hide_game(self, exe):
        gcfg = read_config(game_config_path(exe))
        gcfg['HIDDEN'] = '1'
        write_config(game_config_path(exe), gcfg)
        self._load_games()

    def on_launch(self, _):
        if self._running_proc is not None:
            self._running_proc.terminate()
            return

        exe, gamedir = self._selected_game()
        proton = self._selected_proton()
        if not exe or not proton:
            show_error(self, "Select a game and a Proton version first.")
            return

        game_path  = str(Path(gamedir) / exe)
        game_cfg   = read_config(game_config_path(exe))
        savedir    = game_cfg.get('SAVEDIR', '')
        launch_env = game_cfg.get('LAUNCH_ENV', '')

        if savedir and os.path.isdir(os.path.dirname(savedir)):
            try:
                setup_save_symlink(savedir, game_save_path(exe))
            except Exception as e:
                print(f"Save symlink warning: {e}")

        env = os.environ.copy()
        env.update({
            'WINE':              str(proton / 'files' / 'bin' / 'wine64'),
            'WINESERVER':        str(proton / 'files' / 'bin' / 'wineserver'),
            'WINEPREFIX':        str(WINEPREFIX_PATH) + '/',
            'WINEDLLOVERRIDES':  'OnlineFix64=n;SteamOverlay64=n;winmm=n,b;dnet=n;steam_api64=n',
            'GAMEID':            '480',
            'PROTONPATH':        str(proton),
            'DXVK_STATE_CACHE':  '1',
            'MANGOHUD':          '1' if self.mangohud else '0',
        })
        # Per-game overrides applied after defaults so they take effect
        if launch_env:
            for pair in launch_env.split():
                if '=' in pair:
                    k, v = pair.split('=', 1)
                    env[k] = v

        proc = subprocess.Popen(['umu-run', game_path], env=env)
        self._running_proc = proc

        self.launch_btn.set_label("Stop")
        self.launch_btn.get_style_context().remove_class('suggested-action')
        self.launch_btn.get_style_context().add_class('destructive-action')
        self.launch_btn.set_sensitive(True)

        GLib.child_watch_add(GLib.PRIORITY_DEFAULT, proc.pid, self._on_game_exit)

    def _on_game_exit(self, pid, _status):
        self._running_proc = None
        self.launch_btn.set_label("Launch")
        self.launch_btn.get_style_context().remove_class('destructive-action')
        self.launch_btn.get_style_context().add_class('suggested-action')
        exe, _ = self._selected_game()
        self.launch_btn.set_sensitive(exe is not None)

    def on_game_settings(self, _):
        exe, _ = self._selected_game()
        if not exe:
            return
        dialog = GameSettingsDialog(self, exe)
        if dialog.run() == Gtk.ResponseType.OK:
            write_config(game_config_path(exe), dialog.get_result())
        dialog.destroy()

    def on_add_game_dir(self, _):
        dialog = Gtk.FileChooserDialog(
            title="Select game directory",
            parent=self,
            action=Gtk.FileChooserAction.SELECT_FOLDER)
        dialog.add_buttons(Gtk.STOCK_CANCEL, Gtk.ResponseType.CANCEL,
                           "Select", Gtk.ResponseType.OK)
        dialog.set_current_folder(self.gamedirs[-1] if self.gamedirs else str(Path.home()))
        if dialog.run() == Gtk.ResponseType.OK:
            chosen = dialog.get_filename()
            if chosen not in self.gamedirs:
                self.gamedirs.append(chosen)
                write_gamedirs(self.gamedirs)
                self._load_games()
                self._load_installer_redist()
        dialog.destroy()

    def on_remove_game_dir(self, gamedir):
        self.gamedirs = [d for d in self.gamedirs if d != gamedir]
        write_gamedirs(self.gamedirs)
        self._load_games()
        self._load_installer_redist()

    def on_rescan_game_dir(self, gamedir):
        self.spinner.start()

        def scan():
            exes = []
            if os.path.isdir(gamedir):
                for exe in get_exe_list(gamedir):
                    gcfg = read_config(game_config_path(exe))
                    if gcfg.get('HIDDEN') == '1':
                        continue
                    exes.append((exe, gcfg))
            GLib.idle_add(self._apply_rescan, gamedir, exes)

        threading.Thread(target=scan, daemon=True).start()

    def _apply_rescan(self, gamedir, exes):
        it = self.store.get_iter_first()
        while it:
            if self.store[it][2] == gamedir:
                while self.store.iter_has_child(it):
                    self.store.remove(self.store.iter_children(it))
                for exe, gcfg in exes:
                    self.store.append(it, [self._display_name(exe, gcfg), exe, gamedir])
                self.tv.expand_row(self.store.get_path(it), False)
                break
            it = self.store.iter_next(it)
        self.spinner.stop()
        return False

    def on_proton_changed(self, combo):
        idx = combo.get_active()
        if idx >= 0:
            self.cfg['PROTONPATH'] = str(self.proton_dirs[idx])
            write_config(CONFIG_FILE, self.cfg)
        self._check_installed_deps()

    def _check_installed_deps(self):
        for verb in self.winetricks_checks:
            sp, lbl = self.winetricks_status[verb]
            sp.stop(); sp.hide()
            lbl.hide()
        self.install_deps_btn.set_sensitive(False)
        self.install_deps_btn.set_label("Checking installed…")

        def worker():
            try:
                r = subprocess.run(
                    ['protontricks', '--no-bwrap', '480', 'list-installed'],
                    capture_output=True, text=True, timeout=30)
                installed = {line.strip() for line in r.stdout.splitlines() if line.strip()}
            except Exception:
                installed = set()
            GLib.idle_add(self._apply_installed_deps, installed)

        threading.Thread(target=worker, daemon=True).start()

    def _apply_installed_deps(self, installed):
        for verb in self.winetricks_checks:
            if verb in installed:
                self._wt_set_status(verb, 'ok')
        if self.install_deps_btn.get_label() == "Checking installed…":
            self.install_deps_btn.set_label("Install selected")
            self.install_deps_btn.set_sensitive(True)
        return False

    def on_install_deps(self, _):
        proton = self._selected_proton()
        if not proton:
            show_error(self, "No Proton version selected.")
            return
        verbs = [v for v, cb in self.winetricks_checks.items() if cb.get_active()]
        if not verbs:
            show_error(self, "Select at least one package to install.")
            return
        self.install_deps_btn.set_sensitive(False)
        self.install_deps_btn.set_label("Please wait…")
        self._run_winetricks(verbs, proton)

    def _run_winetricks(self, verbs, _proton):
        def worker():
            for verb in verbs:
                GLib.idle_add(self._wt_set_status, verb, 'running')
                result = subprocess.run(
                    ['protontricks', '--no-bwrap', '480', verb])
                ok = result.returncode == 0
                GLib.idle_add(self._wt_set_status, verb, 'ok' if ok else 'error')
                if ok:
                    GLib.idle_add(self.winetricks_checks[verb].set_active, False)
            GLib.idle_add(self.install_deps_btn.set_label, "Install selected")
            GLib.idle_add(self.install_deps_btn.set_sensitive, True)

        threading.Thread(target=worker, daemon=True).start()

    def _wt_set_status(self, verb, status):
        spinner, lbl = self.winetricks_status[verb]
        if status == 'running':
            lbl.hide()
            spinner.show()
            spinner.start()
        elif status == 'ok':
            spinner.stop(); spinner.hide()
            lbl.set_markup('<span color="#57a773" size="x-large">✓</span>')
            lbl.show()
        elif status == 'error':
            spinner.stop(); spinner.hide()
            lbl.set_markup('<span color="#e05c5c" size="x-large">✗</span>')
            lbl.show()
        return False

    def _populate_installer(self, roots):
        for child in self.installer_list.get_children():
            self.installer_list.remove(child)
        self.installer_checks.clear()
        self.installer_status.clear()

        for label, base_str in roots:
            base = Path(base_str)
            groups = {}
            for exe in sorted(base.rglob('*.exe')):
                folder = str(exe.relative_to(base).parent)
                groups.setdefault(folder, []).append(exe)
            if not groups:
                continue

            hdr = Gtk.ListBoxRow(selectable=False, activatable=False)
            hdr_lbl = Gtk.Label(label=label, xalign=0,
                                margin_start=6, margin_top=4, margin_bottom=2)
            hdr_lbl.get_style_context().add_class('dim-label')
            hdr.add(hdr_lbl)
            self.installer_list.add(hdr)

            for exe in groups.pop('.', []):
                self._add_installer_row(exe)
            for folder in sorted(groups):
                sfhdr = Gtk.ListBoxRow(selectable=False, activatable=False)
                sfhdr.add(Gtk.Label(label=folder, xalign=0,
                                    margin_start=14, margin_top=2, margin_bottom=1))
                self.installer_list.add(sfhdr)
                for exe in groups[folder]:
                    self._add_installer_row(exe, indent=True)

        self.installer_list.show_all()
        self._update_installer_run_btn()

    def _add_installer_row(self, exe, indent=False):
        exe_path = str(exe)
        row = Gtk.ListBoxRow()
        hbox = Gtk.Box(spacing=8, margin=6,
                       margin_start=22 if indent else 10)
        cb = Gtk.CheckButton()
        cb.connect('toggled', lambda _: self._update_installer_run_btn())
        hbox.pack_start(cb, False, False, 0)
        hbox.pack_start(Gtk.Label(label=exe.name, xalign=0), True, True, 0)
        spinner = Gtk.Spinner(width_request=16, height_request=16, no_show_all=True)
        status_lbl = Gtk.Label(no_show_all=True)
        hbox.pack_end(status_lbl, False, False, 0)
        hbox.pack_end(spinner,    False, False, 0)
        row.add(hbox)
        self.installer_list.add(row)
        self.installer_checks[exe_path] = cb
        self.installer_status[exe_path] = (spinner, status_lbl)

    def _load_installer_redist(self):
        roots = []
        for gd in self.gamedirs:
            for rd in sorted(Path(gd).rglob('_CommonRedist')):
                roots.append((f"{Path(gd).name} › {rd.relative_to(gd)}", str(rd)))
        if roots:
            self.installer_source_lbl.set_markup("<b>_CommonRedist</b> from game directories")
            self._populate_installer(roots)
        else:
            self.installer_source_lbl.set_markup(
                "<i>No _CommonRedist found — use Browse to pick a folder</i>")
            self._populate_installer([])

    def on_installer_browse(self, _):
        fc = Gtk.FileChooserDialog(
            title="Select directory",
            parent=self,
            action=Gtk.FileChooserAction.SELECT_FOLDER)
        fc.add_buttons(Gtk.STOCK_CANCEL, Gtk.ResponseType.CANCEL,
                       "Select", Gtk.ResponseType.OK)
        fc.set_current_folder(self.gamedirs[0] if self.gamedirs else str(Path.home()))
        if fc.run() == Gtk.ResponseType.OK:
            chosen = fc.get_filename()
            self.installer_source_lbl.set_markup(f"<i>{chosen}</i>")
            self._populate_installer([(Path(chosen).name, chosen)])
        fc.destroy()

    def _update_installer_run_btn(self):
        any_checked = any(cb.get_active() for cb in self.installer_checks.values())
        if self.installer_run_btn.get_label() != "Please wait…":
            self.installer_run_btn.set_sensitive(any_checked)

    def on_run_installer(self, _):
        proton = self._selected_proton()
        if not proton:
            show_error(self, "No Proton version selected.")
            return

        checked = [p for p, cb in self.installer_checks.items() if cb.get_active()]
        if not checked:
            return

        self.installer_run_btn.set_sensitive(False)
        self.installer_run_btn.set_label("Please wait…")

        env = os.environ.copy()
        env.update({
            'WINEPREFIX': str(WINEPREFIX_PATH) + '/',
            'PROTONPATH':  str(proton),
            'GAMEID':      '0',
        })

        def worker():
            for exe_path in checked:
                GLib.idle_add(self._set_installer_status, exe_path, 'running')
                result = subprocess.run(['umu-run', exe_path], env=env)
                ok = result.returncode == 0
                GLib.idle_add(self._set_installer_status, exe_path, 'ok' if ok else 'error')
                if ok:
                    GLib.idle_add(self._uncheck_installer, exe_path)
            GLib.idle_add(self.installer_run_btn.set_label, "Run selected")
            GLib.idle_add(self._update_installer_run_btn)

        threading.Thread(target=worker, daemon=True).start()

    def _set_installer_status(self, exe_path, status):
        if exe_path not in self.installer_status:
            return False
        spinner, lbl = self.installer_status[exe_path]
        if status == 'running':
            lbl.hide()
            spinner.show()
            spinner.start()
        elif status == 'ok':
            spinner.stop(); spinner.hide()
            lbl.set_markup('<span color="#57a773" size="x-large">✓</span>')
            lbl.show()
        elif status == 'error':
            spinner.stop(); spinner.hide()
            lbl.set_markup('<span color="#e05c5c" size="x-large">✗</span>')
            lbl.show()
        return False

    def _uncheck_installer(self, exe_path):
        if exe_path in self.installer_checks:
            self.installer_checks[exe_path].set_active(False)
        return False

    def on_reset(self, _):
        dialog = Gtk.MessageDialog(
            transient_for=self, flags=0,
            message_type=Gtk.MessageType.WARNING,
            buttons=Gtk.ButtonsType.YES_NO,
            text="Reset all config?")
        dialog.format_secondary_text(
            "This will clear all game directories, Proton preference, "
            "and per-game settings. Save files are NOT deleted.")
        if dialog.run() == Gtk.ResponseType.YES:
            if CONFIG_FILE.exists():   CONFIG_FILE.unlink()
            if GAMEDIRS_FILE.exists(): GAMEDIRS_FILE.unlink()
            if GAMES_DIR.exists():     shutil.rmtree(GAMES_DIR)
            self.cfg      = {}
            self.gamedirs = []
            self.store.clear()
        dialog.destroy()


# ── Startup checks ─────────────────────────────────────────────────────────────
def check_steam_running():
    try:
        result = subprocess.run(['pgrep', '-x', 'steam'], capture_output=True)
        return result.returncode == 0
    except Exception:
        return False

def check_app480_installed():
    manifest = STEAM_ROOT / 'steamapps' / 'appmanifest_480.acf'
    return manifest.exists()

def wait_for_steam(parent=None):
    d = Gtk.MessageDialog(
        transient_for=parent, flags=0,
        message_type=Gtk.MessageType.INFO,
        buttons=Gtk.ButtonsType.CANCEL,
        text="Starting Steam…")
    d.format_secondary_text("Waiting for Steam to launch before continuing.")
    d.show()

    subprocess.Popen(['steam'])

    def poll():
        if check_steam_running():
            d.response(Gtk.ResponseType.OK)
            return False
        return True

    GLib.timeout_add(1500, poll)
    response = d.run()
    d.destroy()
    return response != Gtk.ResponseType.CANCEL

def wait_for_spacewar(parent=None):
    d = Gtk.MessageDialog(
        transient_for=parent, flags=0,
        message_type=Gtk.MessageType.INFO,
        buttons=Gtk.ButtonsType.CANCEL,
        text="Installing Spacewar (App 480)…")
    d.format_secondary_text("Waiting for the installation to finish before continuing.")
    d.show()

    subprocess.Popen(['steam', 'steam://install/480'])

    def poll():
        if check_app480_installed():
            d.response(Gtk.ResponseType.OK)
            return False
        return True

    GLib.timeout_add(2000, poll)
    response = d.run()
    d.destroy()
    return response != Gtk.ResponseType.CANCEL

def run_startup_checks(parent=None):
    if not check_steam_running():
        d = Gtk.MessageDialog(
            transient_for=parent, flags=0,
            message_type=Gtk.MessageType.QUESTION,
            buttons=Gtk.ButtonsType.YES_NO,
            text="Steam is not running")
        d.format_secondary_text("Launch Steam now and wait for it to start?")
        response = d.run(); d.destroy()
        if response != Gtk.ResponseType.YES:
            return False
        if not wait_for_steam(parent):
            return False

    if not check_app480_installed():
        d = Gtk.MessageDialog(
            transient_for=parent, flags=0,
            message_type=Gtk.MessageType.QUESTION,
            buttons=Gtk.ButtonsType.YES_NO,
            text="Spacewar (App 480) is not installed")
        d.format_secondary_text(
            "App 480 is required for the Proton prefix.\n"
            "Open Steam to install it now and wait?")
        response = d.run(); d.destroy()
        if response != Gtk.ResponseType.YES:
            return False
        if not wait_for_spacewar(parent):
            return False

    return True

# ── App entry point ────────────────────────────────────────────────────────────
class App(Gtk.Application):
    def __init__(self):
        super().__init__(application_id='com.monkeylauncher.app')

    def do_activate(self):
        if not run_startup_checks():
            self.quit()
            return
        win = MonkeyLauncher(self)
        win.show_all()
        win.present()

if __name__ == '__main__':
    App().run()
