import os
import re
import shutil
import subprocess
import time
from pathlib import Path

from .config import STEAM_ROOT, WINEPREFIX_PATH
from .logging_setup import log

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

def get_spacewar_dir():
    for lib in get_steam_libs() or [STEAM_ROOT]:
        manifest = lib / 'steamapps' / 'appmanifest_480.acf'
        if not manifest.exists():
            continue
        m = re.search(r'"installdir"\s+"([^"]+)"', manifest.read_text())
        installdir = m.group(1) if m else 'Spacewar'
        d = lib / 'steamapps' / 'common' / installdir
        if d.is_dir():
            return d
    return None

def find_spacewar_exe():
    d = get_spacewar_dir()
    if not d:
        return None
    exes = sorted(d.rglob('*.exe'))
    return exes[0] if exes else None

def check_steam_running():
    try:
        result = subprocess.run(['pgrep', '-x', 'steam'], capture_output=True)
        return result.returncode == 0
    except Exception:
        return False

def check_app480_installed():
    libs = get_steam_libs() or [STEAM_ROOT]
    return any((lib / 'steamapps' / 'appmanifest_480.acf').exists() for lib in libs)

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

def run_through_proton(exe_path, proton_path):
    env = os.environ.copy()
    env.update({
        'WINE':       str(proton_path / 'files' / 'bin' / 'wine64'),
        'WINESERVER': str(proton_path / 'files' / 'bin' / 'wineserver'),
        'WINEPREFIX': str(WINEPREFIX_PATH) + '/',
        'WINEDLLOVERRIDES':  'OnlineFix64=n;SteamOverlay64=n;winmm=n,b;dnet=n;steam_api64=n',
        'PROTONPATH': str(proton_path),
        'DXVK_STATE_CACHE':  '1',
        'GAMEID':     '480',
    })
    log.info(f"Running installer via Proton: {exe_path} (proton={proton_path.name})")
    subprocess.Popen(['umu-run', exe_path], env=env, cwd=os.path.dirname(exe_path))

def sync_steamclient_files():
    """Copies the real Valve steamclient files into the shared prefix's fake
    Steam install (drive_c/Program Files (x86)/Steam/).

    Real Steam does this itself via Proton whenever STEAM_COMPAT_CLIENT_INSTALL_PATH
    is set, which is how it normally ends up there. But umu-run, invoked as a
    plain CLI command (`umu-run <exe>`, as we do), resets that variable to an
    empty string internally before it ever reaches Proton — so Proton never
    performs this copy for any prefix set up or run through umu-run. Without
    it, OnlineFix (and other Goldberg-style Steam emulators) can't find the
    "original" steamclient to fall back to for calls they don't implement,
    and fail with "steamclient not found". Safe to call any time; just
    re-copies over whatever's there.
    """
    legacycompat = STEAM_ROOT / 'legacycompat'
    dest = WINEPREFIX_PATH / 'pfx' / 'drive_c' / 'Program Files (x86)' / 'Steam'
    files = [
        ('steamclient.dll',          'steamclient.dll'),
        ('steamclient64.dll',        'steamclient64.dll'),
        ('GameOverlayRenderer64.dll', 'GameOverlayRenderer64.dll'),
        ('SteamService.exe',         'steam.exe'),
        ('Steam.dll',                'Steam.dll'),
    ]
    if not (WINEPREFIX_PATH / 'pfx').is_dir():
        return
    dest.mkdir(parents=True, exist_ok=True)
    for src, tgt in files:
        srcfile = legacycompat / src
        if srcfile.is_file():
            try:
                shutil.copy2(srcfile, dest / tgt)
            except Exception as e:
                log.warning(f"Could not copy {src} into prefix Steam dir: {e}")
    log.debug(f"Synced steamclient files into {dest}")

def bootstrap_proton_prefix(proton_path):
    """Creates the shared compatdata/480 prefix by launching Spacewar once
    through the given Proton, then killing it as soon as the prefix
    directory shows up. Runs quietly (no dialog) — just a blocking wait."""
    exe = find_spacewar_exe()
    if not exe:
        log.error("Could not find a Spacewar executable to initialize the Proton prefix.")
        return False

    log.info(f"Proton prefix not found — setting it up (first run) using {proton_path.name}…")
    env = os.environ.copy()
    env.update({
        'WINE':       str(proton_path / 'files' / 'bin' / 'wine64'),
        'WINESERVER': str(proton_path / 'files' / 'bin' / 'wineserver'),
        'WINEPREFIX': str(WINEPREFIX_PATH) + '/',
        'PROTONPATH': str(proton_path),
        'GAMEID':     '480',
    })
    log.debug(f"Bootstrapping prefix with {exe} via {proton_path.name}")
    proc = subprocess.Popen(['umu-run', str(exe)], env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # WINEPREFIX_PATH itself becomes "pfx" (often as a same-directory symlink,
    # created almost immediately) — not a reliable signal that wine has
    # actually finished initializing. system.reg/user.reg only get written
    # once wineboot completes, so wait for those instead; otherwise this
    # terminates Spacewar mid-setup and leaves a half-initialized prefix.
    pfx = WINEPREFIX_PATH / 'pfx'
    system_reg = WINEPREFIX_PATH / 'system.reg'
    user_reg   = WINEPREFIX_PATH / 'user.reg'
    waited = 0
    while not (pfx.is_dir() and system_reg.exists() and user_reg.exists()) \
            and proc.poll() is None and waited < 180:
        time.sleep(1)
        waited += 1

    ready = pfx.is_dir() and system_reg.exists() and user_reg.exists()
    if proc.poll() is None:
        log.debug("Terminating bootstrap process now that the prefix exists")
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    if ready:
        log.info(f"Proton prefix created at {pfx}")
    else:
        log.error("Proton prefix setup timed out or failed.")
    return ready
