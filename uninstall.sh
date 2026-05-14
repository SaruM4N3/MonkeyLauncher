#!/bin/bash
set -e

INSTALL_BIN="$HOME/.local/bin"
INSTALL_LIB="$HOME/.local/lib/monkeylauncher"
INSTALL_DESKTOP="$HOME/.local/share/applications"
ICON_DEST="$HOME/.local/share/icons/monkeylauncher.png"
CONFIG_DIR="$HOME/.config/MonkeyLauncher"

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
BOLD='\033[1m'
NC='\033[0m'

ok()      { echo -e "${GREEN}[✓]${NC} $1"; }
warn()    { echo -e "${YELLOW}[!]${NC} $1"; }
info()    { echo -e "    $1"; }
section() { echo -e "\n${BOLD}$1${NC}"; }

echo ""
echo -e "${BOLD}  MonkeyLauncher Uninstaller${NC}"
echo "  ==========================="
echo ""

# ── Remove binaries & lib ──────────────────────────────────────────────────────
section "Removing binaries…"

for bin in MonkeyLauncher MonkeyLauncherCLI; do
  if [ -f "$INSTALL_BIN/$bin" ]; then
    rm -f "$INSTALL_BIN/$bin"
    ok "Removed $INSTALL_BIN/$bin"
  else
    warn "$INSTALL_BIN/$bin not found — skipping"
  fi
done

if [ -d "$INSTALL_LIB" ]; then
  rm -rf "$INSTALL_LIB"
  ok "Removed $INSTALL_LIB"
fi

# ── Remove icon ────────────────────────────────────────────────────────────────
section "Removing icon…"

if [ -f "$ICON_DEST" ]; then
  rm -f "$ICON_DEST"
  ok "Removed $ICON_DEST"
else
  warn "Icon not found — skipping"
fi

# ── Remove desktop entry ───────────────────────────────────────────────────────
section "Removing desktop entry…"

DESKTOP_FILE="$INSTALL_DESKTOP/monkeylauncher.desktop"
if [ -f "$DESKTOP_FILE" ]; then
  rm -f "$DESKTOP_FILE"
  ok "Removed $DESKTOP_FILE"
  if command -v update-desktop-database &>/dev/null; then
    update-desktop-database "$INSTALL_DESKTOP"
  fi
else
  warn "Desktop entry not found — skipping"
fi

# ── Clean shell rc files ───────────────────────────────────────────────────────
section "Cleaning shell config files…"

remove_from_rc() {
  local rc="$1"
  [ -f "$rc" ] || return 0
  # Remove the alias line and its comment header
  if grep -q "MonkeyLauncher" "$rc"; then
    # Use a temp file to strip the comment + alias block
    grep -v "# MonkeyLauncher CLI" "$rc" \
      | grep -v "alias MonkeyLauncher=" \
      > "$rc.mklncher_tmp" && mv "$rc.mklncher_tmp" "$rc"
    ok "Cleaned $rc"
  else
    info "$rc — nothing to remove"
  fi
}

remove_from_fish() {
  local rc="$HOME/.config/fish/config.fish"
  [ -f "$rc" ] || return 0
  if grep -q "MonkeyLauncher" "$rc"; then
    grep -v "# MonkeyLauncher CLI" "$rc" \
      | grep -v "alias MonkeyLauncher=" \
      > "$rc.mklncher_tmp" && mv "$rc.mklncher_tmp" "$rc"
    ok "Cleaned $rc"
  else
    info "$rc — nothing to remove"
  fi
}

remove_from_rc "$HOME/.bashrc"
remove_from_rc "$HOME/.zshrc"
remove_from_fish

info "Note: PATH additions in rc files were not removed (safe to keep)."

# ── Optionally remove config / saves ──────────────────────────────────────────
section "User data…"

if [ -d "$CONFIG_DIR" ]; then
  echo -e "  ${YELLOW}Found user data at:${NC} $CONFIG_DIR"
  echo -e "  This includes your game configs and save symlinks."
  echo ""
  read -r -p "  Delete all user data? [y/N] " confirm
  echo ""
  if [[ "$confirm" =~ ^[Yy]$ ]]; then
    rm -rf "$CONFIG_DIR"
    ok "Removed $CONFIG_DIR"
  else
    ok "User data kept at $CONFIG_DIR"
  fi
else
  info "No user data found at $CONFIG_DIR"
fi

echo ""
echo -e "${GREEN}${BOLD}Done!${NC} MonkeyLauncher has been uninstalled."
echo -e "  Run ${BOLD}source ~/.bashrc${NC} (or ~/.zshrc) to clear the alias from your current session."
echo ""
