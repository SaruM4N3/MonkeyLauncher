import subprocess
from pathlib import Path

import gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, GLib

from .config import CONFIG_FILE, WINEPREFIX_PATH, read_config, write_config
from .dialogs import show_error
from .logging_setup import log
from .main_window import MonkeyLauncher
from .steam import (bootstrap_proton_prefix, check_app480_installed, check_steam_running,
                    get_proton_dirs, sync_steamclient_files)

# ── Startup checks ─────────────────────────────────────────────────────────────
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

def pick_bootstrap_proton(parent=None):
    """Returns the Proton dir to use for the initial prefix setup: the saved
    favorite if there is one, otherwise prompts the user to pick one (and
    saves that choice, same as the regular favorite-Proton setting)."""
    cfg = read_config(CONFIG_FILE)
    saved = cfg.get('PROTONPATH', '')
    if saved and Path(saved).is_dir():
        return Path(saved)

    proton_dirs = get_proton_dirs()
    if not proton_dirs:
        show_error(parent, "No Proton installation found in your Steam libraries.\n"
                            "Install a Proton version via Steam first.")
        return None

    d = Gtk.Dialog(title="Select a Proton version", transient_for=parent, flags=0)
    d.set_default_size(420, -1)
    d.add_buttons("Cancel", Gtk.ResponseType.CANCEL, "OK", Gtk.ResponseType.OK)
    box = d.get_content_area()
    box.set_spacing(8)
    label = Gtk.Label(
        label="MonkeyLauncher needs a Proton version to set up its shared prefix.",
        wrap=True, margin=12)
    box.pack_start(label, False, False, 0)
    combo = Gtk.ComboBoxText(margin_start=12, margin_end=12, margin_bottom=12)
    for pd in proton_dirs:
        combo.append_text(pd.name)
    combo.set_active(0)
    box.pack_start(combo, False, False, 0)
    d.show_all()

    proton = None
    if d.run() == Gtk.ResponseType.OK:
        idx = combo.get_active()
        if idx >= 0:
            proton = proton_dirs[idx]
            cfg['PROTONPATH'] = str(proton)
            write_config(CONFIG_FILE, cfg)
            log.info(f"Favorite Proton saved: {proton.name}")
    d.destroy()
    return proton

def run_startup_checks(parent=None):
    log.debug("Running startup checks (Steam running? App 480 installed?)")
    if not check_steam_running():
        log.info("Steam is not running")
        d = Gtk.MessageDialog(
            transient_for=parent, flags=0,
            message_type=Gtk.MessageType.QUESTION,
            buttons=Gtk.ButtonsType.YES_NO,
            text="Steam is not running")
        d.format_secondary_text("Launch Steam now and wait for it to start?")
        response = d.run(); d.destroy()
        if response != Gtk.ResponseType.YES:
            log.warning("User declined to launch Steam — aborting startup")
            return False
        if not wait_for_steam(parent):
            log.warning("User cancelled while waiting for Steam to start")
            return False

    if not check_app480_installed():
        log.info("Steam App 480 (Spacewar) is not installed")
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
            log.warning("User declined to install App 480 — aborting startup")
            return False
        if not wait_for_spacewar(parent):
            log.warning("User cancelled while waiting for App 480 to install")
            return False

    if not (WINEPREFIX_PATH / 'pfx').is_dir():
        proton = pick_bootstrap_proton(parent)
        if not proton:
            log.warning("No Proton version selected — cannot set up the prefix")
            return False
        if not bootstrap_proton_prefix(proton):
            show_error(parent, "Could not set up the Proton prefix automatically.\n"
                                "Try a different Proton version, or launch Spacewar once from Steam.")
            return False

    sync_steamclient_files()

    log.debug("Startup checks passed")
    return True

# ── App entry point ────────────────────────────────────────────────────────────
class App(Gtk.Application):
    def __init__(self):
        super().__init__(application_id='com.monkeylauncher.app')

    def do_activate(self):
        log.info("Starting MonkeyLauncher GUI")
        if not run_startup_checks():
            log.info("Startup checks failed or were cancelled — exiting")
            self.quit()
            return
        win = MonkeyLauncher(self)
        win.show_all()
        win.present()
