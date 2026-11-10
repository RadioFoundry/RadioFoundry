<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# DMR PA key-up

A radio that keys its PA for every DMR burst has to reach full power by the burst's first symbol. A PA control loop
comes up slowly from a small drive: at a low power step it can take 3-4 ms, while the top step takes about 1 ms. A
fixed lead long enough for the low steps keys the top step early and cuts into the receive time before our slot, which
an SFR ([sfr.md](sfr.md)) needs. So the key-up drives the PA control harder for a moment first, by an amount that
depends on the step, and takes every value from the radio's own power calibration table.

Code: `core/radio/pa_keyup.h`. A radio's hardware page says whether it keys this way and with which constants.

## 1. The sequence

`d` is the step's drive from the power cal table at the transmit frequency, `top` the highest step's drive there and
`lowest` the lowest step's. All times are before the burst's first symbol.

| Point | PA drive | PA |
|---|---|---|
| `boost_us` | boost = d + blend × (top% × top − d) | off |
| `hold_us` | hold = max(lowest, hold% × d), at most d | off |
| `pa_us` | d | on |

| Constant | What it sets |
|---|---|
| `blend_pct` | how far the boost goes from d toward its target, 0-100 % |
| `top_pct` | the boost target, % of the top step's drive, 50-120 % |
| `hold_pct` | the hold, % of d, 0-100 % |
| `boost_us`, `hold_us`, `pa_us` | the three points, in that order; `boost_us` 0 = no boost or hold: the PA keyed at `pa_us` at d |

The boost charges the PA control with the PA still off. A low step gets a large boost and the top step almost none, so
no step is boosted much over the top step's drive, and only for about a millisecond. The hold lets the control settle
back toward d before the PA comes on. Nothing is stored per frequency or per step: the cal table already has the drives.

Integer math, 32-bit: `boost = d + (top * top_pct / 100 - d) * blend_pct / 100` (no boost part when the target is
under d), clamped to the board's drive limit and never under d.

## 2. What a board does

A board with per-burst keying opts in; one that doesn't call it keys its PA as before.

| Step | Call |
|---|---|
| Start-up | `pa_keyup_init(defaults, max_ahead_us, max_drive)`: its constants, the earliest point its burst sequence can run (after the burst task's wake and any switching that has to go first) and its drive limit |
| Key-up (radio task) | `pa_keyup_plan(d, lowest, top)` with its cal table's drives at the transmit frequency; keep the result and the constants (`pa_keyup_cfg`) for the bursts |
| Each burst | the three points in time order, the RF switch to transmit placed among them by its own time |

The RF switch has its own lead. A switch that holds the burst task for longer (a synthesizer retune on a split channel)
goes before the boost, so it can't delay the boost's points. A short one goes where its time falls. On a simplex
channel, the later the switch, the more of the inbound burst before ours the radio still hears.

## 3. Debug link

The constants are RAM: RADIO op 37 ([debuglink.md](debuglink.md)) changes them for tuning, from the next key-up, and a
restart restores the board's. The `core` view shows `pa_keyup` (the constants) and `pa_keyup.last` (the last key-up's
boost, hold and drive).

```
rfoundry_debug.py radio pa-keyup 55 111 80 1500 500 150
rfoundry_debug.py radio pa-keyup default
```

## 4. Tuning a radio

Tune on a dummy load, with raw I/Q from the SDR bench at 1 MS/s or more, about 20 bursts per setting. For each burst,
find the first symbol from the sync, and take the median power envelope against it.

| Measure | Target |
|---|---|
| The -3 dB point after the first symbol | within 0.15 ms, every step |
| The first symbol's level against the settled level | within 3 dB at the top steps |
| Overshoot above the settled level | as small as the above allows; the low steps overshoot most |

1. Start with `blend_pct` 50, `top_pct` 100, `hold_pct` 80 and the points at 1.5, 0.5 and 0.15 ms.
2. Raise `blend_pct` until the lowest step is up by the first symbol, then `top_pct` a little if the middle steps lag.
3. If a step's level at the first symbol varies a lot between runs, raise `hold_pct`.
4. Check every step at both band edges and the band center, then split channels and the first burst of a call.

The charts show a radio before and after, at its lowest step and its top step: the estimated PA output (a curve fit
of the measured median), and the drive through each sequence.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../dev/img/dmr-pa-keyup-ramp-dark.png">
  <img alt="Estimated PA output through a key-up at the lowest and top step: before, a fixed full-drive kick, and this sequence" src="../dev/img/dmr-pa-keyup-ramp-light.png">
</picture>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../dev/img/dmr-pa-keyup-drive-dark.png">
  <img alt="PA drive through a key-up at the lowest and top step for the same three sequences" src="../dev/img/dmr-pa-keyup-drive-light.png">
</picture>

A fixed full-scale kick at key-up drives far over the top step and still leaves the low step late; the boost from the
table brings both up by the first symbol.
