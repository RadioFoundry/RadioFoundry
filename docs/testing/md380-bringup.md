<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The OpenDigitalRadio Authors -->

# MD-380 bring-up checklist

The MD-380 port (`make BOARD=md380`, and `BAND=vhf` for VHF radios) builds, passes its host tests and has run
on a radio. This is the order to try it in, what each stage proves, and what to change when it doesn't
work. The stage numbers match the unknowns table in [hardware-md380.md](../spec/hardware-md380.md) section 17. Every
row names the knob in the source that the likely failure points at.

The radio is a TYT MD-380, MD-390 or Retevis RT3, with or without GPS. Stock firmware D002.032 or newer is assumed
to be on it at the start.

## Status (2026-10-18)

On-air results from a tester's radio (1 W, against an AnyTone 878, [issue 3](https://github.com/sfowlr/OpenDigitalRadio/issues/3)):

| Test | Result |
|---|---|
| FM TX and RX | Work, with audio |
| DMR TX | Works with voice: the other radio decodes the call and plays clear audio (0x36 = 0x00 while the stream runs) |
| DMR RX | Calls are taken with the caller and talk group, and their voice reaches the codec clean (393 frames, 2 bit errors), but the speaker plays only a faint TDMA buzz ([hardware-md380.md](../spec/hardware-md380.md) 4.9) |
| DMR RX (2026-10-19, owner's radio) | A keyup is one call now (it used to drop after about 1 s and come back by late entry: the voice was on the other slot). Decoder output clean (`voice.rx_peak` 10-12k, 0 bit errors). Speech now plays from the speaker with the LINEOUT codec values (17d89001), and transmit is clear; the routing tests and the fix are in [hardware-md380.md](../spec/hardware-md380.md) 4.10 (our tests, not authoritative) |

What made DMR receive work: configuration port reads sampled one bit late (4.2), the voice port too fast to read
(4.4), and the receive flow the HR_C5000 needs (DecodeStart, voice read at the ticks, 4.9). Open work: the receive
audio path to the speaker, issue 2, and the features the board leaves off. The draft pull request for
`feature/md380_follow_up` holds the task list.

## 0. Before anything

1. **Back up the radio** with the stock CPS (or `tools/md380_migrate.py backup`, [md380-migration.md](../spec/md380-migration.md)): the codeplug (ODR's open-firmware layout overwrites it, hardware-md380.md 12.2)
   and, if the CPS offers it, the calibration view (ODR only reads the calibration, never writes it).
2. Check that the bootloader works with the stock firmware: hold **PTT and the upper side key** while switching on;
   the LED blinks and the radio appears as USB 0483:DF11 (`tools/stm32_dfu.py --list`). This is the recovery path for
   every stage: reload the stock firmware with TYT's updater.
3. Build and flash (the key file is yours; the tool checks it by SHA-256, hardware-md380.md 14.2):

   ```
   make BOARD=md380                          # UHF; BAND=vhf for a VHF radio
   tools/stm32_dfu.py --model md380 --key ~/keys/md380.key build/md380/odr_md380.bin
   ```

   The plain image fills flash sectors 3-7 only and leaves the codec sectors (8, 10, 11) alone. Coming from stock
   firmware there is no codec there yet, so the first flash for DMR voice is the merged image: with
   `MD-380-D2.32(AD).bin` in `codecs/`, the build also writes `build/md380/odr_md380_codec.bin`, which moves the codec
   code to sector 11 (no key file needed; [codec-md380.md](../spec/codec-md380.md)). After that the plain image is
   enough for an update.
4. Antenna on, **no PTT** until stage 6, and then only into a dummy load.
5. The SWD pins become the bandwidth switch and microphone supply once the radio starts (hardware-md380.md 2.6). For a
   debugger session build with `MD380_DEFS=-DMD380_KEEP_SWD` and connect under reset.

Batch the knob changes: every stage below lists what to try, so one rebuild can carry several fixes. The build-time
knobs go in one make variable: `make BOARD=md380 MD380_DEFS="-DMD380_PANEL_TYPE=1 -DMD380_KEEP_SWD"`.

Knobs for the DMR and audio bring-up (issue 3):

| Knob | Does |
|---|---|
| `-DMD380_I2S_PROBE` | logs `i2s ck N ws N blk N` once a second (`odr_debug.py log`): edges on the HR_C5000's I2S clock and word select, read from the GPIO inputs, and the audio block count. `ck 0 ws 0` = the chip isn't driving I2S |
| `-DIN_STEP_MAX_US=40000u -DOUT_OF_STEP_US=40000u` | widens the DMR engine's report window (1-12 ms after the timeslot tick, out of step from 22 ms): the HR_C5000's reports come 22 ms or later, and the HR_C6000 window drops them all |
| `-DMD380_I2S_MASTER` | the STM32 generates CK and WS (SPI3 master receive at 8 kHz from PLLI2S), but only if both lines follow a pull-up and a pull-down first; it logs `i2s: ck free/driven, ws free/driven`. On the bench both were driven: the HR_C5000 holds them |
| `-DHRC5000_INT_LOG` | records the first 40 system interrupts (flags 0x82, 0x51, 0x52, 0x42, microseconds after the tick) and writes them to the log once a second (needs `MD380_I2S_PROBE`, whose task prints them) |
| `-DHRC5000_I2S_CTL_ON=<v>` | the 0x36 value written while a PCM stream runs (default 0x00: 0x70 left the clocks stopped, [hardware-md380.md](../spec/hardware-md380.md) 4.8) |
| `-DHRC5000_I2S_CYCLE` | each PCM stream that starts takes the next 0x36 value of 0x00, 0x10 ... 0x60, logged as `0x36 now NN` (needs `MD380_I2S_PROBE`); key the other radio once per value |
| `-DHRC5000_TICK_LOG` | one call's timeline: every timeslot tick with the voice port's 27 bytes, and the system interrupts, 120 entries from a call's first interrupt; two 240-byte voice RAM snapshots mid-call (needs `MD380_I2S_PROBE`) |
| `-DMD380_VOICE_SPI_PRESCALER=<v>` | the voice port's SPI2 prescaler (default `SPI_BAUDRATEPRESCALER_32`, 1.125 MHz; 8 gave bad reads, 4.4) |
| `-DMD380_I2S_CKPOL=1` | I2S clock polarity high (default low) |
| `-DHRC_FORCE_CC_ANY` | every receive command deframes any color code (`CTL_CC_ANY`) |
| `-DHRC_INT_04_RX` | treats system interrupt flag 0x04 as a received burst too: the HR_C5000's voice sync reports come with 0x04 alone |

With SNMP off by default (`boards/md380/config.mk`) all of them fit together.

## 1. It boots

| Look for | Proves | If not |
|---|---|---|
| The radio stays on after the knob is turned on (power latch) | vector table at 0x0800C000, `board_early_init` | Nothing ran: check the .bin starts with a stack pointer in SRAM and a reset vector in 0x0800Cxxx; re-enter the bootloader |
| USB serial enumerates ("MD-380 serial") | 8 MHz HSE, the PLL, OTG_FS | No USB but the backlight: HSE didn't start (`board.c` falls back to bypass, then HSI without USB). Unknown 1 |
| `tools/odr_debug.py --port PORT version` says `board=MD-380` (or `MD-380 VHF`) | the debug link | |

## 2. Display

| Look for | Proves | If not |
|---|---|---|
| Backlight on | TIM8 CH1 on PC6 | `display_md380.c` backlight set-up; the LAMP pin may be plain GPIO, not PWM |
| The Home screen, right side up, black background | FSMC timing, controller init | Rebuild with `MD380_PANEL_TYPE=1` (or the st7735 driver's other types), then `MD380_PANEL_MADCTL_XOR` (0x40, 0x80, 0xC0) for orientation. Unknown 2 |
| No band of garbage at the edges | 160 x 128 window, no inset | `CONFIG_DISPLAY_INSET` in `config.mk`, the panel's row offset in `display_md380.c` |

## 3. Keys, channel switch, power, flash

Open Diagnostics (Menu > Settings > Diagnostics, or `odr_debug.py diag`). The board lines follow the DMR counters.

| Check | If not |
|---|---|
| "keys r0 r1 r2": one bit per key press; the UI acts as the key's label says | Matrix positions in `input_md380.c` (`matrix[][]`), hardware-md380.md 9.1. Unknown 3 |
| SK1 = monitor, SK2 = talk target | the K3 row's columns |
| "ch x" changes by one per detent across all 16 positions, the channel steps the right way | Settled: the Gray code table in `input_md380.c` (hardware-md380.md 9.2) |
| PTT (and the accessory PTT) key the PTT bit | PE11 / PE12 pulls in `input_md380.c` |
| "vol" follows the knob; the red LED lights on TX later | PA0 / PE1 (the sources disagree). Unknown 5 |
| "bat raw R NNNNmV" matches a meter within 2 % | `MD380_BATT_COUNTS_X10` in `power_md380.c`. Unknown 6 |
| Switching off with the knob: the radio dies at once | if it stays on, the switch is sensed, not direct: `switch_on()` in `power_md380.c`. Unknown 7 |
| "flash EF4018 16384K" (W25Q128) or a 1 MB part | `flash_md380.c` accepts any JEDEC type 0x40. Unknown 8 |
| Settings survive a power cycle (16 MB parts) | the flash map in `flash_md380.c` |

## 4. Synthesizer, HR_C5000, calibration

| Check | If not |
|---|---|
| "C5000 10=80" after boot in FM (`--` means the read returned FF, a dead bus) | configuration port timing or pins (`radio_md380.c` `bus_byte`; clock PC13, MISO PE3); PE6 power-down polarity. Unknown 14 |
| "cal factory" and sane bytes on "ref / lo / hi / rx" | the security register read (`flash_md380.c`), the row layout in `calibration_md380.c` ("power order" etc. says which check failed). Unknown 9 |
| "pll lock NNNNNNk" on every channel across the band | the frequency plan (`MD380_TCXO_HZ`, `MD380_IF_HZ`, `MD380_RX_LO_ABOVE`, `MD380_VCO_MULT`), the SKY72310 word timing (clock PE4), lock detect polarity (PD10). Unknowns 10, 11 |
| An SDR near the radio sees the receive LO where the plan puts it (UHF: f - 49.95 MHz; VHF: 2 x (f + 49.95 MHz) at the VCO) | same |

## 5. FM receive

| Check | If not |
|---|---|
| A signal generator or another radio on a simplex channel opens squelch and plays clean audio | DMR_SW, FM_SW, FM_MUTE, SPK_C, AFCO polarities (`radio_md380.c` `rx_path`, `speaker_apply`); the HR_C5000 feed-through 0x36. Unknown 12 |
| 12.5 / 25 kHz channels sound right (no distortion on wide, no hiss on narrow) | W/N_SW (PA13) polarity in `set_wide` |
| "rssi" and "busy" move with the signal: record both at -120, -110, -100, -80, -60 dBm | fit `status()` (`RSSI_FLOOR_DBM`, `RSSI_SPAN_DB`, the noise scale). Unknown 13 |
| The squelch setting opens and closes where expected | the noise mapping (larger = noisier) |

## 6. FM transmit (dummy load)

| Check | If not |
|---|---|
| TX refused with "cal none" | correct: no uncalibrated drive |
| Carrier on frequency within 1 kHz (SDR or counter) | the TX VCO multiplier, `MD380_TX_OFFSET_HZ`, the reference tune (`hrc5000_set_ref`, DAC2) |
| 1 W and 5 W within 20 % on a power meter at both band edges and center | APC scaling (table x 16) in `calibration_md380.c`. Unknown 16 |
| Deviation about 2.5 / 5 kHz on narrow / wide, voice clean | the HR_C6000 FM TX table on the C5000, the FM modulation pair. Unknown 15 |
| Microphone audible at all | PA14 (mic power) taken over, i.e. not built with `MD380_KEEP_SWD` |

With `CONFIG_SUBTONE` (on by default; sub-audio, [hardware-md380.md](../spec/hardware-md380.md) 6.4):

| Check | If not |
|---|---|
| Receive: a generator sending 100.0 Hz CTCSS at about 15 % deviation opens a channel with that RX tone and no other (try 97.4 and 103.5); the Home screen shows "tone" | `MD380_SUBTONE_MIN_AMP` (the level on PA2: read `md380_adc_raw` of IN2 with and without the tone), the ADC rate (`md380_adc_scan_rate_hz`), whether PA2 sees the audio at all |
| Receive: DCS 023N and 023I from a generator each open only their own channel | `MD380_SUBTONE_RX_INVERT=1` if they are swapped |
| Transmit: CTCSS 100.0 Hz on a dummy load, an SDR or service monitor shows the tone at 0.5 to 0.75 kHz deviation (CTCSS) and the channel's DCS decodes on another radio | `MD380_SUBTONE_PERCENT` for the deviation; no tone at all: whether PC7 reaches the modulator (scope on PC7 first, 17.6 kHz PWM with a 100 Hz envelope), `MD380_SUBTONE_TX_INVERT=1` for DCS polarity |
| Voice still clean with the tone on, and the squelch tail short | the tone level; the turn-off burst is not written |

## 7. DMR receive

Needs the codec merged: put `MD-380-D2.32(AD).bin` in `codecs/`, build, and flash `build/md380/odr_md380_codec.bin`
([codec-md380.md](../spec/codec-md380.md)); the debug link VERSION line then reports `codec=md380-d2032-s11`. Without
it the radio is FM only; the slot engine can still be checked without voice:

| Check | If not |
|---|---|
| On a DMR channel with traffic: "tk" climbs by 33 per second, voice and LC counters move | the IF path (DMR_SW), the C5000 DMR mode (0x10 = 0x6B), interrupts on PC0-PC2 |
| The caller's ID and talk group show | the embedded LC (`hrc6000_voice_lcss`), the 0x5F stand-in for DMO / RMO |
| With a codec profile: clean voice, "aud blk" climbing, no "ov" | I2S directions and format in `audio_md380.c`. Unknown 17. The receive audio route is open: start from [hardware-md380.md](../spec/hardware-md380.md) 4.10, which lists what was tried, so it isn't repeated |
| No codec profile with the merged image flashed | the boot check in `vocoder_md380.c` failed: descriptor, placement or a CRC of the moved codec (codec-md380.md, Hosting) |
| After an update with the plain image (keep mode): still `codec=md380-d2032-s11` and clean voice RX and TX | the update reached a codec sector (8, 10 or 11), or the image passed 0x08080000 |

## 8. DMR transmit (dummy load, then a hotspot or repeater)

| Check | If not |
|---|---|
| The PA (PC5) keys once per 60 ms for 30 ms on a scope, the burst inside the keyed window on an SDR | the RF TX interrupt lead and the TIM7 hold (`PA_HOLD_TICKS`). Unknown 18 |
| A hotspot or another radio decodes our ID, talk group and (with the codec) voice | 0x14-0x16 byte order, the LCSS values, the small TX set (`hrc5000_dmr_path`) |
| CSBK / data (SMS) accepted by the other side | the 10-byte chip-CRC writes (`hrc6000_chip_crc_len`) |
| VHF radios: receive and transmit both work | the I / Q swap (0x01 = 0xF0). Unknown 19 |

## 9. GPS

| Check | If not |
|---|---|
| G model: "gps yes N/0" with N climbing once per second; APRS shows a position after a fix | USART3 RX pin (PD9), `GPS_BAUD`, the power pin (PD8). Unknown 20 |
| Non-G model: "gps no 0/0", no GPS menus, settings and codeplug still work | nothing may drive PD8 on these radios: if the flash fails after start-up, stop and report |

## 10. After the bench

Update [hardware-md380.md](../spec/hardware-md380.md): every confirmed fact from (D / F / ?) to (C) with the date,
corrected facts with what was measured, and the unknowns table. Update the radios table in the README.
