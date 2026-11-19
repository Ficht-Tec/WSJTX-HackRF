# WSJTX-HackRF

Native HackRF support layer and WSJT-X integration for Linux.

This project builds a private, self-contained WSJT-X installation with a HackRF bridge, persistent HackRF gain controls, USB diagnostics and additional logging/export features, without replacing the system WSJT-X installation.

Current public release: **v0.1.0**

## Status

The current release is focused exclusively on **HackRF**. PlutoSDR and Ettus/USRP support are intentionally left for a later version.

The public installer has been validated end-to-end on **Ubuntu 20.04**. The build scripts are also intended for newer Ubuntu releases, but only environments actually tested should be considered confirmed.

## Main features

- WSJT-X 3.0.2 built from upstream source.
- Private Hamlib 4.5.5 installation.
- Private libhackrf 2021.03.1 installation.
- Native HackRF bridge on `127.0.0.1:4532`.
- HackRF RX controls: LNA, VGA and AMP.
- HackRF TX VGA control.
- Persistent HackRF gain settings.
- Conservative first-run defaults: LNA=0, VGA=0, AMP=0, TX VGA=0.
- Explicit bridge enable/disable control.
- Bridge disabled by default.
- WSJT-X still starts if the HackRF bridge cannot start.
- HackRF / USB diagnostics.
- Explicit ADIF export.
- REG1TEST EDI export.
- Coexists with a distro-provided WSJT-X installation.
- No Docker dependency in the public release.

## Bridge behaviour

The HackRF bridge is **not started by default**. The user explicitly enables it from the HackRF controls. On the next WSJT-X start, the launcher attempts to start the bridge.

When the bridge is enabled, configure WSJT-X Radio settings as:

```text
Rig: Hamlib NET rigctl
Network Server: 127.0.0.1:4532
```

The project does not automatically overwrite the user's normal radio/CAT configuration.

## Installation

```bash
./scripts/install-ubuntu.sh
```

If executable bits were lost while copying the tree:

```bash
bash ./scripts/install-ubuntu.sh
```

Default prefix:

```text
~/.local/lib/wsjtx-hackrf
```

Launcher:

```text
~/.local/bin/wsjtx-hackrf
```

The system `/usr/bin/wsjtx` is not replaced. See [docs/INSTALL.md](docs/INSTALL.md).

## First start

WSJT-X still requires the usual user configuration: callsign, grid locator, audio devices and radio/CAT configuration if required. The HackRF bridge remains disabled until explicitly enabled.

## Upstream versions

```text
WSJT-X   v3.0.2
Hamlib   4.5.5
HackRF   2021.03.1
```

Exact commits are stored in `VERSIONS`.

## Documentation

- [Installation](docs/INSTALL.md)
- [Architecture](docs/ARCHITECTURE.md)
- [HackRF integration](docs/HACKRF.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)
- [Changelog](CHANGELOG.md)

## RF responsibility

This project controls radio-frequency hardware. The operator remains responsible for legal frequency/mode use, output power, amplifier drive, antenna gain, filtering, impedance matching, load/antenna condition, spectral purity and local regulations. The zero-gain defaults are a conservative starting point, not a substitute for proper station engineering.

## Scope of v0.1.0

This release is intentionally limited to the validated HackRF integration. PlutoSDR, Ettus/USRP and more automatic bridge/radio configuration are deferred to later releases.

## License

WSJT-X, Hamlib and HackRF are independent upstream projects and retain their respective licenses and copyrights. Before publishing, the repository should include the applicable license for original bridge code and all required notices for patches derived from upstream WSJT-X.

## License and upstream projects

WSJTX-HackRF is an independent community project and is not an official
WSJT-X release.

Original WSJTX-HackRF code and patches are distributed under the GNU
General Public License version 3 unless otherwise noted.

WSJT-X is Copyright by Joseph Taylor, K1JT, and the WSJT Development Team,
and is distributed under GPLv3.

Hamlib and HackRF/libhackrf are independent upstream projects and retain
their own licenses and copyrights.

See `LICENSE` and `NOTICE` for details.
