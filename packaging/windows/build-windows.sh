#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
build=${BUILD:-$root/build-windows}
out=${OUT:-$root/dist}

if [ "${MSYSTEM:-}" != UCRT64 ]; then
    echo "Run this from the \"MSYS2 UCRT64\" shell (or use build-windows.cmd)." >&2
    exit 1
fi

if [ "${1:-}" = --install-deps ]; then
    pacman -S --needed --noconfirm git zip \
        mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,qt6-base,qt6-multimedia,qt6-tools,opus,zstd,openssl,zlib}
fi

version=${VERSION:-$(git -c safe.directory='*' -C "$root" describe --tags --always 2>/dev/null || echo dev)}
name="NXGameViewer-$version-windows-x64"

cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$build"

if [ "${RUN_TESTS:-1}" = 1 ]; then
    (cd "$build" && QT_QPA_PLATFORM=offscreen ctest --output-on-failure)
fi

bash "$root/packaging/windows/deploy.sh" "$build" "$out/$name"
rm -f "$out/$name.zip"
(cd "$out" && zip -qr "$name.zip" "$name")
echo "Built $out/$name.zip"
