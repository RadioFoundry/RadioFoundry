<!-- SPDX-License-Identifier: MIT -->

# Air band AM receive and broadcast FM receive

Status: written, not yet tested on a radio (2026-10-22). What the radios RadioFoundry supports can do on the aeronautical
VHF band (AM, 108-137 MHz) and FM broadcast (65-108 MHz), and how RadioFoundry does it on any radio that has the hardware.
Both are receive only and need no menu: the frequency decides. An FM channel or the VFO in the air band is received
in AM; one in the broadcast band plays on the broadcast receiver.

## 1. Which radios

A radio has air band receive when its build has `CONFIG_AM_RX` and its board sets `CAP_AM_RX` and `rx_am`, and
broadcast receive when it has `CONFIG_BCAST_FM` and sets `CAP_BCAST_FM` and `board.bcast`. Which radios do is in
[feature-matrix.md](../feature-matrix.md) (`am_rx`, `bcast_fm`), generated from the boards. A radio's own facts (pins,
audio path, open questions, bring-up checks) are on its hardware and bring-up pages.

## 2. Chips

| Chip | Air band AM | Notes |
|---|---|---|
| FD6818 | yes (inferred) | BK4819 class: the AF output type in 0x47 bits 11:8, type 7 = AM on the BK4819 (Apache-2.0 UV-K5 firmware). Unconfirmed on the FD6818 until a radio's bring-up |
| BK4819 | yes | the same register, documented |
| AT1846S | no | FM and the DMR baseband modes only (datasheet, our driver) |

| Broadcast tuner | Driver |
|---|---|
| RDA5807M | `drivers/bcast/rda5807m` (4.2) |
| RDA5802N | none checked yet; the RDA5807M driver may serve it after a bring-up check |

## 3. Air band AM

### 3.1 Representation: derived from the frequency, codeplug unchanged

The open-firmware CPS codeplug has no AM modulation, and RadioFoundry keeps the standard format as long as possible. So AM is not
stored: **an FM channel or the VFO tuned inside 108-137 MHz is received in AM** on a radio that can. This is how
most radios with an air band behave, it needs no CPS change, and qdmr can program air channels today (as FM
channels in that range). The band is an ITU allocation, the same everywhere, so the range is a core constant
(`RADIO_AM_LO_HZ` / `RADIO_AM_HI_HZ` in `core/radio/radio_am.h`), not a board value. A DMR channel in the range stays
DMR (and can't transmit there). Against a stored AM flag: it would need the native codeplug first, and an AM
channel outside the air band has no use on these radios.

The Codeplug Editor follows the same rule (`listenBand()` in `site/static/js/codeplug/model.js`, with the air band and
65-108 MHz for broadcast): an RX frequency typed in either band turns on RX Only, the channel list shows the mode the
radio uses, and the Problems list warns about a DMR channel there or a TX frequency there without RX Only.

### 3.2 Behavior

| Item | Behavior |
|---|---|
| When | `CONFIG_AM_RX`, the board has `CAP_AM_RX` and `board.radio->rx_am`, the channel is FM and its RX frequency is in [108, 137) MHz |
| Tuning | `tune()` calls `rx_am(hz, bw)` instead of `rx()`; if the board refuses, FM as before. `view.am` says AM is on |
| Transmit | refused with `TXDENY_AM` whenever `view.am`, and for any TX frequency in the air band on an AM radio (split channels, DMR channels, the VFO, the data service, the hotspot's frequencies) |
| Squelch | on the RSSI: `radio_am_noise()` maps dBm onto the FM noise scale (-18 dBm = 0, 1 dB per step), so the levels and hysteresis are the FM ones: level 1 opens above -82 dBm, 5 above -70, 9 above -58. The boards' AM RSSI isn't calibrated: empty air band channels read -76 to -86 dBm on a radio (2026-10-22), so level 5 sits about 8 dB above that. `view.noise` holds that value in AM. Scan uses the same value |
| Sub-audio | ignored in AM (`tone_needed()` is false, the tone counts as heard) |
| Steps | `radio_req_vfo_step_size()` (RREQ_VFO_STEP_SIZE, stored in `settings_t.vfo_step_hz`; 0 is auto, the default: 25 kHz at width 25K or in AM, else `RADIO_VFO_STEP_HZ`, recomputed on every view change, with `view.vfo_step_auto` set; Settings › Talk › VFO step); 8333 means the exact 8.33 kHz grid (n x 25/3 kHz, `radio_am_step()` snaps and steps without drift); 25 kHz is a plain step |
| Band table | the board lists the air band as an RX-only band ({108000000, 136000000, false} after the VHF TX band) |
| Screen | "AM" in place of "FM" for the mode on Home, in the strip, on the mode tile and in the channel list. Home's chips are "Air Band AM" and the squelch level (no tone, width or power). PTT shows "Air band: receive only" |

### 3.3 Board contract (hal_radio.h)

`rx_am(hz, bw)` tunes and receives AM; the board leaves AM at the next `rx()` or `set_mode()`. `status()` gives an
RSSI in dBm (noise isn't used in AM) and `tone_ok = true`. `tx()` is never called while AM is on. NULL = no AM.
On an FD6818 `rx_am` is `rx` plus the AF output type AM (`fd6818_set_am`, 0x47 = 0x0740; inferred, section 2);
`rx()` and `set_mode()` write FM back.

## 4. Broadcast FM

### 4.1 A second receiver, chosen by the frequency

Broadcast FM is wideband FM (about 200 kHz) on a separate chip, so it is a separate receiver (`hal_bcast.h`,
`board.bcast`, `CAP_BCAST_FM`), not a channel mode. As with the air band, nothing is stored: **an FM channel or the
VFO tuned inside the broadcast receiver's range** (`board.bcast->lo_khz` to `hi_khz`, 65-108 MHz on the RDA5807M) plays
on it. The helpers are in `core/radio/radio_bcast.h`. The channel receiver stays on its last frequency, muted.

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

### 4.3 Behavior

| Item | Behavior |
|---|---|
| When | `CONFIG_BCAST_FM`, the board has `CAP_BCAST_FM` and `board.bcast`, the channel is FM and its RX frequency is in the receiver's range. `view.bcast` says the broadcast receiver plays |
| Tuning | the receiver is powered and tuned to the nearest point of its grid (`radio_bcast_khz`). Leaving the range powers it off. If the chip doesn't answer, `view.bcast_fault` is set and the radio stays idle |
| Transmit | refused with `TXDENY_BCAST` whenever `view.bcast`, and for any TX frequency in the range on a radio with the receiver (split channels, DMR channels, the VFO, the hotspot's frequencies) |
| Squelch | on the receiver's RSSI (about dBuV): noise = 97 - RSSI on the FM noise scale, so level 5 opens above 45 dBuV, 1 above 33, 9 above 57. Set from a receiver without an antenna (2026-10-22): empty frequencies read 22 to 37, stations 50 and up. The signal meter shows RSSI - 107 dBm. Only a scan uses it (to stop on stations): otherwise the state stays RX, the LED stays on, the screen shows the level and the audio plays whatever the signal (`bcast_listening()`) |
| Sub-audio | ignored (`tone_needed()` is false) |
| Volume | the radio's own volume where it has one (`radio_volume_soft`), mapped onto the receiver's 0-15; a fixed 12 where a knob after the amplifier sets the level |
| Steps | the VFO's automatic step is the broadcast step setting (4.4). An explicit VFO step (Settings › Talk › VFO step) is used as set |
| Power | battery save is off while the receiver plays; hotspot mode powers it off |
| Screen | Home's chips are "Broadcast FM" alone (no tone, width, power or squelch level). The mode word stays "FM". PTT shows "Broadcast: receive only" |
| APRS | a beacon's carrier sense reads the channel receiver on the APRS frequency, as on any channel |

### 4.4 Broadcast step

Settings › Talk & listen › FM broadcast step, `radio_settings_t.bcast_step`, stored in `settings_t.rs_bcast_step`
(page 1; `bcast_step + 1`, 0 = the default). The row shows only on a radio with the receiver (`UI_FEAT_BCAST`).

| Setting | VFO knob and Up / Down |
|---|---|
| 200 kHz (default) | to the next odd tenth of a MHz: 88.1, 88.3 ... 107.9, the broadcast channels of the Americas. From an even tenth or a point between, the first step lands on the next odd tenth in that direction (90.2 up is 90.3, down is 90.1) |
| 100 kHz | every 100 kHz, from wherever the VFO is |

Either way any frequency can be typed in: 90.2 is tuned and heard as entered.

### 4.5 Board glue

The HR_C7000 boards share one composition, `platform/hrc7000/bcast_hrc.c` (`hrc_bcast_init`): the RDA5807M driver on
a bit-banged I2C bus (open-drain SDA) on the board's two pins, an optional enable pin and level, and the board's
audio path callback. A board fills in `hrc_bcast_cfg_t` and sets `board.bcast = &rda5807m_hal`; its pins and audio path
are on its hardware page.

The board decides how the broadcast reaches the speaker. The core keeps the channel receiver's audio off while the
broadcast plays, so the two never ask for the speaker at once (hal_bcast.h `audio`).

## 5. Build switches

| Switch | Default | Covers |
|---|---|---|
| `CONFIG_AM_RX` | on with FM; off where the receiver has no AM | `radio_am.c` in the radio core, RREQ_VFO_STEP_SIZE; needs FM |
| `CONFIG_BCAST_FM` | on; off where there is no broadcast receiver | `radio_bcast.c` in the radio core, the board's tuner glue, the step setting |

The board must also set the capability bits; without them the code does nothing at run time. Per-board defaults:
[feature-matrix.md](../feature-matrix.md).

## 6. Tests

| Test | Covers |
|---|---|
| `tests/host/radioListenBandsTest.c` | AM and broadcast channels and VFO, TX refusal, squelch, the steps, a dead receiver |
| `tests/host/fd6818Test.c` | the AF output word for AM and back to FM |
| `tests/host/uiFlowTest.c` | the FM broadcast step row on a radio with the receiver |
| `tests/hil/test_listen_bands.py` | the VFO at 118.0 MHz gives `am=1`; at 100.1 MHz `bcast=1` and `bcast_fault=0`. Nothing keys |
