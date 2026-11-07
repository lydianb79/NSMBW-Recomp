#!/usr/bin/env bash
# build.sh - full pipeline: your RVZ -> extracted game -> linked image ->
# recompiled C -> native macOS app (and the headless runner).
#
#   bash scripts/build.sh "/path/to/New Super Mario Bros Wii (Europe).rvz"
#   bash scripts/build.sh --skip-extract          # reuse ./game
#
# Requirements (macOS): Xcode Command Line Tools (clang), CMake, Python 3, zstd
#   xcode-select --install
#   brew install cmake zstd
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME="$ROOT/game"
BUILD="$ROOT/build"
CBUILD="$ROOT/build/cmake"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
RVZ=""
SKIP_EXTRACT=0
CONFIG="${CONFIG:-Release}"

for a in "$@"; do
  case "$a" in
    --skip-extract) SKIP_EXTRACT=1 ;;
    --debug) CONFIG=RelWithDebInfo ;;
    *) RVZ="$a" ;;
  esac
done

step() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
need() { command -v "$1" >/dev/null 2>&1 || { echo "missing '$1' - $2"; exit 1; }; }

need python3 "install Python 3"
need clang   "run: xcode-select --install"
need cmake   "run: brew install cmake"

if [ "$SKIP_EXTRACT" = 0 ]; then
  [ -n "$RVZ" ] || { echo "usage: $0 <your NSMBW .rvz>   (or --skip-extract)"; exit 1; }
  step "Disc info"
  python3 "$ROOT/tools/rvz.py" info "$RVZ"
  step "Extracting game files to $GAME"
  python3 "$ROOT/tools/rvz.py" extract "$RVZ" "$GAME"
fi
[ -f "$GAME/sys/main.dol" ] || { echo "no $GAME/sys/main.dol - extract first"; exit 1; }

DOL_SHA=$(shasum "$GAME/sys/main.dol" | cut -d' ' -f1)
if [ "$DOL_SHA" != "a5c5821adea919638fe431e79ba6bd63120431db" ]; then
  echo "warning: main.dol sha1 $DOL_SHA differs from the PAL rev the HLE addresses were made for"
  echo "         (runtime/src/hle/addrs_smnp01.h). Use tools/xref.py to re-locate them."
fi

step "Decompressing REL modules"
mkdir -p "$BUILD/rels"
for lz in "$GAME"/files/rels/*.rel.LZ; do
  python3 "$ROOT/tools/nlz.py" "$lz" "$BUILD/rels/$(basename "${lz%.LZ}")"
done

step "Static linking main.dol + RELs"
python3 "$ROOT/tools/image.py" link "$GAME/sys/main.dol" "$BUILD"/rels/*.rel -o "$BUILD" | tail -6

step "Recompiling PowerPC -> C"
python3 "$ROOT/tools/recomp.py" "$BUILD" -c "$ROOT/config/functions.json"

step "Configuring ($CONFIG)"
cmake -S "$ROOT" -B "$CBUILD" -DCMAKE_BUILD_TYPE="$CONFIG" -DCMAKE_C_COMPILER=clang -DBUILD_DATA_DIR="$BUILD" \
      $(command -v ninja >/dev/null && echo -G Ninja)

step "Compiling (this takes a while: ~41k functions)"
cmake --build "$CBUILD" -j "$JOBS"

if [ "$(uname)" = Darwin ]; then
  APP="$CBUILD/NSMBW Recomp.app"
  mkdir -p "$ROOT/dist"
  rm -rf "$ROOT/dist/NSMBW Recomp.app"
  cp -R "$APP" "$ROOT/dist/"
  SUPPORT="$HOME/Library/Application Support/NSMBWRecomp"
  mkdir -p "$SUPPORT"
  if ! grep -q '^game_dir' "$SUPPORT/settings.ini" 2>/dev/null; then
    echo "game_dir = $GAME" >> "$SUPPORT/settings.ini"
  fi
  step "Done: $ROOT/dist/NSMBW Recomp.app"
  echo "Headless runner: $CBUILD/nsmbw-headless \"$BUILD\" \"$GAME\" -v -t 20"
else
  step "Done: $CBUILD/nsmbw-headless \"$BUILD\" \"$GAME\""
fi
