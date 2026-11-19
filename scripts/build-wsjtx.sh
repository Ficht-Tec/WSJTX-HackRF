#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/common.sh"

echo "=== WSJT-X $WSJTX_TAG + WSJTX-HackRF patches ==="

rm -rf "$WSJTX_SRC" "$WSJTX_BUILD" "$WSJTX_PREFIX"

git clone https://github.com/WSJTX/wsjtx.git "$WSJTX_SRC"
git -C "$WSJTX_SRC" checkout --detach "$WSJTX_COMMIT"

ACTUAL="$(git -C "$WSJTX_SRC" rev-parse HEAD)"

if [ "$ACTUAL" != "$WSJTX_COMMIT" ]; then
    echo "ERROR: commit WSJT-X inesperado:" >&2
    echo "  esperado: $WSJTX_COMMIT" >&2
    echo "  obtenido: $ACTUAL" >&2
    exit 1
fi

PATCHES=("$PROJECT_ROOT"/patches/wsjtx/*.patch)

if [ "${#PATCHES[@]}" -ne 9 ]; then
    echo "ERROR: se esperaban 9 patches WSJT-X y hay ${#PATCHES[@]}" >&2
    exit 1
fi

echo
echo "Aplicando ${#PATCHES[@]} patches en orden..."

for PATCH in "${PATCHES[@]}"; do
    echo
    echo "  -> $(basename "$PATCH")"

    git -C "$WSJTX_SRC" apply --check "$PATCH"
    git -C "$WSJTX_SRC" apply "$PATCH"
done

git -C "$WSJTX_SRC" diff --check

EXTRA_CMAKE_ARGS=()

if [[ -r /etc/os-release ]]; then
    # shellcheck source=/dev/null
    source /etc/os-release

    if [[ "${ID:-}" == "ubuntu" && "${VERSION_ID:-}" == "20.04" ]]; then
        echo
        echo "Ubuntu 20.04 detectado: habilitando compatibilidad librt para MAP65"
        EXTRA_CMAKE_ARGS+=(
            '-DCMAKE_EXE_LINKER_FLAGS=-Wl,--no-as-needed -lrt -Wl,--as-needed'
        )
    fi
fi

cmake \
    -S "$WSJTX_SRC" \
    -B "$WSJTX_BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$WSJTX_PREFIX" \
    -DWSJT_GENERATE_DOCS=OFF \
    -DWSJT_SKIP_MANPAGES=ON \
    -DHamlib_INCLUDE_DIR="$HAMLIB_PREFIX/include" \
    -DHamlib_LIBRARY="$HAMLIB_PREFIX/lib/libhamlib.so" \
    -DRIGCTL_EXE="$HAMLIB_PREFIX/bin/rigctl" \
    -DRIGCTLD_EXE="$HAMLIB_PREFIX/bin/rigctld" \
    -DRIGCTLCOM_EXE="$HAMLIB_PREFIX/bin/rigctlcom" \
    "${EXTRA_CMAKE_ARGS[@]}"

cmake --build "$WSJTX_BUILD" -j"$(jobs)"
cmake --install "$WSJTX_BUILD"

test -x "$WSJTX_PREFIX/bin/wsjtx"

echo
echo "WSJT-X instalado en:"
echo "  $WSJTX_PREFIX"
echo
"$WSJTX_PREFIX/bin/wsjtx" --version 2>/dev/null || true
