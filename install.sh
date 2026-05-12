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

# ── Detect package manager ─────────────────────────────────────────────────────
# On Arch: prefer AUR helpers (paru > yay) for AUR packages, fall back to pacman
section "Detecting package manager…"

AUR_HELPER=""
PKG_MGR=""

if command -v pacman &>/dev/null; then
  PKG_MGR="pacman"
  if   command -v paru &>/dev/null; then AUR_HELPER="paru"
  elif command -v yay  &>/dev/null; then AUR_HELPER="yay"
  fi
  if [ -n "$AUR_HELPER" ]; then
    ok "Arch Linux — pacman + AUR helper: $AUR_HELPER"
  else
    ok "Arch Linux — pacman (no AUR helper found)"
    warn "Some packages may be AUR-only (umu-launcher, mangohud)."
    warn "Consider installing paru or yay for automatic AUR support."
  fi
elif command -v apt-get &>/dev/null; then
  PKG_MGR="apt"
  ok "Debian/Ubuntu — apt"
elif command -v dnf &>/dev/null; then
  PKG_MGR="dnf"
  ok "Fedora/RHEL — dnf"
elif command -v zypper &>/dev/null; then
  PKG_MGR="zypper"
  ok "openSUSE — zypper"
else
  PKG_MGR="unknown"
  warn "Unknown package manager — you will need to install dependencies manually."
fi

# install via AUR helper if available, else pacman/apt/dnf
# usage: install_pkg <pacman> <apt> <dnf> <zypper>
install_pkg() {
  local pacman="$1" apt="$2" dnf="$3" zypper="${4:-$3}"
  case "$PKG_MGR" in
    pacman)
      if [ -n "$AUR_HELPER" ]; then
        $AUR_HELPER -S --noconfirm --needed --overwrite '*' $pacman
      else
        sudo pacman -S --noconfirm --needed --overwrite '*' $pacman
      fi ;;
    apt)    sudo apt-get install -y "$apt"    ;;
    dnf)    sudo dnf install -y    "$dnf"    ;;
    zypper) sudo zypper install -y "$zypper" ;;
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

# ── Runtime dependencies ───────────────────────────────────────────────────────
section "Checking runtime dependencies…"

#                        cmd         pacman        apt                  dnf              zypper
check_or_install         fzf         fzf           fzf                  fzf              fzf
check_or_install         winetricks  winetricks    winetricks           winetricks       winetricks
check_or_install         xdg-open    xdg-utils     xdg-utils            xdg-utils        xdg-utils
check_or_install         pgrep       procps-ng     procps               procps-ng        procps

# mangohud: not in official apt/dnf repos — warn on non-Arch
if command -v mangohud &>/dev/null; then
  ok "mangohud"
elif [ "$PKG_MGR" = "pacman" ]; then
  warn "mangohud not found — installing…"
  install_pkg mangohud mangohud mangohud mangohud
  command -v mangohud &>/dev/null && ok "mangohud" || fail "Failed to install mangohud"
else
  warn "mangohud not in official repos for your distro — install it manually (https://github.com/flightlessmango/MangoHud) then re-run."
fi

# umu-launcher: AUR on Arch, unofficial on other distros
if command -v umu-run &>/dev/null; then
  ok "umu-run"
elif [ "$PKG_MGR" = "pacman" ]; then
  warn "umu-launcher not found — installing…"
  install_pkg umu-launcher umu-launcher umu-launcher umu-launcher
  command -v umu-run &>/dev/null && ok "umu-run" || fail "Failed to install umu-launcher"
else
  warn "umu-launcher is not in official repos for your distro — install it manually (https://github.com/Open-Wine-Components/umu-launcher) then re-run."
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

# ── Build tools ────────────────────────────────────────────────────────────────
section "Checking build tools…"

if command -v pyinstaller &>/dev/null; then
  ok "pyinstaller"
  PYINSTALLER="pyinstaller"
elif python3 -m PyInstaller --version &>/dev/null 2>&1; then
  ok "pyinstaller (python module)"
  PYINSTALLER="python3 -m PyInstaller"
else
  warn "pyinstaller not found — installing…"
  install_pkg python-pyinstaller python3-pyinstaller python3-pyinstaller python3-pyinstaller
  if   command -v pyinstaller                        &>/dev/null; then PYINSTALLER="pyinstaller"
  elif python3 -m PyInstaller --version &>/dev/null 2>&1;         then PYINSTALLER="python3 -m PyInstaller"
  else fail "Failed to install pyinstaller"
  fi
fi

# ── Compile ────────────────────────────────────────────────────────────────────
section "Compiling…"
mkdir -p "$BUILD_DIR" "$INSTALL_BIN"

echo "  GUI (pyinstaller)…"
$PYINSTALLER \
  --onefile \
  --name MonkeyLauncher \
  --distpath "$BUILD_DIR" \
  --workpath "$BUILD_DIR/.pyinstaller_work" \
  --specpath "$BUILD_DIR/.pyinstaller_spec" \
  --noconfirm \
  --clean \
  --hidden-import gi \
  --hidden-import gi.repository.Gtk \
  --hidden-import gi.repository.GLib \
  --hidden-import gi.repository.Pango \
  --hidden-import gi.repository.Gdk \
  "$RESOURCES/MonkeyLauncherGUI.py" \
  > "$BUILD_DIR/pyinstaller.log" 2>&1 \
  && ok "GUI compiled" \
  || fail "PyInstaller failed — see $BUILD_DIR/pyinstaller.log"

echo "  CLI…"
install -m 755 "$RESOURCES/MonkeyLauncherCLI.sh" "$BUILD_DIR/MonkeyLauncherCLI"
ok "CLI ready"

# ── Install binaries ───────────────────────────────────────────────────────────
section "Installing binaries…"

install -m 755 "$BUILD_DIR/MonkeyLauncher"    "$INSTALL_BIN/MonkeyLauncher"
install -m 755 "$BUILD_DIR/MonkeyLauncherCLI" "$INSTALL_BIN/MonkeyLauncherCLI"
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

ALIAS_LINE="alias MonkeyLauncher='$INSTALL_BIN/MonkeyLauncherCLI'"

add_alias_to() {
  local rc="$1"
  if [ -f "$rc" ]; then
    # Ensure ~/.local/bin is in PATH
    if ! grep -q 'local/bin' "$rc"; then
      echo "" >> "$rc"
      echo 'export PATH="$HOME/.local/bin:$PATH"' >> "$rc"
      ok "PATH updated in $rc"
    fi
    # Add alias
    if grep -q "alias MonkeyLauncher=" "$rc"; then
      ok "Alias already present in $rc"
    else
      echo "" >> "$rc"
      echo "# MonkeyLauncher CLI" >> "$rc"
      echo "$ALIAS_LINE" >> "$rc"
      ok "Alias added to $rc"
    fi
  fi
}

add_alias_to_fish() {
  local rc="$HOME/.config/fish/config.fish"
  [ -f "$rc" ] || return 0
  if ! grep -q 'local/bin' "$rc"; then
    echo "" >> "$rc"
    echo 'fish_add_path "$HOME/.local/bin"' >> "$rc"
    ok "PATH updated in $rc"
  fi
  if grep -q "alias MonkeyLauncher" "$rc"; then
    ok "Alias already present in $rc"
  else
    echo "" >> "$rc"
    echo "# MonkeyLauncher CLI" >> "$rc"
    echo "alias MonkeyLauncher='$INSTALL_BIN/MonkeyLauncherCLI'" >> "$rc"
    ok "Alias added to $rc"
  fi
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
echo -e "  CLI → run ${BOLD}MonkeyLauncher${NC} (alias) or ${BOLD}MonkeyLauncherCLI${NC} directly"
echo ""
