# MonkeyLauncher

A launcher for online-fix Windows games on Linux, built on top of [umu-launcher](https://github.com/Open-Wine-Components/umu-launcher) and Proton.

Available as a **GTK3 GUI** and a **terminal CLI** (fzf-based).

---

## Features

- Scans a game directory for `.exe` files and lets you pick one to launch
- Auto-detects all Proton versions installed across your Steam libraries
- Save a favorite Proton version to skip the prompt on every launch
- Per-game launch environment variables (e.g. `DRI_PRIME=1 GAMEMODE=1`)
- Save directory management — saves are symlinked out of the Proton prefix into `~/.config/MonkeyLauncher/saves/` so they survive prefix resets
- MangoHud overlay toggle (GUI button / CLI `-d` flag)
- Install winetricks packages (vcrun, dotnet, DirectX, OpenAL…) or run a local `.exe` installer into any Proton prefix
- Pre-configured `WINEDLLOVERRIDES` for online-fix compatibility

---

## Requirements

| Dependency | Purpose |
|---|---|
| Steam (running) | Required for the Proton prefix |
| Steam App 480 (Spacewar) | Used as the shared Proton prefix |
| [umu-launcher](https://github.com/Open-Wine-Components/umu-launcher) | Runs the game through Proton |
| winetricks | Dependency installation |
| fzf | CLI interface |
| python3-gobject + GTK3 | GUI only |
| mangohud | Optional overlay |

> On Arch Linux all of the above are available via pacman/AUR. On other distros,
> `umu-launcher` and `mangohud` are not in official repos — see their respective
> project pages for install instructions.

---

## Install

```bash
./install.sh
```

Installs into `~/.local/bin/` and creates a `.desktop` entry. Adds a shell alias for the CLI to `.bashrc` / `.zshrc` / `config.fish`.

```bash
./uninstall.sh
```

Removes binaries, icon, desktop entry, and shell aliases. Optionally deletes user data.

---

## Usage

### GUI

```bash
MonkeyLauncher
```

1. **Menu → Setup game directory** — point it at your games folder
2. Select a game from the list, pick a Proton version, hit **Launch**
3. Optionally set a favorite Proton, per-game env vars, or save directory via **Game Settings**

### CLI

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

---

## Config

All config is stored in `~/.config/MonkeyLauncher/`:

```
~/.config/MonkeyLauncher/
├── config          # global: GAMEDIR, PROTONPATH
├── games/          # per-game: LAUNCH_ENV, SAVEDIR
└── saves/          # save files, symlinked from the Proton prefix
```

---

## Build a release (Docker)

Produces binaries compatible with any Linux distro running glibc ≥ 2.31 (Ubuntu 20.04+, Arch, Fedora 36+, Debian 12+…).

```bash
./build-release.sh
```

Output lands in `dist/`. The GUI binary still requires `python3-gobject` + GTK3 on the target machine at runtime (GObject introspection cannot be bundled).

---

## Project structure

```
src/                  source files (GUI, CLI, icon, desktop entry)
docker/               Dockerfile and build entrypoint
dist/                 build output (gitignored)
install.sh
uninstall.sh
build-release.sh
```
