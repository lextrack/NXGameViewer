#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
image=nxgameviewer-appimage
version=${VERSION:-$(git -C "$root" describe --tags --always 2>/dev/null || echo dev)}

docker build -t "$image" "$root/packaging/linux"
docker run --rm \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -e VERSION="$version" \
    -e RUN_TESTS="${RUN_TESTS:-1}" \
    -v "$root:/src" \
    "$image"
