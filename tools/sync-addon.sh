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
