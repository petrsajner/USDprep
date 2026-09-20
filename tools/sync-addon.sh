#!/bin/sh
# Sync the UsdPrep addon from this repository into the usdtweak clone.
# usdtweak auto-discovers addons by globbing src/addons/*/CMakeLists.txt —
# dropping the folder in is the sanctioned integration (no host file edits).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/src/addon/UsdPrep"
DST="$ROOT/third_party/usdtweak/src/addons/UsdPrep"
mkdir -p "$DST"
rm -f "$DST"/*.cpp "$DST"/*.h
cp "$SRC"/*.cpp "$SRC"/*.h "$SRC/CMakeLists.txt" "$DST/"
echo "Synced UsdPrep addon -> $DST"

# The few changes usdprep needs in usdtweak itself (tools/usdtweak-patches),
# applied once: a patch that is already in is skipped.
for PATCH in "$ROOT"/tools/usdtweak-patches/*.patch; do
    [ -e "$PATCH" ] || continue
    if git -C "$ROOT/third_party/usdtweak" apply --reverse --check "$PATCH" 2>/dev/null; then
        echo "Already applied: $(basename "$PATCH")"
    else
        git -C "$ROOT/third_party/usdtweak" apply "$PATCH"
        echo "Applied: $(basename "$PATCH")"
    fi
done
