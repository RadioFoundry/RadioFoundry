<!-- SPDX-License-Identifier: MIT -->

# Air band AM receive and broadcast FM receive

> **Parked (2026-10-10).** Work stopped here when the owner moved the focus to the DM-1701. Done and committed:
> the AM receive logic in the radio core (section 3, `radioAmTest`), the RDA5807M driver and a bit-banged I2C
> master (4.2, `rda5807mTest`), and the broadcast receiver's behavior (4.3, `bcastTest`). Both switches are off
> on every board except the DM-32UV. On the DM-1701 the code is identical; the image is 8 bytes larger, the two
> optional HAL pointers (`hal_radio_t.rx_am`, `board_t.bcast`, NULL there). The DM-32 build wasn't compiled (no csky
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
> 5. On the radio: the bring-up steps in 6, which answer the open questions in 2.3.
> 6. Owner decisions: whether the FD6818 "Confidential Programming guide" seen on mikrocontroller.net may be used
>    (not read; probably not, under the clean-room rules), six presets and the 2 s resume delay, the AM step UI
>    (8.33k / 25k on the Step tile), and refusing TX in 108-137 MHz only on AM-capable boards.

Status: research and design, 2026-10-10. What the radios ODR supports can do on the aeronautical VHF band (AM,
108-137 MHz) and FM broadcast (65-108 MHz), how ODR does it generically, and what the DM-32UV bring-up must check.

Source tags follow [radios.md](radios.md): **C** confirmed (manufacturer, datasheet, bench, or the owner's own
use of the radio), **D** documented by a reverse-engineering project or permissively licensed code, **F** forum or
retailer claim, **?** unknown. **BIN** / **KP** / **COM** / **INF** are the tags of
[hardware-dm32.md](hardware-dm32.md) (disassembly, the dh4570 port, the community pin map, our inference).

## 1. What each radio can do

| Radio | Air band AM RX | Broadcast FM RX | Notes |
|---|---|---|---|
| Baofeng DM-32UV | **yes** (C: owner, stock firmware; Baofeng spec "AM 108-136 MHz") | **yes** (C: Baofeng spec "FM 65-108 MHz") | AM on the FD6818 (INF, 2.2); broadcast on an RDA5807M (COM pin map, INF) |
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

## 2. DM-32UV: what is known

### 2.1 Public claims

- Baofeng's product page and the RadioGuru review's table list "Additional RX bands: AM 108-136 MHz, FM 65-108 MHz"
  and the aviation band as a feature (C) ([baofengradio.com](https://www.baofengradio.com/products/dm-32uv),
  [review](https://www.baofengradio.com/blogs/news/baofeng-dm-32uv-review-radioguru)).
- The owner confirms the stock firmware receives aviation AM (C). Treat AM air band RX as a capability of the radio.

### 2.2 Which chip demodulates AM

Not decided by any source. The evidence:

| Fact | Source | Points to |
|---|---|---|
| The FD6818's registers behave like the BK4819's: frequency in 10 Hz units in 0x38 / 0x39, state 0x30 = 0xBFF1 RX / 0xC1FE TX (the BK4819's enable bits: VCO calibration, RX link, AF DAC, discriminator, PLL, RX DSP), CTCSS word = Hz x 20.64888 in 0x07, RSSI in 0x67 bits 8:0, AF output 0x47, AF gain 0x48 | hardware-dm32.md 8 (KP, BIN); BK4819 register names in the Apache-2.0 UV-K5 firmware ([bk4819-regs.h](https://github.com/DualTachyon/uv-k5-firmware)) | the FD6818 is a BK4819-class part (INF) |
| The BK4819 selects its AF output type in 0x47 bits 11:8 (0x6040 \| type << 8); type 7 is AM | UV-K5 firmware (D) | AM is a register setting on that class of chip |
| ODR's FM set-up writes 0x47 = 0x6042 (AF type 0, muted); DMR writes 0x0140 (type 1) | hardware-dm32.md 8.2 (KP) | in FM the FD6818's own AF output is silent: the HR_C7000 demodulates FM from I/Q (its RX I/Q bias registers, modem-hrc7000.md) |
| The FD6818 band table covers 16 MHz to over 740 MHz, with rows at 93 and 124 MHz | hardware-dm32.md 8.3 (KP) | the synthesizer tunes the air band |
| GPIO 0x2F is described as "speaker source switch (PWM/codec vs FD6818 audio)" | hardware-dm32.md 5 (COM) | the FD6818's AF output can reach the speaker directly |
| No AM demodulator noted in the HR_C7000 user guide summary | modem-hrc7000.md (DS) | AM is not the C7000's job (INF, low confidence: re-read the guide) |

**Working hypothesis (INF):** AM is demodulated by the FD6818 (0x47 AF type 7), its AF output goes to the speaker
amplifier through the source switch (0x2F) instead of the C7000 codec, and the 108-136 MHz signal comes in through the
VHF front end (VHF LNA, GPIO 0x22; the 136-174 MHz filter costs sensitivity at the low end). The alternative is AM
audio fed into the C7000's audio path. Bring-up step 6.1 decides between them.

### 2.3 Open questions

| # | Question | How bring-up answers it |
|---|---|---|
| A1 | Is 0x47 AF type 7 the FD6818's AM output? | 6.1: stock radio on ATIS for reference; ODR with `rx_am`, compare audio |
| A2 | Which speaker source (0x2F level) carries FD6818 AF? | 6.1: try both levels while receiving AM |
| A3 | AM filter bandwidth (0x43) and AGC (0x13, 0x09 table) for AM | 6.1: listen to a strong and a weak station; the UV-K5 needed AGC changes for AM (D) |
| A4 | RSSI offset in AM (the squelch's dBm) | 6.1: signal generator at -100 dBm, read `rssi` on the debug link |
| B1 | Is the RDA5807M really at PTA25 / PTA26, and does it answer at 0x11? | 6.2: chip ID 0x58xx over the debug link probe |
| B2 | Its reference clock (32.768 kHz crystal or a clock signal; 0x02 CLK_MODE, RCLK_DIRECT) | 6.2: tune a known station with each setting |
| B3 | Pull-up on SDA (or the pin's own pull) | 6.2: the chip ID read fails without one |
| B4 | Where its audio goes (into the amplifier directly, through 0x2F, or into the codec) and whether its supply is switched by a GPIO | 6.2: listen with each speaker path set |
| B5 | Antenna for broadcast (the main antenna through the VHF path, or the headset cable) | 6.2: compare stations with and without a headset |

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
DM-32 plan (not written): FD6818 FM register set, 0x47 = 0x6742 (AF type 7, A1), the AF gain in 0x48, VHF LNA on,
C7000 FM playback off, speaker source to the FD6818 (A2).

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

The board decides how the broadcast reaches the speaker and arbitrates with the channel's audio. DM-32: unknown (B4);
the glue must keep the amplifier on while either source plays and give the channel priority.

### 4.6 Screen (planned, docs/ux principles)

Tools > "FM radio" (row 9; 8 without diagnostics), a list with the frequency, station, stereo and signal in the body:
1 Listen on / off, 2 Seek up, 3 Seek down, 4-9 presets 1-6 (SELECT tunes, long SELECT saves the current frequency,
the global "save" meaning), 0 Volume. Left / Right step 100 kHz (on the Volume row they change the volume). Budget:
Home to listening in 4 presses (Hub, Tools, 9, 1). "Paused: channel busy" in the body while muted.

## 5. Build switches

| Switch | Default | Covers |
|---|---|---|
| `CONFIG_AM_RX` | n (DM-32UV: = FM) | `radio_am.c` use in the radio core, RREQ_VFO_STEP_SIZE; needs FM |
| `CONFIG_BCAST_FM` | n (DM-32UV: y) | `core/bcast` |

The board must also set the capability bits; without them the code does nothing at run time.

## 6. Bring-up checks (DM-32UV)

See [dm32-bringup.md](../testing/dm32-bringup.md) step 11.

### 6.1 AM

1. Reference: the stock radio (or a scanner) on the local ATIS or tower frequency; note the time and frequency.
2. ODR on the same frequency (VFO or an FM channel): Home shows AM (once the UI part lands); the debug link's
   `view` shows `am=1`, and the FD6818's 0x47 and 0x30 read back over the chip-register command.
3. No audio: change the speaker source level (0x2F), then try the C7000 path (A2). Distorted on strong signals: AGC
   (A3). Squelch too tight or loose: measure the RSSI with a generator and adjust the offset (A4).
4. PTT on the air channel: the deny screen, the red LED stays off, nothing on an SDR monitoring the frequency.

### 6.2 Broadcast

1. Probe the RDA5807M chip ID on PTA25 / PTA26 (B1, B3).
2. Power on, tune a strong local station, try both reference clock settings (B2).
3. Find the audio path (B4) and the antenna (B5); then the traffic policy: a call on the channel mutes the broadcast,
   which comes back 2 s after.
