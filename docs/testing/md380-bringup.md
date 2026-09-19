<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The OpenDigitalRadio Authors -->

# MD-380 bring-up checklist

The MD-380 port (`make BOARD=md380`, and `MD380_BAND=vhf` for VHF radios) builds and passes its host tests, but it has
never run on a radio. This is the order to try it in, what each stage proves, and what to change when it doesn't
work. The stage numbers match the unknowns table in [hardware-md380.md](../spec/hardware-md380.md) section 17. Every
row names the knob in the source that the likely failure points at.

The radio is a TYT MD-380, MD-390 or Retevis RT3, with or without GPS. Stock firmware D002.032 or newer is assumed
to be on it at the start.

## 0. Before anything

1. **Back up the radio** with the stock CPS (or `tools/md380_migrate.py backup`, [md380-migration.md](../spec/md380-migration.md)): the codeplug (ODR's legacy layout overwrites it, hardware-md380.md 12.2)
   and, if the CPS offers it, the calibration view (ODR only reads the calibration, never writes it).
2. Check that the bootloader works with the stock firmware: hold **PTT and the upper side key** while switching on;
   the LED blinks and the radio appears as USB 0483:DF11 (`tools/stm32_dfu.py --list`). This is the recovery path for
   every stage: reload the stock firmware with TYT's updater.
3. Build and flash (the key file is yours; the tool checks it by SHA-256, hardware-md380.md 14.2):

   ```
   make BOARD=md380                          # UHF; MD380_BAND=vhf for a VHF radio
   tools/stm32_dfu.py --model md380 --key ~/keys/md380.key build/md380/odr_md380.bin
   ```

   The plain image fills flash sectors 3-6 only. Sector 6 also holds the start of the stock codec, so flashing the
   plain image leaves the radio without a working codec (FM only). For DMR voice flash the merged image instead:
   with `MD-380-D2.32(AD).bin` in `codecs/`, the build also writes `build/md380/odr_md380_codec.bin` (no key file
   needed; [codec-md380.md](../spec/codec-md380.md)).
4. Antenna on, **no PTT** until stage 6, and then only into a dummy load.
5. The SWD pins become the bandwidth switch and microphone supply once the radio starts (hardware-md380.md 2.6). For a
   debugger session build with `MD380_DEFS=-DMD380_KEEP_SWD` and connect under reset.

Batch the knob changes: every stage below lists what to try, so one rebuild can carry several fixes. The build-time
knobs go in one make variable: `make BOARD=md380 MD380_DEFS="-DMD380_PANEL_TYPE=1 -DMD380_CH_GRAY=1"`.

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
| "ch x" changes by one per detent across all 16 positions, the channel steps the right way | `MD380_CH_ACTIVE_HIGH`, `MD380_CH_GRAY`, line order PIN_CH_B0..B3 in `pins.h`. Unknown 4 |
| PTT (and the accessory PTT) key the PTT bit | PE11 / PE12 pulls in `input_md380.c` |
| "vol" follows the knob; the red LED lights on TX later | PA0 / PE1 (the sources disagree). Unknown 5 |
| "bat raw R NNNNmV" matches a meter within 2 % | `MD380_BATT_COUNTS_X10` in `power_md380.c`. Unknown 6 |
| Switching off with the knob: the radio dies at once | if it stays on, the switch is sensed, not direct: `switch_on()` in `power_md380.c`. Unknown 7 |
| "flash EF4018 16384K" (W25Q128) or a 1 MB part | `flash_md380.c` accepts any JEDEC type 0x40. Unknown 8 |
| Settings survive a power cycle (16 MB parts) | the flash map in `flash_md380.c` |

## 4. Synthesizer, HR_C5000, calibration

| Check | If not |
|---|---|
| "C5000 10=xx" shows a value (not "--") | configuration port timing or the shared bus (`radio_md380.c` `bus_byte`); PE6 power-down polarity. Unknown 14 |
| "cal factory" and sane bytes on "ref / lo / hi / rx" | the security register read (`flash_md380.c`), the row layout in `calibration_md380.c` ("power order" etc. says which check failed). Unknown 9 |
| "pll lock NNNNNNk" on every channel across the band | the frequency plan (`MD380_TCXO_HZ`, `MD380_IF_HZ`, `MD380_RX_LO_ABOVE`, `MD380_VCO_MULT`), the SKY72310 word timing, lock detect polarity (PD10). Unknowns 10, 11 |
| An SDR near the radio sees the receive LO where the plan puts it (UHF: f - 49.95 MHz; VHF: 2 x (f + 49.95 MHz) at the VCO) | same; VCOVCC_SW polarity |

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
| Carrier on frequency within 1 kHz (SDR or counter) | the TX VCO multiplier, `MD380_TX_OFFSET_HZ`, the reference tune (`hrc6000_c5000_set_ref`, DAC2) |
| 1 W and 5 W within 20 % on a power meter at both band edges and center | APC scaling (table x 16) in `calibration_md380.c`. Unknown 16 |
| Deviation about 2.5 / 5 kHz on narrow / wide, voice clean | the HR_C6000 FM TX table on the C5000, the FM modulation pair. Unknown 15 |
| Microphone audible at all | PA14 (mic power) taken over, i.e. not built with `MD380_KEEP_SWD` |

## 7. DMR receive

Needs the codec merged: put `MD-380-D2.32(AD).bin` in `codecs/`, build, and flash `build/md380/odr_md380_codec.bin`
([codec-md380.md](../spec/codec-md380.md)); the debug link VERSION line then reports `codec=md380-d2032`. Without it
the radio is FM only; the slot engine can still be checked without voice:

| Check | If not |
|---|---|
| On a DMR channel with traffic: "tk" climbs by 33 per second, voice and LC counters move | the IF path (DMR_SW), the C5000 DMR mode (0x10 = 0x6B), interrupts on PC0-PC2 |
| The caller's ID and talk group show | the embedded LC (`hrc6000_voice_lcss`), the 0x5F stand-in for DMO / RMO |
| With a codec profile: clean voice, "aud blk" climbing, no "ov" | I2S directions and format in `audio_md380.c`. Unknown 17 |

## 8. DMR transmit (dummy load, then a hotspot or repeater)

| Check | If not |
|---|---|
| The PA (PC5) keys once per 60 ms for 30 ms on a scope, the burst inside the keyed window on an SDR | the RF TX interrupt lead and the TIM7 hold (`PA_HOLD_TICKS`). Unknown 18 |
| A hotspot or another radio decodes our ID, talk group and (with the codec) voice | 0x14-0x16 byte order, the LCSS values, the small TX set (`hrc6000_c5000_dmr_path`) |
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
