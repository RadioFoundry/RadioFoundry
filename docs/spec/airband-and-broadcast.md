<!-- SPDX-License-Identifier: MIT -->

# Air band AM receive and broadcast FM receive

> **Parked (2026-10-10).** Work stopped here when the owner moved the focus to the DM-1701. Done and committed:
> the AM receive logic in the radio core (section 3, `radioAmTest`), the RDA5807M driver and a bit-banged I2C
> master (4.2, `rda5807mTest`), and the broadcast receiver's behavior (4.3, `bcastTest`). Both switches are off
> on every board except the DM-32UV. On the other boards the code is identical; the image is 8 bytes larger, for
> the two optional HAL pointers (`hal_radio_t.rx_am`, `board_t.bcast`, NULL there). The DM-32 build wasn't compiled (no csky
> compiler on the CI machine).
>
> **Parked: revisit** (in order):
> 1. DM-32 board glue: `rx_am` in `boards/dm32/radio_dm32.c` (FD6818 0x47 AF type AM, speaker source switch, 3.3),
>    a `hal_bcast_t` on the RDA5807M (`boards/dm32/bcast_dm32.c` over `drivers/bus/i2c_gpio` on PTA25 / PTA26),
>    CAP_AM_RX / CAP_BCAST_FM and the 108-136 MHz RX-only band in `boards/dm32/board.c`. Add
>    `drivers/bcast/rda5807m drivers/bus/i2c_gpio` to `BOARD_DIRS` in `boards/dm32/board.mk`.
> 2. `core/bcast/bcast_task.c`: the RTOS glue (a request queue, the published view, called from the radio task's
>    poll), the settings fields of 4.4, and the UI services.
> 3. The UI: "FM radio" as Tools row 9 (4.6), "AM" for the mode on Home and in the strip, the Step quick tile
>    (8.33k, 25k) wired to `radio_req_vfo_step_size`, TXDENY_AM's text ("Air band: receive only" / "Pick another
>    channel"); uiFlowTest flows and gallery shots; the simulator's `radio_req_vfo_step_size` and `view.am`.
> 4. Debug link view keys (`am=`, `bcast.*`), docs/user (DM-32UV, not yet tested), README status line.
> 5. On the radio: [dm32-bringup.md](../testing/dm32-bringup.md) 11, which answers the open questions in `hardware-dm32.md` 8.7.
> 6. Owner decisions: whether the FD6818 "Confidential Programming guide" seen on mikrocontroller.net may be used
>    (not read; probably not, under the clean-room rules), six presets and the 2 s resume delay, the AM step UI
>    (8.33k / 25k on the Step tile), and refusing TX in 108-137 MHz only on AM-capable boards.

Status: research and design, 2026-10-10. What the radios ODR supports can do on the aeronautical VHF band (AM,
108-137 MHz) and FM broadcast (65-108 MHz), and how ODR does it on any radio that has the hardware.

Source tags follow [radios.md](radios.md): **C** confirmed (manufacturer, datasheet, bench, or the owner's own
use of the radio), **D** documented by a reverse-engineering project or permissively licensed code, **F** forum or
retailer claim, **?** unknown. **BIN** / **KP** / **COM** / **INF** are the tags of
[hardware-dm32.md](hardware-dm32.md) (disassembly, the dh4570 port, the community pin map, our inference).

## 1. What each radio can do

| Radio | Air band AM RX | Broadcast FM RX | Notes |
|---|---|---|---|
| Baofeng DM-32UV | **yes** (C: owner, stock firmware; Baofeng spec "AM 108-136 MHz") | **yes** (C: Baofeng spec "FM 65-108 MHz") | AM on the FD6818 (INF, `hardware-dm32.md` 8.7); broadcast on an RDA5807M (COM pin map, INF) |
| Baofeng DM-1701 / RT84 | no (C: AT1846S is FM only, bench driver) | no chip found (?) | stays FM / DMR |
| TYT MD-UV380 / RT3S | no (AT1846S, D) | none known (?) | |
| TYT MD-9600 / RT90 | no (SKY72310 + FM receive chain, D) | none known (?) | |
| TYT MD-380 / RT3 | no (GT3136 FM receiver, D/F) | none known (?) | |
| Radioddity GD-77 family | no (AT1846S, D) | none known (?) | |
| Radioddity GD-88 | ? (spec sheet lists 136-174 / 400-480 only, F) | ? | RF chips unknown |
| Anytone 878 family | no airband in any spec found (F) | **yes**, 87.5-108 MHz WFM receiver (F, retailers) | chip unknown (?) |
| Radtel RT-4D (not planned) | yes (BK4819, D) | yes (F) | the BK4819 AF output has an AM type (D, Apache-2.0 UV-K5 firmware) |

The AT1846S has no AM demodulator: its datasheet and our driver know only FM and the DMR baseband modes. Any radio
built on it gets neither feature from ODR. The design below serves any radio whose RF chip has an AM mode
(CAP_AM_RX) and any radio with a second broadcast receiver chip (CAP_BCAST_FM).

## 2. Radio facts

Each radio's air band and broadcast hardware is on its hardware page. The DM-32UV's (which chip demodulates AM, the
open questions A1-A4 and B1-B5) is [hardware-dm32.md](hardware-dm32.md) 8.7; its bring-up checks are
[dm32-bringup.md](../testing/dm32-bringup.md) 11.

## 3. Air band AM (done in the radio core)

### 3.1 Representation: derived from the frequency, codeplug unchanged

The legacy CPS codeplug has no AM modulation, and ODR keeps the standard format as long as possible. So AM is not
stored: **an FM channel or the VFO tuned inside 108-137 MHz is received in AM** on a radio that can. This is how
most radios with an air band behave, it needs no CPS change, and qdmr can program air channels today (as FM
channels in that range). The band is an ITU allocation, the same everywhere, so the range is a core constant
(`RADIO_AM_LO_HZ` / `RADIO_AM_HI_HZ` in `core/radio/radio_am.h`), not a board value. A DMR channel in the range stays
DMR (and can't transmit there). Against a stored AM flag: it would need the native codeplug first, and an AM
channel outside the air band has no use on these radios.

### 3.2 Behavior

| Item | Behavior |
|---|---|
| When | `CONFIG_AM_RX`, the board has `CAP_AM_RX` and `board.radio->rx_am`, the channel is FM and its RX frequency is in [108, 137) MHz |
| Tuning | `tune()` calls `rx_am(hz, bw)` instead of `rx()`; if the board refuses, FM as before. `view.am` says AM is on |
| Transmit | refused with `TXDENY_AM` whenever `view.am`, and for any TX frequency in the air band on an AM radio (split channels, DMR channels, the VFO, the data service, the hotspot's frequencies) |
| Squelch | on the RSSI: `radio_am_noise()` maps dBm onto the FM noise scale (-60 dBm = 0, 1 dB per step), so the levels and hysteresis are the FM ones: level 1 opens above -124 dBm, 5 above -112, 9 above -100. `view.noise` holds that value in AM. Scan uses the same value |
| Sub-audio | ignored in AM (`tone_needed()` is false, the tone counts as heard) |
| Steps | `radio_req_vfo_step_size()` (RREQ_VFO_STEP_SIZE, stored in `settings_t.vfo_step_hz`); 8333 means the exact 8.33 kHz grid (n x 25/3 kHz, `radio_am_step()` snaps and steps without drift); 25 kHz is a plain step |
| Band table | the board lists the air band as an RX-only band ({108000000, 136000000, false} after the VHF TX band) |

### 3.3 Board contract (hal_radio.h)

`rx_am(hz, bw)` tunes and receives AM; the board leaves AM at the next `rx()` or `set_mode()`. `status()` gives an
RSSI in dBm (noise isn't used in AM) and `tone_ok = true`. `tx()` is never called while AM is on. NULL = no AM.
The DM-32UV's planned `rx_am` (not written): `hardware-dm32.md` 8.7.

## 4. Broadcast FM (driver and behavior done; glue and UI not)

### 4.1 A second receiver, not a channel mode

Broadcast FM is wideband FM (about 200 kHz) on a separate chip, so it is a separate receiver (`hal_bcast.h`,
`board.bcast`, `CAP_BCAST_FM`), not a channel mode. The channel receiver keeps running while the broadcast plays.

### 4.2 RDA5807M (drivers/bcast/rda5807m)

From the public datasheet (RDA Microelectronics, rev 1.1; register summary checked against
[done.land](https://done.land/components/data/datatransmission/wireless/fmpublicbroadcast/receiver/rda5807m/)).

| Item | Value |
|---|---|
| Bus | I2C, random access at 0x11: write reg, hi, lo; read reg, repeated START, hi, lo. Chip ID 0x58 in 0x00 bits 15:8 |
| 0x02 | DHIZ b15, DMUTE b14 (1 = audio), MONO, BASS, RCLK non-calibrate b11, RCLK direct b10, SEEKUP b9, SEEK b8, SKMODE b7 (0 = wrap), CLK_MODE b6:4, RDS_EN b3, NEW_METHOD b2, SOFT_RESET b1, ENABLE b0 |
| 0x03 | CHAN b15:6, TUNE b4, BAND b3:2 (2 = 76-108, 3 = 65-76 with 0x07 b9), SPACE b1:0 (2 = 50 kHz) |
| 0x04 / 0x05 / 0x07 | de-emphasis 50 us b11 / INT_MODE, SEEKTH b11:8, LNA port b7:6, VOLUME b3:0 / soft blend threshold, 65M mode b9, soft blend b1 |
| 0x0A / 0x0B | STC b14, SF b13, ST b10, READCHAN b9:0 / RSSI b15:9, FM_TRUE b8, FM_READY b7 |

The driver tunes 65-76 and 76-108 MHz on a 50 kHz grid, keeps shadows so TUNE and SEEK are never written twice, and
follows the chip's STC. The board supplies the bus; `drivers/bus/i2c_gpio` is a bit-banged master (open-drain SDA,
push-pull SCL, no clock stretching).

### 4.3 Behavior (core/bcast)

Power on / off, tune (snapped to the grid), step 100 kHz with wrap, seek (muted while it sweeps, the result taken
at STC), volume 0-15, six presets (recall, save the current frequency). The receiver is muted ("paused") while the
channel has traffic the user should hear: squelch open or monitor (FM, AM), a DMR call we play or our own, the DMR
reply window, a transmission or refused PTT. A DMR call on our slot that we don't play doesn't pause it. It comes
back 2 s after the traffic ends. Hotspot mode switches it off. On a radio where the two receivers share the
speaker, the board's arbitration makes the channel win (hal_bcast.h `audio`).

### 4.4 Settings (not added yet)

To append to `settings_t` (settings-store.md 3; 16 bytes, the record has 68 left):
`uint16_t bc_freq` (10 kHz units, 0 = never set), `uint8_t bc_volume` (`SETTINGS_UNSET8` = default),
`uint8_t bc_flags` (bit 0: listening at power off), `uint16_t bc_preset[6]` (10 kHz units, 0 = empty). They map onto
`bcast_saved_t`. Check the space left against the other packages' appends when merging.

### 4.5 Audio path (board-specific)

The board decides how the broadcast reaches the speaker and arbitrates with the channel's audio. The glue keeps the
amplifier on while either source plays and gives the channel priority. On the DM-32UV the path is unknown
(`hardware-dm32.md` 8.7, B4).

### 4.6 Screen (planned, docs/ux principles)

Tools > "FM radio" (row 9; 8 without diagnostics), a list with the frequency, station, stereo and signal in the body:
1 Listen on / off, 2 Seek up, 3 Seek down, 4-9 presets 1-6 (SELECT tunes, long SELECT saves the current frequency,
the global "save" meaning), 0 Volume. Left / Right step 100 kHz (on the Volume row they change the volume). Budget:
Home to listening in 4 presses (Hub, Tools, 9, 1). "Paused: channel busy" in the body while muted.

## 5. Build switches

| Switch | Default | Covers |
|---|---|---|
| `CONFIG_AM_RX` | n; on for boards with an AM receiver | `radio_am.c` use in the radio core, RREQ_VFO_STEP_SIZE; needs FM |
| `CONFIG_BCAST_FM` | n; on for boards with a broadcast receiver | `core/bcast` |

The board must also set the capability bits; without them the code does nothing at run time. Per-board defaults:
[feature-matrix.md](../feature-matrix.md).
