# Architecture

## Overview

```text
WSJT-X
  |
  | Hamlib NET / CAT
  | 127.0.0.1:4532
  v
WSJTX-HackRF bridge
  |
  | libhackrf
  v
HackRF
```

Audio is exchanged through dedicated PulseAudio / PipeWire-Pulse endpoints.

## Why a bridge

The bridge keeps HackRF USB/RF code out of WSJT-X, keeps the WSJT-X patch set smaller, allows independent CAT diagnostics and lets WSJT-X continue running even if the HackRF side fails.

## Activation

No `~/.config/wsjtx-hackrf/bridge-enabled` marker: WSJT-X starts normally and HackRF is not probed.

Marker present: the launcher starts the bridge, waits for local CAT and then starts WSJT-X. If bridge startup fails, WSJT-X still starts.

## Gain settings

Persistent keys:

```text
HackRF/LNA
HackRF/VGA
HackRF/AMP
HackRF/TXVGA
```

First-run defaults are zero/off.

## Private runtime tree

```text
~/.local/lib/wsjtx-hackrf/
├── hamlib/
├── hackrf/
├── wsjtx/
└── bridge/
```

## Patch series

1. HackRF controls entry in Radio settings.
2. Native HackRF gain controls.
3. CAT gain control and persistent settings.
4. Bridge USB diagnostics.
5. Explicit ADIF export.
6. REG1TEST EDI export core.
7. REG1TEST EDI export dialog.
8. Restore HackRF gains at startup.
9. Explicit bridge enable control and conservative first-run gains.

Patches are applied sequentially because later patches depend on earlier ones.
