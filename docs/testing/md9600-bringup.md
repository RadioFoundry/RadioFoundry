<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# MD-9600 bring-up checklist

The MD-9600 port (`make BOARD=md9600`) builds, passes its host tests and runs on a radio. This is the order to try it
in, what each stage proves, and what to change when it doesn't work. The unknowns are numbered as in
[hardware-md9600.md](../spec/hardware-md9600.md) section 17. Every row names the knob in the source that the likely
failure points at.

The radio is a TYT MD-9600 or Retevis RT90, any board revision (hardware-md9600.md 16). One image drives every
revision; the revision is a stored setting (19.3).

The feature checklist, with every feature's required HIL tests and the last recorded result, is
`tools/bringup.py list --radio md9600`. `tools/bringup.py run --radio md9600` records a run
([tests/hil/README.md](../../tests/hil/README.md#bring-up-mode)). This page keeps the hardware steps and what to
change when one fails.

## Status (2026-11-02)

First radio: a V1-V3 board (its original firmware reported version 06.09), on a dummy load, no signal generator.

| Area | Result |
|---|---|
| Boot, USB, debug link, CPS | Work |
| Display | Works after turning it 180 degrees (segments reversed, COM normal, column offset 4; unknown 22) |
| Keys, key lock | Work (HIL); encoder and mic keypad not checked by hand yet |
| Data flash, calibration, option block | Read: `cal factory`, `opt factory`, band edges from the option block |
| Board revision | Stored by hand as V1-V3 before the installers learned to read it |
| FM receive, VHF | Works: a NOAA station through the dummy load, at 25 kHz |
| FM receive audio | Loud at the bottom of the volume knob and overdriven. The HR_C6000 settings, FM_SW and the IF chip words were compared with the original firmware's and changed live without effect; whether the original firmware sounds the same on this radio is not checked |
| UHF receive, DMR receive | Not checked (no signal source at hand) |
| Transmit | Off in the build (`CONFIG_MD9600_TX`, `CONFIG_MD9600_DMR_TX`) |

## 0. Before anything

1. **Back up the radio** while its original firmware runs: the data flash (codeplug, contact list) and the
   firmware version (hardware-md9600.md 15). RadioFoundry's open-firmware layout overwrites the codeplug area, and its
   settings claim erases 0x011000-0x01AFFF. RadioFoundry never writes the flash security registers (calibration and
   option block).
2. **Bootloader:** DC off (the power key only puts the radio in standby), hold **P1 and the orange key**, DC on. The
   screen stays blank and the backlight blinks at 1 Hz (14.1). The radio's DFU interface string tells the bootloader
   from the original firmware, which has the same USB IDs and strings.
3. Build and flash. With the radio's original firmware file in `codecs/` the build also writes the image with the
   radio's own codec, `build/md9600/rfoundry_md9600_codec.bin` ([codec-md9600.md](../spec/codec-md9600.md)):

   ```
   make BOARD=md9600
   python3 tools/release.py --board md9600 --version v0.0.0-test --out build/dist
   cd tools/rfoundry && python3 -m rfoundry flash ../../build/dist/rfoundry-md9600-v0.0.0-test.bin \
       --stock FILE --board md9600 --codec merge --yes
   ```

   Later updates go through the stub (`stub/stubtool.py PORT --rfoundry-enter --board md9600 --write-app IMAGE`).
4. **Board revision:** the installers store it after the first boot (19.3). Check it with
   `rfoundry_debug.py hwopt`; set it by hand (`hwopt "Board revision" V1-V3|V4|V4A/V5`) if it reads "not set". The
   diagnostics page shows the source.
5. **Codeplug:** write one (CPS or `tools/cps_client.py`) before judging the channel screens. Until then the codeplug
   area holds the original firmware's own bytes. Writes are refused until the settings area is claimed
   (`rfoundry_debug.py storage claim --out DIR`, which saves the area first).

## 1. It boots

| Look for | Proves | If not |
|---|---|---|
| USB serial enumerates, `rfoundry_debug.py --port PORT version` says `board=MD-9600` | clocks, OTG_FS, the debug link | No USB: the HSE didn't start (`board.c` falls back to bypass, then HSI without USB). Unknown 18 |
| `rfoundry_debug.py chip` | the MCU, RDP level 1 (normal) | |

## 2. Display

| Look for | Proves | If not |
|---|---|---|
| The home screen upright, no column cut off | ST7567 init, orientation, column offset | `seg_reverse`, `com_reverse`, `col_offset` in `display_md9600.c`. Unknown 22 |
| Readable contrast | electronic volume 0x28 | `contrast` in `display_md9600.c` |
| Odd glyphs in a channel name | nothing wrong with the display: the codeplug area still holds the original firmware's bytes (step 0.5) | |

## 3. Keys, encoder, power, flash

| Look for | Proves | If not |
|---|---|---|
| `test_keys_idle`, `test_keys_move_the_channel` pass | the key matrix | rows and columns in `input_md9600.c` |
| Encoder: one channel per detent, the right direction (`pytest tests/hil/test_encoder.py --manual -s`) | EXTI10 / EXTI11, steps per detent | Unknown 23 |
| `diag`: `supply` near the supply voltage | ADC1 IN1 | `adc_md9600.c` scale |
| `diag`: `flash ... mapped`, `cal factory`, `opt factory` | SPI2, the security registers | `flash_md9600.c`, `cal_md9600.c` |

## 4. FM receive

Put the radio on a VFO frequency with a known signal (`rfoundry_debug.py radio vfo on`, `radio freq F`, `radio bw 25`
for a 25 kHz station).

| Look for | Proves | If not |
|---|---|---|
| `diag`: the band's `rssi` up and `nz` down against an empty frequency | the synthesizer locks, the IF chip runs | the revision (step 0.4), then `rfplan_md9600.c` |
| Audio with the squelch open (`radio monitor`) | FM_SW, FM_MUTE, the amplifier mute, U_V_AF_SW | `speaker_md9600.h`, `rfseq_md9600.c` |
| The volume knob covers a usable range | the receive audio level | Open: see Status |

A `CONFIG_AUDIO_PROBE` build lets `rfoundry_debug.py aprobe reg|wreg 4 REG [VAL]` read and write HR_C6000 registers
live.

## 5. DMR receive, transmit

Not reached yet. DMR receive needs a DMR signal on a channel of the codeplug; transmit stays off in the build until
the RF sequence is checked (19.8).
