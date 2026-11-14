# Drumwelt

**Percussion found in nature, for the M-VAVE FM-1.** A sibling of [Groundkeeper](https://github.com/fwembu/groundkeeper).

Each drum sound is an object: a rock (bass drums and toms), a stick (woodblocks and pipes) or a pile of leaves and
seeds (shakers). REC finds one at random, SAVE keeps it in a kit of eight. The keys play the object in hand, tuned to
the spell's scale; every object has a Euclidean cycle of its own, and PLAY runs them all.

| | |
| --- | --- |
| Keys F3..E5 | play the object in hand, in the scale |
| F5, F#5, G5 | what REC finds: rocks, sticks, leaves (lit) |
| REC | a tap finds a new object; held: the one before comes back |
| SAVE | a tap keeps the object in hand in the kit; held on a kit object: out of the kit, back in hand |
| ALGORITHM | round the kit's objects |
| SELECT, PRESETS | the spell, its scales |
| PLAY | the cycles run / stop |
| OCT-, OCT+ | the octave |

| Page | KNOB 1 | KNOB 2 | KNOB 3 | KNOB 4 |
| --- | --- | --- | --- | --- |
| HOME | Size | Matter | Hollow | Strike |
| ENV | Attack | Hold | Decay | Level |
| ARP | Steps | Hits | Rotate | Rate |
| ARP (again) | Chance | Wander | | |
| FX | Filter | Resonance | Space | Mix |
| GLO | Tempo | Pulse | Chords | Tonic |
| SEL | Season | Roots | Shape | |

## Install

Every change to `main` builds by itself and replaces the
[test build](https://github.com/fwembu/drumwelt/releases/tag/test-build). On Windows, run
`tools/windows/INSTALL-DRUMWELT.bat`: it fetches the newest build and installs it over USB after asking. Elsewhere:
`python3 tools/fm1_install.py drumwelt.fwsc`. Hold OCT- while switching on for the USB rescue.

## Build and test

`tests/run_host.sh [script]` runs the firmware on a computer: the sound as a WAV and the screens as PNG in
`build/host/` (`tests/scripts/kit.txt` finds, keeps and plays a small kit). `tests/check_device.sh` checks the device
build's syntax without the FM-1's compiler; the real package is built by `.github/workflows/release.yml`.
The firmware is `firmware/src/dinghy.c`; everything under it is [Dinghy](https://github.com/zvenson/dinghy)'s
platform, at 0.5 (newer FM-1s' flash chip included). `docs/` are Dinghy's.

## License

GPL-3.0-only, as Felucca and SLOOP: you may change and share it, and whoever gets your firmware gets its source.
Details, credits and third-party parts in [LICENSING.md](LICENSING.md).

The platform is Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments, with SLOOP's changes (isod89);
the SMK-37 layer by M. Geisler (genkigenki); the loader's screen after Skiff (illuminus5). Dinghy (the app, the host
mode, the docs) by zvenson.
