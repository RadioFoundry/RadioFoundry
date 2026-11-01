<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# MD-2017 bring-up checklist

The MD-2017 port (`make BOARD=md2017`) builds and passes its host tests. Nothing has run on a radio yet. This page is
the order to try it in, what each stage proves, and what to change when it doesn't work. The unknown numbers match the
unknowns table in [hardware-md2017.md](../spec/hardware-md2017.md) section 13. Every row names the knob in the source
that the likely failure points at.

The radio is a TYT MD-2017 or Retevis RT82, with or without GPS. The port is the UV380 board family's code
(`boards/uv380_family`) with this radio's data in `boards/md2017/board.c` (`uvf_variant`, `uvf_key_matrix`) and its
pins under `UVF_PINS_MD2017` in `boards/uv380_family/pins.h`.

The feature checklist, with every feature's required HIL tests and the last recorded result, is
`tools/bringup.py list --radio md2017`. `tools/bringup.py run --radio md2017` records a run
([tests/hil/README.md](../../tests/hil/README.md#bring-up-mode)). This page keeps the hardware steps and what to
change when one fails.

## Status

Not run on a radio. The codec builds were checked on a host only (untested on the radio).

## 0. Before anything

1. Back up the radio: the codeplug with the stock CPS, then the whole SPI flash and the internal flash through the
   stub (`rfoundry backup`). The factory calibration is in the SPI flash security registers; RadioFoundry only reads it.
2. Build: `make BOARD=md2017`. With a supported TYT or Retevis file in `codecs/`
   ([codec-md2017.md](../spec/codec-md2017.md), "Stock files"), the build also writes `_codec.bin` with the codec merged
   in.
3. Flash with `rfoundry flash` through the TYT bootloader or the web flasher. TYT's guide: PTT and the orange top key
   held at power on; if that starts the radio normally, try PTT and SK1 (unknown 9).

## 1. It boots

| Check | Pass | If not |
|---|---|---|
| USB enumerates as `RadioFoundry MD-2017` | the debug link answers `info` | HSE mode: `diag` line "hse" (`boards/uv380_family/system_uvf.c`) |
| Boot reason, crash log empty | `rfoundry_debug.py crash` empty | `tools/crash_decode.py` on the report |

## 2. Display

| Check | Pass | If not |
|---|---|---|
| Panel type | `diag` "panel type" 0-3 | unknown 2: `uvf_variant.panel_type` / `panel_rgb` |
| Picture upright, not mirrored, colors right | the screen capture (`rfoundry_debug.py screen`) matches the gallery | the type's MADCTL (`drivers/display/st7735`), `madctl_xor` in `display_uvf.c` |
| Backlight lights and dims | the brightness setting changes the current draw | unknown 1: `PIN_LCD_BACKLIGHT` in `pins.h` (PB2) |

## 3. Keys, ring, power, flash

| Check | Pass | If not |
|---|---|---|
| Each key reports the right name | the debug link's key view, each key pressed by a helper at the radio | unknown 3: `uvf_key_matrix`; `diag` shows raw rows |
| Ring: up, down, left, right, center | one key press per push in the key view | unknown 4: swap `PIN_NAV_LEFT` / `PIN_NAV_RIGHT` (`pins.h`); `nav_scan` in `input_uvf.c` |
| Orange top key | `KEY_TOP` in the key view | `PIN_KEY_TOP` (PE13) |
| Battery voltage within 0.1 V of a meter | `diag` "bat raw" | unknown 7: `uvf_variant.bat_mul` / `bat_div` |
| Volume 0 at the stop, 100 at the top | `diag` "vol raw" at both ends | unknown 7: `vol_min`, `vol_break`, `vol_max` |
| SPI flash JEDEC 0xEF4018 | `info` | `flash_uvf.c` (`device`) |
| Calibration read | `diag` "cal" factory or working | `calibration_uvf.c` |

## 4. AT1846S, HR_C6000

| Check | Pass | If not |
|---|---|---|
| HR_C6000 answers | `diag` "C6000 4:10=80", "errs 0" | unknown 10: `PIN_C6000_*` in `pins.h`, `clk_port` in `radio_uvf.c` |
| Both AT1846S answer | "errs 0" after init | `rf_addr` in `board.c`, `PIN_RF_I2C_*` (I2C1 PB6 / PB7) |
| Second receiver silent | no hiss with the main receiver squelched | unknown 5: `PIN_RX2_AUDIO_MUX` level in `system_uvf.c` |

## 5. FM receive

`tests/hil` FM receive cases on 461.4625 against the SDR bench (`--fm-mhz`). Then a VHF frequency: the RX supply pins
differ from the DM-1701 (`PIN_RX_UHF_5V`, `PIN_RX_VHF_5V`).

## 6. FM transmit (dummy load)

The lowest power first. `tests/hil` FM transmit cases with `--allow-rf`.

| Check | Pass | If not |
|---|---|---|
| Carrier on frequency, deviation | the bench's FM check | the TX offset in `radio_uvf.c`, the calibration's I/Q rows |
| Power on UHF and VHF | output near the level's power on both bands | unknown 11: `PA_BAND_UHF` in `pins.h` |
| TX audio | modulated voice | unknown 12: mic power |
| Power per level | a power meter at each level, both band edges | unknown 8: `uvf_variant.frac`, `cal_k` |

## 7. DMR receive

The codec needs the merged image (`_codec.bin`). `diag` DMR counters, `tests/hil` DMR receive cases from the bench HAT.

## 8. DMR transmit (dummy load, then a hotspot or repeater)

| Check | Pass | If not |
|---|---|---|
| The bench decodes our bursts and voice | `tests/hil -m radio_tx` DMR cases | unknown 13: the codec descriptor (`vocoder_md2017.c` profile) |

## 9. GPS

GPS models only: the module is powered on PA9, NMEA on USART1 (PA10, 9600 baud).

## 10. After the bench

Record each result in [hardware-md2017.md](../spec/hardware-md2017.md) (unknowns table) and in this page's Status, and
move the radio's `[status]` in `boards/md2017/board.toml` on.
