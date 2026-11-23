# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Print the soft modem's transmit pulses (core/air/air_softmodem_tx.c) as C tables.

Each pulse is sampled at ten samples a symbol over eight symbols each side (161 taps), in Q14, scaled so that its
taps sum to ten: a run of one symbol sits at that symbol's level. The formulas are the bench tools' (p25tool,
nxdntool) in units of the symbol rate:

- C4FM (TIA-102.BAAA-A 9.3): raised cosine, roll-off 0.2, times the shaper (pi f T) / sin(pi f T)
- NXDN (TS 1-A 3.4): root raised cosine, roll-off 0.2, times the shaper sin(pi f T) / (pi f T)

Usage: python3 tools/air_pulses.py
"""
import math

OSR = 10
SPAN = 8
ALPHA = 0.2
STEP = 1 / 960          # frequency step, in units of the symbol rate


def pulse(root, shaper):
    f1, f2 = (1 - ALPHA) / 2, (1 + ALPHA) / 2
    taps = []
    for i in range(2 * SPAN * OSR + 1):
        t = (i - SPAN * OSR) / OSR          # in symbols
        v, f = 0.0, STEP / 2
        while f < f2:
            hf = 1.0 if f < f1 else 0.5 + 0.5 * math.cos(math.pi * (f - f1) / (f2 - f1))
            if root:
                hf = math.sqrt(hf)
            x = math.pi * f
            v += hf * shaper(x) * math.cos(2 * math.pi * f * t)
            f += STEP
        taps.append(v)
    s = sum(taps)
    return [round(v * OSR / s * 16384) for v in taps]


def table(name, taps):
    rows = [", ".join(f"{v}" for v in taps[i:i + 12]) for i in range(0, len(taps), 12)]
    return f"static const int16_t {name}[AIR_PULSE_TAPS] = {{\n\t" + ",\n\t".join(rows) + ",\n};"


def main():
    print(table("pulse_c4fm", pulse(False, lambda x: x / math.sin(x))))
    print(table("pulse_nxdn", pulse(True, lambda x: math.sin(x) / x)))


if __name__ == "__main__":
    main()
