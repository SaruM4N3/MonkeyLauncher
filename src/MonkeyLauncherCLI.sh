#!/bin/bash

CONFIG_FILE="$HOME/.config/MonkeyLauncher/config"
GAMES_DIR="$HOME/.config/MonkeyLauncher/games"
SAVES_BASE="$HOME/.config/MonkeyLauncher/saves"
STEAM_ROOT="$HOME/.local/share/Steam"
WINEPREFIX_PATH="$STEAM_ROOT/steamapps/compatdata/480"
MANGOHUD=0
SETUP=0
SETUP_PROTON=0
SETUP_ENV=0
INSTALL_DEPS=0

# ── Logging ──────────────────────────────────────────────────────────────────
LOG_DIR="$HOME/.config/MonkeyLauncher/logs"
LOG_FILE="$LOG_DIR/monkeylauncher-cli.log"
mkdir -p "$LOG_DIR"
# Cap the log file so it doesn't grow unbounded across runs.
if [ -f "$LOG_FILE" ] && [ "$(stat -c%s "$LOG_FILE" 2>/dev/null || echo 0)" -gt 2000000 ]; then
  tail -c 500000 "$LOG_FILE" > "$LOG_FILE.tmp" 2>/dev/null && mv "$LOG_FILE.tmp" "$LOG_FILE"
fi

DEBUG=0
[ "${MONKEYLAUNCHER_DEBUG:-0}" = "1" ] && DEBUG=1
for arg in "$@"; do [ "$arg" = "-v" ] && DEBUG=1; done

LVL_DEBUG=0; LVL_INFO=1; LVL_WARN=2; LVL_ERROR=3
CONSOLE_LEVEL=$LVL_INFO
[ "$DEBUG" -eq 1 ] && CONSOLE_LEVEL=$LVL_DEBUG

COLOR_GRAY='\033[2;37m'
COLOR_CYAN='\033[0;36m'
COLOR_YELLOW='\033[1;33m'
COLOR_RED='\033[0;31m'
LOG_NC='\033[0m'

_log() {
  local lvl="$1" name="$2" color="$3"; shift 3
  local ts; ts=$(date '+%H:%M:%S')
  printf '%s %-5s %s\n' "$ts" "$name" "$*" >> "$LOG_FILE"
  if [ "$lvl" -ge "$CONSOLE_LEVEL" ]; then
    echo -e "${color}${ts} ${name}${LOG_NC} $*" >&2
  fi
}
log_debug() { _log "$LVL_DEBUG" "DEBUG" "$COLOR_GRAY"   "$@"; }
log_info()  { _log "$LVL_INFO"  "INFO"  "$COLOR_CYAN"   "$@"; }
log_warn()  { _log "$LVL_WARN"  "WARN"  "$COLOR_YELLOW" "$@"; }
log_error() { _log "$LVL_ERROR" "ERROR" "$COLOR_RED"    "$@"; }

# ── Startup checks ─────────────────────────────────────────────────────────────
if ! pgrep -x steam &>/dev/null; then
  read -r -p "Steam is not running. Launch it now and wait? [Y/n] " ans
  if [[ "$ans" == [nN] ]]; then
    exit 1
  fi
  steam &>/dev/null &
  log_info "Waiting for Steam to start…"
  until pgrep -x steam &>/dev/null; do sleep 2; done
  log_info "Steam is running."
fi

if [ ! -f "$STEAM_ROOT/steamapps/appmanifest_480.acf" ]; then
  read -r -p "Spacewar (App 480) is not installed. Open Steam to install it and wait? [Y/n] " ans
  if [[ "$ans" == [nN] ]]; then
    exit 1
  fi
  steam "steam://install/480" &>/dev/null &
  log_info "Waiting for Spacewar to finish installing…"
  until [ -f "$STEAM_ROOT/steamapps/appmanifest_480.acf" ]; do sleep 3; done
  log_info "Spacewar installed."
fi

for arg in "$@"; do
  case "$arg" in
    -d) MANGOHUD=1 ;;
    -s) SETUP=1 ;;
    -p) SETUP_PROTON=1 ;;
    -e) SETUP_ENV=1 ;;
    -i) INSTALL_DEPS=1 ;;
    -v) DEBUG=1 ;;
    -r)
      read -r -p "Reset config and clear all settings? [y/N] " confirm
      [[ "$confirm" != [yY] ]] && exit 0
      log_warn "Resetting all config at user's request"
      rm -f "$CONFIG_FILE"
      rm -rf "$GAMES_DIR"
      echo "Config cleared."
      exit 0
      ;;
    -h)
      echo "Usage: MonkeyLauncher.sh [OPTIONS]"
      echo ""
      echo "Options:"
      echo "  -s    Setup: set the game directory to scan for .exe files"
      echo "  -p    Set favorite Proton version (saved, skips prompt on launch)"
      echo "  -e    Open game settings page (per-game launch env vars + save dir)"
      echo "  -i    Install dependencies into the Proton prefix via winetricks"
      echo "  -r    Reset: clear all saved config"
      echo "  -d    Enable MangoHud overlay"
      echo "  -v    Verbose: show debug logs (also: MONKEYLAUNCHER_DEBUG=1)"
      echo "  -h    Show this help message"
      echo ""
      echo "Config:  $CONFIG_FILE"
      echo "Saves:   $SAVES_BASE/"
      echo "Logs:    $LOG_FILE"
      exit 0
      ;;
  esac
done

# --- Shared: game label -> sanitized key ---
game_key() {
  local key="${1//\//__}"
  echo "${key// /_}"
}

game_config_path() { echo "$GAMES_DIR/$(game_key "$1")"; }
game_save_path()   { echo "$SAVES_BASE/$(game_key "$1")"; }

# --- Shared: pick_directory ---
pick_directory() {
  local current="${1:-/}"
  while true; do
    local entries=("[ Select this directory ]")
    [ "$current" != "/" ] && entries+=("[ .. ]")
    while IFS= read -r -d '' d; do
      entries+=("${d##*/}")
    done < <(find "$current" -maxdepth 1 -mindepth 1 -type d -not -name 'proc' -not -name 'sys' -not -name 'dev' -print0 2>/dev/null | sort -z)
    local choice
    choice=$(printf '%s\n' "${entries[@]}" | fzf --prompt="$current/ " --height=40% --border)
    case "$choice" in
      "") return 1 ;;
      "[ Select this directory ]") echo "$current"; return 0 ;;
      "[ .. ]") current=$(dirname "$current") ;;
      *) current="$current/$choice" ;;
    esac
  done
}

# --- Shared: collect Proton installations ---
collect_proton_dirs() {
  local VDF="$STEAM_ROOT/steamapps/libraryfolders.vdf"
  if [ ! -f "$VDF" ]; then
    log_error "Steam library config not found: $VDF"
    return 1
  fi
  mapfile -t STEAM_LIBS < <(grep '"path"' "$VDF" | awk -F'"' '{print $4}')
  PROTON_DIRS=()
  for lib in "${STEAM_LIBS[@]}"; do
    while IFS= read -r -d '' dir; do
      PROTON_DIRS+=("$dir")
    done < <(find "$lib/steamapps/common" -maxdepth 1 -name 'Proton*' -type d -print0 2>/dev/null | sort -z)
  done
  if [ ${#PROTON_DIRS[@]} -eq 0 ]; then
    log_error "No Proton installations found."
    return 1
  fi
  log_debug "Found ${#PROTON_DIRS[@]} Proton installation(s): ${PROTON_DIRS[*]##*/}"
}

# --- Shared: build exe list ---
build_exe_list() {
  mapfile -t EXE_PATHS < <(find "$GAMEDIR" -name "*.exe" -type f \
    -not -path '*/_CommonRedist/*' \
    -not -path '*/Binaries/*' \
    -not -name '*CrashHandler*.exe' \
    | sort)
  EXE_LABELS=("${EXE_PATHS[@]#"$GAMEDIR/"}")
  log_debug "Found ${#EXE_PATHS[@]} exe(s) in $GAMEDIR"
}

# --- Shared: set up save dir symlink before launch ---
setup_save_symlink() {
  local prefix_savedir="$1"
  local ml_savedir="$2"
  mkdir -p "$ml_savedir"
  if [ -d "$prefix_savedir" ] && [ ! -L "$prefix_savedir" ]; then
    # First time: migrate existing saves into MonkeyLauncher dir
    log_debug "Migrating existing saves from $prefix_savedir to $ml_savedir"
    cp -a "$prefix_savedir/." "$ml_savedir/"
    rm -rf "$prefix_savedir"
    echo "Migrated existing saves to $ml_savedir"
  fi
  ln -sfn "$ml_savedir" "$prefix_savedir"
}

# --- Setup game directory ---
if [ "$SETUP" -eq 1 ]; then
  GAMEDIR=$(pick_directory /)
  [ -z "$GAMEDIR" ] && exit 0
  mkdir -p "$(dirname "$CONFIG_FILE")"
  SAVED_PROTON=$(grep '^PROTONPATH=' "$CONFIG_FILE" 2>/dev/null | cut -d= -f2-)
  { echo "GAMEDIR=$GAMEDIR"; [ -n "$SAVED_PROTON" ] && echo "PROTONPATH=$SAVED_PROTON"; } > "$CONFIG_FILE"
  log_info "Game directory saved: $GAMEDIR"
  exit 0
fi

# --- Load global config (required for -e, -p, -i too) ---
if [ ! -f "$CONFIG_FILE" ]; then
  log_error "No game directory configured. Run with -s first."
  exit 1
fi
GAMEDIR=$(grep '^GAMEDIR=' "$CONFIG_FILE" | cut -d= -f2-)
PROTONPATH=$(grep '^PROTONPATH=' "$CONFIG_FILE" | cut -d= -f2-)
log_debug "Loaded config: GAMEDIR=$GAMEDIR PROTONPATH=${PROTONPATH:-(none saved)}"

# --- Game settings page ---
if [ "$SETUP_ENV" -eq 1 ]; then
  build_exe_list
  if [ ${#EXE_PATHS[@]} -eq 0 ]; then
    log_error "No .exe files found in $GAMEDIR"
    exit 1
  fi

  GAME_LABEL=$(printf '%s\n' "${EXE_LABELS[@]}" | fzf --prompt="Select game > " --height=40% --border)
  [ -z "$GAME_LABEL" ] && exit 0

  GAME_CONF=$(game_config_path "$GAME_LABEL")
  ML_SAVEDIR=$(game_save_path "$GAME_LABEL")
  CURRENT_ENV=$(grep '^LAUNCH_ENV=' "$GAME_CONF" 2>/dev/null | cut -d= -f2-)
  CURRENT_SAVEDIR=$(grep '^SAVEDIR=' "$GAME_CONF" 2>/dev/null | cut -d= -f2-)

  while true; do
    SETTING=$(printf '%s\n' \
      "Launch env:  ${CURRENT_ENV:-(none)}" \
      "Save dir:    ${CURRENT_SAVEDIR:-(not set)}" \
      "Open save dir in file manager" \
      "Done" \
      | fzf --prompt="$GAME_LABEL > " --height=40% --border --no-multi)
    [ -z "$SETTING" ] && break

    case "$SETTING" in
      "Launch env:"*)
        echo "Enter space-separated VAR=value pairs (leave empty to clear):"
        read -e -i "$CURRENT_ENV" -p "> " CURRENT_ENV
        ;;
      "Save dir:"*)
        # Browse inside the Proton prefix to locate where this game saves
        PREFIX_ROOT="$WINEPREFIX_PATH/pfx/drive_c/users/steamuser"
        [ ! -d "$PREFIX_ROOT" ] && PREFIX_ROOT="$WINEPREFIX_PATH/pfx"
        echo "Browse to the game's save folder inside the Proton prefix:"
        CURRENT_SAVEDIR=$(pick_directory "$PREFIX_ROOT")
        [ -n "$CURRENT_SAVEDIR" ] && echo "Prefix save path set. Saves will be stored in: $ML_SAVEDIR"
        ;;
      "Open save dir"*)
        if [ -d "$ML_SAVEDIR" ]; then
          xdg-open "$ML_SAVEDIR" 2>/dev/null &
        elif [ -n "$CURRENT_SAVEDIR" ]; then
          echo "No saves yet — launch the game once first."
          sleep 2
        else
          echo "No save directory configured."
          sleep 2
        fi
        ;;
      "Done") break ;;
    esac
  done

  mkdir -p "$GAMES_DIR"
  { [ -n "$CURRENT_ENV" ]     && echo "LAUNCH_ENV=$CURRENT_ENV"
    [ -n "$CURRENT_SAVEDIR" ] && echo "SAVEDIR=$CURRENT_SAVEDIR"; } > "$GAME_CONF"
  [ ! -s "$GAME_CONF" ] && rm -f "$GAME_CONF"
  echo "Settings saved."
  exit 0
fi

# --- Setup favorite Proton ---
if [ "$SETUP_PROTON" -eq 1 ]; then
  collect_proton_dirs || exit 1
  PROTON_LABEL=$(printf '%s\n' "${PROTON_DIRS[@]##*/}" | fzf --prompt="Proton > " --height=40% --border)
  [ -z "$PROTON_LABEL" ] && exit 0
  PROTONPATH=$(printf '%s\n' "${PROTON_DIRS[@]}" | grep -F "/$PROTON_LABEL")
  SAVED_GAMEDIR=$(grep '^GAMEDIR=' "$CONFIG_FILE" | cut -d= -f2-)
  { [ -n "$SAVED_GAMEDIR" ] && echo "GAMEDIR=$SAVED_GAMEDIR"; echo "PROTONPATH=$PROTONPATH"; } > "$CONFIG_FILE"
  log_info "Favorite Proton saved: $PROTON_LABEL"
  exit 0
fi

# --- Install dependencies ---
if [ "$INSTALL_DEPS" -eq 1 ]; then
  collect_proton_dirs || exit 1
  PROTON_LABEL=$(printf '%s\n' "${PROTON_DIRS[@]##*/}" | fzf --prompt="Install into Proton > " --height=40% --border)
  [ -z "$PROTON_LABEL" ] && exit 0
  PROTONPATH=$(printf '%s\n' "${PROTON_DIRS[@]}" | grep -F "/$PROTON_LABEL")

  PACKAGES=(
    "[ Run .exe from game directory ]"
    "vcrun2022        Visual C++ 2015-2022 Redistributable"
    "vcrun2019        Visual C++ 2015-2019 Redistributable"
    "vcrun2013        Visual C++ 2013 Redistributable"
    "vcrun2010        Visual C++ 2010 Redistributable"
    "dotnet48         .NET Framework 4.8"
    "dotnet6          .NET 6 Runtime"
    "dotnet7          .NET 7 Runtime"
    "dotnet8          .NET 8 Runtime"
    "d3dx9            DirectX 9 (d3dx9)"
    "d3dcompiler_47   D3D Shader Compiler 47"
    "d3dx11_43        DirectX 11 (d3dx11)"
    "openal           OpenAL audio library"
    "faudio           FAudio (XAudio2 reimplementation)"
    "xact             XACT audio engine"
    "xna40            XNA Framework 4.0"
    "physx            NVIDIA PhysX"
    "mfc140           MFC 14.0 (Visual Studio 2015)"
  )

  SELECTED=$(printf '%s\n' "${PACKAGES[@]}" | \
    fzf --multi --prompt="Dependencies (TAB to select) > " --height=60% --border \
        --header="TAB to select multiple, ENTER to install")
  [ -z "$SELECTED" ] && exit 0

  if echo "$SELECTED" | grep -q '^\[ Run .exe from game directory \]'; then
    mapfile -t ALL_EXES < <(find "$GAMEDIR" -name "*.exe" -type f | sort)
    ALL_LABELS=("${ALL_EXES[@]#"$GAMEDIR/"}")
    EXE_LABEL=$(printf '%s\n' "${ALL_LABELS[@]}" | \
      fzf --prompt="Installer > " --height=60% --border \
          --header="All .exe files including CommonRedist and Binaries")
    [ -z "$EXE_LABEL" ] && exit 0
    EXE_PATH="$GAMEDIR/$EXE_LABEL"
    log_info "Running installer: $EXE_PATH"
    WINE="$PROTONPATH/files/bin/wine64" \
    WINESERVER="$PROTONPATH/files/bin/wineserver" \
    WINEPREFIX="$WINEPREFIX_PATH/" \
    umu-run "$EXE_PATH"
    log_debug "Installer exited with status $?"
  else
    VERBS=$(echo "$SELECTED" | awk '{print $1}' | tr '\n' ' ')
    log_info "Installing dependencies: $VERBS"
    WINE="$PROTONPATH/files/bin/wine64" \
    WINESERVER="$PROTONPATH/files/bin/wineserver" \
    WINEPREFIX="$WINEPREFIX_PATH/" \
    winetricks --unattended $VERBS
    log_debug "winetricks exited with status $?"
  fi
  exit 0
fi

# --- Pick game ---
build_exe_list
if [ ${#EXE_PATHS[@]} -eq 0 ]; then
  log_error "No .exe files found in $GAMEDIR"
  exit 1
fi

GAME_LABEL=$(printf '%s\n' "${EXE_LABELS[@]}" | fzf --prompt="Game > " --height=40% --border)
[ -z "$GAME_LABEL" ] && exit 0
GAME="$GAMEDIR/$GAME_LABEL"

# --- Load per-game config ---
GAME_CONF=$(game_config_path "$GAME_LABEL")
LAUNCH_ENV=$(grep '^LAUNCH_ENV=' "$GAME_CONF" 2>/dev/null | cut -d= -f2-)
SAVEDIR=$(grep '^SAVEDIR=' "$GAME_CONF" 2>/dev/null | cut -d= -f2-)

# --- Wire up save dir symlink ---
if [ -n "$SAVEDIR" ]; then
  setup_save_symlink "$SAVEDIR" "$(game_save_path "$GAME_LABEL")"
fi

# --- Pick Proton version (skip if favorite is set) ---
if [ -z "$PROTONPATH" ]; then
  collect_proton_dirs || exit 1
  PROTON_LABEL=$(printf '%s\n' "${PROTON_DIRS[@]##*/}" | fzf --prompt="Proton > " --height=40% --border)
  [ -z "$PROTON_LABEL" ] && exit 0
  PROTONPATH=$(printf '%s\n' "${PROTON_DIRS[@]}" | grep -F "/$PROTON_LABEL")
fi

# --- Launch ---
log_info "Launching $GAME_LABEL via $(basename "$PROTONPATH")"
log_debug "Launch command: umu-run $GAME"
log_debug "Launch env overrides: ${LAUNCH_ENV:-(none)}, savedir: ${SAVEDIR:-(none)}, mangohud: $MANGOHUD"

env $LAUNCH_ENV \
  WINEPREFIX="$WINEPREFIX_PATH/" \
  WINEDLLOVERRIDES="OnlineFix64=n;SteamOverlay64=n;winmm=n,b;dnet=n;steam_api64=n" \
  GAMEID=480 \
  PROTONPATH="$PROTONPATH" \
  DXVK_STATE_CACHE=1 \
  MANGOHUD=$MANGOHUD \
  umu-run "$GAME"
log_debug "Game process exited with status $?"
