import re
from pathlib import Path

# ── Paths ─────────────────────────────────────────────────────────────────────
CONFIG_DIR      = Path.home() / '.config' / 'MonkeyLauncher'
CONFIG_FILE     = CONFIG_DIR / 'config'
GAMEDIRS_FILE   = CONFIG_DIR / 'gamedirs'
GAMES_DIR       = CONFIG_DIR / 'games'
SAVES_BASE      = CONFIG_DIR / 'saves'
LOG_DIR         = CONFIG_DIR / 'logs'
LOG_FILE        = LOG_DIR / 'monkeylauncher.log'
COVERS_DIR      = CONFIG_DIR / 'covers'

def _detect_steam_root():
    """Locates the real Steam install dir. This varies by distro/packaging:
    Arch puts it straight at ~/.local/share/Steam, while the official Debian/
    Ubuntu package installs to ~/.steam/debian-installation instead. Steam
    itself maintains ~/.steam/root and ~/.steam/steam as symlinks to wherever
    it actually lives (the same symlink Proton resolves for
    STEAM_COMPAT_CLIENT_INSTALL_PATH), so prefer those over guessing."""
    home = Path.home()
    candidates = [
        home / '.steam' / 'root',
        home / '.steam' / 'steam',
        home / '.local' / 'share' / 'Steam',
        home / '.steam' / 'debian-installation',
        home / '.var' / 'app' / 'com.valvesoftware.Steam' / '.local' / 'share' / 'Steam',
        home / '.var' / 'app' / 'com.valvesoftware.Steam' / 'data' / 'Steam',
        home / 'snap' / 'steam' / 'common' / '.local' / 'share' / 'Steam',
    ]
    for c in candidates:
        if (c / 'steamapps').is_dir():
            return c
    return home / '.local' / 'share' / 'Steam'

STEAM_ROOT      = _detect_steam_root()
WINEPREFIX_PATH = STEAM_ROOT / 'steamapps' / 'compatdata' / '480'

EXCLUDE_DIRS  = {'_CommonRedist', 'Binaries'}
EXCLUDE_NAMES = re.compile(r'CrashHandler', re.IGNORECASE)

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

# ── Game directory scanning ───────────────────────────────────────────────────
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
