#!/bin/bash
set -e

OUTPUT="${OUTPUT:-/output}"
mkdir -p "$OUTPUT"

echo ""
echo "  MonkeyLauncher — Docker Release Build"
echo "  ======================================"
echo "  glibc: $(ldd --version | head -1)"
echo ""

# Ensure pyinstaller is available (no-op inside Docker, installs if run on host)
if ! command -v pyinstaller &>/dev/null; then
  echo "[~] pyinstaller not found — installing via pip3…"
  pip3 install --quiet pyinstaller
fi

# Locate GObject typelib directory so we can bundle it into the binary
TYPELIB_DIR=""
for candidate in \
    /usr/lib/x86_64-linux-gnu/girepository-1.0 \
    /usr/lib64/girepository-1.0 \
    /usr/lib/girepository-1.0; do
  if [ -d "$candidate" ]; then
    TYPELIB_DIR="$candidate"
    break
  fi
done

if [ -z "$TYPELIB_DIR" ]; then
  echo "[✗] Could not locate GObject typelib directory — GTK will not render correctly." >&2
  exit 1
fi
echo "[~] Typelibs: $TYPELIB_DIR"

echo "[1/2] Compiling GUI (pyinstaller)…"
pyinstaller \
  --onefile \
  --name MonkeyLauncher \
  --distpath "$OUTPUT" \
  --workpath /tmp/pi_work \
  --specpath /tmp/pi_spec \
  --noconfirm \
  --clean \
  --collect-all gi \
  --hidden-import gi \
  --hidden-import gi.repository.Gtk \
  --hidden-import gi.repository.GLib \
  --hidden-import gi.repository.Pango \
  --hidden-import gi.repository.Gdk \
  --add-data "$TYPELIB_DIR:gi_typelibs" \
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
