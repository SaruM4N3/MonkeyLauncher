import os
import shlex
import shutil
import subprocess
import threading
from pathlib import Path

import gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, GLib, GdkPixbuf

from .config import (
    CONFIG_DIR, CONFIG_FILE, GAMEDIRS_FILE, GAMES_DIR, LOG_DIR, WINEPREFIX_PATH,
    game_config_path, game_save_path, get_exe_list,
    read_config, read_gamedirs, write_config, write_gamedirs,
)
from .covers import cover_cache_path, default_game_name, fetch_cover
from .dialogs import GameSettingsDialog, InstallDepsDialog, WINETRICKS_PACKAGES, show_error
from .logging_setup import log
from .steam import get_proton_dirs, setup_save_symlink

def parse_launch_tokens(text, env):
    """Parse a launch-options string (global or per-game) Steam-%command%-
    style: KEY=VALUE tokens are applied to `env` in place, "%command%"
    marks where the game's launch command goes, and every other token
    before/after it becomes a prefix/suffix arg. Returns (prefix, suffix)."""
    prefix, suffix = [], []
    if not text:
        return prefix, suffix
    tokens = shlex.split(text)
    if '%command%' in tokens:
        idx = tokens.index('%command%')
        before, after = tokens[:idx], tokens[idx + 1:]
    else:
        before, after = [], tokens
    for toks, bucket in ((before, prefix), (after, suffix)):
        for tok in toks:
            if '=' in tok and not tok.startswith('-'):
                k, v = tok.split('=', 1)
                env[k] = v
            else:
                bucket.append(tok)
    return prefix, suffix

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
        self.show_hidden   = False
        self._running_proc = None
        self._cover_fetch_running = False
        self._placeholder_cache = {}

        log.debug(f"Detected {len(self.proton_dirs)} Proton installation(s): "
                  f"{[d.name for d in self.proton_dirs]}")

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

        self.view_toggle = Gtk.ToggleButton(label="Preview")
        self.view_toggle.set_tooltip_text("Toggle between list and cover preview")
        self.view_toggle.connect('toggled', self.on_view_toggle)
        search_bar.pack_start(self.view_toggle, False, False, 0)

        self.show_hidden_toggle = Gtk.ToggleButton(label="Show hidden")
        self.show_hidden_toggle.set_active(False)
        self.show_hidden_toggle.set_tooltip_text(
            "Show deactivated games ('Remove from list'-ed) so they can be reactivated")
        self.show_hidden_toggle.connect('toggled', self.on_show_hidden_toggle)
        search_bar.pack_start(self.show_hidden_toggle, False, False, 0)

        lib_box.pack_start(search_bar, False, False, 0)
        lib_box.pack_start(Gtk.Separator(), False, False, 0)

        scroll = Gtk.ScrolledWindow(vexpand=True)
        scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)

        # TreeStore: col0=display, col1=exe_relpath (empty for dir rows),
        # col2=gamedir, col3=hidden (dims the row; dir rows are always False)
        self.store = Gtk.TreeStore(str, str, str, bool)
        self.filter = self.store.filter_new()
        self.filter.set_visible_func(self._game_filter)
        self.tv = Gtk.TreeView(model=self.filter, headers_visible=False)
        game_col = Gtk.TreeViewColumn("Game")
        game_renderer = Gtk.CellRendererText()
        game_col.pack_start(game_renderer, True)
        game_col.add_attribute(game_renderer, 'text', 0)
        game_col.set_cell_data_func(game_renderer, self._render_hidden_dim)
        self.tv.append_column(game_col)
        self.tv.connect('row-activated', self.on_game_activated)
        self.tv.connect('button-press-event', self.on_tree_button_press)
        self.tv.get_selection().connect('changed', self.on_selection_changed)
        scroll.add(self.tv)

        # ── Preview (grid) view ────────────────────────────────────────────────
        grid_scroll = Gtk.ScrolledWindow(vexpand=True)
        grid_scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)

        # Flat ListStore (no directory grouping): col0=cover, col1=display,
        # col2=exe_relpath, col3=gamedir, col4=hidden
        self.grid_store = Gtk.ListStore(GdkPixbuf.Pixbuf, str, str, str, bool)
        self.grid_filter = self.grid_store.filter_new()
        self.grid_filter.set_visible_func(self._grid_filter)
        self.icon_view = Gtk.IconView(model=self.grid_filter)
        pixbuf_renderer = Gtk.CellRendererPixbuf()
        self.icon_view.pack_start(pixbuf_renderer, False)
        self.icon_view.add_attribute(pixbuf_renderer, 'pixbuf', 0)
        self.icon_view.set_cell_data_func(pixbuf_renderer, self._render_hidden_dim)
        text_renderer = Gtk.CellRendererText()
        text_renderer.set_alignment(0.5, 0)
        self.icon_view.pack_start(text_renderer, True)
        self.icon_view.add_attribute(text_renderer, 'text', 1)
        self.icon_view.set_cell_data_func(text_renderer, self._render_hidden_dim)
        self.icon_view.set_item_width(130)
        self.icon_view.connect('item-activated', self.on_grid_item_activated)
        self.icon_view.connect('selection-changed', self.on_selection_changed)
        self.icon_view.connect('button-press-event', self.on_grid_button_press)
        grid_scroll.add(self.icon_view)

        self.view_stack = Gtk.Stack()
        self.view_stack.add_named(scroll, 'list')
        self.view_stack.add_named(grid_scroll, 'grid')

        # Overlay the view stack with a centered spinner shown during scanning
        overlay = Gtk.Overlay(vexpand=True)
        overlay.add(self.view_stack)
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

        # ── Help page ─────────────────────────────────────────────────────────
        help_scroll = Gtk.ScrolledWindow(vexpand=True)
        help_scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        help_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18, margin=16)

        def add_help_section(title, body):
            title_lbl = Gtk.Label(xalign=0)
            title_lbl.set_markup(f'<b>{title}</b>')
            help_box.pack_start(title_lbl, False, False, 0)
            body_lbl = Gtk.Label(xalign=0, wrap=True)
            body_lbl.set_markup(body)
            help_box.pack_start(body_lbl, False, False, 0)

        add_help_section("Library", (
            "Click <b>+ Add game directory</b> to point MonkeyLauncher at a folder "
            "containing your games — it scans recursively for executables.\n"
            "Double-click a game (or select it and click <b>Launch</b>) to start it "
            "through Proton.\n"
            "Use the search bar to filter, <b>Show full path</b> to see the underlying "
            "exe paths, <b>Preview</b> to switch to a cover-art grid view, and "
            "<b>Show hidden</b> to reveal games you've removed from the list.\n"
            "Right-click a game for <b>Game Settings</b> or to remove it from the "
            "list; right-click a directory to rescan or remove it from the library.\n"
            "<b>MangoHud</b> toggles the performance overlay for the next launch."
        ))

        add_help_section("Game Settings", (
            "Open via the <b>Game Settings</b> button or right-click menu on a game.\n"
            "<b>Display</b> — rename the game and set custom cover art.\n"
            "<b>Compatibility</b> — per-game WINEDLLOVERRIDES: check a DLL to force "
            "it native/builtin, or add your own.\n"
            "<b>Launch Options</b> — see below.\n"
            "<b>Save Directory</b> — point at the save path inside the Proton "
            "prefix so MonkeyLauncher can symlink it to a stable location."
        ))

        add_help_section("Launch options", (
            "<tt>KEY=VALUE</tt> tokens (e.g. <tt>GAMEMODE=1 DRI_PRIME=1</tt>) are "
            "applied as environment variables for the launch.\n"
            "Anything else is treated as a command-line token, Steam-style: put "
            "<tt>%command%</tt> where the game's launch command should go, and put "
            "wrapper programs before it, e.g. <tt>gamemoderun %command%</tt>.\n"
            "If you leave out <tt>%command%</tt>, plain tokens (e.g. "
            "<tt>-windowed -novid</tt>) are appended after the game instead.\n"
            "You can mix all three freely, e.g. "
            "<tt>PROTON_LOG=1 gamemoderun %command% -windowed</tt>."
        ))

        add_help_section("Settings tab", (
            "<b>Proton</b> — pick which installed Proton/UMU build is used to "
            "launch games, and set <b>Global launch options</b> (same syntax as "
            "a game's Launch Options) applied to every game; per-game options "
            "are layered on top and win on conflicting env vars.\n"
            "<b>Installer</b> — run bundled Windows installers/redistributables "
            "(e.g. DirectX, VC++) through Proton before playing.\n"
            "<b>Dependencies</b> — install common winetricks packages.\n"
            "<b>Advanced</b> — jump to the config/logs folders, or reset all "
            "MonkeyLauncher config (game directories, Proton choice, per-game "
            "settings); save files are kept."
        ))

        help_scroll.add(help_box)
        self.stack.add_titled(help_scroll, 'help', 'Help')

        # ── Settings page (Steam-settings-style: sections left, content right) ──
        settings_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL)

        self.settings_stack = Gtk.Stack()
        self.settings_stack.set_transition_type(Gtk.StackTransitionType.CROSSFADE)

        settings_sidebar = Gtk.StackSidebar()
        settings_sidebar.set_stack(self.settings_stack)
        settings_row.pack_start(settings_sidebar, False, False, 0)
        settings_row.pack_start(Gtk.Separator(orientation=Gtk.Orientation.VERTICAL), False, False, 0)
        settings_row.pack_start(self.settings_stack, True, True, 0)

        # ── Proton ─────────────────────────────────────────────────────────────
        # Pinned to the top at its natural size — Gtk.Stack sizes every page
        # to match the tallest one (Installer/Dependencies have long lists),
        # and without this the version row stretches/centers into that
        # extra space instead of staying put.
        proton_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8, margin=16)
        proton_box.set_valign(Gtk.Align.START)

        version_row = Gtk.Box(spacing=8)
        version_row.pack_start(Gtk.Label(label="Version:"), False, False, 0)
        self.proton_combo = Gtk.ComboBoxText(hexpand=True)
        self._populate_proton_combo()
        self.proton_combo.connect('changed', self.on_proton_changed)
        version_row.pack_start(self.proton_combo, True, True, 0)
        proton_box.pack_start(version_row, False, False, 0)

        global_opts_row = Gtk.Box(spacing=8)
        global_opts_row.pack_start(Gtk.Label(label="Global launch options:"), False, False, 0)
        self.global_launch_opts_entry = Gtk.Entry(
            hexpand=True, placeholder_text="e.g. gamemoderun %command%")
        self.global_launch_opts_entry.set_text(self.cfg.get('GLOBAL_LAUNCH_ENV', ''))
        self.global_launch_opts_entry.connect('activate', self.on_global_launch_opts_changed)
        self.global_launch_opts_entry.connect('focus-out-event', self.on_global_launch_opts_changed)
        global_opts_row.pack_start(self.global_launch_opts_entry, True, True, 0)
        proton_box.pack_start(global_opts_row, False, False, 0)
        note = Gtk.Label(xalign=0, wrap=True)
        note.set_markup('<small>Applied to every game, same syntax as a game\'s Launch '
                        'Options (see Help). Per-game options are applied on top and '
                        'win on conflicting env vars.</small>')
        proton_box.pack_start(note, False, False, 0)

        self.settings_stack.add_titled(proton_box, 'proton', 'Proton')

        # ── Installer ──────────────────────────────────────────────────────────
        inst_outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8, margin=16)

        inst_top = Gtk.Box(spacing=8)
        self.installer_source_lbl = Gtk.Label(xalign=0, hexpand=True, wrap=True)
        inst_browse_btn = Gtk.Button(label="Browse directory…")
        inst_browse_btn.connect('clicked', self.on_installer_browse)
        inst_top.pack_start(self.installer_source_lbl, True, True, 0)
        inst_top.pack_start(inst_browse_btn, False, False, 0)
        inst_outer.pack_start(inst_top, False, False, 0)

        inst_scroll = Gtk.ScrolledWindow(vexpand=True)
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

        self.settings_stack.add_titled(inst_outer, 'installer', 'Installer')

        # ── Dependencies (winetricks) ─────────────────────────────────────────
        deps_outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8, margin=16)

        deps_scroll = Gtk.ScrolledWindow(vexpand=True)
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

        self.settings_stack.add_titled(deps_outer, 'deps', 'Dependencies')

        # ── Advanced ───────────────────────────────────────────────────────────
        advanced_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8, margin=16)

        folders_row = Gtk.Box(spacing=8)
        open_config_btn = Gtk.Button(label="Open config folder")
        open_config_btn.connect('clicked', lambda _: subprocess.Popen(['xdg-open', str(CONFIG_DIR)]))
        folders_row.pack_start(open_config_btn, False, False, 0)
        open_logs_btn = Gtk.Button(label="Open logs folder")
        open_logs_btn.connect('clicked', lambda _: subprocess.Popen(['xdg-open', str(LOG_DIR)]))
        folders_row.pack_start(open_logs_btn, False, False, 0)
        advanced_box.pack_start(folders_row, False, False, 0)

        advanced_box.pack_start(Gtk.Separator(), False, False, 0)

        advanced_box.pack_start(Gtk.Label(
            label="Resetting clears all game directories, Proton preference, "
                  "and per-game settings.\nSave files are NOT deleted.",
            xalign=0, wrap=True), False, False, 0)
        reset_btn = Gtk.Button(label="Reset all config…")
        reset_btn.get_style_context().add_class('destructive-action')
        reset_btn.connect('clicked', self.on_reset)
        advanced_box.pack_start(reset_btn, False, False, 0)
        self.settings_stack.add_titled(advanced_box, 'advanced', 'Advanced')

        self.stack.add_titled(settings_row, 'settings', 'Settings')

    def _display_name(self, exe, gcfg):
        if self.show_fullpath:
            return exe
        return gcfg.get('NAME') or default_game_name(exe)

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

        for row in self.grid_store:
            gcfg = read_config(game_config_path(row[2]))
            row[1] = self._display_name(row[2], gcfg)

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
        # Dir row: visible if at least one child is both active (or
        # show-hidden is on) and matches the search
        if not model[it][1]:
            child = model.iter_children(it)
            while child:
                if (self.show_hidden or not model[child][3]) \
                        and (not query or query in model[child][0].lower()):
                    return True
                child = model.iter_next(child)
            return False
        if model[it][3] and not self.show_hidden:
            return False
        return not query or query in model[it][0].lower()

    def _grid_filter(self, model, it, _):
        if model[it][4] and not self.show_hidden:
            return False
        query = self.search_entry.get_text().lower()
        return not query or query in model[it][1].lower()

    def _render_hidden_dim(self, cell_layout, cell, model, it, data=None):
        """Shared cell-data-func for both the list and grid views: dims
        deactivated ('Remove from list'-ed) games instead of hiding them
        outright, so they can be found again and reactivated."""
        hidden_col = model.get_n_columns() - 1
        cell.set_property('sensitive', not model[it][hidden_col])

    # ── Cover art ──────────────────────────────────────────────────────────────
    def _placeholder_pixbuf(self, size=110):
        if size in self._placeholder_cache:
            return self._placeholder_cache[size]
        try:
            pixbuf = Gtk.IconTheme.get_default().load_icon(
                'applications-games', size, Gtk.IconLookupFlags.FORCE_SIZE)
        except GLib.Error:
            pixbuf = GdkPixbuf.Pixbuf.new(GdkPixbuf.Colorspace.RGB, True, 8, size, size)
            pixbuf.fill(0x33333380)
        self._placeholder_cache[size] = pixbuf
        return pixbuf

    def _load_grid_pixbuf(self, path, size=110):
        try:
            pixbuf = GdkPixbuf.Pixbuf.new_from_file(str(path))
        except GLib.Error as e:
            log.debug(f"Could not load cover image {path}: {e}")
            return None
        w, h = pixbuf.get_width(), pixbuf.get_height()
        if w <= 0 or h <= 0:
            return None
        # Not every game has a portrait cover — center-crop wide fallback
        # art (Steam header/hero banners) so it doesn't render as a
        # paper-thin sliver next to actual portrait covers.
        if w / h > 1.4:
            crop_w = int(h * 1.4)
            pixbuf = pixbuf.new_subpixbuf((w - crop_w) // 2, 0, crop_w, h)
            w = crop_w
        scale = size / w
        return pixbuf.scale_simple(size, max(1, int(h * scale)), GdkPixbuf.InterpType.BILINEAR)

    def _grid_pixbuf_for(self, exe):
        """Loads an already-cached cover for a fresh grid row, if there is
        one — otherwise the placeholder, pending a fetch. Without this,
        every reload (startup, add/remove dir, rescan) would show
        placeholders even for games whose cover was already downloaded in
        a previous session, since _start_cover_fetch skips cache hits."""
        cache_path = cover_cache_path(exe)
        if cache_path.exists():
            pixbuf = self._load_grid_pixbuf(cache_path)
            if pixbuf:
                return pixbuf
        return self._placeholder_pixbuf()

    def _start_cover_fetch(self):
        if self._cover_fetch_running:
            return
        pending = [(row[2], row[3]) for row in self.grid_store
                  if not cover_cache_path(row[2]).exists()]
        if not pending:
            return
        self._cover_fetch_running = True
        log.debug(f"Fetching {len(pending)} missing cover(s) from the Steam store")

        def worker():
            for exe, gamedir in pending:
                path = fetch_cover(exe)
                if path:
                    GLib.idle_add(self._apply_cover, exe, gamedir, path)
            self._cover_fetch_running = False
            log.debug("Cover fetch pass finished")

        threading.Thread(target=worker, daemon=True).start()

    def _apply_cover(self, exe, gamedir, path):
        pixbuf = self._load_grid_pixbuf(path)
        if pixbuf:
            for row in self.grid_store:
                if row[2] == exe and row[3] == gamedir:
                    row[0] = pixbuf
                    break
        return False

    # ── Data loading ───────────────────────────────────────────────────────────
    def _load_games(self):
        self.store.clear()
        self.launch_btn.set_sensitive(False)
        self.game_settings_btn.set_sensitive(False)
        self.spinner.start()
        log.debug(f"Scanning {len(self.gamedirs)} game director(y/ies): {self.gamedirs}")

        def scan():
            results = []
            for gamedir in list(self.gamedirs):
                if not os.path.isdir(gamedir):
                    log.warning(f"Game directory no longer exists, skipping: {gamedir}")
                    continue
                exes = []
                for exe in get_exe_list(gamedir):
                    gcfg = read_config(game_config_path(exe))
                    exes.append((exe, gcfg, gamedir))
                if exes:
                    results.append((gamedir, exes))
            GLib.idle_add(self._apply_games, results)

        threading.Thread(target=scan, daemon=True).start()

    def _apply_games(self, results):
        total = sum(len(exes) for _, exes in results)
        log.info(f"Found {total} game(s) across {len(results)} director(y/ies)")
        self.grid_store.clear()
        for gamedir, exes in results:
            parent = self.store.append(None, [Path(gamedir).name, '', gamedir, False])
            for exe, gcfg, gdir in exes:
                display = self._display_name(exe, gcfg)
                hidden  = gcfg.get('HIDDEN') == '1'
                self.store.append(parent, [display, exe, gdir, hidden])
                self.grid_store.append([self._grid_pixbuf_for(exe), display, exe, gdir, hidden])
        self.tv.expand_all()
        self.spinner.stop()
        if self.view_stack.get_visible_child_name() == 'grid':
            self._start_cover_fetch()
        return False  # remove from idle queue

    def _selected_game(self):
        """Returns (exe_relpath, gamedir) for the selected game row, or (None, None)."""
        if self.view_stack.get_visible_child_name() == 'grid':
            items = self.icon_view.get_selected_items()
            if not items:
                return None, None
            it = self.grid_filter.get_iter(items[0])
            return self.grid_filter[it][2], self.grid_filter[it][3]
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
        self.grid_filter.refilter()

    def on_view_toggle(self, btn):
        if btn.get_active():
            self.view_stack.set_visible_child_name('grid')
            btn.set_label("List")
            self._start_cover_fetch()
        else:
            self.view_stack.set_visible_child_name('list')
            btn.set_label("Preview")
        self.on_selection_changed(None)

    def on_show_hidden_toggle(self, btn):
        self.show_hidden = btn.get_active()
        self.filter.refilter()
        self.tv.expand_all()
        self.grid_filter.refilter()

    def on_grid_item_activated(self, icon_view, path):
        self.on_launch(None)

    def on_grid_button_press(self, icon_view, event):
        if event.button != 3:
            return False
        path = icon_view.get_path_at_pos(int(event.x), int(event.y))
        if not path:
            return False
        icon_view.select_path(path)
        it = self.grid_filter.get_iter(path)
        exe = self.grid_filter[it][2]
        is_hidden = self.grid_filter[it][4]

        menu = Gtk.Menu()
        settings_item = Gtk.MenuItem(label="Game Settings")
        settings_item.connect('activate', lambda _: self.on_game_settings(None))
        menu.append(settings_item)
        if is_hidden:
            activate_item = Gtk.MenuItem(label="Activate")
            activate_item.connect('activate', lambda _: self.on_activate_game(exe))
            menu.append(activate_item)
        else:
            hide_item = Gtk.MenuItem(label="Remove from list")
            hide_item.connect('activate', lambda _: self.on_hide_game(exe))
            menu.append(hide_item)
        menu.show_all()
        menu.popup_at_pointer(event)
        return False

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
            is_hidden = model[it][3]
            settings_item = Gtk.MenuItem(label="Game Settings")
            settings_item.connect('activate', lambda _: self.on_game_settings(None))
            menu.append(settings_item)
            if is_hidden:
                activate_item = Gtk.MenuItem(label="Activate")
                activate_item.connect('activate', lambda _: self.on_activate_game(exe))
                menu.append(activate_item)
            else:
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

    def on_activate_game(self, exe):
        gcfg = read_config(game_config_path(exe))
        gcfg.pop('HIDDEN', None)
        write_config(game_config_path(exe), gcfg)
        self._load_games()

    def on_launch(self, _):
        if self._running_proc is not None:
            log.info(f"Stopping game (pid={self._running_proc.pid})")
            self._running_proc.terminate()
            return

        exe, gamedir = self._selected_game()
        proton = self._selected_proton()
        if not exe or not proton:
            log.warning("Launch attempted without a selected game/Proton version")
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
                log.warning(f"Save symlink warning for {exe}: {e}")

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
        # Global overrides applied first, per-game overrides applied after so
        # they win on conflicting env vars. KEY=VALUE tokens become env vars;
        # everything else is a command-line token, Steam-%command%-style:
        # tokens before "%command%" wrap/prefix the launch command (e.g.
        # "gamemoderun %command%"), tokens after it (or all of them, if
        # %command% is omitted) are appended as extra args to the game.
        # Global wrapping goes outermost, per-game innermost.
        global_launch_env = self.cfg.get('GLOBAL_LAUNCH_ENV', '')
        g_prefix, g_suffix = parse_launch_tokens(global_launch_env, env)
        p_prefix, p_suffix = parse_launch_tokens(launch_env, env)

        cmd = g_prefix + p_prefix + ['umu-run', game_path] + p_suffix + g_suffix

        log.info(f"Launching {exe} via {proton.name}")
        log.debug(f"Launch command: {' '.join(cmd)}")
        log.debug(f"Global launch overrides: {global_launch_env or '(none)'}, "
                  f"per-game overrides: {launch_env or '(none)'}, "
                  f"savedir: {savedir or '(none)'}, mangohud: {self.mangohud}")

        proc = subprocess.Popen(cmd, env=env)
        self._running_proc = proc

        self.launch_btn.set_label("Stop")
        self.launch_btn.get_style_context().remove_class('suggested-action')
        self.launch_btn.get_style_context().add_class('destructive-action')
        self.launch_btn.set_sensitive(True)

        GLib.child_watch_add(GLib.PRIORITY_DEFAULT, proc.pid, self._on_game_exit)

    def _on_game_exit(self, pid, _status):
        log.info(f"Game process exited (pid={pid}, status={_status})")
        self._running_proc = None
        self.launch_btn.set_label("Launch")
        self.launch_btn.get_style_context().remove_class('destructive-action')
        self.launch_btn.get_style_context().add_class('suggested-action')
        exe, _ = self._selected_game()
        self.launch_btn.set_sensitive(exe is not None)

    def on_game_settings(self, _):
        exe, gamedir = self._selected_game()
        if not exe:
            return
        dialog = GameSettingsDialog(self, exe)
        if dialog.run() == Gtk.ResponseType.OK:
            write_config(game_config_path(exe), dialog.get_result())
            self._refresh_game_row(exe, gamedir)
        dialog.destroy()

    def _refresh_game_row(self, exe, gamedir):
        """Re-reads a single game's config/cover after Game Settings closes,
        so a manually set name or cover shows up immediately."""
        gcfg = read_config(game_config_path(exe))
        display = self._display_name(exe, gcfg)

        it = self.store.get_iter_first()
        while it:
            child = self.store.iter_children(it)
            while child:
                if self.store[child][1] == exe:
                    self.store[child][0] = display
                child = self.store.iter_next(child)
            it = self.store.iter_next(it)

        pixbuf = self._grid_pixbuf_for(exe)
        for row in self.grid_store:
            if row[2] == exe and row[3] == gamedir:
                row[0] = pixbuf
                row[1] = display
                break

        if self.view_stack.get_visible_child_name() == 'grid':
            self._start_cover_fetch()

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
                    hidden = gcfg.get('HIDDEN') == '1'
                    self.store.append(it, [self._display_name(exe, gcfg), exe, gamedir, hidden])
                self.tv.expand_row(self.store.get_path(it), False)
                break
            it = self.store.iter_next(it)

        grid_it = self.grid_store.get_iter_first()
        while grid_it:
            nxt = self.grid_store.iter_next(grid_it)
            if self.grid_store[grid_it][3] == gamedir:
                self.grid_store.remove(grid_it)
            grid_it = nxt
        for exe, gcfg in exes:
            hidden = gcfg.get('HIDDEN') == '1'
            self.grid_store.append(
                [self._grid_pixbuf_for(exe), self._display_name(exe, gcfg), exe, gamedir, hidden])

        self.spinner.stop()
        if self.view_stack.get_visible_child_name() == 'grid':
            self._start_cover_fetch()
        return False

    def on_proton_changed(self, combo):
        idx = combo.get_active()
        if idx >= 0:
            self.cfg['PROTONPATH'] = str(self.proton_dirs[idx])
            write_config(CONFIG_FILE, self.cfg)
        self._check_installed_deps()

    def on_global_launch_opts_changed(self, widget, _event=None):
        self.cfg['GLOBAL_LAUNCH_ENV'] = self.global_launch_opts_entry.get_text().strip()
        write_config(CONFIG_FILE, self.cfg)
        return False

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
        log.info(f"Installing winetricks packages: {', '.join(verbs)}")

        def worker():
            for verb in verbs:
                GLib.idle_add(self._wt_set_status, verb, 'running')
                log.debug(f"protontricks --no-bwrap 480 {verb}")
                result = subprocess.run(
                    ['protontricks', '--no-bwrap', '480', verb])
                ok = result.returncode == 0
                if ok:
                    log.info(f"Installed {verb}")
                else:
                    log.error(f"Failed to install {verb} (exit {result.returncode})")
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
                log.info(f"Running installer: {exe_path}")
                result = subprocess.run(['umu-run', exe_path], env=env)
                ok = result.returncode == 0
                if ok:
                    log.info(f"Installer finished: {exe_path}")
                else:
                    log.error(f"Installer failed: {exe_path} (exit {result.returncode})")
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
            log.warning("Resetting all config (games, dirs, Proton preference) at user's request")
            if CONFIG_FILE.exists():   CONFIG_FILE.unlink()
            if GAMEDIRS_FILE.exists(): GAMEDIRS_FILE.unlink()
            if GAMES_DIR.exists():     shutil.rmtree(GAMES_DIR)
            self.cfg      = {}
            self.gamedirs = []
            self.store.clear()
            self.global_launch_opts_entry.set_text('')
        dialog.destroy()
