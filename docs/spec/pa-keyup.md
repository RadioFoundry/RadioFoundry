<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# DMR PA key-up

A radio that keys its PA for every DMR burst has to reach full power by the burst's first symbol. A PA power loop
comes up slowly from a small drive: at a low power step it can take 3-4 ms, while the top step takes about 1 ms. A
fixed lead long enough for the low steps keys the top step early and cuts into the receive time before our slot, which
an SFR ([sfr.md](sfr.md)) needs. So the key-up drives the PA control harder for a moment first, for a time that
depends on the step, and takes every value from the radio's own power calibration table.

Code: `core/radio/pa_keyup.h`. A radio's hardware page says whether it keys this way and with which constants.

## 1. How the loop behaves

A PA power loop takes its reference from the drive through a low-pass, and gives no output until that reference has
added up to a fixed amount. Two things follow, each seen in key-up captures:

| What | Effect |
|---|---|
| A dead zone | a drive under a certain value does nothing to the loop |
| A knee | no output until the drive above the dead zone has added up to a fixed amount (drive counts × time). A low step's own drive takes milliseconds to get there, a high drive a fraction of one |

So a drive `b`, written with the output off, brings the output to a step's power after

```
T(d, b) = (knee × (top − dead) + slope × (d − dead)) / (b − dead)
```

`d` is the step's drive from the power cal table at the transmit frequency, `top` the highest step's drive there and
`dead` the dead zone's edge. `knee` is the time the knee takes at the top drive, `slope` the extra time a step needs
at its own drive. At `b = d` the top step is up after `knee + slope`; a low step takes far longer.

## 2. The sequence

All times are before the burst's first symbol.

| Case | PA drive |
|---|---|
| T(d, d) ≤ `window_us` | d, T(d, d) before the first symbol. Nothing else |
| otherwise | a boost b from `window_us`, then d from `settle_us`. b is the lowest drive that fits: T(d, b) = `window_us` − `settle_us` |

The PA is switched on at `pa_us`, or with the first drive when that comes later.

A loop that comes up fast runs past its power, most at the low steps, where the PA's output is steepest against
the loop's control. `ease_pm` sets the fastest approach, the drive a = dead + `ease_pm` × `ref` / 1000:

| Case | PA drive |
|---|---|
| d ≤ a | as above; a boost stops at a |
| d > a | a from T(a, a) + s before the first symbol, then d from s, with s = `slope_us` × (d − a) / (d − dead) |

So a step over the approach drive comes up as a step of that drive would, regulates there, and takes its own drive
for the time the law gives it from a's power to its own. It starts ahead of the window when it has to: the window
is the boosts', and the board's earliest point bounds it. The carrier is on the air at a's power during s.

A step that is fast by itself is keyed late and leaves its time to the receiver. A slow one gets only the boost it
needs: a short high boost overshoots with the timer's jitter, a long low one doesn't. No boost starts earlier than
the window. The boost is never over `PA_KEYUP_BOOST_MAX_PCT` (120 %) of the top step's drive or the board's drive
limit; a step that needs more comes up late, and the window is too short for it.

| Constant | What it sets |
|---|---|
| `dead_pm` | the dead zone's edge, in 1/1000 of `ref` |
| `knee_us` | the time the top step's drive takes to bring the loop to the PA's knee |
| `slope_us` | the time a step's own drive takes from there to the step |
| `window_us` | the earliest a key-up starts. A step slower than this is boosted |
| `settle_us` | a boost ends this long before the first symbol (under `window_us`) |
| `pa_us` | the PA on this long before, or with the first drive when that is later |
| `ease_pm` | the fastest approach: the drive over the dead zone a key-up comes up on, in 1/1000 of `ref` (0: no limit) |

Seven 16-bit values per band with its own PA. None is a drive count. `ref` is a drive the radio's own power
calibration gives at the transmit frequency: its lowest step's drive, or the drive where its table says the output
reaches zero (0: no dead zone). `top` and `d` come from the same table. So one set of constants serves every radio of
a design: what differs from radio to radio is in its calibration table, and nothing is stored per step or per
frequency.

A board that keys its PA with the drive at one fixed time uses `PA_KEYUP_FIXED(us)`: no knee, every step `us` ahead.

Integer math, 32-bit, one or two divides per key-up:

```
dead = ref * dead_pm / 1000
q    = knee_us * (top - dead) + slope_us * (d - dead)
lead = q / (d - dead)                              // d in the dead zone: window_us
b    = dead + ceil(q / (window_us - settle_us))    // only when lead > window_us
ease = ref * ease_pm / 1000                        // d - dead > ease: the eased key-up
s    = slope_us * (d - dead - ease) / (d - dead)
t    = (knee_us * (top - dead) + slope_us * ease) / ease + s
```

## 3. What a board does

A board with per-burst keying opts in; one that doesn't call it keys its PA as before.

| Step | Call |
|---|---|
| Start-up | `pa_keyup_init(defaults, max_ahead_us, max_drive)`: its constants, the earliest point its burst sequence can run (after the burst task's wake and any switching that has to go first) and its drive limit |
| Key-up (radio task), a board whose bands have their own PA | `pa_keyup_select(constants)` with the transmit band's, before the plan |
| Key-up (radio task) | `pa_keyup_plan(d, top, ref)` with its cal table's drives at the transmit frequency; keep the result for the bursts |
| Key-up (radio task) | `pa_keyup_points(&plan, pts)`: the points in time order; `pa_keyup_insert(pts, n, us, kind)` adds a board's own point (the RF switch to transmit, a supply) at its time, ahead of the points at that time, with a kind from `PA_KEYUP_PT_BOARD` on |
| Each burst | the points in time order, by the board's burst task or its timer |

A board that times the points with a hardware timer from a reference event a fixed time before the first symbol (an
RF TX interrupt, a baseband's burst line) takes them as steps:

| Where | Call |
|---|---|
| Key-up (radio task) | `pa_keyup_steps(pts, n, ref_us, min_us, steps)`: the points at one time in one step, the first step no earlier than `min_us` (the timer's shortest wait) after the reference |
| The reference | the timer to the first step, `ref_us` minus its time |
| The timer's handler | the step's points in order, then `pa_keyup_step_wait(base, next, elapsed, min_us)`: the wait to the next step from the timer's last update (`base`), or 0 when the next step is due within `min_us` of the count so far, and goes at once from the same base |

Only points at one time share a step. Whether a later point goes with a step depends on when the handler runs, so the
handler decides from its timer's count, and a late handler never takes a point early. A timer restarted at each step
(a one-shot) waits from step to step and doesn't need the check.

The RF switch has its own lead on the PA's point. A switch that holds the burst task for longer (a synthesizer retune
on a split channel) goes before the boost, so it can't delay the boost's points. On a simplex channel, the later the
switch, the more of the inbound burst before ours the radio still hears.

## 4. Debug link

The constants are RAM: RADIO op 37 ([debuglink.md](debuglink.md)) changes them, from the next key-up, and a restart
restores the board's. Constants set this way apply on every band until `default` gives the board's back. RADIO op 36
sets a drive that is not in the table, for one transmission; the key-up is planned for that drive.

| `core` view key | Values |
|---|---|
| `pa_keyup` | the seven constants, in the table's order |
| `pa_keyup.limits` | the board's earliest point (µs) and its drive limit |
| `pa_keyup.last` | the last key-up: the first drive (a boost, or under the step's drive the approach drive; 0: none), drive, `boost_us`, `pa_us`, `drive_us`, then the `top` and `ref` it was planned against |

```
rfoundry_debug.py radio pa-keyup 900 120 250 1500 300 400        # six values: no fastest approach
rfoundry_debug.py radio pa-keyup 900 120 250 1500 300 400 350    # with ease_pm
rfoundry_debug.py radio pa-keyup default
```

RADIO op 40, the key run, keys the radio by itself: one request carries the key and gap lengths, optionally the seven
constants (set first, as op 37), and up to 21 drives (28 without the constants; a request is 64 bytes). Per drive the
radio sets it (as op 36), holds PTT for the key (its time-out just past it), waits for the key-down and the gap, then
takes the next. The host link is held until the run ends and nothing is sent while the radio transmits, so a radio
whose own RF upsets its USB link (VHF near the cable) still runs every key in time. Nothing is kept in RAM: the drives
are read from the request as the keys run. The `features` view's `pa_keyrun.last` is the keys run and the keys asked. A key counts only when the radio transmitted during it: a channel it can't transmit on (receive only, past the test TX limit) stops the run at the first key, with none run.
After the reply, flag bit 1 has the host port leave the bus and come back (`hal_stream_t.reattach`: a USB port stops
its stack, resets the controller and starts again, every 4 s for about a minute until a host takes it). Flag bit 2
restarts the radio instead. A restart loses the RAM settings: the constants, the test TX limit and `pa_keyrun.last`.
A host can give up on a radio's USB port while the radio transmits, and then ignore the port's return for a while.
A restart brings it back where a reattach doesn't. `--key-run` (`reattach`) or `--key-run restart` asks for one after
every run and opens the link again. After a restart it tunes the radio again and counts the keys in the capture. A
radio whose USB drops at every key is calibrated by one command.

```
rfoundry_debug.py radio pa-keyrun 300 350 517 800 1073           # three keys, the constants the radio has
```

## 5. Targets

Measured on a dummy load, per burst, against the first symbol found from the burst's sync. The timing figures come
from the gateway's ramp measurements; the shape from the burst's envelope (`trace` and `tail`, 0.1 ms envelope at
48 kS/s, dB from the burst's settled level), judged by `tools/bench/pa_shape.py`:

| Measure | Target | Fault |
|---|---|---|
| The −3 dB point | 0.10 to 0.45 ms before the first symbol | `early`, `late` |
| The −30 dB point of the rise | no earlier than 0.8 ms before the first symbol: the guard time is the receiver's too | `early` |
| The slot before | at most −45 dB (or 6 dB over the envelope's floor) | `pre-slot` |
| The rise from −40 to −6 dB | one rise: never back down 2 dB, never flat (under 1.5 dB per 0.1 ms for 0.12 ms) | `hook`, `knee` |
| The first 10 symbols (2.0 ms) | RMS 0.5 dB, every point within 1 dB, mean within 0.5 dB of the burst's level | `overshoot`, `sag`, `creep`, `unsettled` |
| The −6 dB point to within 1 dB | 0.4 ms at most, unless the first symbols are flat | `creep` |
| The highest point of the key-up | 1.5 dB over the burst's level | `peak` |
| The −3 dB point over a capture's bursts | spread (standard deviation) 50 us at most | `jitter` |
| The last 10 symbols | within 1 dB | `droop` |
| The key-down | one fall: not back up 6 dB once under −30 dB; nothing over −45 dB after the slot's end | `rebound`, `after-slot` |
| On the air (within 20 dB of the burst's level) | not before the slot's start, 1.25 ms before the first symbol; not after its end, 1.25 ms after the last symbol | `before-slot`, `after-slot` |

A DMR carrier's envelope is flat (4FSK), so whatever moves in the first symbols is the PA. Overshoot is their mean over
the burst's level; sag their mean under it; creep a level still climbing to it (its −6 dB point more than 0.4 ms from
within 1 dB, and the first symbols' trend upward); unsettled the rest. A level that comes up within 6 dB at once and
creeps the last dB (a PA held just over its turn-on edge) is a fault like the others.

`pa_shape.py RUN.json` prints each level's figures and faults from a saved run, and the envelope binned: ten 0.125 ms
bins of the guard time and one per symbol after, in dB:

    445 L0 after  floor -60 pre -60 | -30 -0.45 -3 -0.32 ms rise 128 us hook 4.1 knee -32 dB @-0.85 | ...  hook knee
               guard -60 -60 -49 -32 -34 -35 -18  -3  -0  -0 | sym  -0  -0   0  -0   0   0   0   0  -0   0

Over the air the floor is higher and the slot before is judged against it: a dummy-load path shows what the air hides.

Each band that has its own PA has its own constants: the knee of one band's PA can be several times another's on the
same radio.

`window_us` and `pa_us` are the board's choices, not measurements. On a simplex channel the board's switch to TX
before the PA must come after the inbound burst before ours, or an SFR repeater misses that burst: keep the window
plus the board's switch time under that.

## 6. Calibration

`tools/bench/pa_cal.py` measures a band's constants in one run. It needs a radio whose firmware keys by this rule
(`pa_keyup` in the `core` view) and a bench gateway whose bench line (TCP 7373) lists `bursts_raw` and `bursts_ramp`
in its `info`. The gateway measures every burst's ramp itself; the tool keys the radio over the debug link and fits
the constants. A gateway that also lists `bursts_trace` sends each burst's envelope, which the plots need.

### Setup

| What | How |
|---|---|
| Load | The radio into a dummy load rated for its top power |
| Tap | Fixed pads to the gateway's SDR. Pads so the top level gives at most −13 dBm at the SDR |
| SDR gain | Manual, `--gain` (default 0 dB) |
| Frequency | One the radio may key into the load. The tool claims it on the gateway for the run |
| Over the air | An antenna on the SDR instead of the pads works for the times: give `--pad-db` the path's rough loss and lower `--gain` until the input doesn't clip. The printed powers then mean nothing |

Pads needed: top power in dBm + 13. A 5 W radio (37 dBm) needs 50 dB, a 50 W radio 60 dB.

### Run

```
tools/bench/pa_cal.py --radio PORT --bench HOST --freq 446.05,146.535 --pad-db 57 --max-w 5 --allow-rf
```

| Option | Value |
|---|---|
| `--radio` | The radio's debug link port |
| `--bench` | The bench gateway, `HOST[:PORT]` |
| `--freq` | MHz, one per band with its own PA |
| `--pad-db` | The pads' total |
| `--max-w` | The highest level's output, W. Checked against `--max-in-dbm` (−13) before any key |
| `--allow-rf` | Required: the radio transmits |
| `--levels` | Optional: the power levels to use, `0,1,2`. Leave out a level that is over the SDR's limit |
| `--max-pct` | Optional: the highest open-loop drive, % of the top level's |
| `--channel` | Optional: key on the channel the radio is on, for a radio without a VFO. It must be a DMR channel that transmits on `--freq`; `--cc` is its color code |
| `--rx-port` | Optional: the SDR's receive port the pads feed, on a gateway that lists `rx_port` |
| `--window-us`, `--settle-us`, `--pa-us` | Optional: the board's values are kept otherwise |
| `--arm`, `--disarm` | Optional: debug link commands a radio needs before and after keying |
| `--out` | Optional: a JSON file with the result and every key's measurement |
| `--plot` | Optional: a directory for the run's plots (below). Needs matplotlib; the tool itself doesn't |
| `--before` | Optional: six constants the plots compare with, instead of the board's |
| `--refit` | Optional: measure new constants even when the board's hold |
| `--rounds` | Optional: verify rounds that may change the constants (4) |
| `--batch` | Optional: keys per capture (6). A step's keys share one capture; 0 or 1 takes a capture for each |
| `--key-s` | Optional: each key's length in seconds (0.5, about 8 bursts) |
| `--key-run` | Optional: a capture's keys go to the radio as one key run (op 40, section 4), the debug link quiet while it transmits. For a radio whose USB link drops while it transmits: a link lost during a run is waited out and opened again, and the run is checked whole |

### What it does

Every key is a DMR transmission of half a second (`--key-s`). A step's keys share one capture, up to `--batch` (6)
of them: the capture's start costs more than a key. A capture that doesn't show every key is keyed again one by one.
Per band:

| Step | Keys | What |
|---|---|---|
| Check | 1 | The lowest level with the board's constants: the path, the SDR's input level, `top` and `ref` |
| As is | one per level | Every level with the board's constants. If all are in time the constants are kept and the run ends (`--refit` goes on): a board may time its key-up by more than the constants say, and a radio that is right needs no new ones |
| Open loop | 6 | The drive alone from the window's start, at the levels' drives and some between (RADIO op 36), never over `top`. Each gives T(d, d) and the overshoot that drive has by itself. Levels that share one drive: drives down to 60 % of it |
| Limit | 1 | The top drive from the board's earliest point. It has to come up that much earlier; if it doesn't, the board's burst sequence can't run a point that early, and the tool says how far it reaches |
| Fit | 0 | `dead_pm`, `knee_us` and `slope_us`: a line through 1 / (d − dead), the dead zone searched. Times that don't fall with the drive give no knee: one lead for every level |
| Ease | 0 | A level whose drive alone ran more than 2.5 dB past its power came up too fast: `ease_pm` starts at 80 % of the lowest such level's drive over the dead zone, and an eased level that still overshoots in a verify round takes it down to 85 % |
| Edge | 0 | A level whose own time moves more than 100 us per 1 % of drive sits on the PA's turn-on edge: its time doesn't hold from key to key, so it is boosted whatever the fit says |
| Verify | one per level, up to four rounds | Every level as the radio keys it. Without slow levels: each level's error goes back into the knee and slope. A level on the edge, or slow on its own drive and not right as the law keys it, has the boost it needs read from its own measurements, on the law's 1 / (b − dead) scale and no further than twice the last step past the drives tried. The constants are then solved over all levels (least squares per dead zone; the smallest worst error, then the smallest sum) to give those boosts and keep the other levels on the law's line. A boost that overshoots while on time ends earlier: `settle_us` up 0.1 ms, and as much again per 3 dB over the limit. The round that held best stands; a round ends the run when every level is up at least 60 us early with half a dB of overshoot to spare |
| Confirm | one per level | The result again: it has to pass again, a level on its own drive within 0.1 ms |

Every level's shape is judged as in 5, boosted or not: a boost that brings a level up early, or over its level for
the first symbols, fails like any other. No level may be on the air before the slot's start (`before-slot`). A key-down still on the air after the slot's end (`after-slot`), or more than
3 dB down at the last symbol's end (`cut-short`), fails the band and starts no refit: no key-up constant moves it. An overshoot a drive has when written alone
counts: `ease_pm` is its cure. Only on a firmware without `ease_pm` (a view with six constants) is it noted and not
counted.

It stops before the next key when the SDR's input clips or is over `--max-in-dbm`. It always restores the board's
constants, the drive and the lowest level at the end. Nothing is written to the radio.

### Result

```
  PASS 446.05 MHz
    level 0 (1.0 W): -3 dB -241 us, over 0.5 dB  OK
    { .dead_pm = 733, .knee_us = 250, .slope_us = 280, .window_us = 1500, .settle_us = 300, .pa_us = 400, .ease_pm = 0 }
    rfoundry_debug.py radio pa-keyup 733 250 280 1500 300 400 0
```

1. `PASS`: put the initializer in the board's key-up constants for that band. `CHECK`: the level's line names the
   target it missed (`late`, `early`, `overshoot`, `sym1`, or `window`: the level is at the boost's ceiling and
   needs a longer window).
2. Check a second radio of the design with the same constants (`pa-keyup` on the debug link, then a verify run).

Levels print as their measured output (W, mW) when the gateway knows its SDR's level; `--dbfs-to-dbm` sets it
against a wattmeter.

### Plots

`--plot DIR` draws two files per band; `tools/bench/pa_cal_plot.py RESULT.json DIR [NAME]` draws a saved `--out` again.
The titles name the radio: the board and build the radio reported, or NAME.

| File | Shows |
|---|---|
| `pa_cal_<MHz>_ramps.png` | The burst's envelope at the lowest, a middle and the top level in one picture, a color each. When the run changed the constants, the board's are dashed (before); a run that kept them draws its one measurement. The slot's timing (ETSI TS 102 361-1) is shaded and named along the top: the neighbor slot, the slot boundary and the 1.25 ms guard time. Key-up: the slot before, the slot's start, the guard time, the key-up window, the planned boost, PA and drive points, the first symbol, the −3 dB target and the measured −3 dB points. Key-down: the last symbol's end and the slot's end |
| `pa_cal_<MHz>_fit.png` | Each open-loop drive's time with the fitted law, the window and the dead zone; each level's −3 dB point, with the board's (before) when the run changed the constants |

Without `bursts_trace` on the gateway only the measured points are drawn.
