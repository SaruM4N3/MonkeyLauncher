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
    manifest = STEAM_ROOT / 'steamapps' / 'appmanifest_480.acf'
    if not manifest.exists():
        return None
    m = re.search(r'"installdir"\s+"([^"]+)"', manifest.read_text())
    installdir = m.group(1) if m else 'Spacewar'
    d = STEAM_ROOT / 'steamapps' / 'common' / installdir
    return d if d.is_dir() else None

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
    manifest = STEAM_ROOT / 'steamapps' / 'appmanifest_480.acf'
    return manifest.exists()

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
    subprocess.Popen(['umu-run', exe_path], env=env)

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

    pfx = WINEPREFIX_PATH / 'pfx'
    waited = 0
    while not pfx.is_dir() and proc.poll() is None and waited < 180:
        time.sleep(1)
        waited += 1

    ready = pfx.is_dir()
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
