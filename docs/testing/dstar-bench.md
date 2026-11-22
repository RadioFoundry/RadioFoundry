<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# D-STAR: the first radio session (DM-32UV)

The plan for the first time D-STAR runs on a radio: what to set up, the order of the tests, what each settles, and
what to write down. Everything below is written and host tested ([dstar-modem.md](../spec/dstar-modem.md),
[dstar-codec.md](../spec/dstar-codec.md), [dstar-codeplug.md](../spec/dstar-codeplug.md)); nothing of it has been on
air. The open questions this session settles are listed in [dstar-modem.md](../spec/dstar-modem.md) 1.4 and in
[hardware-dm32.md](../spec/hardware-dm32.md) 9.3, "Unverified on the radio"; each step names the ones it answers.

Order matters: receive first (nothing of ours on air), then the transmit path into a dummy load with the SDR, then our
over decoded by another radio, then a gateway. A step that fails stops the ones after it: the fixes are in the step.

Status words as in `docs/spec/gotchas.md`. Every command here is `tools/dstar_bench.py` (its `--help`) or
`tools/rfoundry_debug.py dstar` ([debuglink.md](../spec/debuglink.md) 3.31); none of them keys without `--allow-rf`.

## 1. Kit and set-up

| Part | What |
|---|---|
| Radio under test | DM-32UV, `make BOARD=dm32 VARIANT=multimode` (D-STAR is on only there: [build-config.md](../build-config.md), Variants), flashed by the owner; programming cable for the debug link |
| RF | Dummy load with a tap or a small pick-up antenna for the SDR; lowest power level; a simplex DV frequency your licence and band plan allow, the same on every radio (`--mhz`) |
| SDR | The shack's RTL-SDR (`rtl_sdr` on the path, or `--capture` with your receiver's command), tuned 25 kHz below the channel by default (off its DC spike); its ppm known ([sdr_fm_check.py](../../tools/sdr_fm_check.py)) |
| D-STAR radios | Icom ID-51 or IC-705 (step 2), IC-9700 (step 4); each with MY set, UR CQCQCQ, simplex (DUP off), a TX message set for step 2b |
| Gateway (step 5, optional) | A D-STAR hotspot (MMDVM with a D-STAR gateway) or a repeater in range, linked to a reflector that has an echo |

Before the first step:

1. On the radio, MY: `#` on Home opens the D-STAR page only on a D-STAR channel, so set it over the debug link first:
   `tools/rfoundry_debug.py dstar station my <CALL>` (and `station msg <text>`), or on the radio once on the channel
   (`#`, `8`).
2. A D-STAR channel: in the Codeplug Editor, a new channel of mode D-STAR on the test frequency, RPT1 and RPT2 blank
   (direct), UR CQCQCQ, transmit allowed; written to the radio. Select it.
3. `tools/rfoundry_debug.py dstar view`: `dstar.on=1`, `dstar.fault=ok`, `dstar.voice=ok` (the vocoder lent its RAM),
   `dstar.tx_my` your call, `dstar.knob.*` the board's defaults. `dstar.fault=no_cal` means no deviation scale: the
   board's guess is 1250, so it shouldn't appear.
4. Save the starting state: `tools/rfoundry_debug.py dstar view > start.txt` and `tools/rfoundry_debug.py version`.

## 2. Receive from an Icom radio

No transmitter of ours. Settles: the receive path (0x2F, 0x2B = 0x0700, FM_BANDWIDTH b6, the RX RAM copy with FM
playback off), the receive polarity, the header CRC's preset and inversion ([dstar-modem.md](../spec/dstar-modem.md)
3.1), slow data, voice.

```
tools/dstar_bench.py watch --seconds 300          # a line per over heard; log in $TMPDIR/dstar-bench/watch.jsonl
```

| Run | The Icom radio sends | Expect |
|---|---|---|
| 2a | 3 s overs, no message, a metre away into its own rubber duck (strong) | A line per over: MY/suffix, UR CQCQCQ, RPT1/RPT2 as it sends them (DIRECT or blank), `polarity` +1 or -1, frames about 150, bad 0. The caller on the screen while it talks, the voice on the speaker |
| 2b | The same with its TX message set | The message on the call screen and in the line (`msg`), from the second superframe on |
| 2c | A 10 s over, the watch started 3 s into it (or the radio switched to the channel mid-over) | Late entry: the header rebuilt from the slow data (`(late entry)`), voice from the second resync on |
| 2d | Weaker: further away, or the dummy load and attenuation | Note where `bad` starts to climb and where the voice breaks up. A break should sound like a short hiss fading out, not a buzz and a click (the codec's concealment, dstar-codec.md 1); note `rx_concealed` against `rx_played` |
| 2e | `pytest tests/hil/test_dstar.py --dstar-call <ITS CALL>` | Passes: a header from the air (CRC good), 100+ frames, few bad |

| Result | Means, and what to do |
|---|---|
| Headers from the air (`heard_slow` 0) | The CRC preset 0xFFFF and the final inversion are right: note it in dstar-modem.md 3.1 |
| Headers only from the slow data (always `late entry`) | The coded header decodes but its CRC fails: the CRC's preset or inversion is wrong. Record `dstar.hdr_corrected` (low = the bits came through, the CRC rule is the problem) |
| `polarity` -1 on every over | The receive path inverts: `tools/dstar_bench.py cal rx_invert 1` (saves the deframer the search; it works either way) |
| Nothing heard, `dstar.rx_samples` climbing | Audio arrives but no sync: listen for hiss (FM playback off makes the speaker quiet between overs); try `rfoundry_debug.py dstar knob rf_rx_2b 0` (no bypass) to see whether 0x2B is the cause; record `dstar.knob.rx_halves` |
| `dstar.rx_samples` not moving | The RX RAM copy stopped with FM playback off: `dstar knob fm_play 1` and repeat 2a (the speaker then plays FM too) |
| FM hiss on the speaker between overs | FM leaks to the line out with 0x2F = 0 and b6 off: record it (hardware-dm32.md 9.3) |
| Voice garbled, frames good | The codec, not the modem: record `rx_played`, `rx_concealed`; a few seconds recorded on a phone for the owner only (never committed) |

## 3. The transmit path on the SDR

Into the dummy load, lowest power. Settles: whether the TX RAM reaches the modulator, which half to fill, the
frequency response (a high-pass to restore), the deviation scale (`tx_1khz`), whether FM_DEV_COEF limits this route.

```
tools/dstar_bench.py txpath --mhz <F> --allow-rf          # about a minute; record txpath.json
```

It keys twelve tones (100 Hz to 3.6 kHz at half the board's 1 kHz scale), a 100 Hz square wave and the GMSK preamble
(1010...) in turn, captures each, and prints the deviation of each tone, the first-order high-pass fitted to them,
the high-pass the square wave's droop gives, and the commands to set:

```
tools/dstar_bench.py cal tx_1khz <N>        # the sample value for 1 kHz
tools/dstar_bench.py cal tx_hpf <fc>        # only if fc is over about 30 Hz
```

Run it again after setting both: the tones within 1 dB of 500 Hz from 300 Hz to 3 kHz, and the preamble's peak about
1.06 kHz (a 1010 run peaks at 0.88 of the +-1.2 kHz a long run reaches, dstar-modem.md 2.2).

| Result | Means, and what to do |
|---|---|
| A carrier, no tone at all | The TX RAM doesn't feed the modulator, or the wrong half is filled: `rfoundry_debug.py dstar knob tx_half 1`, repeat; `dstar.knob.tx_misses` counting means the interrupt named neither half. If neither half works the route doesn't work on this radio: stop here and record it |
| Tones choppy at 32 ms | The half order: `tx_half` the other way |
| Rising response at the top | Pre-emphasis still on: `dstar knob tx_bw_clr 0x50` (both bits) and check `rf_tx_2b` 0xC007 |
| Deviation stops growing between the 50 % and a 100 % run | The limiter acts on this route: `dstar knob dev_coef <value>` (FM_DEV_COEF with a higher limiter, modem-hrc7000.md 8.1), repeat |
| fc from the tones and from the square differ by more than 30 % | Not first order: record both, set the tones' figure |

The same per tone by hand: `rfoundry_debug.py dstar test tone 1000 50`, `rfoundry_debug.py ptt --seconds 2`, the
capture through `tools/dstar_bench.py analyze CAPTURE.cs8 --kind tone --hz 1000 --offset 25000`;
`dstar test off` afterwards.

## 4. Our over: the SDR, then an IC-9700

Settles: the transmit polarity, the slow data schedule an Icom radio expects (dstar-modem.md 3.5), turnaround, voice
through the encoder.

```
tools/dstar_bench.py over --mhz <F> --seconds 5 --allow-rf      # talk into the radio's microphone while it keys
```

It captures an ordinary over: the frame sync found in the capture says which polarity went out (`found inverted`:
`tools/dstar_bench.py cal tx_invert 1`), the deviation should peak near 1.2 kHz, and the radio's counters (frames
encoded, underruns, overflows, the microphone's peak) are recorded. `tx_peak` near 0 means the microphone doesn't
reach the mic PCM in FM work mode (hardware-dm32.md 9.3): stop and record it.

Then with the IC-9700 on the frequency (its DV mode, simplex):

| Run | Do | Expect on the IC-9700 |
|---|---|---|
| 4a | PTT 5 s, talking | Our MY (and suffix) as the caller; the voice understandable |
| 4b | A message set (`dstar station msg TEST 123`), PTT 10 s | The message shown |
| 4c | 4a with `tx_invert` the other way | Nothing decoded: confirms the setting from the SDR |
| 4d | Short overs, 1 s | The end pattern seen (the IC-9700's squelch closes at once, no tail of noise): the board's 64 ms latency covers the buffers. A tail cut short or a long noise tail: record the end time on the SDR capture against the PTT release |
| 4e | The IC-9700 sends to us | As step 2: a second radio's view of our receiver |

Rate the voice each way 1-5 and note what is wrong (level, pitch, breaks). Voice is the codec's: its known limits are in
dstar-codec.md.

## 5. The echo reflector, through a hotspot or a repeater

Only once steps 2 and 4 pass. A channel for the hotspot or repeater: RPT1 its callsign with the module letter in the
eighth place, RPT2 its callsign with G, via a repeater on, UR CQCQCQ (the Codeplug Editor).

| Run | On the radio | Expect |
|---|---|---|
| 5a | `#` `4` (Echo test), PTT 5 s, release | Home said "Echo test next over"; our 5 s played back after the release, the header showing the echo's callsign |
| 5b | `#` `5`, type the reflector and module (`REF030C`), `OK`, PTT 1 s | The gateway's link announcement; `#` `7` (Gateway info) then PTT 1 s says what is linked |
| 5c | PTT with UR CQCQCQ | Heard through the reflector (a station there, or its dashboard) |
| 5d | `#` `6` (Unlink), PTT 1 s | The unlink announcement |

The gateway needs MY registered where its network requires it; a refusal shows as no answer at all.

## 6. What to record each run

One directory per session (the scripts write into `$TMPDIR/dstar-bench`; copy it), and a short note per step with:

| Item | Where from |
|---|---|
| Date, firmware commit, build (`VARIANT=multimode`) | `rfoundry_debug.py version` |
| The radio's RF set-up (A or B) and board revision | hardware-dm32.md 8.10; Settings > Advanced > Hardware |
| Frequency, power level, attenuation, SDR and its ppm | by hand |
| The other radio: model, firmware, its settings (MY, UR, RPT1/2, message, DUP) | by hand |
| The dstar view before and after | `rfoundry_debug.py dstar view` (calibration in force, the board's knobs, every counter) |
| Each over heard | `watch.jsonl` (step 2) |
| The transmit path | `txpath.json`, its captures (I/Q of tones, never speech) |
| Our overs | `over.json` |
| The calibration settled | `cal` commands run, and the view after |
| Voice quality, each way | a 1-5 rating and a sentence |
| What failed and the knob that changed it | the step's table |

The captures of tones and the GMSK preamble may be kept with the results; recordings of speech are never committed
(AGENTS.md). Once the values hold, they become the board's defaults in `boards/dm32/flat_dm32.c`, and every item of
hardware-dm32.md 9.3's table that the session settled is marked measured, with the date.
