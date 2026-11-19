#!/usr/bin/env bash
set -Eeuo pipefail

SELF="$(readlink -f "${BASH_SOURCE[0]}")"
PREFIX="$(cd "$(dirname "$SELF")/.." && pwd)"

BRIDGE_START="$PREFIX/bridge/bin/run-bridge-hackrf"
WSJTX_BIN="$PREFIX/wsjtx/bin/wsjtx"
HAMLIB="$PREFIX/hamlib"

STATE_ROOT="${XDG_STATE_HOME:-$HOME/.local/state}/wsjtx-hackrf"
CONFIG_ROOT="${XDG_CONFIG_HOME:-$HOME/.config}/wsjtx-hackrf"

BRIDGE_LOG="$STATE_ROOT/logs/bridge.log"
BRIDGE_ENABLE_FILE="$CONFIG_ROOT/bridge-enabled"

CAT_HOST="127.0.0.1"
CAT_PORT="4532"

BRIDGE_PID=''
BRIDGE_PGID=''
BRIDGE_ACTIVE=0

cleanup()
{
    trap - EXIT INT TERM

    if [[ "$BRIDGE_ACTIVE" -eq 1 && -n "${BRIDGE_PGID:-}" ]]; then

        kill -TERM -- "-$BRIDGE_PGID" 2>/dev/null || true

        for _ in $(seq 1 30); do
            if ! kill -0 -- "-$BRIDGE_PGID" 2>/dev/null; then
                break
            fi
            sleep 0.1
        done

        if kill -0 -- "-$BRIDGE_PGID" 2>/dev/null; then
            echo "Forzando cierre del grupo bridge..."
            kill -KILL -- "-$BRIDGE_PGID" 2>/dev/null || true
        fi
    fi

    if [[ -n "${BRIDGE_PID:-}" ]]; then
        wait "$BRIDGE_PID" 2>/dev/null || true
    fi
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

echo
echo "======================================"
echo " WSJT-X / HackRF"
echo "======================================"
echo

[[ -x "$WSJTX_BIN" ]] || {
    echo "ERROR: WSJT-X no está instalado:"
    echo "  $WSJTX_BIN"
    exit 1
}

[[ -e "$HAMLIB/lib/libhamlib.so" ]] || {
    echo "ERROR: falta Hamlib privada:"
    echo "  $HAMLIB/lib/libhamlib.so"
    exit 1
}

mkdir -p "$(dirname "$BRIDGE_LOG")" "$CONFIG_ROOT"


#
# Bridge HackRF:
# desactivado completamente por defecto.
#

if [[ ! -f "$BRIDGE_ENABLE_FILE" ]]; then

    echo "Bridge HackRF: desactivado."
    echo

else

    echo "Bridge HackRF: habilitado."

    if [[ ! -x "$BRIDGE_START" ]]; then

        echo "AVISO: bridge HackRF no disponible."
        echo "WSJT-X arrancará sin bridge."
        echo

    elif ! command -v nc >/dev/null || ! command -v setsid >/dev/null; then

        echo "AVISO: faltan utilidades para iniciar el bridge."
        echo "WSJT-X arrancará sin bridge."
        echo

    elif nc -z "$CAT_HOST" "$CAT_PORT" >/dev/null 2>&1; then

        echo "AVISO: el puerto CAT $CAT_HOST:$CAT_PORT ya está ocupado."
        echo "No se iniciará otro bridge."
        echo

    else

        echo "Intentando iniciar bridge HackRF..."

        setsid "$BRIDGE_START" &
        BRIDGE_PID=$!
        BRIDGE_PGID=$BRIDGE_PID

        CAT_OK=0

        for _ in $(seq 1 50); do

            if ! kill -0 "$BRIDGE_PID" 2>/dev/null; then
                break
            fi

            if nc -z "$CAT_HOST" "$CAT_PORT" >/dev/null 2>&1; then
                CAT_OK=1
                break
            fi

            sleep 0.1
        done

        if [[ "$CAT_OK" -eq 1 ]]; then

            BRIDGE_ACTIVE=1

            #
            # Condición inicial conservadora.
            #
            # Si WSJT-X tiene ganancias guardadas,
            # las restaurará después de arrancar.
            #

            for CMD in \
                '\set_lna 0' \
                '\set_vga 0' \
                '\set_amp 0' \
                '\set_tx_vga 0'
            do
                printf '%s\n' "$CMD" |
                    nc -w 2 "$CAT_HOST" "$CAT_PORT" >/dev/null 2>&1 \
                    || true
            done

            echo
            echo "Bridge HackRF activo."
            echo "Ganancias iniciales: LNA=0 VGA=0 AMP=0 TX_VGA=0"

            STATUS="$(
                printf '\\status\n' |
                    nc -w 2 "$CAT_HOST" "$CAT_PORT" 2>/dev/null \
                    || true
            )"

            if [[ -n "$STATUS" ]]; then
                echo "$STATUS"
            fi

            echo

        else

            echo
            echo "AVISO: no se pudo iniciar el bridge HackRF."

            if [[ -f "$BRIDGE_LOG" ]]; then
                tail -10 "$BRIDGE_LOG" || true
            fi

            echo
            echo "WSJT-X arrancará igualmente sin bridge."
            echo

            if [[ -n "$BRIDGE_PGID" ]]; then
                kill -TERM -- "-$BRIDGE_PGID" 2>/dev/null || true
            fi

            wait "$BRIDGE_PID" 2>/dev/null || true

            BRIDGE_PID=''
            BRIDGE_PGID=''
            BRIDGE_ACTIVE=0
        fi
    fi
fi


#
# Entorno WSJT-X
#

export LD_LIBRARY_PATH="$HAMLIB/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

if [[ "$BRIDGE_ACTIVE" -eq 1 ]]; then
    export WSJTX_HACKRF_BRIDGE_LOG="$BRIDGE_LOG"
else
    unset WSJTX_HACKRF_BRIDGE_LOG 2>/dev/null || true
fi

echo "Iniciando WSJT-X..."
echo

"$WSJTX_BIN"
WSJTX_RC=$?

echo
echo "WSJT-X terminado (rc=$WSJTX_RC)."

exit "$WSJTX_RC"
