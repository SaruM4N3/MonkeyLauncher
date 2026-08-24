#!/bin/bash
set -e

RESOURCES="$(cd "$(dirname "$0")/src" && pwd)"
INSTALL_BIN="$HOME/.local/bin"
INSTALL_DESKTOP="$HOME/.local/share/applications"
BUILD_DIR="$(cd "$(dirname "$0")" && pwd)/dist"

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
BOLD='\033[1m'
NC='\033[0m'

ok()      { echo -e "${GREEN}[✓]${NC} $1"; }
warn()    { echo -e "${YELLOW}[!]${NC} $1"; }
fail()    { echo -e "${RED}[✗]${NC} $1"; exit 1; }
info()    { echo -e "    $1"; }
section() { echo -e "\n${BOLD}$1${NC}"; }

echo ""
echo -e "${BOLD}  MonkeyLauncher Installer${NC}"
echo "  ========================"
echo ""

# ── Detect distribution ────────────────────────────────────────────────────────
section "Detecting distribution…"

DISTRO_NAME="Unknown"
if [ -f /etc/os-release ]; then
  # shellcheck disable=SC1091
  . /etc/os-release
  DISTRO_NAME="${PRETTY_NAME:-${NAME:-Unknown}}"
else
  warn "/etc/os-release not found — distribution name unknown, falling back to package-manager detection."
fi
ok "Distribution: $DISTRO_NAME"

# ── Detect package manager ─────────────────────────────────────────────────────
# On Arch: prefer AUR helpers (paru > yay) for AUR packages, fall back to pacman.
# The package manager (not the distro name) drives which install commands run
# below, since it covers derivatives (Manjaro, Nobara, Ubuntu flavors…) too.
section "Detecting package manager…"

AUR_HELPER=""
PKG_MGR=""

if command -v pacman &>/dev/null; then
  PKG_MGR="pacman"
  if   command -v paru &>/dev/null; then AUR_HELPER="paru"
  elif command -v yay  &>/dev/null; then AUR_HELPER="yay"
  fi
  if [ -n "$AUR_HELPER" ]; then
    ok "$DISTRO_NAME — pacman + AUR helper: $AUR_HELPER"
  else
    ok "$DISTRO_NAME — pacman (no AUR helper found)"
    warn "Some packages may be AUR-only. Consider installing paru or yay for automatic AUR support."
  fi
elif command -v apt-get &>/dev/null; then
  PKG_MGR="apt"
  ok "$DISTRO_NAME — apt"
elif command -v dnf &>/dev/null; then
  PKG_MGR="dnf"
  ok "$DISTRO_NAME — dnf"
elif command -v zypper &>/dev/null; then
  PKG_MGR="zypper"
  ok "$DISTRO_NAME — zypper"
else
  PKG_MGR="unknown"
  warn "Unknown package manager for $DISTRO_NAME — you will need to install dependencies manually."
fi

# install via AUR helper if available, else pacman/apt/dnf
# usage: install_pkg <pacman> <apt> <dnf> <zypper>
install_pkg() {
  local pacman="$1" apt="$2" dnf="$3" zypper="${4:-$3}"
  # Each branch is allowed to fail (unknown package, no repo, …) without
  # killing the whole script under `set -e` — callers check afterward
  # (via `command -v`) whether the install actually worked.
  case "$PKG_MGR" in
    pacman)
      if [ -n "$AUR_HELPER" ]; then
        $AUR_HELPER -S --noconfirm --needed --overwrite '*' $pacman || true
      else
        sudo pacman -S --noconfirm --needed --overwrite '*' $pacman || true
      fi ;;
    # $apt/$dnf/$zypper are intentionally unquoted: some callers pass
    # multiple space-separated package names in one string (e.g. the GTK3
    # bindings), and each needs to reach the package manager as a separate
    # argument — quoting them would pass "pkg1 pkg2" as a single (bogus)
    # package name instead.
    apt)    sudo apt-get install -y $apt    || true ;;
    dnf)    sudo dnf install -y    $dnf    || true ;;
    zypper) sudo zypper install -y $zypper || true ;;
    *)      warn "Install '$pacman' manually then re-run." ;;
  esac
}

check_or_install() {
  local cmd="$1" pacman="$2" apt="$3" dnf="$4" zypper="${5:-$4}"
  if command -v "$cmd" &>/dev/null; then
    ok "$cmd"
  else
    warn "$cmd not found — installing…"
    install_pkg "$pacman" "$apt" "$dnf" "$zypper"
    command -v "$cmd" &>/dev/null && ok "$cmd" || fail "Failed to install $cmd"
  fi
}

# Like check_or_install, but never aborts the script on failure — used for
# best-effort steps (e.g. build dependencies for umu-launcher from source)
# where "not available on this distro" is an expected, recoverable outcome.
try_install() {
  local cmd="$1" pacman="$2" apt="$3" dnf="$4" zypper="${5:-$4}"
  if command -v "$cmd" &>/dev/null; then
    ok "$cmd"
    return 0
  fi
  warn "$cmd not found — installing…"
  install_pkg "$pacman" "$apt" "$dnf" "$zypper"
  if command -v "$cmd" &>/dev/null; then
    ok "$cmd"
    return 0
  fi
  warn "Failed to install $cmd"
  return 1
}

# Ensures `python3 -m pip` works, installing python3-pip if needed (it's a
# separate package from python3 on Debian/Ubuntu/Fedora/openSUSE). Used
# wherever we fall back to pip.
# `python3 -m pip --version` can succeed while `python3 -m venv` still fails:
# on Debian/Ubuntu, ensurepip's bundled wheels ship in the separate
# python3-venv package, not python3-pip. Check the thing we actually need.
_venv_works() {
  local d; d=$(mktemp -d)
  python3 -m venv "$d" &>/dev/null
  local status=$?
  rm -rf "$d"
  return $status
}

ensure_pip() {
  if _venv_works; then
    ok "python3-pip"
    return 0
  fi
  warn "python3 venv/pip support not found — installing…"
  install_pkg python-pip "python3-pip python3-venv" python3-pip python3-pip
  if _venv_works; then
    ok "python3-pip"
    return 0
  fi
  warn "Failed to install python3 venv/pip support"
  return 1
}

# ── Runtime dependencies ───────────────────────────────────────────────────────
section "Checking runtime dependencies…"

#                        cmd            pacman           apt           dnf           zypper
check_or_install         fzf            fzf              fzf           fzf           fzf

# winetricks lives in Debian's "contrib" component, which isn't enabled by
# default on a fresh install (unlike Ubuntu, where the equivalent "universe"
# component usually already is) — give a specific hint instead of the
# generic failure message in that case.
if ! command -v winetricks &>/dev/null; then
  warn "winetricks not found — installing…"
  install_pkg winetricks winetricks winetricks winetricks
  if command -v winetricks &>/dev/null; then
    ok "winetricks"
  elif [ "$PKG_MGR" = "apt" ] && [[ "$DISTRO_NAME" == Debian* ]]; then
    fail "Failed to install winetricks — on Debian it lives in the 'contrib' component, which is disabled by default. Enable it (e.g. 'sudo apt edit-sources', add 'contrib' next to 'main'), run 'sudo apt update', then re-run this installer."
  else
    fail "Failed to install winetricks"
  fi
fi

check_or_install         xdg-open       xdg-utils        xdg-utils     xdg-utils     xdg-utils
check_or_install         pgrep          procps-ng        procps        procps-ng     procps

# protontricks: packaged natively everywhere we support (Debian: contrib,
# Ubuntu: multiverse, Fedora, openSUSE, Arch) — pip is just a last-resort
# fallback in case a particular repo setup doesn't have it.
if command -v protontricks &>/dev/null; then
  ok "protontricks"
else
  warn "protontricks not found — installing…"
  install_pkg protontricks protontricks protontricks protontricks
  if command -v protontricks &>/dev/null; then
    ok "protontricks"
  elif ensure_pip && python3 -m pip install --user --quiet protontricks; then
    ok "protontricks (via pip)"
  else
    fail "Failed to install protontricks"
  fi
fi

# mangohud: official package on pacman/apt/dnf/zypper
check_or_install mangohud mangohud mangohud mangohud mangohud

# umu-launcher: official package on Arch (pacman) and Nobara (dnf). Elsewhere
# (Debian/Ubuntu, vanilla Fedora, openSUSE…) there's no official package in the
# default repos — we still try the native package manager first (in case the
# user already has a relevant third-party repo enabled), then build from
# source per the project's own instructions if that fails.
if command -v umu-run &>/dev/null; then
  ok "umu-run"
else
  warn "umu-run not found — trying the official package for $PKG_MGR…"
  UMU_INSTALLED=0
  case "$PKG_MGR" in
    pacman) install_pkg umu-launcher umu-launcher umu-launcher umu-launcher ;;
    dnf)    install_pkg umu-launcher umu-launcher umu-launcher umu-launcher ;;
    zypper) install_pkg umu-launcher umu-launcher umu-launcher umu-launcher ;;
  esac
  command -v umu-run &>/dev/null && UMU_INSTALLED=1

  if [ "$UMU_INSTALLED" -eq 1 ]; then
    ok "umu-run"
  else
    section "No official umu-launcher package for $DISTRO_NAME — building from source…"
    warn "Needs git, make and python3-pip (already checked below) — may take a minute."

    UMU_BUILD_OK=1
    try_install git     git     git     git     git     || UMU_BUILD_OK=0
    try_install make    make    make    make    make    || UMU_BUILD_OK=0
    # The build itself needs python3 (venv + pip) — checked again, harmlessly,
    # in the "Python & GTK3" section further down.
    try_install python3 python3 python3 python3 python3 || UMU_BUILD_OK=0

    # The build's venv step needs pip/ensurepip, not just python3 itself.
    ensure_pip || UMU_BUILD_OK=0

    if [ "$UMU_BUILD_OK" -eq 1 ]; then
      UMU_SRC_DIR="$(mktemp -d)"
      if git clone --recurse-submodules --depth 1 \
           https://github.com/Open-Wine-Components/umu-launcher "$UMU_SRC_DIR"; then
        if ! ( cd "$UMU_SRC_DIR" && ./configure.sh --user-install && make && make install ); then
          UMU_BUILD_OK=0
        fi
      else
        UMU_BUILD_OK=0
      fi
      rm -rf "$UMU_SRC_DIR"
    fi

    if [ "$UMU_BUILD_OK" -eq 1 ] && command -v umu-run &>/dev/null; then
      ok "umu-run (built from source) → $HOME/.local/bin/umu-run"
    else
      warn "Could not build umu-launcher automatically."
      warn "Install it manually: https://github.com/Open-Wine-Components/umu-launcher"
    fi
  fi
fi

# umu-run + Python 3.14 compatibility: Python 3.14 introduced a built-in
# compression.zstd module that conflicts with pyzstd when libzstd is too old
# (missing ZSTD_defaultCLevel, added in libzstd 1.5.5). Ensure zstd is current.
if command -v umu-run &>/dev/null; then
  if python3 -c "import sys; exit(0 if sys.version_info >= (3,14) else 1)" 2>/dev/null; then
    warn "Python 3.14 detected — ensuring libzstd is up to date for umu-run compatibility…"
    install_pkg zstd zstd zstd zstd
    # Also upgrade pyzstd in case the installed version predates Python 3.14 support
    python3 -m pip install --user --quiet --upgrade pyzstd 2>/dev/null && ok "pyzstd updated" || true
    # Verify umu-run actually imports cleanly now
    if python3 -c "import umu" 2>/dev/null || umu-run --help &>/dev/null; then
      ok "umu-run Python 3.14 compatibility"
    else
      warn "umu-run may still have import issues — try: sudo pacman -Syu zstd"
    fi
  fi
fi

# ── Python & GTK3 ─────────────────────────────────────────────────────────────
section "Checking Python / GTK3…"

check_or_install python3 python3 python3 python3 python3

if python3 -c "import gi; gi.require_version('Gtk','3.0'); from gi.repository import Gtk" &>/dev/null; then
  ok "python3-gi + GTK3 runtime"
else
  warn "python3-gi / GTK3 not found — installing…"
  install_pkg "python-gobject gtk3" \
              "python3-gi gir1.2-gtk-3.0 libgtk-3-0" \
              "python3-gobject gtk3" \
              "python3-gobject typelib-1_0-Gtk-3_0"
  python3 -c "import gi; gi.require_version('Gtk','3.0'); from gi.repository import Gtk" \
    && ok "python3-gi + GTK3 runtime" \
    || fail "Failed to install GTK3 bindings"
fi

# Verify GObject typelibs are present (needed at runtime by the PyInstaller binary)
if ! python3 -c "
import gi, sys
for mod, ver in [('Gtk','3.0'),('Gdk','3.0'),('Pango','1.0'),('GLib','2.0')]:
    try:
        gi.require_version(mod, ver)
        __import__('gi.repository.' + mod)
    except Exception as e:
        print(f'Missing typelib: {mod}-{ver} ({e})', file=sys.stderr)
        sys.exit(1)
" 2>/dev/null; then
  warn "GObject typelibs incomplete — installing…"
  install_pkg "gobject-introspection gtk3" \
              "gir1.2-gtk-3.0 gir1.2-gdk-3.0 gir1.2-pango-1.0" \
              "gobject-introspection gtk3" \
              "typelib-1_0-Gtk-3_0 typelib-1_0-Pango-1_0"
  ok "GObject typelibs installed"
else
  ok "GObject typelibs (Gtk, Gdk, Pango, GLib)"
fi

# ── Fonts ─────────────────────────────────────────────────────────────────────
section "Checking fonts…"

if command -v fc-list &>/dev/null && fc-list : family 2>/dev/null | grep -qi "dejavu\|liberation\|noto"; then
  ok "GTK fonts (DejaVu / Liberation / Noto already present)"
else
  warn "Core GTK fonts not found — installing…"
  install_pkg "ttf-dejavu noto-fonts" \
              "fonts-dejavu-core fonts-liberation fonts-noto-core fontconfig" \
              "dejavu-fonts-all liberation-fonts google-noto-fonts-common" \
              "dejavu-fonts liberation-fonts google-noto-fonts"
  command -v fc-cache &>/dev/null && fc-cache -f
  ok "fonts installed"
fi

# ── Clean up old PyInstaller artifacts ────────────────────────────────────────
rm -f "$BUILD_DIR/MonkeyLauncher"
rm -rf "$BUILD_DIR/.pyinstaller_work" "$BUILD_DIR/.pyinstaller_spec" "$BUILD_DIR/.venv"

# ── Install app files ──────────────────────────────────────────────────────────
section "Installing app files…"

INSTALL_LIB="$HOME/.local/lib/monkeylauncher"
mkdir -p "$INSTALL_LIB" "$INSTALL_BIN"

install -m 644 "$RESOURCES/MonkeyLauncherGUI.py" "$INSTALL_LIB/MonkeyLauncherGUI.py"
ok "GUI script → $INSTALL_LIB/MonkeyLauncherGUI.py"

# ── Install binaries ───────────────────────────────────────────────────────────
section "Installing binaries…"

# GUI launcher wrapper — runs the Python script with the system interpreter
cat > "$INSTALL_BIN/MonkeyLauncher" <<WRAPPER
#!/bin/sh
exec python3 "$INSTALL_LIB/MonkeyLauncherGUI.py" "\$@"
WRAPPER
chmod 755 "$INSTALL_BIN/MonkeyLauncher"
ok "MonkeyLauncher → $INSTALL_BIN/MonkeyLauncher"

install -m 755 "$RESOURCES/MonkeyLauncherCLI.sh" "$INSTALL_BIN/MonkeyLauncherCLI"
ok "MonkeyLauncher    → $INSTALL_BIN/MonkeyLauncher"
ok "MonkeyLauncherCLI → $INSTALL_BIN/MonkeyLauncherCLI"

# ── Install icon ───────────────────────────────────────────────────────────────
section "Installing icon…"

ICON_SRC="$RESOURCES/logo.png"
ICON_DEST="$HOME/.local/share/icons/monkeylauncher.png"

if [ -f "$ICON_SRC" ]; then
  mkdir -p "$(dirname "$ICON_DEST")"
  install -m 644 "$ICON_SRC" "$ICON_DEST"
  ok "Icon → $ICON_DEST"
else
  warn "logo.png not found in .resources — using fallback icon"
  ICON_DEST="applications-games"
fi

# ── Install desktop entry (with resolved Exec and Icon paths) ──────────────────
section "Installing desktop entry…"

mkdir -p "$INSTALL_DESKTOP"
cat > "$INSTALL_DESKTOP/monkeylauncher.desktop" <<EOF
[Desktop Entry]
Name=MonkeyLauncher
Comment=Wine/Proton game launcher
Exec=$INSTALL_BIN/MonkeyLauncher
Icon=$ICON_DEST
Type=Application
Categories=Game;
Terminal=false
StartupNotify=true
EOF

if command -v update-desktop-database &>/dev/null; then
  update-desktop-database "$INSTALL_DESKTOP"
fi
ok "Desktop entry → $INSTALL_DESKTOP/monkeylauncher.desktop"
info "Exec: $INSTALL_BIN/MonkeyLauncher"
info "Icon: $ICON_DEST"

# ── Shell alias ────────────────────────────────────────────────────────────────
section "Setting up shell alias…"

add_alias_to() {
  local rc="$1"
  [ -f "$rc" ] || return 0
  if ! grep -q 'local/bin' "$rc"; then
    echo "" >> "$rc"
    echo 'export PATH="$HOME/.local/bin:$PATH"' >> "$rc"
    ok "PATH updated in $rc"
  fi
  # Remove any stale MonkeyLauncher aliases before rewriting
  grep -v "alias MonkeyLauncher=" "$rc" \
    | grep -v "alias monkeylauncher-cli=" \
    | grep -v "# MonkeyLauncher$" \
    > "$rc.mklncher_tmp" && mv "$rc.mklncher_tmp" "$rc"
  echo "" >> "$rc"
  echo "# MonkeyLauncher" >> "$rc"
  echo "alias MonkeyLauncher='$INSTALL_BIN/MonkeyLauncher'" >> "$rc"
  echo "alias monkeylauncher-cli='$INSTALL_BIN/MonkeyLauncherCLI'" >> "$rc"
  ok "Aliases written to $rc"
}

add_alias_to_fish() {
  local rc="$HOME/.config/fish/config.fish"
  [ -f "$rc" ] || return 0
  if ! grep -q 'local/bin' "$rc"; then
    echo "" >> "$rc"
    echo 'fish_add_path "$HOME/.local/bin"' >> "$rc"
    ok "PATH updated in $rc"
  fi
  grep -v "alias MonkeyLauncher" "$rc" \
    | grep -v "alias monkeylauncher-cli" \
    | grep -v "# MonkeyLauncher$" \
    > "$rc.mklncher_tmp" && mv "$rc.mklncher_tmp" "$rc"
  echo "" >> "$rc"
  echo "# MonkeyLauncher" >> "$rc"
  echo "alias MonkeyLauncher='$INSTALL_BIN/MonkeyLauncher'" >> "$rc"
  echo "alias monkeylauncher-cli='$INSTALL_BIN/MonkeyLauncherCLI'" >> "$rc"
  ok "Aliases written to $rc"
}

add_alias_to "$HOME/.bashrc"
add_alias_to "$HOME/.zshrc"
add_alias_to_fish

info "Run 'source ~/.bashrc' (or ~/.zshrc) to activate the alias in the current session."

# ── PATH check ─────────────────────────────────────────────────────────────────
if [[ ":$PATH:" != *":$INSTALL_BIN:"* ]]; then
  warn "$INSTALL_BIN not in current session PATH — will be active after next login or sourcing your rc file."
fi

echo ""
echo -e "${GREEN}${BOLD}Done!${NC}"
echo -e "  GUI → run ${BOLD}MonkeyLauncher${NC}"
echo -e "  CLI → run ${BOLD}monkeylauncher-cli${NC}"
echo ""
