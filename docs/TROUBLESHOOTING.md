# Troubleshooting

## Bridge not starting

Check the explicit marker:

```bash
ls -l ~/.config/wsjtx-hackrf/bridge-enabled
```

## HackRF not connected

A typical message is:

```text
hackrf_open => HackRF not found (-5)
```

WSJT-X should still start. Verify the device with:

```bash
~/.local/lib/wsjtx-hackrf/hackrf/bin/hackrf_info
```

## CAT unavailable

```bash
nc -z 127.0.0.1 4532
```

WSJT-X should use `Hamlib NET rigctl` and `127.0.0.1:4532`.

## Port 4532 occupied

```bash
ss -ltnp | grep ':4532'
```

The launcher avoids starting a second bridge on an occupied port.

## Missing PortAudio development package

```bash
sudo apt-get install -y portaudio19-dev
```

## Linker cannot find -ludev

```bash
sudo apt-get install -y libudev-dev
```

## Ubuntu 20.04 MAP65 / shm_open

The current build script carries the explicit `librt` compatibility handling required by the tested Ubuntu 20.04 environment.

## Qt SkipEmptyParts errors

The patch set contains the Qt 5.12 compatibility fixes required by Ubuntu 20.04. If this reappears, verify that the complete current patch directory was copied before rebuilding.

## Wrong number of patches

The release expects nine patches:

```bash
printf '%s\n' patches/wsjtx/*.patch | wc -l
```

Expected result: `9`.

## Git asks for user.name/user.email

The current build uses `git apply`, not `git am`. If Git asks for identity, an old build script is probably being used.

## Lost executable permissions

```bash
chmod +x scripts/*.sh
```

## Logs

```text
~/.local/state/wsjtx-hackrf/logs/bridge.log
```
