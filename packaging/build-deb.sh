#!/bin/bash
# Stages the .deb contents and builds it with dpkg-deb.
# Needs dpkg-dev plus the C++ build dependencies; run directly on a
# Debian/Ubuntu host or via Docker
# (see docker/deb-builder.Dockerfile) on distros that don't have it.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIST="$ROOT/dist"
STAGE="$(mktemp -d)"
chmod 755 "$STAGE"
trap 'rm -rf "$STAGE"' EXIT

VERSION="$(cat "$ROOT/VERSION")"

mkdir -p "$DIST"

# ── Build & stage package contents ──────────────────────────────────────────
# The app is compiled C++ (gtkmm-3.0), so this needs g++, make, pkg-config,
# libgtkmm-3.0-dev, libcurl4-openssl-dev and nlohmann-json3-dev — see
# docker/deb-builder.Dockerfile.
make -C "$ROOT" clean
make -C "$ROOT" DESTDIR="$STAGE" PREFIX=/usr install
install -Dm644 "$ROOT/packaging/debian/copyright" "$STAGE/usr/share/doc/monkeylauncher/copyright"

# ── Control file ─────────────────────────────────────────────────────────────
install -d "$STAGE/DEBIAN"
sed "s/^Version: VERSION_PLACEHOLDER/Version: $VERSION/" \
  "$ROOT/packaging/debian/control" > "$STAGE/DEBIAN/control"

INSTALLED_SIZE=$(du -sk "$STAGE" --exclude=DEBIAN | cut -f1)
sed -i "/^Description:/i Installed-Size: $INSTALLED_SIZE" "$STAGE/DEBIAN/control"

# ── Build ────────────────────────────────────────────────────────────────────
OUT="$DIST/monkeylauncher_${VERSION}_amd64.deb"
dpkg-deb --root-owner-group --build "$STAGE" "$OUT"

echo "Debian package → $OUT"
