#!/bin/bash
set -e

DIST="$(cd "$(dirname "$0")" && pwd)/dist"
IMAGE="mklncher-builder"

mkdir -p "$DIST"

echo "Building Docker image…"
docker build -f "$(dirname "$0")/docker/Dockerfile" -t "$IMAGE" "$(dirname "$0")"

echo "Running build…"
docker run --rm -v "$DIST:/output" "$IMAGE"

echo "Release binaries:"
ls -lh "$DIST/"
