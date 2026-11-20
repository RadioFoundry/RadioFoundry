<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Fusion: the first radio tests (DM-32UV)

The plan for the first time Yaesu System Fusion runs on a radio, in two parts: what can be proved with **no Fusion
radio** (an SDR to listen, an SDR to transmit), then a **short on-air session with a visiting Yaesu radio** that
settles only what a real Yaesu can. Everything is written and host tested ([ysf-modem.md](../spec/ysf-modem.md),
[ysf-codeplug.md](../spec/ysf-codeplug.md)); nothing of it has been on air. The questions are
[ysf-modem.md](../spec/ysf-modem.md) 1.5 and the flat paths' list in [hardware-dm32.md](../spec/hardware-dm32.md) 9.3;
each step names the ones it answers.

Every command is `tools/ysf_bench.py` (its `--help`) or `tools/ysf_ref.py`. Nothing keys without `--allow-rf`. Each
run writes a JSON record in `$TMPDIR/ysf-bench` (or `--out`): send those and the captures back, not just a pass.

## 1. Kit and set-up

| Part | What |
|---|---|
| Radio under test | DM-32UV, `make BOARD=dm32 VARIANT=multimode` (Fusion is on only where the IP gateway is left out), flashed by the owner; programming cable for the debug link |
| RF | Dummy load with a tap or a small pick-up antenna; lowest power; a simplex frequency your licence and band plan allow for C4FM (`--mhz`) |
| Receiving SDR | The shack's RTL-SDR (`rtl_sdr` on the path, or `--capture` with another receiver's command), 25 kHz below the channel by default; its ppm known |
| Transmitting SDR (part 2) | A PlutoSDR or HackRF (an RTL-SDR only receives), behind at least 60 dB of attenuation into the radio's antenna socket (a fixed pad chain; never radiate) |
| Visiting radio (part 3) | Any Yaesu Fusion radio (FT-70D, FT-3D, FT-5D, FTM-300/400/500, FT-991A, FTM-200, ...) with its owner |

Before anything:

1. A Fusion channel on the test frequency in the Codeplug Editor (mode Fusion, destination blank = ALL, DG-IDs 0,
   transmit allowed) and the Fusion callsign on the Radio tab (or the radio name as the callsign). Write, select it.
2. `tools/ysf_bench.py view`: `ysf.on=1`, `ysf.fault=ok`, `ysf.codec=1` (the stock codec merged), `ysf.tx_src` your
   call, `ysf.no_call=0`. `fault=no_cal`: the board knows no deviation scale (its guess is 1250, so this shouldn't
   appear).
3. If the D-STAR bench session has run on this radio, its `tx_1khz`, polarity and high-pass findings are the flat
   paths' and hold for Fusion too: set them first (the same `set_flat_cal`).

## 2. Part one: no Fusion radio

### 2.1 Our transmission on the SDR (the transmit route, the levels, the polarity)

```
tools/ysf_bench.py over --mhz 438.625 --seconds 4 --allow-rf      # talk into the mic while it keys
```

The capture is decoded by `tools/ysf_ref.py`'s decoder, written apart from the radio's code. It prints the frames, the
callsign units, the inner and outer levels in Hz, their ratio, the polarity and the sync bit errors.

| Result | Means, and what to do |
|---|---|
| Headers 2 (or 1), terminator 1, every FICH good, CSD1 `ALL` from your call | The transmit route carries C4FM and our framing is right |
| Outer level not 2700 Hz | Set `tx_1khz` to the current value times the factor printed; repeat until within 5 % |
| Ratio not 3.00 (within 0.3) | The route clips (the FM limiter: try the `dev_coef` knob) or isn't flat (`rf_tx_2b`, `tx_bw_clr`); repeat |
| `inverted` | Set `tx_invert` (a Yaesu radio would decode nothing) |
| Only the second header decodes | The key-up eats the first: expected, that is why two are sent. Neither: lengthen nothing yet, note it |
| No frame at all | Is anything on air? `tools/sdr_fm_check.py CAPTURE --offset 25000` for the carrier and its deviation |

Record: the JSON (it has the frames), the radio's `tx_1khz` and knobs as finally set.

### 2.2 A synthesized signal into the radio (the receive path)

```
tools/ysf_bench.py synth s.cs8 --rate 2000000 --src G4TST --frames 50          # HackRF (cs8)
hackrf_transfer -t s.cs8 -f 438625000 -s 2000000 -x 0                           # through the attenuators
tools/ysf_bench.py synth s.c16 --rate 2400000 --format cs16 --src G4TST         # PlutoSDR: replay with its tools
tools/ysf_bench.py watch --seconds 60                                            # in another terminal, first
```

| Run | Signal | Expect |
|---|---|---|
| 2.2a | strong (attenuation set for about -70 dBm at the radio) | One line: `G4TST > ALL DN DG-ID 0`, frames 250, `FICH bad 0`, `sync BER 0`, `upright` or `inverted`; the caller on the screen. Voice frames are a test pattern: the codec plays noise-like sound, which is right |
| 2.2b | `--dgid 20`, the channel hearing 20, then 30 | 20 plays; 30 shows `(muted)` and plays nothing |
| 2.2c | `--order all` | `whiten-all` counts every DCH unit: the receiver takes either whitening order (1.5) |
| 2.2d | stepping the attenuation down in 3 dB steps | note the level where `FICH bad` and the sync BER climb, and where the callsign stops arriving |

`sync BER` at a strong signal settles the receive path's low-frequency response: 0 means flat enough; a floor of 1e-3
or more means a high-pass (set `rx_hpf_hz` to the knee the D-STAR bench measured, or try 100, 200, 300 and keep the
lowest floor).

### 2.3 Our own transmission replayed (the two halves together)

Capture an over of ours with 2.1, then play the capture back into the radio through the transmitting SDR (the RTL-SDR's
cs8 at 1.536 Msps plays on a HackRF with `-s 1536000`; resample for a Pluto). `watch` shows our callsign and frames:
our transmitter and our receiver agree through real RF, levels and all, with no Fusion radio. It doesn't prove a Yaesu
agrees: both halves could share a mistake in a field the standard leaves open (1.5), which is what part 3 is for.

## 3. Part two: the visiting Yaesu radio (about 20 minutes)

Both radios on the test frequency, lowest power, a metre apart (no attenuators). Run the scripted checklist: it prompts
each step, snapshots the radio's `ysf.*` keys before and after, and records the answers and notes as it goes, so a
session cut short keeps what it did:

```
tools/ysf_bench.py session
```

| # | Step (minutes) | Settles |
|---|---|---|
| 1 | Set-up: the Yaesu in DN, DG-ID 0; note its model and firmware (2) | |
| 2 | **Receive DN**: the Yaesu sends 5 s of speech. Its callsign on our screen, the voice clear (2) | the CRC bit order, the DCH whitening order (`whiten-all` in the line: all or none), the receive polarity, our levels' tolerance of a real transmitter |
| 3 | **Late entry**: switch ours to the channel 2 s into a Yaesu over (1) | the callsign from the DCH within a second |
| 4 | **DG-ID**: the Yaesu at 20; ours hearing 0, then 20, then 30 (3) | that radios carry the DG-ID in the FICH squelch code |
| 5 | **Receive VW**: the Yaesu in VW, 5 s of speech (1) | VW played on the radio: the full-rate decoder in real time (`ysf.rx_concealed` hardly grows; [ysf-modem.md](../spec/ysf-modem.md) 4.1, 4.3) |
| 6 | **Transmit DN**: ours sends 5 s of speech. The Yaesu shows our callsign and plays us (2) | our levels (wide), our polarity and whitening order on a Yaesu receiver, the header's FN / FT, our silence frames |
| 7 | **Transmit VW**: ours on a channel set to Voice sent VW (`ysf.tx_vw=1`), 5 s of speech (2) | a Yaesu radio plays our VW (the layout read off one radio's transmission), the encoder in real time (`ysf.tx_underruns` 0) |
| 8 | **Transmit DG-ID**: ours at 20, the Yaesu at 20, then 30 (2) | the same field going out |
| 9 | **Short overs**: three 1 s overs from ours (1) | the first word not clipped (key-up against the two headers) |
| 10 | **Range**: walk the Yaesu away until each side breaks up (3) | a rough comparison of our receiver with a Yaesu's |
| 11 | **Repeater** (optional): through a Fusion repeater or hotspot in range (3) | the repeater passes us; its callsigns shown |

**Record, so nobody has to be asked back**: the session JSON (every step's keys and answers); the Yaesu's model,
firmware, callsign and the DG-ID and DN / VW settings used; for each failed step what the Yaesu's own screen showed
(a photo is best); and, if possible, an RTL-SDR capture of step 2 (`rtl_sdr -f <sdr Hz> -s 1536000 -n 15000000
yaesu.cs8` while it talks), decoded later with `tools/ysf_ref.py decode yaesu.cs8 --offset 25000 --voice`. That one
capture answers most of [ysf-modem.md](../spec/ysf-modem.md) 1.5 offline: the destination a Yaesu sends, its header's
FN / FT, its Rem fields, its whitening order and its levels. The capture holds the owner's voice and callsign: it stays
local and is never committed (AGENTS.md).

| Finding | Change |
|---|---|
| `whiten-all` counted every unit of the Yaesu's | Set the transmitter's DCH order to `YSF_WHITEN_ALL` (`radio_ysf.c`, `cfg.order`), re-run step 6 |
| The Yaesu decodes nothing of ours, but the SDR decodes us | Polarity (`tx_invert`), then levels; then the whitening order |
| The Yaesu shows our callsign but no audio | The voice bit order inside the 49 bits, or the codec's silence frames: note it, capture it |
| A DG-ID mismatch | Where the Yaesu puts the DG-ID: the capture shows the FICH |
| VW from ours shows on the Yaesu but plays garbled or not at all | Capture our over on the SDR and decode it (`bench/decode.py --mode ysf --wav`): clean there means the Yaesu wants another field (FN / FT, CM, CSD3); garbled there means our encoder or the frame |
| VW from the Yaesu plays with gaps | The CPU (ysf-modem.md 4.3): `ysf.rx_concealed` against `ysf.rx_played`; the HI/LO multiply for `core/mbe` is the first remedy |

## 4. After the session

Update [ysf-modem.md](../spec/ysf-modem.md) 1.5 with each question answered (and the record it came from), the board's
calibration with the values set, and [hardware-dm32.md](../spec/hardware-dm32.md) 9.3's list.
