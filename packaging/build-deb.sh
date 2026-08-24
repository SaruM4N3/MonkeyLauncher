#!/bin/bash
# Stages the .deb contents and builds it with dpkg-deb.
# Needs dpkg-dev; run directly on a Debian/Ubuntu host or via Docker
# (see docker/deb-builder.Dockerfile) on distros that don't have it.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIST="$ROOT/dist"
STAGE="$(mktemp -d)"
chmod 755 "$STAGE"
trap 'rm -rf "$STAGE"' EXIT

VERSION="$(cat "$ROOT/VERSION")"

mkdir -p "$DIST"

# ── Stage package contents ──────────────────────────────────────────────────
install -d "$STAGE/usr/lib/monkeylauncher"
install -m644 "$ROOT/src/MonkeyLauncherGUI.py" "$STAGE/usr/lib/monkeylauncher/MonkeyLauncherGUI.py"
cp -r "$ROOT/src/monkeylauncher" "$STAGE/usr/lib/monkeylauncher/monkeylauncher"
find "$STAGE/usr/lib/monkeylauncher/monkeylauncher" -type d -exec chmod 755 {} +
find "$STAGE/usr/lib/monkeylauncher/monkeylauncher" -type f -exec chmod 644 {} +

install -d "$STAGE/usr/bin"
install -m755 "$ROOT/src/MonkeyLauncherCLI.sh" "$STAGE/usr/bin/MonkeyLauncherCLI"
cat > "$STAGE/usr/bin/MonkeyLauncher" <<'WRAPPER'
#!/bin/sh
exec python3 /usr/lib/monkeylauncher/MonkeyLauncherGUI.py "$@"
WRAPPER
chmod 755 "$STAGE/usr/bin/MonkeyLauncher"

install -Dm644 "$ROOT/src/monkeylauncher.desktop" "$STAGE/usr/share/applications/monkeylauncher.desktop"
install -Dm644 "$ROOT/src/logo.png" "$STAGE/usr/share/icons/hicolor/1024x1024/apps/monkeylauncher.png"
install -Dm644 "$ROOT/packaging/debian/copyright" "$STAGE/usr/share/doc/monkeylauncher/copyright"

# ── Control file ─────────────────────────────────────────────────────────────
install -d "$STAGE/DEBIAN"
sed "s/^Version: VERSION_PLACEHOLDER/Version: $VERSION/" \
  "$ROOT/packaging/debian/control" > "$STAGE/DEBIAN/control"

INSTALLED_SIZE=$(du -sk "$STAGE" --exclude=DEBIAN | cut -f1)
sed -i "/^Description:/i Installed-Size: $INSTALLED_SIZE" "$STAGE/DEBIAN/control"

# ── Build ────────────────────────────────────────────────────────────────────
OUT="$DIST/monkeylauncher_${VERSION}_all.deb"
dpkg-deb --root-owner-group --build "$STAGE" "$OUT"

echo "Debian package → $OUT"
