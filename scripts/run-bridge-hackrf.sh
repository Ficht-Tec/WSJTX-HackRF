#!/usr/bin/env bash
set -Eeuo pipefail

SELF="$(readlink -f "${BASH_SOURCE[0]}")"
BRIDGE_PREFIX="$(cd "$(dirname "$SELF")/.." && pwd)"

BIN="$BRIDGE_PREFIX/bin/wsjtx-hackrf-bridge"

STATE_ROOT="${XDG_STATE_HOME:-$HOME/.local/state}/wsjtx-hackrf"
LOGS="$STATE_ROOT/logs"

if [[ -n "${XDG_RUNTIME_DIR:-}" ]]; then
    RUNTIME_ROOT="$XDG_RUNTIME_DIR/wsjtx-hackrf"
else
    RUNTIME_ROOT="/tmp/wsjtx-hackrf-$UID"
fi

FIFO="$RUNTIME_ROOT/tx-audio.pcm"

TXMAX="${WSJTX_RF_MAX_SEC:-0}"

RXMOD=''
TXMOD=''
REC_PID=''

mkdir -p "$LOGS" "$RUNTIME_ROOT"
chmod 700 "$RUNTIME_ROOT"

[[ -x "$BIN" ]] || {
    echo "ERROR: no existe el bridge:" >&2
    echo "  $BIN" >&2
    exit 1
}

for cmd in pactl parec pacat; do
    command -v "$cmd" >/dev/null || {
        echo "ERROR: falta el comando $cmd" >&2
        exit 1
    }
done

pactl info >/dev/null || {
    echo "ERROR: no se puede acceder al servidor de audio PulseAudio/PipeWire-Pulse" >&2
    exit 1
}

cleanup()
{
    trap - EXIT INT TERM

    if [[ -n "${REC_PID:-}" ]]; then
        kill "$REC_PID" 2>/dev/null || true
        wait "$REC_PID" 2>/dev/null || true
    fi

    if [[ -n "${TXMOD:-}" ]]; then
        pactl unload-module "$TXMOD" >/dev/null 2>&1 || true
    fi

    if [[ -n "${RXMOD:-}" ]]; then
        pactl unload-module "$RXMOD" >/dev/null 2>&1 || true
    fi

    rm -f "$FIFO" 2>/dev/null || true
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM


#
# Sink RX
#

if ! pactl list short sinks |
     awk '{print $2}' |
     grep -Fxq wsjtx_hackrf_rx
then
    RXMOD="$(
        pactl load-module module-null-sink \
            sink_name=wsjtx_hackrf_rx \
            rate=48000 \
            channels=1 \
            format=s16le \
            sink_properties=device.description=WSJTX_HACKRF_RX
    )"
fi


#
# FIFO TX
#

if [[ -e "$FIFO" && ! -p "$FIFO" ]]; then
    echo "ERROR: $FIFO existe pero no es FIFO" >&2
    exit 1
fi

[[ -p "$FIFO" ]] || mkfifo -m 600 "$FIFO"


#
# Sink TX
#

if ! pactl list short sinks |
     awk '{print $2}' |
     grep -Fxq wsjtx_hackrf_tx
then
    TXMOD="$(
        pactl load-module module-null-sink \
            sink_name=wsjtx_hackrf_tx \
            rate=48000 \
            channels=1 \
            format=s16le \
            sink_properties=device.description=WSJTX_HACKRF_TX
    )"
fi


#
# Captura TX desde el monitor del sink
#

parec \
    --record \
    --raw \
    --format=s16le \
    --rate=48000 \
    --channels=1 \
    --latency-msec=50 \
    --process-time-msec=10 \
    --device=wsjtx_hackrf_tx.monitor \
    > "$FIFO" \
    2> "$LOGS/parec.log" &

REC_PID=$!


echo
echo "WSJT-X / HackRF bridge"
echo "CAT       : 127.0.0.1:4532"
echo "RF TX     : ENABLED"
echo "TX VGA    : controlled by CAT"
echo "TX max    : $TXMAX s (0 = bridge default/unlimited)"
echo "Bridge log: $LOGS/bridge.log"
echo


"$BIN" \
    --live 0 - \
    --port 4532 \
    --tx-fifo "$FIFO" \
    --tx-dir "$LOGS" \
    --tx-max-sec "$TXMAX" \
    --rf-enable \
    --load-50ohm-confirmed \
    2> "$LOGS/bridge.log" |
pacat \
    --playback \
    --raw \
    --format=s16le \
    --rate=48000 \
    --channels=1 \
    --device=wsjtx_hackrf_rx \
    --latency-msec=100
