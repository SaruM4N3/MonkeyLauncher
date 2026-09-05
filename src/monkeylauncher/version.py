from pathlib import Path

def get_version():
    """Reads the VERSION file installed alongside MonkeyLauncherGUI.py
    (INSTALL_LIB/VERSION for both source and package installs), falling
    back to the repo root for a dev checkout run straight out of src/."""
    here = Path(__file__).resolve()
    for candidate in (here.parent.parent / 'VERSION', here.parent.parent.parent / 'VERSION'):
        if candidate.exists():
            return candidate.read_text().strip()
    return '0.0.0'

__version__ = get_version()
