import json
import shutil
import tarfile
import tempfile
import urllib.request
from pathlib import Path

from .logging_setup import log
from .version import __version__ as CURRENT_VERSION

REPO       = 'SaruM4N3/MonkeyLauncher'
API_LATEST = f'https://api.github.com/repos/{REPO}/releases/latest'

# INSTALL_LIB is wherever this package's parent dir is — ~/.local/lib/monkeylauncher
# for a source install, /usr/lib/monkeylauncher for a .deb/Arch package, or the
# repo's src/ dir for a dev checkout run in place.
INSTALL_LIB     = Path(__file__).resolve().parent.parent
INSTALL_BIN     = Path.home() / '.local' / 'bin'
ICON_DEST       = Path.home() / '.local' / 'share' / 'icons' / 'monkeylauncher.png'

def is_source_install():
    """True for an install.sh (~/.local) install, False for a distro package
    (.deb/Arch, both under /usr) — those get updated via the package manager
    instead of having their files overwritten here."""
    try:
        INSTALL_LIB.relative_to(Path.home())
        return True
    except ValueError:
        return False

def is_dev_checkout():
    """True when running straight out of a git checkout (e.g. `python3
    src/MonkeyLauncherGUI.py`) rather than an installed copy — one-click
    update is skipped there since overwriting the working tree isn't wanted."""
    return (INSTALL_LIB.parent / '.git').exists()

def _parse_version(v):
    parts = []
    for p in v.lstrip('vV').split('.'):
        digits = ''.join(c for c in p if c.isdigit())
        parts.append(int(digits) if digits else 0)
    return tuple(parts)

def is_newer(latest, current=CURRENT_VERSION):
    return _parse_version(latest) > _parse_version(current)

def check_latest_release():
    """Returns info about the latest GitHub release, or raises on
    network/parse failure."""
    req = urllib.request.Request(API_LATEST, headers={
        'User-Agent': 'MonkeyLauncher',
        'Accept':     'application/vnd.github+json',
    })
    with urllib.request.urlopen(req, timeout=10) as resp:
        data = json.load(resp)
    return {
        'version':     data.get('tag_name', '').lstrip('vV'),
        'body':        data.get('body', ''),
        'html_url':    data.get('html_url', ''),
        'tarball_url': data.get('tarball_url', ''),
    }

def perform_source_update(tarball_url):
    """Downloads and extracts the given release tarball, then copies app
    files over the current source install in place — mirrors install.sh's
    'Install app files' section. Runtime dependencies are left untouched,
    since a normal release doesn't change them."""
    with tempfile.TemporaryDirectory() as tmp_str:
        tmp = Path(tmp_str)
        archive = tmp / 'release.tar.gz'
        req = urllib.request.Request(tarball_url, headers={'User-Agent': 'MonkeyLauncher'})
        with urllib.request.urlopen(req, timeout=30) as resp, open(archive, 'wb') as f:
            shutil.copyfileobj(resp, f)

        with tarfile.open(archive) as tf:
            tf.extractall(tmp)

        roots = [p for p in tmp.iterdir() if p.is_dir()]
        if not roots:
            raise RuntimeError("Downloaded release archive was empty")
        root = roots[0]
        src  = root / 'src'

        INSTALL_LIB.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src / 'MonkeyLauncherGUI.py', INSTALL_LIB / 'MonkeyLauncherGUI.py')

        pkg_dst = INSTALL_LIB / 'monkeylauncher'
        if pkg_dst.exists():
            shutil.rmtree(pkg_dst)
        shutil.copytree(src / 'monkeylauncher', pkg_dst)

        version_file = root / 'VERSION'
        if version_file.exists():
            shutil.copy2(version_file, INSTALL_LIB / 'VERSION')

        cli_dst = INSTALL_BIN / 'MonkeyLauncherCLI'
        if (src / 'MonkeyLauncherCLI.sh').exists():
            shutil.copy2(src / 'MonkeyLauncherCLI.sh', cli_dst)
            cli_dst.chmod(0o755)

        if (src / 'logo.png').exists():
            ICON_DEST.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src / 'logo.png', ICON_DEST)

    log.info(f"Updated MonkeyLauncher files in {INSTALL_LIB}")
