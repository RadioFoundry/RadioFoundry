<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# MD-UV380 bring-up checklist

The MD-UV380 port (`make BOARD=mduv380`, and `POWER=10w` for an MD-UV390 Plus 10 W) builds and passes its host tests.
Nothing has run on a radio yet. This page is the order to try it in, what each stage proves, and what to change when
it doesn't work. The stage numbers match the unknowns table in [hardware-mduv380.md](../spec/hardware-mduv380.md)
section 14. Every row names the knob in the source that the likely failure points at.

The radio is a TYT MD-UV380, MD-UV390 or Retevis RT3S, with or without GPS. The port is the UV380 board family's code
(`boards/uv380_family`, shared with the DM-1701) with this radio's data in `boards/mduv380/board.c` (`uvf_variant`,
`uvf_key_matrix`). Most stages check one entry of that data.

The bench is remote: nobody presses keys or looks at the screen. Every check below reads something over the debug
link (`tools/rfoundry_debug.py`) or on the SDR bench, and the diagnostics page lines (`diag`) carry the raw readings.

The feature checklist, with every feature's required HIL tests and the last recorded result, is
`tools/bringup.py list --radio mduv380`. `tools/bringup.py run --radio mduv380` records a run
([tests/hil/README.md](../../tests/hil/README.md#bring-up-mode)). This page keeps the hardware steps and what to
change when one fails.

## Status

Not run on a radio. The codec builds were checked on a host only (untested on the radio): TYT 19.26; TYT 19.74 and
219.74, 5 W and 10 W; TYT and Retevis 18.11 and 18.16; Retevis RT3S 19.06 and 219.60. A first DMR test
should use a 19.26 file and one whose code goes to the start of sector 10 (RT3S 219.60, any 219.74, S19.74).

## 0. Before anything

1. Back up the radio: the codeplug with the stock CPS, then the whole SPI flash and the internal flash through the
   stub (`rfoundry backup`). The factory calibration is in the SPI flash security registers; RadioFoundry only reads it.
2. Build: `make BOARD=mduv380` (5 W) or `make BOARD=mduv380 POWER=10w` (Plus 10 W). With a supported TYT or Retevis
   file in `codecs/` ([codec-mduv380.md](../spec/codec-mduv380.md), "Stock files"), the build also writes `_codec.bin`
   with the codec merged in.
3. Flash with `rfoundry flash` through the TYT bootloader (PTT and SK1 held at power on) or the web flasher.

## 1. It boots

| Check | Pass | If not |
|---|---|---|
| USB enumerates as `RadioFoundry MD-UV380` | the debug link answers `info` | HSE mode (unknown 1): `diag` line "hse": `ext` or `xtal` says which started; `HSI` means neither (`boards/uv380_family/system_uvf.c`) |
| Boot reason, crash log empty | `rfoundry_debug.py crash` empty | `tools/crash_decode.py` on the report |

## 2. Display

| Check | Pass | If not |
|---|---|---|
| Panel type | `diag` "panel type" 0-3 | unknown 2: the factory byte (security register 3, 0x1D) maps through `uvf_variant.panel_type` / `panel_rgb` |
| Picture upright, not mirrored, colors right | the screen capture (`rfoundry_debug.py screen`) matches the gallery | the MADCTL of the type (`drivers/display/st7735`), `madctl_xor` in `display_uvf.c` |
| All 128 rows | no blank or wrapped band at the bottom | `UVF_LCD_HIDDEN_ROWS` (board.mk) |
| Backlight dims | the brightness setting changes the current draw | PD8 (5 W) / PC6 (10 W): `pins.h` |

## 3. Keys, channel knob, power, flash

| Check | Pass | If not |
|---|---|---|
| Each key reports the right name | the debug link's key view, with each key pressed by the HIL key test or a helper at the radio | unknown 4: `uvf_key_matrix` in `boards/mduv380/board.c`; `diag` shows raw rows |
| Encoder: clockwise is the next channel | `rfoundry_debug.py` encoder counters | unknown 5: the decoder's rest state (`input_uvf.c`) |
| Battery voltage within 0.1 V of a meter | `diag` "bat raw" | unknown 6: `uvf_variant.bat_mul` / `bat_div` |
| Volume 0 at the stop, 100 at the top | `diag` "vol raw" at both ends | unknown 7: `vol_min`, `vol_break`, `vol_max` |
| SPI flash JEDEC 0xEF4018 | `info` | `flash_uvf.c` (`device`) |
| Calibration read | `diag` "cal" factory or working | the k factors `uvf_variant.cal_k`, the reader `calibration_uvf.c` |
| 10 W marker | read security register 3 byte 0x1C on a 5 W and on a Plus 10 W radio (`rfoundry backup` keeps the security registers); `diag` "TX off: 10 W radio, 5 W image" means a 5 W image found the marker | unknown 3: the TYT images never read the byte, so the marker is unconfirmed; a 10 W image doesn't require it. `MDUV380_DEFS=-DMDUV380_10W_CHECK=0` skips it |

## 4. AT1846S, HR_C6000

| Check | Pass | If not |
|---|---|---|
| Both chips answer | `diag` "C6000 4:10=80", "errs 0" | `radio_uvf.c` bus set-up (the same pins as the DM-1701) |

## 5. FM receive

`tests/hil` FM receive cases on 461.4625 against the SDR bench (`--fm-mhz`).

## 6. FM transmit (dummy load)

The lowest power first. `tests/hil` FM transmit cases with `--allow-rf`.

| Check | Pass | If not |
|---|---|---|
| Carrier on frequency, deviation | the bench's FM check | the TX offset in `radio_uvf.c`, the calibration's I/Q rows |
| Power per level | a power meter at each level, both band edges | unknown 10: `uvf_variant.frac`, `cal_k` |

## 7. DMR receive

The codec needs the merged image (`_codec.bin`). `diag` DMR counters, `tests/hil` DMR receive cases from the bench HAT.

## 8. DMR transmit (dummy load, then a hotspot or repeater)

| Check | Pass | If not |
|---|---|---|
| The bench decodes our bursts and voice | `tests/hil -m radio_tx` DMR cases | unknown 9: AT1846S 0x44 AF gain; unknown 12: the codec descriptor (`vocoder_mduv380.c` profile) |

## 9. GPS

G models only. `diag` and the APRS position: the module is powered on PA9 for 5 s at start-up, kept on when NMEA is
heard on USART1 (PA10, 9600 baud), switched off otherwise (unknown 8, `boards/mduv380/board.c`).

## 10. After the bench

Record each result in [hardware-mduv380.md](../spec/hardware-mduv380.md) (unknowns table) and in this page's Status,
and move the radio's `[status]` in `boards/mduv380/board.toml` on.
