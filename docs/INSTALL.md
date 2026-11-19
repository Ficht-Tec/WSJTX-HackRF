# Installation

## Supported platform

v0.1.0 targets Ubuntu. The complete public build has been validated on **Ubuntu 20.04**. Newer Ubuntu releases may work, but should only be described as tested after a clean build.

## Coexistence

The installer does not replace a distro WSJT-X package. `/usr/bin/wsjtx` can coexist with `~/.local/lib/wsjtx-hackrf/wsjtx/bin/wsjtx`.

## Install

```bash
./scripts/install-ubuntu.sh
```

If executable bits were lost:

```bash
bash ./scripts/install-ubuntu.sh
```

Do **not** run the whole installer with `sudo`. It invokes `sudo` only where needed.

## Main Ubuntu dependencies

The installer includes build tools plus `libusb-1.0-0-dev`, `libudev-dev`, `libfftw3-dev`, `portaudio19-dev`, Boost, Qt5 development packages, `pulseaudio-utils`, `netcat-openbsd`, `util-linux` and `desktop-file-utils`. The exact package list in `scripts/install-ubuntu.sh` is authoritative.

## Built components

- Hamlib 4.5.5 -> `~/.local/lib/wsjtx-hackrf/hamlib`
- HackRF 2021.03.1 -> `~/.local/lib/wsjtx-hackrf/hackrf`
- WSJT-X 3.0.2 -> `~/.local/lib/wsjtx-hackrf/wsjtx`
- bridge -> `~/.local/lib/wsjtx-hackrf/bridge`

The installer does not update HackRF firmware.

## Build cache

Default: `~/.cache/wsjtx-hackrf-build`

Override with `WSJTX_HACKRF_BUILD_ROOT`.

## First start

```bash
~/.local/bin/wsjtx-hackrf
```

Configure callsign, locator and audio normally. The bridge is disabled by default.

## Enable HackRF bridge

Enable `Enable HackRF bridge on next startup`, then configure:

```text
Settings -> Radio
Rig: Hamlib NET rigctl
Network Server: 127.0.0.1:4532
```

Restart WSJT-X.

The marker is stored at:

```text
~/.config/wsjtx-hackrf/bridge-enabled
```

## Logs

```text
~/.local/state/wsjtx-hackrf/logs
```

## Rebuild one component

```bash
bash ./scripts/build-hamlib.sh
bash ./scripts/build-hackrf.sh
bash ./scripts/build-wsjtx.sh
bash ./scripts/build-bridge.sh
```

If only a WSJT-X patch changes, Hamlib and HackRF do not need rebuilding.
