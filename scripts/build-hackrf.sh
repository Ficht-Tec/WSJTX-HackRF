#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/common.sh"

echo "=== HackRF $LIBHACKRF_RELEASE ==="

rm -rf "$HACKRF_SRC" "$HACKRF_BUILD" "$HACKRF_PREFIX"

git clone https://github.com/greatscottgadgets/hackrf.git "$HACKRF_SRC"
git -C "$HACKRF_SRC" checkout --detach "$LIBHACKRF_COMMIT"

ACTUAL="$(git -C "$HACKRF_SRC" rev-parse HEAD)"

case "$ACTUAL" in
    "$LIBHACKRF_COMMIT"*)
        ;;
    *)
        echo "ERROR: commit HackRF inesperado: $ACTUAL" >&2
        exit 1
        ;;
esac

cmake \
    -S "$HACKRF_SRC/host" \
    -B "$HACKRF_BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$HACKRF_PREFIX" \
    -DUDEV_RULES_PATH="$HACKRF_PREFIX/share/udev/rules.d"

cmake --build "$HACKRF_BUILD" -j"$(jobs)"
cmake --install "$HACKRF_BUILD"

test -f "$HACKRF_PREFIX/include/libhackrf/hackrf.h" \
    || test -f "$HACKRF_PREFIX/include/hackrf.h"

test -e "$HACKRF_PREFIX/lib/libhackrf.so"

echo
echo "libhackrf instalada en:"
echo "  $HACKRF_PREFIX"

LD_LIBRARY_PATH="$HACKRF_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$HACKRF_PREFIX/bin/hackrf_info" 2>/dev/null || true
