# Changelog

## v0.1.0

Initial public HackRF-focused release.

### Added

- Native Ubuntu build/install workflow.
- Private per-user installation under `~/.local/lib/wsjtx-hackrf`.
- WSJT-X 3.0.2 source build.
- Hamlib 4.5.5 private build.
- libhackrf 2021.03.1 private build.
- Native WSJTX-HackRF bridge.
- Local CAT service on `127.0.0.1:4532`.
- HackRF RX LNA/VGA/AMP controls.
- HackRF TX VGA control.
- Persistent HackRF gain settings.
- Conservative first-run gain defaults: 0 / 0 / off / 0.
- Explicit bridge enable control.
- Bridge disabled by default.
- WSJT-X startup continues when the bridge cannot start.
- HackRF / USB diagnostics.
- Explicit ADIF export.
- REG1TEST EDI export.
- User desktop launcher.
- Ubuntu 20.04 compatibility handling.
- Sequential WSJT-X patch application.

### Clean-install fixes

- Replaced `git am` with `git apply`, so Git user identity is not required.
- Changed patch application to sequential order.
- Added `portaudio19-dev`.
- Added `libudev-dev`.
- Added Qt 5.12-compatible `SkipEmptyParts` handling.
- Added Ubuntu 20.04 MAP65/librt linker compatibility.

### Not included

- PlutoSDR support.
- Ettus / USRP support.
- Automatic modification of the user's WSJT-X radio configuration.
