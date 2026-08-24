#!/bin/bash
# Builds every distro package and drops the artifacts in dist/.
#  - Arch package: built natively via makepkg (needs an Arch-based host).
#  - Debian package: built via Docker (needs dpkg-deb, which most hosts
#    other than Debian/Ubuntu won't have installed).
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIST="$ROOT/dist"
mkdir -p "$DIST"

if command -v makepkg &>/dev/null; then
  "$ROOT/packaging/build-arch.sh"
else
  echo "[!] makepkg not found — skipping Arch package (run packaging/build-arch.sh on an Arch host)"
fi

if command -v dpkg-deb &>/dev/null; then
  "$ROOT/packaging/build-deb.sh"
elif command -v docker &>/dev/null; then
  echo "Building .deb via Docker (debian:bookworm-slim)…"
  docker build -f "$ROOT/docker/deb-builder.Dockerfile" -t mklncher-deb-builder "$ROOT"
  docker run --rm --user "$(id -u):$(id -g)" -v "$ROOT:/build" mklncher-deb-builder
else
  echo "[!] Neither dpkg-deb nor docker found — skipping .deb package" >&2
fi

echo ""
echo "Release artifacts:"
ls -lh "$DIST/"
