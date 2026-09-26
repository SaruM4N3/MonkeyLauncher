#!/bin/bash
# Installs everything MonkeyLauncher needs, without building or installing
# the app itself (that's install.sh, which calls this script first):
#   - runtime tools (fzf, winetricks, protontricks, mangohud, umu-launcher…)
#   - build toolchain + libraries (g++, make, gtkmm-3.0, libcurl, nlohmann-json)
#   - fonts
# Supports pacman, apt, dnf and zypper. Safe to re-run.
set -e

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

# install.sh sets ML_INSTALLER=1 so the banner/footer only show when this
# script is run on its own.
if [ -z "$ML_INSTALLER" ]; then
  echo ""
  echo -e "${BOLD}  MonkeyLauncher Dependencies${NC}"
  echo "  ==========================="
  echo ""
fi

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
    # below (python3 is also needed by umu-launcher itself).
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
# (missing ZSTD_defaultCLevel, added in libzstd 1.5.5). Only act when umu-run
# is actually broken — otherwise there's nothing to fix.
umu_works() { python3 -c "import umu" 2>/dev/null || umu-run --help &>/dev/null; }

if command -v umu-run &>/dev/null; then
  if python3 -c "import sys; exit(0 if sys.version_info >= (3,14) else 1)" 2>/dev/null \
       && ! umu_works; then
    warn "umu-run fails on Python 3.14 — ensuring libzstd is up to date…"
    install_pkg zstd zstd zstd zstd
    # Also upgrade pyzstd in case the installed version predates Python 3.14 support
    python3 -m pip install --user --quiet --upgrade pyzstd 2>/dev/null && ok "pyzstd updated" || true
    if umu_works; then
      ok "umu-run Python 3.14 compatibility"
    else
      warn "umu-run may still have import issues — try: sudo pacman -Syu zstd"
    fi
  fi
fi

# ── Build toolchain & libraries (MonkeyLauncher is C++ / gtkmm-3.0) ────────────
section "Checking build dependencies…"

#                        cmd            pacman           apt           dnf                  zypper
check_or_install         g++            gcc              g++           gcc-c++              gcc-c++
check_or_install         make           make             make          make                 make
check_or_install         pkg-config     pkgconf          pkg-config    pkgconf-pkg-config   pkg-config

# Development libraries, detected through pkg-config.
# usage: check_or_install_lib <pkg-config module> <pacman> <apt> <dnf> <zypper>
check_or_install_lib() {
  local mod="$1" pacman="$2" apt="$3" dnf="$4" zypper="${5:-$4}"
  if pkg-config --exists "$mod" 2>/dev/null; then
    ok "$mod"
  else
    warn "$mod not found — installing…"
    install_pkg "$pacman" "$apt" "$dnf" "$zypper"
    pkg-config --exists "$mod" 2>/dev/null && ok "$mod" || fail "Failed to install $mod"
  fi
}

#                        module          pacman          apt                     dnf             zypper
check_or_install_lib     gtkmm-3.0       gtkmm3          libgtkmm-3.0-dev        gtkmm30-devel   gtkmm3-devel
check_or_install_lib     libcurl         curl            libcurl4-openssl-dev    libcurl-devel   libcurl-devel
check_or_install_lib     nlohmann_json   nlohmann-json   nlohmann-json3-dev      json-devel      nlohmann_json-devel

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


if [ -z "$ML_INSTALLER" ]; then
  echo ""
  echo -e "${GREEN}${BOLD}Done!${NC} All dependencies are installed."
  echo -e "  Next: run ${BOLD}./install.sh${NC} to build and install MonkeyLauncher."
  echo ""
fi
