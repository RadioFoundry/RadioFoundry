# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Print a transmit tap's low-end pre-compensator (hal_baseband_t.tx_lf) as a C table.

A modulator path with first-order high-pass corners below its band (an audio input's coupling) loses the low end of
a four-level signal: a run of one level sags, and the levels around it move. The soft modem undoes that before the
pulse, on the symbol levels: tx_lf is a filter at the symbol rate whose taps look ahead of the symbol (tap at is the
symbol itself), the least squares inverse of the corners over random symbols, with a penalty on the levels it makes
(lam) so that a long run doesn't drive the path past its range. The path's in-band shape is tx_eq's.

The corners are the path's own, measured with tx_eq in place (each a first-order section s / (s + 2 pi f)).

Usage: python3 tools/air_tx_lf.py --corners 73,125 [--rate 4800] [--taps 97] [--at 48] [--lam 0.003]
"""
import argparse
import math


def highpass(corners, rate, n):
    """The corners' impulse response at rate: first-order sections, matched z, unity gain at half the rate"""
    x = [1.0] + [0.0] * (n - 1)
    for c in corners:
        z = math.exp(-2 * math.pi * c / rate)
        y, xp, yp = [], 0.0, 0.0
        for v in x:
            yp = (v - xp) * (1 + z) / 2 + z * yp
            xp = v
            y.append(yp)
        x = y
    return x


def solve(a, b):
    """a x = b by Gaussian elimination with partial pivoting (a is small)"""
    n = len(b)
    m = [row[:] + [b[i]] for i, row in enumerate(a)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(m[r][c]))
        m[c], m[p] = m[p], m[c]
        for r in range(c + 1, n):
            f = m[r][c] / m[c][c]
            if f:
                for k in range(c, n + 1):
                    m[r][k] -= f * m[c][k]
    x = [0.0] * n
    for r in range(n - 1, -1, -1):
        x[r] = (m[r][n] - sum(m[r][k] * x[k] for k in range(r + 1, n))) / m[r][r]
    return x


def design(corners, rate, taps, at, lam):
    """Taps k[i], i = 0..taps-1, k[i] weighing the symbol at - i ahead: minimizes E|g * (k * x) - x|^2 + lam E|k * x|^2
    for white x (g the corners at the symbol rate)"""
    g = highpass(corners, rate, 8 * rate)
    r = [sum(g[n] * g[n + lag] for n in range(len(g) - lag)) for lag in range(taps)]
    a = [[r[abs(i - j)] + (lam if i == j else 0.0) for j in range(taps)] for i in range(taps)]
    b = [g[at - i] if at - i >= 0 else 0.0 for i in range(taps)]
    return solve(a, b)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--corners", required=True, help="Hz, comma separated")
    ap.add_argument("--rate", type=int, default=4800, help="symbols a second")
    ap.add_argument("--taps", type=int, default=97)
    ap.add_argument("--at", type=int, default=48, help="the tap of the symbol itself (even: the table serves half the "
                    "rate too)")
    ap.add_argument("--lam", type=float, default=0.003)
    a = ap.parse_args()
    k = design([float(c) for c in a.corners.split(",")], a.rate, a.taps, a.at, a.lam)
    q = [round(v * 16384) for v in k]
    rows = [", ".join(str(v) for v in q[i:i + 12]) for i in range(0, len(q), 12)]
    print(f"// tools/air_tx_lf.py --corners {a.corners} --rate {a.rate} --taps {a.taps} --at {a.at} --lam {a.lam}")
    print("\t" + ",\n\t".join(rows) + ",")


if __name__ == "__main__":
    main()
