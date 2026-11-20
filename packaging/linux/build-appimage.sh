#!/usr/bin/env bash
set -euo pipefail

SRC=${SRC:-/src}
BUILD=${BUILD:-/tmp/build}
OUT=${OUT:-$SRC/dist}
VERSION=${VERSION:-$(git -c safe.directory='*' -C "$SRC" describe --tags --always 2>/dev/null || echo dev)}

cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT_ROOT"
cmake --build "$BUILD"

if [ "${RUN_TESTS:-1}" = 1 ]; then
    (cd "$BUILD" && QT_QPA_PLATFORM=offscreen ctest --output-on-failure)
fi

APPDIR="$BUILD/AppDir"
rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD" --prefix /usr

export QMAKE="$QT_ROOT/bin/qmake"
export LD_LIBRARY_PATH="$QT_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
platforms=()
for plugin in "$QT_ROOT"/plugins/platforms/libqwayland*.so "$QT_ROOT"/plugins/platforms/libqoffscreen.so; do
    [ -f "$plugin" ] && platforms+=("$(basename "$plugin")")
done
EXTRA_PLATFORM_PLUGINS=$(IFS=';'; echo "${platforms[*]}")
export EXTRA_PLATFORM_PLUGINS
export OUTPUT="NXGameViewer-$VERSION-x86_64.AppImage"

mkdir -p "$OUT"
cd "$OUT"
linuxdeploy --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/NXGameViewer" \
    --desktop-file "$SRC/packaging/linux/nxgameviewer.desktop" \
    --icon-file "$SRC/packaging/nxgameviewer.svg" \
    --plugin qt \
    --output appimage

status=0
QT_QPA_PLATFORM=offscreen timeout 10 "./$OUTPUT" >/dev/null 2>&1 || status=$?
if [ "$status" -ne 124 ]; then
    echo "The AppImage did not start (exit code $status)." >&2
    exit 1
fi
echo "Built $OUT/$OUTPUT"
