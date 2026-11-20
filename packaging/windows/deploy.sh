#!/usr/bin/env bash
set -euo pipefail

build=$1
out=$2
root=$(cd "$(dirname "$0")/../.." && pwd)

rm -rf "$out"
mkdir -p "$out"
cp "$build/NXGameViewer.exe" "$out/"
cp "$root/LICENSE" "$root/THIRD_PARTY.md" "$out/"

deployqt=$(command -v windeployqt6 || command -v windeployqt)
"$deployqt" --release --no-translations "$out/NXGameViewer.exe"

prefix=$(cygpath -u "$MINGW_PREFIX")
for _ in 1 2 3 4; do
    find "$out" \( -name '*.exe' -o -name '*.dll' \) -print0 \
        | xargs -0 -n 1 ldd 2>/dev/null \
        | awk -v p="$prefix/bin/" 'index($3, p) == 1 { print $3 }' \
        | sort -u \
        | while read -r dll; do
            [ -f "$out/$(basename "$dll")" ] || cp "$dll" "$out/"
        done
done
echo "Deployed to $out"
