#!/bin/bash
set -e

OUTPUT="${OUTPUT:-/output}"
mkdir -p "$OUTPUT"

echo ""
echo "  MonkeyLauncher — Docker Release Build"
echo "  ======================================"
echo "  glibc: $(ldd --version | head -1)"
echo "  g++:   $(g++ --version | head -1)"
echo ""

echo "[1/2] Compiling GUI (make)…"
make -C /build > /tmp/build.log 2>&1 \
  && echo "[✓] GUI compiled" \
  || { echo "[✗] Build failed:"; cat /tmp/build.log; exit 1; }
install -m 755 /build/build/monkeylauncher "$OUTPUT/MonkeyLauncher"

echo "[2/2] Installing CLI…"
install -m 755 /build/src/MonkeyLauncherCLI.sh "$OUTPUT/MonkeyLauncherCLI"
echo "[✓] CLI ready"

echo ""
echo "Build complete. Output:"
ls -lh "$OUTPUT/"
echo ""
echo "Note: the GUI binary still needs libgtkmm-3.0 and libcurl on the target machine."
echo ""
