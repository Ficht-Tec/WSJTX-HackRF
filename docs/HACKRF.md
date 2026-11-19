# HackRF integration

## Scope

v0.1.0 supports HackRF through a private libhackrf build pinned to release 2021.03.1. The installer does not automatically update firmware.

## Default gains

```text
LNA    = 0 dB
VGA    = 0 dB
AMP    = OFF
TX VGA = 0 dB
```

These are conservative defaults, not recommended operating values.

## Gain ranges

RX LNA: `0, 8, 16, 24, 32, 40 dB`

RX VGA: `0..62 dB` in 2 dB steps.

TX VGA: `0..47 dB`.

AMP: OFF/ON.

## Bridge

The bridge is disabled by default. Enable it explicitly, then configure Hamlib NET on `127.0.0.1:4532` and restart WSJT-X.

## Diagnostics

The HackRF controls dialog exposes bridge/USB state so common failures can be seen without a debugger.

## RF responsibility

The correct gain depends on the complete station: antenna gain, feedline loss, external LNA, filters, attenuators, PA drive requirements and operating frequency. The operator is responsible for legal power and spectral purity.
