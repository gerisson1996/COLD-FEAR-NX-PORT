#!/usr/bin/env bash
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export DEVKITPRO="${DEVKITPRO:-/c/devkitPro}"
TK="${XBOXRECOMP_DIR:-$REPO/xboxrecomp}"
GEN="${COLDFEAR_GEN_DIR:-$REPO/gen}"
BUILD="${BUILD_DIR:-$REPO/build-switch-coldfear}"

if [[ -z "$TK" || ! -f "$TK/CMakeLists.txt" ]]; then
    echo "Set XBOXRECOMP_DIR to the xboxrecomp toolkit directory." >&2
    exit 1
fi
if [[ ! -f "$GEN/recomp_funcs.h" ]]; then
    echo "No generated code in $GEN; run tools/regen.ps1 first." >&2
    exit 1
fi
if [[ ! -f "$REPO/game/default.xbe" ]]; then
    echo "No extracted game files in $REPO/game." >&2
    exit 1
fi
if [[ ! -f "$DEVKITPRO/cmake/Switch.cmake" ]]; then
    echo "devkitPro Switch toolchain not found at $DEVKITPRO." >&2
    exit 1
fi

mkdir -p "$REPO/analysis"

# main.c prints __DATE__ __TIME__ in the log's BUILD line. Ninja only
# recompiles it when it changes, so the stamp stayed at the last edit of
# main.c and a log could not tell which NRO produced it. Touch it every build.
touch "$REPO/src/main.c"

cmake -S "$REPO" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DXBOXRECOMP_DIR="$TK" \
    -DCOLDFEAR_GEN_DIR="$GEN" \
    -DCOLDFEAR_VULKAN="${VULKAN:-ON}"

ninja -C "$BUILD" "-j${JOBS:-6}" 2>&1 | tee "$REPO/analysis/build-switch.log"
echo "Built $BUILD/coldfear_recomp.nro"
