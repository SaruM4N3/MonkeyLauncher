# MonkeyLauncher

![Platform](https://img.shields.io/badge/Platform-Linux-orange)
![C++](https://img.shields.io/badge/C%2B%2B-17-blue)
![License](https://img.shields.io/badge/License-Educational-green)

A launcher for online-fix Windows games on Linux, built on top of [umu-launcher](https://github.com/Open-Wine-Components/umu-launcher) and Proton.

Available as a **GTK3 GUI** and a **terminal CLI** (fzf-based).

---

## Table of Contents

- [Features](#features)
- [Requirements](#requirements)
- [Install](#install)
- [Usage](#usage)
- [Config](#config)
- [Build](#build)
- [Project Structure](#project-structure)
- [Disclaimer](#disclaimer)

---

## Features

<summary><b>Game Management</b></summary>

- Scans a game directory for `.exe` files and lets you pick one to launch
- Auto-detects all Proton versions installed across your Steam libraries
- Save a favorite Proton version to skip the prompt on every launch
- Per-game launch environment variables (e.g. `DRI_PRIME=1 GAMEMODE=1`)
- List view or cover-art Preview view (GUI) — covers are looked up on the Steam store (no account/API key needed) and cached locally

<summary><b>Save Management</b></summary>

Save directory management — saves are symlinked out of the Proton prefix into `~/.config/MonkeyLauncher/saves/` so they survive prefix resets

<summary><b>Additional Features</b></summary>

- MangoHud overlay toggle (GUI button / CLI `-d` flag)
- Install winetricks packages (vcrun, dotnet, DirectX, OpenAL…) or run a local `.exe` installer into any Proton prefix
- Pre-configured `WINEDLLOVERRIDES` for online-fix compatibility

---

## Requirements

<summary><b>Required Dependencies</b></summary>

| Dependency | Purpose |
|---|---|
| Steam (running) | Required for the Proton prefix |
| Steam App 480 (Spacewar) | Used as the shared Proton prefix |
| [umu-launcher](https://github.com/Open-Wine-Components/umu-launcher) | Runs the game through Proton |
| winetricks | Dependency installation |
| fzf | CLI interface |

<summary><b>Optional Dependencies</b></summary>

| Dependency | Purpose |
|---|---|
| gtkmm 3 (`gtkmm3` / `libgtkmm-3.0`) + libcurl | GUI only (runtime) |
| mangohud | Optional overlay |

<summary><b>Build Dependencies (source install only)</b></summary>

The GUI is written in C++17 with [gtkmm-3.0](https://gtkmm.org/). Installing from source compiles it, which needs:

| Dependency | Arch | Debian / Ubuntu | Fedora | openSUSE |
|---|---|---|---|---|
| C++ compiler, make, pkg-config | `gcc make pkgconf` | `g++ make pkg-config` | `gcc-c++ make pkgconf-pkg-config` | `gcc-c++ make pkg-config` |
| gtkmm 3 | `gtkmm3` | `libgtkmm-3.0-dev` | `gtkmm30-devel` | `gtkmm3-devel` |
| libcurl | `curl` | `libcurl4-openssl-dev` | `libcurl-devel` | `libcurl-devel` |
| nlohmann-json | `nlohmann-json` | `nlohmann-json3-dev` | `json-devel` | `nlohmann_json-devel` |

`install.sh` installs these for you. Distro packages (`.deb` / Arch) ship the compiled binary and only need the runtime libraries.

<summary><b>Installation Notes</b></summary>

> `install.sh` detects your distro (Arch, Debian/Ubuntu, Fedora, openSUSE — via
> `/etc/os-release`) and installs what it can automatically through
> pacman/apt/dnf/zypper. `mangohud` is packaged everywhere. `umu-launcher` has
> an official package on Arch and Nobara only; everywhere else (including
> vanilla Fedora and openSUSE) the installer builds it from source instead
> (this pulls in the Rust toolchain).

---

## Install

<details>
<summary><b>Installation via package (Debian/Ubuntu, Arch)</b></summary>

Grab the `.deb` or `.pkg.tar.zst` from the [latest release](https://github.com/SaruM4N3/MonkeyLauncher/releases/latest) and install it with your package manager — dependencies are pulled in automatically.

**Debian / Ubuntu:**
```bash
sudo apt install ./monkeylauncher_*.deb
```
> `winetricks` and `protontricks` live in Debian's `contrib` component, which isn't enabled by default. If the install fails on those, enable it first (`sudo apt edit-sources`, add `contrib` next to `main`, then `sudo apt update`).

**Arch:**
```bash
sudo pacman -U monkeylauncher-*.pkg.tar.zst
```
> `umu-launcher` isn't in Arch's official repos — install it separately via an AUR helper (`paru -S umu-launcher`) or build it from source.

Both packages install to `/usr` and add a `MonkeyLauncher` app entry + `MonkeyLauncherCLI` command.

</details>

<details>
<summary><b>Installation from source</b></summary>

```bash
./install.sh
```

Compiles the GUI (`make`), installs into `~/.local/bin/` and `~/.local/lib/monkeylauncher/`, and creates a `.desktop` entry. Adds a shell alias for the CLI to `.bashrc` / `.zshrc` / `config.fish`. Detects your distro and installs runtime dependencies automatically (including building `umu-launcher` from source where there's no native package).

</details>

<details>
<summary><b>Uninstallation</b></summary>

```bash
./uninstall.sh
```

Removes binaries, icon, desktop entry, and shell aliases. Optionally deletes user data.

</details>

---

## Usage

<details>
<summary><b>GUI Usage</b></summary>

```bash
MonkeyLauncher
```

1. **Menu → Setup game directory** — point it at your games folder
2. Select a game from the list, pick a Proton version, hit **Launch**
3. Optionally set a favorite Proton, per-game env vars, or save directory via **Game Settings**
4. **Settings → Advanced → Check for Updates** — checks the latest GitHub release; source installs (`install.sh`) can update in place from there, package installs (`.deb`/Arch) are pointed at their package manager instead

</details>

<details>
<summary><b>CLI Usage</b></summary>

```bash
MonkeyLauncher [OPTIONS]
```

| Flag | Action |
|---|---|
| *(none)* | Pick a game with fzf and launch |
| `-s` | Set the game directory |
| `-p` | Save a favorite Proton version |
| `-e` | Edit per-game env vars and save directory |
| `-i` | Install winetricks packages into a Proton prefix |
| `-d` | Enable MangoHud overlay |
| `-r` | Reset all config |
| `-h` | Show help |

**First run:**
```bash
MonkeyLauncher -s   # set your game directory
MonkeyLauncher      # pick a game and launch
```

</details>

---

## Config

<details>
<summary><b>Configuration Files</b></summary>

All config is stored in `~/.config/MonkeyLauncher/`:

```
~/.config/MonkeyLauncher/
├── config          # global: GAMEDIR, PROTONPATH
├── games/          # per-game: LAUNCH_ENV, SAVEDIR
├── saves/          # save files, symlinked from the Proton prefix
├── covers/         # cached cover art (GUI Preview view)
└── logs/           # debug logs
```

</details>

<details>
<summary><b>Debug Logs</b></summary>

Both the GUI and CLI log to colored console output plus a persistent, rotating file in `~/.config/MonkeyLauncher/logs/`. Verbose (debug-level) logging is off by default — enable it with `-v` (CLI) / `--debug` (GUI), or `MONKEYLAUNCHER_DEBUG=1`.

</details>

---

## Build

<details>
<summary><b>Build a Release (Docker)</b></summary>

Compiles the GUI inside Ubuntu 20.04, so the binary runs on any Linux distro running glibc ≥ 2.31 (Ubuntu 20.04+, Arch, Fedora 36+, Debian 12+…).

```bash
./build-release.sh
```

Output lands in `dist/`. The GUI binary still requires gtkmm 3 and libcurl on the target machine at runtime (they are linked dynamically).

</details>

<details>
<summary><b>Build from source (development)</b></summary>

```bash
make            # → build/monkeylauncher
./build/monkeylauncher --debug
make install DESTDIR=/tmp/stage PREFIX=/usr   # stage a system-wide install
```

Needs the [build dependencies](#requirements) above. `--debug` (or `-v`, or `MONKEYLAUNCHER_DEBUG=1`) enables verbose logging.

</details>

<details>
<summary><b>Build distro packages (.deb / Arch)</b></summary>

```bash
./packaging/build-all.sh
```

Compiles the GUI and builds a `.deb` (via Docker, using `debian:bookworm-slim`) and an Arch package (via `makepkg` — needs an Arch-based host). Both are `x86_64`/`amd64` packages that install to `/usr` and declare real package-manager dependencies instead of bundling anything. Output lands in `dist/`. See `packaging/arch/PKGBUILD` and `packaging/debian/control` for the exact dependency lists.

</details>

---

## Project Structure

<details>
<summary><b>Directory Layout</b></summary>

```
cpp/                    GUI (C++17 / gtkmm-3.0)
├── app.cpp             entry point + startup checks (Steam running, App 480, Proton prefix)
├── main_window.*       main window: library (list/grid), Help, Settings
├── dialogs.*           Game Settings + Install Dependencies dialogs
├── config.*            paths, config/gamedirs files, game scanning
├── steam.*             Steam libraries, Proton discovery, prefix bootstrap
├── covers.*            Steam-store cover lookup
├── updater.*           GitHub release check + in-place source update
├── proc.*, http.*      subprocess and libcurl helpers
├── logging.*, version.*, ui.*, common.*
src/
├── MonkeyLauncherCLI.sh    CLI (fzf-based)
├── monkeylauncher.desktop
└── logo.png
Makefile              builds build/monkeylauncher
docker/               Dockerfile and build entrypoint
dist/                 build output (gitignored)
install.sh
uninstall.sh
build-release.sh
```

</details>

---

## Disclaimer

### Educational Purpose Only

This project was created by a student for educational and research purposes only.

MonkeyLauncher does not provide, distribute, host, or include any copyrighted game files, cracks, or bypass tools. Users are responsible for ensuring they legally own any games they run with this software and for complying with the laws and terms of service applicable in their country.

The project is intended to study Linux compatibility, Proton/Wine behavior, launcher development, and related technologies.

---
