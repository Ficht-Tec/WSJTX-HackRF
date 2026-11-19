#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"

# shellcheck source=/dev/null
source "$PROJECT_ROOT/VERSIONS"

PREFIX="${WSJTX_HACKRF_PREFIX:-$HOME/.local/lib/wsjtx-hackrf}"
WORK_ROOT="${WSJTX_HACKRF_BUILD_ROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/wsjtx-hackrf-build}"

SRC_ROOT="$WORK_ROOT/src"
BUILD_ROOT="$WORK_ROOT/build"

HAMLIB_SRC="$SRC_ROOT/hamlib"
HAMLIB_BUILD="$BUILD_ROOT/hamlib"
HAMLIB_PREFIX="$PREFIX/hamlib"

HACKRF_SRC="$SRC_ROOT/hackrf"
HACKRF_BUILD="$BUILD_ROOT/hackrf"
HACKRF_PREFIX="$PREFIX/hackrf"

WSJTX_SRC="$SRC_ROOT/wsjtx"
WSJTX_BUILD="$BUILD_ROOT/wsjtx"
WSJTX_PREFIX="$PREFIX/wsjtx"

BRIDGE_BUILD="$BUILD_ROOT/bridge"
BRIDGE_PREFIX="$PREFIX/bridge"

mkdir -p "$SRC_ROOT" "$BUILD_ROOT" "$PREFIX"

export PATH="$HAMLIB_PREFIX/bin:$PATH"
export PKG_CONFIG_PATH="$HACKRF_PREFIX/lib/pkgconfig:$HAMLIB_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export CMAKE_PREFIX_PATH="$HAMLIB_PREFIX;$HACKRF_PREFIX${CMAKE_PREFIX_PATH:+;$CMAKE_PREFIX_PATH}"

jobs()
{
    command -v nproc >/dev/null 2>&1 && nproc || echo 2
}
