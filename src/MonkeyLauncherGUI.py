#!/usr/bin/env python3

import os
import sys

# When bundled by PyInstaller the typelibs are packed alongside the binary;
# point GI to them before the first require_version call.
if getattr(sys, 'frozen', False):
    _bundle = sys._MEIPASS
    _typelibs = os.path.join(_bundle, 'gi_typelibs')
    if os.path.isdir(_typelibs):
        os.environ['GI_TYPELIB_PATH'] = _typelibs

import gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, GLib, Pango, Gdk

import re
import shutil
import subprocess
from pathlib import Path

# ── Paths ─────────────────────────────────────────────────────────────────────
CONFIG_DIR      = Path.home() / '.config' / 'MonkeyLauncher'
CONFIG_FILE     = CONFIG_DIR / 'config'
GAMES_DIR       = CONFIG_DIR / 'games'
SAVES_BASE      = CONFIG_DIR / 'saves'
STEAM_ROOT      = Path.home() / '.local' / 'share' / 'Steam'
WINEPREFIX_PATH = STEAM_ROOT / 'steamapps' / 'compatdata' / '480'

EXCLUDE_DIRS  = {'_CommonRedist', 'Binaries'}
EXCLUDE_NAMES = re.compile(r'CrashHandler', re.IGNORECASE)

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
        self.set_default_size(500, 280)
        self.label = label
        self.cfg = read_config(game_config_path(label))

        self.add_button("Cancel", Gtk.ResponseType.CANCEL)
        self.add_button("Save",   Gtk.ResponseType.OK)
        self.set_default_response(Gtk.ResponseType.OK)

        grid = Gtk.Grid(column_spacing=12, row_spacing=12, margin=18)
        self.get_content_area().add(grid)

        # Launch env
        grid.attach(Gtk.Label(label="Launch env vars:", xalign=1), 0, 0, 1, 1)
        self.env_entry = Gtk.Entry(hexpand=True,
                                   placeholder_text="e.g. DRI_PRIME=1 GAMEMODE=1")
        self.env_entry.set_text(self.cfg.get('LAUNCH_ENV', ''))
        grid.attach(self.env_entry, 1, 0, 1, 1)

        # Save dir (prefix path)
        grid.attach(Gtk.Label(label="Prefix save dir:", xalign=1), 0, 1, 1, 1)
        savebox = Gtk.Box(spacing=6, hexpand=True)
        self.save_entry = Gtk.Entry(hexpand=True,
                                    placeholder_text="Path inside Proton prefix")
        self.save_entry.set_text(self.cfg.get('SAVEDIR', ''))
        browse_btn = Gtk.Button(label="Browse…")
        browse_btn.connect('clicked', self.on_browse_save)
        savebox.pack_start(self.save_entry, True, True, 0)
        savebox.pack_start(browse_btn, False, False, 0)
        grid.attach(savebox, 1, 1, 1, 1)

        ml_save = game_save_path(label)
        note = Gtk.Label(xalign=0, wrap=True)
        note.set_markup(f'<small>Saves stored in: <i>{ml_save}</i></small>')
        grid.attach(note, 1, 2, 1, 1)

        # Open saves button
        open_btn = Gtk.Button(label="Open save folder")
        open_btn.connect('clicked', self.on_open_saves)
        grid.attach(open_btn, 1, 3, 1, 1)

        self.show_all()

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
        return {
            'LAUNCH_ENV': self.env_entry.get_text().strip(),
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

def run_through_proton(exe_path, proton_path):
    env = os.environ.copy()
    env.update({
        'WINE':        str(proton_path / 'files' / 'bin' / 'wine64'),
        'WINESERVER':  str(proton_path / 'files' / 'bin' / 'wineserver'),
        'WINEPREFIX':  str(WINEPREFIX_PATH) + '/',
    })
    subprocess.Popen(['umu-run', exe_path], env=env)

# ── Main window ────────────────────────────────────────────────────────────────
class MonkeyLauncher(Gtk.ApplicationWindow):
    def __init__(self, app):
        super().__init__(application=app, title="MonkeyLauncher")
        self.set_default_size(700, 520)
        self.set_icon_name('applications-games')

        self.cfg         = read_config(CONFIG_FILE)
        self.proton_dirs = get_proton_dirs()
        self.all_exes    = []
        self.mangohud    = False

        self._build_ui()
        self._load_games()

    # ── UI construction ────────────────────────────────────────────────────────
    def _build_ui(self):
        # Header bar
        hb = Gtk.HeaderBar(show_close_button=True)
        hb.set_title("MonkeyLauncher")
        self.set_titlebar(hb)

        # Settings menu
        menu_btn = Gtk.MenuButton()
        menu_btn.set_image(Gtk.Image.new_from_icon_name('open-menu-symbolic',
                                                         Gtk.IconSize.BUTTON))
        menu = Gtk.Menu()
        for label, cb in [
            ("Setup game directory",  self.on_setup_dir),
            ("Set favorite Proton",   self.on_setup_proton),
            (None, None),
            ("Install dependencies",  self.on_install_deps),
            (None, None),
            ("Reset all config",      self.on_reset),
        ]:
            if label is None:
                menu.append(Gtk.SeparatorMenuItem())
            else:
                item = Gtk.MenuItem(label=label)
                item.connect('activate', cb)
                menu.append(item)
        menu.show_all()
        menu_btn.set_popup(menu)
        hb.pack_end(menu_btn)

        # Main layout
        vbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.add(vbox)

        # Search
        self.search_entry = Gtk.SearchEntry(margin=10,
                                            placeholder_text="Search games…")
        self.search_entry.connect('search-changed', self.on_search_changed)
        vbox.pack_start(self.search_entry, False, False, 0)

        vbox.pack_start(Gtk.Separator(), False, False, 0)

        # Game list
        scroll = Gtk.ScrolledWindow(vexpand=True)
        scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        self.store = Gtk.ListStore(str)
        self.filter = self.store.filter_new()
        self.filter.set_visible_func(self._game_filter)
        self.tv = Gtk.TreeView(model=self.filter, headers_visible=False)
        col = Gtk.TreeViewColumn("Game", Gtk.CellRendererText(), text=0)
        self.tv.append_column(col)
        self.tv.connect('row-activated', self.on_game_activated)
        self.tv.get_selection().connect('changed', self.on_selection_changed)
        scroll.add(self.tv)
        vbox.pack_start(scroll, True, True, 0)

        vbox.pack_start(Gtk.Separator(), False, False, 0)

        # Bottom bar
        bottom = Gtk.Box(spacing=10, margin=10)
        vbox.pack_start(bottom, False, False, 0)

        # Proton selector
        bottom.pack_start(Gtk.Label(label="Proton:"), False, False, 0)
        self.proton_combo = Gtk.ComboBoxText(hexpand=True)
        self._populate_proton_combo()
        bottom.pack_start(self.proton_combo, True, True, 0)

        # MangoHud toggle
        self.mangohud_btn = Gtk.ToggleButton(label="MangoHud")
        self.mangohud_btn.connect('toggled', lambda b: setattr(self, 'mangohud', b.get_active()))
        bottom.pack_start(self.mangohud_btn, False, False, 0)

        # Game settings button
        self.settings_btn = Gtk.Button(label="Game Settings")
        self.settings_btn.set_sensitive(False)
        self.settings_btn.connect('clicked', self.on_game_settings)
        bottom.pack_start(self.settings_btn, False, False, 0)

        # Launch button
        self.launch_btn = Gtk.Button(label="Launch")
        self.launch_btn.get_style_context().add_class('suggested-action')
        self.launch_btn.set_sensitive(False)
        self.launch_btn.connect('clicked', self.on_launch)
        bottom.pack_start(self.launch_btn, False, False, 0)

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
        return query in model[it][0].lower()

    # ── Data loading ───────────────────────────────────────────────────────────
    def _load_games(self):
        self.store.clear()
        gamedir = self.cfg.get('GAMEDIR', '')
        if not gamedir or not os.path.isdir(gamedir):
            return
        self.all_exes = get_exe_list(gamedir)
        for exe in self.all_exes:
            self.store.append([exe])

    def _selected_label(self):
        sel = self.tv.get_selection()
        model, it = sel.get_selected()
        if it:
            return model[it][0]
        return None

    def _selected_proton(self):
        idx = self.proton_combo.get_active()
        if idx >= 0 and idx < len(self.proton_dirs):
            return self.proton_dirs[idx]
        return None

    # ── Signals ────────────────────────────────────────────────────────────────
    def on_search_changed(self, _):
        self.filter.refilter()

    def on_selection_changed(self, _):
        has = self._selected_label() is not None
        self.launch_btn.set_sensitive(has)
        self.settings_btn.set_sensitive(has)

    def on_game_activated(self, tv, path, col):
        self.on_launch(None)

    def on_launch(self, _):
        label = self._selected_label()
        proton = self._selected_proton()
        if not label or not proton:
            show_error(self, "Select a game and a Proton version first.")
            return

        gamedir  = self.cfg.get('GAMEDIR', '')
        game_path = str(Path(gamedir) / label)
        game_cfg  = read_config(game_config_path(label))
        savedir   = game_cfg.get('SAVEDIR', '')
        launch_env = game_cfg.get('LAUNCH_ENV', '')

        # Wire up save symlink
        if savedir and os.path.isdir(os.path.dirname(savedir)):
            try:
                setup_save_symlink(savedir, game_save_path(label))
            except Exception as e:
                print(f"Save symlink warning: {e}")

        env = os.environ.copy()
        if launch_env:
            for pair in launch_env.split():
                if '=' in pair:
                    k, v = pair.split('=', 1)
                    env[k] = v

        env.update({
            'WINEPREFIX':        str(WINEPREFIX_PATH) + '/',
            'WINEDLLOVERRIDES':  'OnlineFix64=n;SteamOverlay64=n;winmm=n,b;dnet=n;steam_api64=n',
            'GAMEID':            '480',
            'PROTONPATH':        str(proton),
            'DXVK_STATE_CACHE':  '1',
            'MANGOHUD':          '1' if self.mangohud else '0',
        })

        subprocess.Popen(['umu-run', game_path], env=env)

    def on_game_settings(self, _):
        label = self._selected_label()
        if not label:
            return
        dialog = GameSettingsDialog(self, label)
        if dialog.run() == Gtk.ResponseType.OK:
            write_config(game_config_path(label), dialog.get_result())
        dialog.destroy()

    def on_setup_dir(self, _):
        dialog = Gtk.FileChooserDialog(
            title="Select game directory",
            parent=self,
            action=Gtk.FileChooserAction.SELECT_FOLDER)
        dialog.add_buttons(Gtk.STOCK_CANCEL, Gtk.ResponseType.CANCEL,
                           "Select", Gtk.ResponseType.OK)
        current = self.cfg.get('GAMEDIR', str(Path.home()))
        dialog.set_current_folder(current)
        if dialog.run() == Gtk.ResponseType.OK:
            self.cfg['GAMEDIR'] = dialog.get_filename()
            write_config(CONFIG_FILE, self.cfg)
            self._load_games()
        dialog.destroy()

    def on_setup_proton(self, _):
        dialog = Gtk.Dialog(title="Set favorite Proton", transient_for=self, flags=0)
        dialog.set_default_size(360, 120)
        dialog.add_buttons(Gtk.STOCK_CANCEL, Gtk.ResponseType.CANCEL,
                           "Save", Gtk.ResponseType.OK)
        box = dialog.get_content_area()
        box.set_spacing(10)
        box.set_margin_top(12); box.set_margin_bottom(12)
        box.set_margin_start(12); box.set_margin_end(12)
        box.pack_start(Gtk.Label(label="Choose your default Proton version:"), False, False, 0)
        combo = Gtk.ComboBoxText()
        saved = self.cfg.get('PROTONPATH', '')
        active = 0
        for i, d in enumerate(self.proton_dirs):
            combo.append_text(d.name)
            if str(d) == saved:
                active = i
        combo.set_active(active)
        box.pack_start(combo, False, False, 0)
        dialog.show_all()
        if dialog.run() == Gtk.ResponseType.OK:
            idx = combo.get_active()
            if idx >= 0:
                self.cfg['PROTONPATH'] = str(self.proton_dirs[idx])
                write_config(CONFIG_FILE, self.cfg)
                self._populate_proton_combo()
        dialog.destroy()

    def on_install_deps(self, _):
        gamedir = self.cfg.get('GAMEDIR', '')
        if not gamedir:
            show_error(self, "Set up a game directory first (-s).")
            return
        if not self.proton_dirs:
            show_error(self, "No Proton installations found.")
            return
        dialog = InstallDepsDialog(self, gamedir, self.proton_dirs)
        if dialog.run() == Gtk.ResponseType.OK:
            verbs  = dialog.get_selected_verbs()
            proton = dialog.get_proton_path()
            dialog.destroy()
            if verbs and proton:
                self._run_winetricks(verbs, proton)
        else:
            dialog.destroy()

    def _run_winetricks(self, verbs, proton):
        env = os.environ.copy()
        env.update({
            'WINE':       str(proton / 'files' / 'bin' / 'wine64'),
            'WINESERVER': str(proton / 'files' / 'bin' / 'wineserver'),
            'WINEPREFIX': str(WINEPREFIX_PATH) + '/',
        })
        subprocess.Popen(['winetricks', '--unattended'] + verbs, env=env)

    def on_reset(self, _):
        dialog = Gtk.MessageDialog(
            transient_for=self, flags=0,
            message_type=Gtk.MessageType.WARNING,
            buttons=Gtk.ButtonsType.YES_NO,
            text="Reset all config?")
        dialog.format_secondary_text(
            "This will clear your game directory, Proton preference, "
            "and all per-game settings. Save files are NOT deleted.")
        if dialog.run() == Gtk.ResponseType.YES:
            if CONFIG_FILE.exists(): CONFIG_FILE.unlink()
            if GAMES_DIR.exists():   shutil.rmtree(GAMES_DIR)
            self.cfg = {}
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

def run_startup_checks(parent=None):
    if not check_steam_running():
        d = Gtk.MessageDialog(
            transient_for=parent, flags=0,
            message_type=Gtk.MessageType.ERROR,
            buttons=Gtk.ButtonsType.OK,
            text="Steam is not running")
        d.format_secondary_text(
            "Please start Steam before launching MonkeyLauncher.")
        d.run(); d.destroy()
        return False

    if not check_app480_installed():
        d = Gtk.MessageDialog(
            transient_for=parent, flags=0,
            message_type=Gtk.MessageType.WARNING,
            buttons=Gtk.ButtonsType.YES_NO,
            text="Spacewar (App 480) is not installed")
        d.format_secondary_text(
            "App 480 is required for the Proton prefix.\n"
            "Open Steam to install it now?")
        response = d.run(); d.destroy()
        if response == Gtk.ResponseType.YES:
            subprocess.Popen(['steam', 'steam://install/480'])
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
