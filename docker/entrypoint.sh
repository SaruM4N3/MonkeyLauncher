#!/bin/bash
set -e

OUTPUT="${OUTPUT:-/output}"
mkdir -p "$OUTPUT"

echo ""
echo "  MonkeyLauncher — Docker Release Build"
echo "  ======================================"
echo "  glibc: $(ldd --version | head -1)"
echo ""

echo "[1/2] Compiling GUI (pyinstaller)…"
pyinstaller \
  --onefile \
  --name MonkeyLauncher \
  --distpath "$OUTPUT" \
  --workpath /tmp/pi_work \
  --specpath /tmp/pi_spec \
  --noconfirm \
  --clean \
  --hidden-import gi \
  --hidden-import gi.repository.Gtk \
  --hidden-import gi.repository.GLib \
  --hidden-import gi.repository.Pango \
  --hidden-import gi.repository.Gdk \
  /build/src/MonkeyLauncherGUI.py \
  > /tmp/pyinstaller.log 2>&1 \
  && echo "[✓] GUI compiled" \
  || { echo "[✗] PyInstaller failed:"; cat /tmp/pyinstaller.log; exit 1; }

echo "[2/2] Installing CLI…"
install -m 755 /build/src/MonkeyLauncherCLI.sh "$OUTPUT/MonkeyLauncherCLI"
echo "[✓] CLI ready"

chmod +x "$OUTPUT/MonkeyLauncher" "$OUTPUT/MonkeyLauncherCLI"

echo ""
echo "Build complete. Output:"
ls -lh "$OUTPUT/"
echo ""
