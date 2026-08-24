#!/bin/bash
# Builds the Arch package natively via makepkg (requires an Arch-based host).
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIST="$ROOT/dist"
ARCH_DIR="$ROOT/packaging/arch"

mkdir -p "$DIST"

echo "Building Arch package…"
(cd "$ARCH_DIR" && makepkg -f --noconfirm --clean)

PKG_FILE=$(find "$ARCH_DIR" -maxdepth 1 -name 'monkeylauncher-*.pkg.tar.*' -printf '%T@ %p\n' | sort -rn | head -1 | cut -d' ' -f2-)
[ -z "$PKG_FILE" ] && { echo "Build failed: no package produced" >&2; exit 1; }

cp "$PKG_FILE" "$DIST/"
echo "Arch package → $DIST/$(basename "$PKG_FILE")"
