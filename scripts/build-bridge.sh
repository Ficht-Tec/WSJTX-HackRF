#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/common.sh"

echo "=== WSJTX-HackRF bridge ==="

rm -rf "$BRIDGE_BUILD" "$BRIDGE_PREFIX"

cmake \
    -S "$PROJECT_ROOT/bridge" \
    -B "$BRIDGE_BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$BRIDGE_PREFIX" \
    -DHACKRF_ROOT="$HACKRF_PREFIX"

cmake --build "$BRIDGE_BUILD" -j"$(jobs)"
cmake --install "$BRIDGE_BUILD"

BRIDGE_BIN="$BRIDGE_PREFIX/bin/wsjtx-hackrf-bridge"

test -x "$BRIDGE_BIN" || {
    echo "ERROR: no se ha instalado el bridge:" >&2
    echo "  $BRIDGE_BIN" >&2
    exit 1
}

echo
echo "Bridge instalado:"
echo "  $BRIDGE_BIN"

echo
echo "Dependencias:"
ldd "$BRIDGE_BIN" || true

echo
echo "RUNPATH:"
readelf -d "$BRIDGE_BIN" 2>/dev/null \
    | grep -E 'RPATH|RUNPATH' \
    || true
