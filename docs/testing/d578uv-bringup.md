<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# AT-D578UV bring-up checklist

The AT-D578UV port builds and passes its host tests: `make BOARD=d578uv` for the GD32 radios (`GD_Ver` on the stock
version screen), `make BOARD=d578uva` for the Artery radios (`AT_Ver`). Nothing has run on a radio yet. This page is
the order to try it in, what each stage proves, and what to change when it doesn't work. Every row names the knob in
the source that the likely failure points at. Both builds share `boards/d578uv`; the Artery build swaps the platform
underneath (`platform/at32f43x`, [hardware-d578uv.md](../spec/hardware-d578uv.md) section 10), so a fix in the
board's code serves both.

Hardware facts: [hardware-d578uv.md](../spec/hardware-d578uv.md) and [modem-sct.md](../spec/modem-sct.md). Its
section 9 lists what is open on the radio; this page is the order to settle it in.

The feature checklist, with every feature's required HIL tests and the last recorded result, is
`tools/bringup.py list --radio d578uv` (or `d578uva`). `tools/bringup.py run --radio d578uv` records a run
([tests/hil/README.md](../../tests/hil/README.md#bring-up-mode)). This page keeps the hardware steps and what to
change when one fails.

Most checks read the diagnostics lines (`tools/rfoundry_debug.py diag`, `boards/d578uv/diag_d578uv.c`):

| Line | Shows |
| --- | --- |
| `bb` | the baseband's boot result (`ok`, `no block`, `bus`, `no app`, `silent`, `no voice`) |
| `keys` | the two key ladders' raw 12-bit samples (PB0, PB1), PA1, and the microphone lines PA9 and PA10 |
| `sup` | the supply in mV (PC1) and the ignition sense's raw sample (PA6) |
| `vol` | the A and B volume pots' raw samples (PC2, PC5) |
| `nand` | the SPI NAND's ID |
| `burst` | DMR transmit bursts the PA gate switched on (PD0's rising edges while keyed), and key-ups refused with PD0 already high (`ref`) |
| `gps` | whether the module was heard (`yes`/`no`), NMEA sentences with a good / a bad checksum, bytes received (`b`) |
| `kt` | whether the KTMicro receiver answered on its I2C bus |
| `rssi` | chip 0's raw RSSI and noise as the radio task last read them, and its signal flag (`sig`) |
| `hmic` | the serial keypad microphone: present (`yes`/`no`), frames accepted / bytes dropped, the key code held (`k`, hex) |

The `gps` and `rssi` lines have the shape the generic health tests read (`tests/hil/test_health.py`).

## Status

Not run on a radio.

## 0. Before anything

1. Note the stock version screen: `GD_Ver` or `AT_Ver`, and the version. `rfoundry detect` (or the web flasher)
   reads the model name from the update mode and picks the image; it refuses the other variant's.
2. Check that the vendor update mode works with the stock firmware: MENU and EXIT held at power-on
   (`[flash.bootloader]` in `boards/d578uv/board.toml`; unconfirmed). Then `rfoundry detect` names the model. This
   is the recovery path for every step below: `rfoundry restore-stock --board d578uv <file>.CDD` writes a vendor set
   back. The vendor bootloader (below 0x08006000) is never written.
3. Keep the stock `.CDD` for the codec slot: the build merges the baseband's boot block from it
   (`codecs/d578uv-stock.cdd`, [hardware-d578uv.md](../spec/hardware-d578uv.md) 4.3). Without it the radio is FM
   only (`bb no block`).
4. Build and flash: `make BOARD=d578uv` (or `d578uva`), then `rfoundry flash --board d578uv build/d578uv/...` in the
   update mode, or the web flasher.
5. SWD: PA13 and PA14 are also the speaker level device's lines, so the firmware turns SWD off when it starts. For a
   first boot with a debugger attached, build with `D578UV_DEFS="-DD578UV_LEVEL_IC=0"` (no speaker level control).

Run the first boots with the antenna on a dummy load and no microphone plugged in: without one PA9 and PA10 read
high and nothing keys the transmitter. Transmit only from stage 7 on, at the lowest power.

## 1. It boots

| Check | Pass | If not |
| --- | --- | --- |
| The radio stays on after the power key is let go | the hold line PC3 is set first in `board_early_init` | Nothing ran: the vector table at 0x08006000, `APP_BASE` in `board.mk`; the bootloader may check the image (re-enter update mode, write the stock set) |
| USB enumerates, the debug link answers `version` | the clocks, USB (PA11/PA12) | GD32: the USB clock prescaler (/2.5 from 120 MHz, `platform/gd32f30x/clock.c`). Artery: the HICK 48 MHz path and ACC (section 10) |
| `rfoundry_debug.py crash` empty | the crash log (KEPT RAM region) survived the boot | a report: `tools/crash_decode.py`. A record that vanishes on a warm reset means the bootloader clears the top of SRAM ([crash-log.md](../spec/crash-log.md) 2.4) |
| Artery: the image runs at all | the clock tree and the flash divider | the PWC LDO and flash divider order (section 10, "Clock"); 0x40023C60 |

## 2. Display and backlight

| Check | Pass | If not |
| --- | --- | --- |
| Picture upright and landscape, colors right | `rfoundry_debug.py screenshot` matches the gallery (160x128) | MADCTL 0x48 and the swapped windows (`display_d578uv.c`); inversion in `drivers/display/st7735` |
| The backlight follows Settings > Display | five steps visible | register 8 of the PA5 device (`backlight_code`, `front_io.h`); 0xFF is full |

## 3. Keys, encoder, microphone, power, NAND

| Check | Pass | If not |
| --- | --- | --- |
| Each key moves `keys` into one window (below 500, 1500, 2500, 3500) | one ladder value per key, 4095 with none | the windows in `input_d578uv.c` (`ladder`). Record which printed key gives which value in hardware-d578uv.md 3.2, then name the keys in `d578uv.h` and `board.c` |
| Encoder: one count per detent, clockwise is the next channel | the debug link's encoder counters | the decoder table in `drivers/input/quadrature`; swap PC8/PC9 in `pins.h` for the direction |
| The plain microphone's PTT, Up and Down | `keys` mic bits; PTT shows as `KEY_PTT` with the RF off (no channel or a receive-only band); `hmic no` | `mic_key_decode` in `front_io.h` |
| The serial keypad microphone: present, PTT, each key | `hmic yes`, frames rising and few dropped bytes; each key's code in `k` while held; PTT as above | `drivers/input/anytone_mic` (the frame layout, `ANYTONE_MIC_TIMEOUT_MS`), the USART set-up in `input_d578uv.c`. Record the digit, `*` and `#` codes in hardware-d578uv.md 3.2 |
| Unplugging the serial microphone with PTT held | the transmission ends within 3 s; `hmic no` | `ANYTONE_MIC_TIMEOUT_MS` |
| `sup` within 0.2 V of a meter | the 11:1 divider | `supply_mv` in `power_d578uv.c` |
| `vol` moves end to end with each knob | raw 0-4095 | PC2/PC5 channels in `power_d578uv.c` |
| Turning the radio off saves and powers down | | PA0 with PA6 below 1.33 V (`switch_on`), PC3 |
| `nand` gives a known ID; settings survive a power cycle | | `nand_d578uv.c` (SPI pins, the ID read). RadioFoundry writes only its own 32 blocks (6.3) |

## 4. Receive audio and FM receive

Tune the FM VFO to a known signal (NOAA, a repeater output, the SDR bench) and to a quiet channel.

| Check | Pass | If not |
| --- | --- | --- |
| Audio from the speaker with the squelch open | `tests/hil` FM receive cases | the shift register's audio bits 0x02, 0x08, 0x40 and PA7 (`radio_d578uv.c`); the level device on PA13/PA14 (`D578UV_LEVEL_IC`) |
| RSSI and the squelch track the signal | `view` rssi, the squelch closes on a quiet channel | the QA4818 RSSI read and its offset (`drivers/rf/qa4818`) |
| CTCSS and DCS open only on the right tone | `tests/hil` subtone cases | the sub-audio registers (`drivers/rf/subaudio`) |
| Both bands, 145 and 435 MHz | | the band bits on chip 0 and the shift register's 0x04 |
| Sensitivity even across each band (the SDR bench's level steps at a band's ends and middle) | | the PA5 trims from the calibration (`trims_a`, `rf_plan.h`; [hardware-d578uv.md](../spec/hardware-d578uv.md) 5.5). Writing the factory defaults instead tells a calibration problem from a formula one |

## 5. Side B, air band, broadcast FM

| Check | Pass | If not |
| --- | --- | --- |
| Dual watch: side B's FM channel heard while side A idles | `tests/hil` dual watch cases | chips 1 and 3 (5.2), the speaker path bit 0x08 |
| `kt yes` | the KTMicro receiver answers on PB6/PB7 | the I2C pins and its address (`drivers/bcast/ktamfm`, 5.4) |
| Air band: an ATIS or tower frequency heard | `tests/hil` AM case | chip 3's LO, 29.25 MHz below (`am_lo_hz` in `rf_plan.h`: try above), and its output bit 6 |
| Broadcast FM: a local station | `tests/hil` broadcast case | the receiver's FM mode, its input with chip 3's bit 6 clear |

## 6. Baseband

`bb ok` after the boot. The baseband boots from the codec slot in about 1 s.

| `bb` | Then |
| --- | --- |
| `no block` | the image has no boot block: build with the stock `.CDD` in `codecs/` |
| `silent` or `bus` | the host port's pins and reset (4.1, 4.2: `modem_d578uv.c`) |
| `no app` | the block was refused: the merge offset (`boards/d578uv/memory.ld` `__codec_slot`, `tools/modem_boot`) |
| `no voice` | a voice set-up write was refused: the codec writes from the calibration (4.4) |

## 7. FM transmit (dummy load, power meter, SDR bench)

The lowest power first, `tests/hil` FM transmit cases with `--allow-rf`.

| Check | Pass | If not |
| --- | --- | --- |
| Carrier on frequency | within a few hundred Hz | a constant error in ppm on both bands: Settings > Advanced > Freq trim (each synthesizer at f x (1 + trim), `synth_hz` in `radio_d578uv.c`); an error that differs by band: the QA4818's frequency words (`drivers/rf/qa4818`) |
| Power per level rises; the lowest is low | a power meter at each level, both bands | the PA DAC (PA4) values from the calibration |
| Deviation and microphone gain | the bench's FM check, `test_mic_gain_deviation` | QA4818 deviation (0x40) and the microphone level (0x18, page 1 0x17 from the calibration's MIC byte, 5.7) |
| CTCSS, the 1750 Hz burst, DTMF on the air | `tests/hil` tone cases | 5.3: TIMER3 PWM on PB9, chip 0's 0x25 bit 0 (microphone mute) |
| An APRS beacon decodes (ham band, `--aprs-test-mhz`) | `tests/hil` APRS transmit cases; the bench's decoder | the tones' level, `AFSK_AMP_NOMINAL` in `tone_d578uv.c` against the deviation; the DTMF drive (5.3) |
| The microphone level follows a voice, and VOX keys on it (FM, VOX on) | the debug link's `vox.level` (about 0 in silence, rising with speech); a VOX transmission ends after the tail | chip 0's 0x64 and its scale (`MIC_AMP_BITS` in `radio_d578uv.c`, 5.6); the mute bit 0x25 bit 0 in receive |
| The PA off and receive back on PTT release | no carrier after release | `d578uv_rf_safe`, PE6, the shift register's 0x10 |

## 8. DMR receive

Select a DMR channel and let the SDR bench send a call.

| Check | Pass | If not |
| --- | --- | --- |
| The call heard, the talker shown | `tests/hil` DMR receive cases | the digital register set (5.1), the call option bits (modem-sct.md 4) |
| Every LC header and terminator reaches the stack | the debug link's DMR counters | call option byte 3 (0x47 here, 0x44 in the stock firmware) |
| Voice level right | | the codec levels and digital speaker gain from the calibration (4.4) |

## 9. DMR transmit (dummy load, then the SDR bench)

| Check | Pass | If not |
| --- | --- | --- |
| `burst` climbs by about 17 a second while keyed, `ref` stays 0 | the baseband pulses PD0 per burst | no bursts: PD0 and EXTI 0 (`PIN_DMR_TX_BURST`), the PA gate (`pa_gate.h`). `ref` counting: PD0 is high at rest, so the line is undriven or its polarity is the opposite (5.1); no DMR key-up goes through until that is settled |
| The bench decodes our bursts and voice | `tests/hil -m radio_tx` DMR cases | `DMR_OFFSET` against the PA gate (4.4), the microphone gain |
| No carrier between bursts and after the call | the SDR bench's power trace | the 800 µs tail (TIMER1), `d578uv_rf_safe` |

## 10. GPS

`gps` good sentences climb about once a second within 8 s of power-on, and the GPS status screen shows satellites.
None: chip 0's register 0x25 bit 2 (the supply), USART1 on PA2/PA3 at 9600 baud (3.6). Bad checksums only: the
baud rate. Good sentences but no GSV with the status screen: the module isn't CASIC, so it took no set-up.

## 11. Flash stub

Once the update mode works (stage 0), try the stub route: `stub/stubtool.py --board d578uv` (or `d578uva`)
([stub-gd32f30x.md](../spec/stub-gd32f30x.md), [stub-at32f43x.md](../spec/stub-at32f43x.md)). Then set `tested =
true` in the board's `[flash.stub]`. First check that the bootloader stays in its update mode with the application's
first word erased (section 9): the stub's interlock relies on it.

## 12. After the bench

Record each result in [hardware-d578uv.md](../spec/hardware-d578uv.md) section 9 (and section 10 for the Artery
radio) and in this page's Status, and move the radio's `[status]` in `boards/d578uv/board.toml` on.
