#!/bin/bash
set -e

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
RESOURCES="$REPO_ROOT/src"
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

# ── Dependencies ───────────────────────────────────────────────────────────────
# Distro detection, runtime tools, build toolchain and fonts live in
# installdependencies.sh (also runnable on its own). Pass --skip-deps to
# skip it when everything is already installed.
if [ "${1:-}" = "--skip-deps" ]; then
  warn "Skipping dependency installation (--skip-deps)"
else
  ML_INSTALLER=1 "$REPO_ROOT/installdependencies.sh"
fi


# ── Clean up old PyInstaller artifacts ────────────────────────────────────────
rm -f "$BUILD_DIR/MonkeyLauncher"
rm -rf "$BUILD_DIR/.pyinstaller_work" "$BUILD_DIR/.pyinstaller_spec" "$BUILD_DIR/.venv"

# ── Build & install app files ──────────────────────────────────────────────────
section "Building MonkeyLauncher…"

info "Compiling (this can take a minute)…"
make -C "$REPO_ROOT" || fail "Build failed — see the compiler output above"
ok "Compiled build/monkeylauncher"

section "Installing app files…"

INSTALL_LIB="$HOME/.local/lib/monkeylauncher"
mkdir -p "$INSTALL_LIB" "$INSTALL_BIN"

# Leftovers from the old Python version: its package directory was named
# "monkeylauncher", the same name the compiled binary uses now.
rm -f  "$INSTALL_LIB/MonkeyLauncherGUI.py"
[ -d "$INSTALL_LIB/monkeylauncher" ] && rm -rf "$INSTALL_LIB/monkeylauncher"

install -m 755 "$REPO_ROOT/build/monkeylauncher" "$INSTALL_LIB/monkeylauncher"
ok "GUI binary → $INSTALL_LIB/monkeylauncher"

install -m 644 "$REPO_ROOT/VERSION" "$INSTALL_LIB/VERSION"
ok "VERSION → $INSTALL_LIB/VERSION"

# ── Install binaries ───────────────────────────────────────────────────────────
section "Installing binaries…"

# GUI launcher wrapper — keeps ~/.local/bin/MonkeyLauncher as the entry point
cat > "$INSTALL_BIN/MonkeyLauncher" <<WRAPPER
#!/bin/sh
exec "$INSTALL_LIB/monkeylauncher" "\$@"
WRAPPER
chmod 755 "$INSTALL_BIN/MonkeyLauncher"

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
