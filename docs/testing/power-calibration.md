<!-- SPDX-License-Identifier: MIT -->

# Transmit power calibration with a wattmeter

How to find the PA drive for each of a radio's power levels, quickly and to about ±0.5 dB. The full procedure runs
once per radio model; each further radio of that model gets the short check. The results go on the radio's hardware
page and into its board power table.

The procedure uses the debug link's PA drive override (RADIO op 36, and the `core` view's `tx_drive` for each level's
drive), the run-time transmit arm and the test TX limit ([debuglink.md](../spec/debuglink.md)). `tools/power_cal.py` does
the keying: it puts the radio in the VFO on the frequency (FM, no sub-audio tone, the TX limit on that frequency alone),
keys each step and asks for the reading.

| Command | Use |
|---|---|
| `power_cal.py --allow-rf MHZ key STEP...` | Key each drive (or `Ln`, power level n; `DRIVE@Ln`, a drive with level n's other settings), 2 s each (`STEP:S` for S seconds) |
| `power_cal.py --allow-rf MHZ zero --hi D --lo D` | The bisection for the zero point and the cliff (3.2) |
| `power_cal.py --allow-rf MHZ check --margin M` | The short check (4): every level, then the margin step; PASS or FAIL |

`--no-prompt` keys without asking, for an operator who reads the meter and reports the readings afterwards (`check
--readings W,W,...` then judges them).

## 1. What the measurements showed

These facts set the procedure. Check them again only on a new model.

| Fact | Consequence |
|---|---|
| Labels on the factory calibration rows can be wrong by 3 dB or more (a "5 W" row gave 9-10 W) | Measure every factory row once; never trust the label |
| Power against drive is steep at the bottom and saturates at the top | Bisect at the bottom, use coarse steps at the top |
| The bottom of the curve follows the **absolute** drive: the drive where output reaches zero (the zero point, Z) moved 0-10 counts between two frequencies 5-6 MHz apart in the same band | The low levels are fixed drives per band, not fractions of a factory row (a fraction put 1 W at 1.9 W). At the very bottom 10 counts is 2-3 dB, so the lowest level's name must fit both frequencies |
| The top of the curve moves with frequency (the 25 W and top drives differed by up to 50 counts between the same two frequencies) | The upper levels follow the factory rows, which are calibrated at several frequencies |
| **The cliff:** 5-20 counts below the zero point, the power loop loses control and the PA jumps to about 10 W | A level is never set within the margin below; the lowest level's drive is checked against the cliff, not just the zero point |
| A drive of 0 is below the cliff | The PA must not be on with the drive at 0: the ramp starts and ends at the zero point (a carrier leak at key-up and a splash at key-down come from this) |
| A warm PA moved the zero point and the cliff by about 5 counts | One hot check per model; no hot checks per radio |
| With the drive changed after key-up, the meter first shows the starting level | The override sets the drive before key-up |
| A board may set more than the drive per level (a transmitter trim register by level and band): the same drive at another level's settings gave 34 W instead of about 45 W | Key each level's drives at that level (`DRIVE@Ln`); a plain drive uses the lowest level's settings |
| Not every PA has the cliff: on one model output fell smoothly to zero and stayed there down to drive 64 of 4095, cold and hot | No margin is needed there; the short check's dark step is any drive below the zero point at every frequency checked |

## 2. Equipment and setup

| Item | Requirement |
|---|---|
| Dummy load | Rated for the radio's top power continuously, at the band's frequencies |
| Wattmeter | In line between the radio and the load, averaging (not peak), resolution 0.01 W below 1 W |
| Supply | The radio's top-power current with margin (about 12 A for a 50 W radio at 13.8 V) |
| Frequencies | Per band: one near the middle (A) and one at least 5 MHz away (B, model runs only). Both must be in the test TX limit |
| Radio | Transmit armed; FM, no sub-audio tone (an unmodulated carrier reads steadiest) |

Each key-up lasts 2 s and is read at its end; 3 s between key-ups. 5 s key-ups only for the warm-up.

## 3. Full procedure (once per model, per band)

About 30 key-ups per band at frequency A, plus 12 at B and 12 hot.

### 3.1 Factory rows (4 key-ups)

Key each factory power row as the radio drives it. Record the drive and watts. A row whose power doesn't rise with its
drive stops the procedure: the factory calibration is unusable.

### 3.2 Zero point and cliff (about 8 key-ups)

Bisect between the lowest row's drive (on the curve) and half of it (below the cliff): `power_cal.py zero`. Each
reading is classified:

| Reading | Meaning |
|---|---|
| 0.02 W or more and under the lowest row's power | on the curve: the new upper bound |
| under 0.02 W | between the cliff and the zero point |
| about the lowest row's power or more, at a drive below it | past the cliff: the new lower bound |

Stop when Z (the highest drive reading under 0.02 W) and the cliff C (the highest drive past the cliff) are each known
to 5 counts.

### 3.3 Low curve (4-6 key-ups)

Key Z + 10, Z + 30, Z + 60 and Z + 120, then halfway to the lowest row if 5 W isn't bracketed. Interpolate the drive for
each low level (for example 100 mW, 1 W, 5 W) on log(P) between neighbors, then key each once to verify (3 key-ups). Accept within
±0.5 dB; else interpolate again with the new point.

### 3.4 Upper levels (3-5 key-ups)

Interpolate each upper level (25 W) between the factory rows on log(P), using the watts measured in 3.1, and verify.
For the top level, key the top row + 100 and + 200 until the target is bracketed or the gain falls under 1 W per 100
counts (saturation). Stop at the radio's rating.

### 3.5 Second frequency (B, about 12 key-ups)

The factory rows (4), Z and C by bisection from A's values (about 4), and each low level at A's drive (3). A low level
that misses its tolerance at B moves to the drive between A's and B's points that fits both, or up to the next name
that does (two key-ups per try); 1 W and above have always fit.

### 3.6 Hot check (about 12 key-ups)

Three 5 s key-ups at about 25 W, then at once Z and C by bisection from the cold values, and the lowest level once.
Record the shift.

### 3.7 Margin and the lowest level

A low level is usable only when its drive lies at least `margin` above the highest cliff seen (A, B, hot), with
`margin = 2 × (largest Z shift seen) + 5` counts, and at least 20. A target below that becomes the lowest drive that
meets the margin, rounded to a level name it reaches (for example 0.25 W instead of 50 mW).

## 4. Short check (each further radio of a model)

About 10 key-ups per band at frequency A, with the model's table in the firmware (`power_cal.py check`):

1. Each power level once through the normal level setting: within ±1 dB of its name (±2 dB for levels under 1 W).
2. The lowest level's drive minus the model's margin plus 5 counts (5 counts above the model's highest cliff), once:
   it must read under 0.02 W. A reading near 10 W means this radio's cliff is higher: run 3.2 and 3.7 for it.

A level outside its tolerance: run the full procedure for that band on this radio.

## 5. What to record

| Where | What |
|---|---|
| The radio's hardware page | Per band and frequency: factory row drives and watts, Z, C, the hot shift, the margin, the drive and measured watts per level |
| The board's power table | The low levels' fixed drives per band, the upper levels' place between the factory rows, the top level's offset, Z per band for the ramp |
| The bring-up records | The short check's readings |

Don't record repeat readings of the same point: the procedure takes each point once.
