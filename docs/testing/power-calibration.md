<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Transmit power calibration with a wattmeter

This procedure finds the PA drive for each of a radio's power levels, quickly and to about ±0.5 dB. Run the full
procedure once per radio model; run the short check on each further radio of that model. The results go into the
board's power table.

The procedure uses the debug link's PA drive override (RADIO op 36, and the `core` view's `tx_drive` for each level's
drive) and the test TX limit ([debuglink.md](../spec/debuglink.md)). `tools/power_cal.py` does the keying: it puts the
radio in the VFO on the frequency (FM, no sub-audio tone, the TX limit on that frequency alone), keys each step, and
asks for the reading.

| Command | Use |
|---|---|
| `power_cal.py --allow-rf MHZ key STEP...` | Key each drive (or `Ln`, power level n) for 2 s (`STEP:S` for S seconds) |
| `power_cal.py --allow-rf MHZ zero --hi D --lo D` | Bisect for the zero point and the cliff (3.2) |
| `power_cal.py --allow-rf MHZ check --margin M` | Run the short check (4): every level, then the margin step; prints PASS or FAIL |

`--no-prompt` keys without asking, for an operator who reads the meter and reports the readings afterwards. Then
`check --readings W,W,...` judges them.

## 1. What the measurements showed

These facts set the procedure. Check them again only on a new model.

| Fact | Consequence |
|---|---|
| Labels on the factory calibration rows can be wrong by 3 dB or more (a "5 W" row gave 9-10 W) | Measure every factory row once; never trust the label |
| Power against drive is steep at the bottom and saturates at the top | Bisect at the bottom; use coarse steps at the top |
| The bottom of the curve follows the **absolute** drive: the drive where output reaches zero (the zero point, Z) moved 0-10 counts between two frequencies 5-6 MHz apart in the same band | The low levels are fixed drives per band, not fractions of a factory row (a fraction put 1 W at 1.9 W). At the very bottom, 10 counts is 2-3 dB, so the lowest level's name must fit both frequencies |
| The top of the curve moves with frequency (the 25 W and top drives differed by up to 50 counts between the same two frequencies) | The upper levels follow the factory rows, which are calibrated at several frequencies |
| **The cliff:** 5-20 counts below the zero point, the power loop loses control and the PA jumps to about 10 W | Never set a level within the margin below; check the lowest level's drive against the cliff, not only the zero point |
| A drive of 0 is below the cliff | The PA must not be on with the drive at 0: the ramp starts and ends at the zero point (otherwise a carrier leaks at key-up and splashes at key-down) |
| A warm PA moved the zero point and the cliff by about 5 counts | One hot check per model; no hot checks per radio |
| With the drive changed after key-up, the meter first shows the starting level | The override sets the drive before key-up |

## 2. Equipment and setup

| Item | Requirement |
|---|---|
| Dummy load | Rated for the radio's top power continuously, at the band's frequencies |
| Wattmeter | In line between the radio and the load; averaging (not peak); resolution 0.01 W below 1 W |
| Supply | The radio's top-power current with margin (about 12 A for a 50 W radio at 13.8 V) |
| Frequencies | Per band: one near the middle (A) and one at least 5 MHz away (B, model runs only). Both must be in the test TX limit |
| Radio | Ready to transmit (no hardware option pending); FM, no sub-audio tone (an unmodulated carrier reads steadiest) |

Each key-up lasts 2 s; read the meter at its end. Wait 3 s between key-ups. Use 5 s key-ups only for the warm-up.

## 3. Full procedure (once per model, per band)

The procedure takes about 30 key-ups per band at frequency A, plus 12 at B and 12 hot.

### 3.1 Factory rows (4 key-ups)

1. Key each factory power row as the radio drives it.
2. Record the drive and the watts.

A row whose power doesn't rise with its drive stops the procedure: the factory calibration is unusable.

### 3.2 Zero point and cliff (about 8 key-ups)

Bisect between the lowest row's drive (on the curve) and half of it (below the cliff) with `power_cal.py zero`. It
classifies each reading:

| Reading | Meaning |
|---|---|
| 0.02 W or more, and under the lowest row's power | On the curve: the new upper bound |
| Under 0.02 W | Between the cliff and the zero point |
| About the lowest row's power or more, at a drive below it | Past the cliff: the new lower bound |

Stop when you know Z (the highest drive reading under 0.02 W) and the cliff C (the highest drive past the cliff) each to
5 counts.

### 3.3 Low curve (4-6 key-ups)

1. Key Z + 10, Z + 30, Z + 60, and Z + 120. If 5 W isn't bracketed, also key halfway to the lowest row.
2. Interpolate the drive for each low level (for example 100 mW, 1 W, 5 W) on log(P) between neighbors.
3. Key each low level once to verify it (3 key-ups). Accept it within ±0.5 dB; otherwise, interpolate again with the
   new point.

### 3.4 Upper levels (3-5 key-ups)

1. Interpolate each upper level (25 W) between the factory rows on log(P), using the watts measured in 3.1, and verify
   it.
2. For the top level, key the top row + 100 and + 200 until the target is bracketed or the gain falls under 1 W per
   100 counts (saturation). Stop at the radio's rating.

### 3.5 Second frequency (B, about 12 key-ups)

1. Key the factory rows (4 key-ups).
2. Find Z and C by bisection from A's values (about 4).
3. Key each low level at A's drive (3).

A low level that misses its tolerance at B moves to the drive between A's and B's points that fits both, or up to the
next name that does (two key-ups per try). 1 W and above have always fit.

### 3.6 Hot check (about 12 key-ups)

1. Key three 5 s key-ups at about 25 W.
2. At once, find Z and C by bisection from the cold values.
3. Key the lowest level once.
4. Record the shift.

### 3.7 Margin and the lowest level

A low level is usable only when its drive lies at least `margin` above the highest cliff seen (A, B, hot), where
`margin = 2 × (largest Z shift seen) + 5` counts, and at least 20. A target below that becomes the lowest drive that
meets the margin, rounded to a level name it reaches (for example 0.25 W instead of 50 mW).

## 4. Short check (each further radio of a model)

The short check takes about 10 key-ups per band at frequency A, with the model's table in the firmware
(`power_cal.py check`). The HIL version (`test_fm_tx_power_levels --manual`) keys every level in one batch at 445 and
146 MHz by default, the middle of the 70 cm and 2 m amateur transmit ranges; you type the readings after it.

| Step | Key | Pass |
|---|---|---|
| 1 | Each power level once, through the normal level setting | Within ±1 dB of its name (±2 dB for levels under 1 W) |
| 2 | The lowest level's drive minus the model's margin plus 5 counts (5 counts above the model's highest cliff), once | Under 0.02 W. A reading near 10 W means this radio's cliff is higher: run 3.2 and 3.7 for it |

If a level is outside its tolerance, run the full procedure for that band on this radio.

## 5. What to record

| Where | What |
|---|---|
| The board's power table and its comment | Per band and frequency: the factory row drives and watts, Z, C, the hot shift, the margin, and the drive and measured watts per level |
| The board's power table | The low levels' fixed drives per band, the upper levels' place between the factory rows, the top level's offset, and Z per band for the ramp |
| The bring-up records | The short check's readings |

Don't record repeat readings of the same point: the procedure takes each point once.
