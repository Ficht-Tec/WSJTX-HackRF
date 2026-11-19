#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
PROJECT_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"

# shellcheck source=/dev/null
source "$SCRIPT_DIR/common.sh"

if [[ ! -r /etc/os-release ]]; then
    echo "ERROR: no se puede identificar el sistema operativo." >&2
    exit 1
fi

# shellcheck source=/dev/null
source /etc/os-release

if [[ "${ID:-}" != "ubuntu" ]]; then
    echo "ERROR: este instalador está destinado a Ubuntu nativo." >&2
    echo "Sistema detectado: ${PRETTY_NAME:-desconocido}" >&2
    exit 1
fi

case "${VERSION_ID:-}" in
    20.04|22.04|24.04)
        ;;
    *)
        echo "ADVERTENCIA: Ubuntu ${VERSION_ID:-desconocido} todavía no está validado."
        echo "Se intentará continuar."
        echo
        ;;
esac

echo
echo "========================================"
echo " WSJTX-HackRF $WSJTX_HACKRF_VERSION"
echo " Instalador Ubuntu nativo"
echo "========================================"
echo
echo "Sistema : ${PRETTY_NAME}"
echo "Destino : $PREFIX"
echo

#
# Dependencias Ubuntu
#

sudo apt-get update

sudo apt-get install -y \
    software-properties-common

sudo add-apt-repository -y universe >/dev/null 2>&1 || true

sudo apt-get update

sudo apt-get install -y \
    build-essential \
    cmake \
    git \
    pkg-config \
    autoconf \
    automake \
    libtool \
    libtool-bin \
    gfortran \
    libusb-1.0-0-dev \
    libudev-dev \
    libfftw3-dev \
    portaudio19-dev \
    libboost-all-dev \
    qtbase5-dev \
    qttools5-dev \
    qttools5-dev-tools \
    qtmultimedia5-dev \
    libqt5serialport5-dev \
    libqt5svg5-dev \
    libqt5sql5-sqlite \
    pulseaudio-utils \
    netcat-openbsd \
    util-linux \
    desktop-file-utils

echo
echo "=== Dependencias Ubuntu instaladas ==="
echo

#
# Construcción
#

"$SCRIPT_DIR/build-hamlib.sh"
"$SCRIPT_DIR/build-hackrf.sh"
"$SCRIPT_DIR/build-wsjtx.sh"
"$SCRIPT_DIR/build-bridge.sh"

#
# Instalación de lanzadores privados
#

mkdir -p \
    "$PREFIX/bin" \
    "$PREFIX/bridge/bin" \
    "$HOME/.local/bin" \
    "$HOME/.local/share/applications"

install -m 0755 \
    "$PROJECT_ROOT/scripts/run-bridge-hackrf.sh" \
    "$PREFIX/bridge/bin/run-bridge-hackrf"

install -m 0755 \
    "$PROJECT_ROOT/scripts/run-wsjtx-hackrf.sh" \
    "$PREFIX/bin/run-wsjtx-hackrf"

ln -sfn \
    "$PREFIX/bin/run-wsjtx-hackrf" \
    "$HOME/.local/bin/wsjtx-hackrf"

#
# Regla udev HackRF
#

UDEV_RULE="$(
    find "$HACKRF_PREFIX/share" \
        -type f \
        -name '53-hackrf.rules' \
        -print \
        -quit \
        2>/dev/null || true
)"

if [[ -n "$UDEV_RULE" ]]; then
    echo
    echo "Instalando regla udev HackRF..."

    sudo install -m 0644 \
        "$UDEV_RULE" \
        /etc/udev/rules.d/53-hackrf.rules

    sudo udevadm control --reload-rules
    sudo udevadm trigger
else
    echo
    echo "AVISO: no se encontró 53-hackrf.rules en la instalación privada."
fi

#
# Lanzador gráfico nativo
#

DESKTOP="$HOME/.local/share/applications/wsjtx-hackrf.desktop"

cat > "$DESKTOP" <<DESKTOP_EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=WSJT-X HackRF
Comment=WSJT-X con interfaz nativa para HackRF
Exec=$HOME/.local/bin/wsjtx-hackrf
Icon=applications-engineering
Terminal=false
Categories=HamRadio;Network;
StartupNotify=true
DESKTOP_EOF

chmod 0644 "$DESKTOP"

desktop-file-validate "$DESKTOP" || true

#
# Resumen
#

echo
echo "========================================"
echo " WSJTX-HackRF instalado"
echo "========================================"
echo
echo "Version:"
echo "  $WSJTX_HACKRF_VERSION"
echo
echo "WSJT-X:"
echo "  $WSJTX_PREFIX/bin/wsjtx"
echo
echo "Bridge:"
echo "  $BRIDGE_PREFIX/bin/wsjtx-hackrf-bridge"
echo
echo "Hamlib privada:"
echo "  $HAMLIB_PREFIX"
echo
echo "libhackrf privada:"
echo "  $HACKRF_PREFIX"
echo
echo "Lanzador:"
echo "  $HOME/.local/bin/wsjtx-hackrf"
echo
echo "Desktop:"
echo "  $DESKTOP"
echo
echo "Logs:"
echo "  ${XDG_STATE_HOME:-$HOME/.local/state}/wsjtx-hackrf/logs"
echo
echo "IMPORTANTE:"
echo "  Si el HackRF estaba conectado durante la instalación,"
echo "  desconéctelo y vuelva a conectarlo para aplicar udev."
echo
echo "La primera ejecución de WSJT-X todavía requiere configurar"
echo "indicativo, locator y dispositivos de audio."
echo
