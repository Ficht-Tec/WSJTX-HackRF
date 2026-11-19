#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/common.sh"

echo "=== Hamlib $HAMLIB_VERSION ==="

rm -rf "$HAMLIB_SRC" "$HAMLIB_BUILD" "$HAMLIB_PREFIX"

git clone https://github.com/Hamlib/Hamlib.git "$HAMLIB_SRC"
git -C "$HAMLIB_SRC" checkout --detach "$HAMLIB_COMMIT"

ACTUAL="$(git -C "$HAMLIB_SRC" rev-parse HEAD)"

case "$ACTUAL" in
    "$HAMLIB_COMMIT"*)
        ;;
    *)
        echo "ERROR: commit Hamlib inesperado: $ACTUAL" >&2
        exit 1
        ;;
esac

mkdir -p "$HAMLIB_BUILD"

(
    cd "$HAMLIB_SRC"
    ./bootstrap
)

(
    cd "$HAMLIB_BUILD"

    "$HAMLIB_SRC/configure" \
        --prefix="$HAMLIB_PREFIX" \
        --disable-static
)

make -C "$HAMLIB_BUILD" -j"$(jobs)"
make -C "$HAMLIB_BUILD" install

test -x "$HAMLIB_PREFIX/bin/rigctl"
test -x "$HAMLIB_PREFIX/bin/rigctld"
test -f "$HAMLIB_PREFIX/include/hamlib/rig.h"

echo
echo "Hamlib instalado en:"
echo "  $HAMLIB_PREFIX"
"$HAMLIB_PREFIX/bin/rigctl" --version | head -1 || true
