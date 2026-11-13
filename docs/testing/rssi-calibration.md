<!-- SPDX-License-Identifier: MIT -->

# RSSI check against the SDR bench

How to check a radio's RSSI reading with the SDR bench: it should rise 1 dB for each dB of signal, from its floor to
the strongest level the bench sends, and in DMR the radio should decode down to a known level. `tools/bench/rssi_cal.py`
steps a bench transmitter down in level, reads the radio over the debug link at each step and fits a line. It
measures and reports; it changes no setting on the radio.

## 1. What it needs

| Item | Requirement |
|---|---|
| Radio | On its debug link, cabled to the SDR bench (or through a dummy load and coupler); the radio's lock |
| Transmitter | A bench gateway whose transmit jobs take `power_dbm` and turn its gain down for it (an SDR's), so the steps are a dB scale. A board that sends only at its 0-255 setting (`--levels`) gives the radio's readings without a fit |
| Frequency | One the radio and the bench may use, leased. FM tunes the radio's VFO to it; DMR needs a codeplug channel on it with the job's color code and its talk group in the receive list |
| Time | About 5 s a step: a 0 to -100 dB sweep in 2 dB steps takes about 5 minutes in FM, a little more in DMR |

## 2. Running it

On the host the radio is cabled to:

```
tools/bench/rssi_cal.py --radio-port PORT --bench-api BENCH --freq MHZ --mode fm --dbm 0:-100:-2 --json fm.json
tools/bench/rssi_cal.py --radio-port PORT --bench-api BENCH --freq MHZ --mode dmr --cc 1 --tg 202 --dbm 0:-80:-2 \
    --json dmr.json
```

| Option | Use |
|---|---|
| `--dbm A:B:S` | The steps, as each job's `power_dbm` (only the differences matter) |
| `--levels A:B:S` | The board's 0-255 setting instead: readings only, no fit |
| `--mode fm` / `dmr` | FM: a carrier; DMR: a voice call of `--superframes` (5) to `--tg` on `--cc`, decoded bursts counted |
| `--seconds` | How long each FM step lasts (2.5 s; the first fifth of the readings is skipped) |
| `--channel` | Leave the radio on its current channel instead of tuning it |
| `--ref-dbm D` | The level at the radio's input at the top step, from a cabled source or a measured path: gives the radio's error |
| `--sensitivity-dbm D` | DMR: the radio's rated sensitivity, taken as the level of the dropout: an absolute anchor to a few dB |

The radio's zone, channel or VFO are put back after the sweep.

## 3. Reading the result

The first line is the floor: the radio's reading with nothing sent. Then one line per step (the bench's dB, the
radio's dBm, the raw RSSI and noise from the diagnostics line where the board has one, the DMR decode rate) and the
result:

| Field | Meaning | Good |
|---|---|---|
| `slope` | Radio dB per bench dB, fitted over the steps at least 6 dB over the floor | 0.95 to 1.05 |
| `worst_residual_db` | The largest step's distance from that line | Under 1 dB |
| `compressed_levels` | Top steps 1.5 dB or more under the line | None |
| `span_db` | The bench range fitted | The more the better: 40 dB or more |
| `offset_db` | Radio dBm less the bench's dB (the median) | Only a radio's error once anchored (`--ref-dbm`, `--sensitivity-dbm`) |
| `dropout_ref_db` | DMR: where the decode rate crosses 95 %, interpolated | Steady from run to run |

Without an anchor the sweep checks the shape (slope, linearity, floor), not the absolute level.

## 4. AT-D578UV

An SDR on the bench, 461.0875 MHz, 2 dB steps:

| | FM | DMR |
|---|---|---|
| Slope | 0.99 | 0.975 |
| Worst residual | 0.7 dB | 0.6 dB |
| Range fitted | 54 dB, down to -107 dBm read | 50 dB, down to -111 dBm read |
| Compression | none | none |
| Floor | -117 dBm | -119 dBm |
| Offset | -57 dB | -63 dB |
| Dropout | (the squelch closed at -107 dBm read) | 100 % decoded down to -111 dBm read, none 2 dB lower |

- The raw QA4818 RSSI moves 2 counts per dB, from 205 at the top step to the floor at about 86.
- DMR reads 6 dB under FM at the same level. Both modes read the same register; another bench board showed the same
  6-8 dB. Step to step the DMR raw value varies by up to 60 counts at the top levels, where FM varies by none,
  as if the reading were taken across the gaps between the radio's slot bursts.
