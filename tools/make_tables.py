#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Drumwelt: compute the tables the firmware needs (the chip has no floating point) -> firmware/src/dw_tables.h

  python3 tools/make_tables.py

The voices' envelopes are counted in chunks of 16 frames at 44.1 kHz (2756.25 a second). A decay is the share of
the level kept a chunk (Q16), -60 dB in the time shown; 64 times, 3 ms .. 10 s, evenly spaced by ratio.
"""
from pathlib import Path

FS = 44100.0
CHUNKS = FS / 16.0
N = 64


def main():
    ms = [3.0 * (10000.0 / 3.0) ** (i / (N - 1)) for i in range(N)]
    keep = [min(65535, round(65536 * 10 ** (-3 / (t / 1000.0 * CHUNKS)))) for t in ms]
    out = ["/* SPDX-License-Identifier: GPL-3.0-only */",
           "/* made by tools/make_tables.py: do not edit */",
           "#define NDK %du" % N,
           "/* a decay: the share of the level kept a chunk of 16 frames (Q16), -60 dB in DK_MS */",
           "static const uint16_t DK[NDK] = {" + ", ".join(str(k) for k in keep) + "};",
           "static const uint16_t DK_MS[NDK] = {" + ", ".join(str(round(t)) for t in ms) + "};", ""]
    Path(__file__).resolve().parent.parent.joinpath("firmware/src/dw_tables.h").write_text("\n".join(out))


if __name__ == "__main__":
    main()
