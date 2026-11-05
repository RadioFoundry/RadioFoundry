<!-- SPDX-License-Identifier: MIT -->
# Retevis RT10 hardware

A DMR and FM handheld for 902-928 MHz on the HR_C7000 SoC, with an FD6818 RF chip, no display and a 16-position channel
knob. A value marked `(measured)` was checked on a radio running RadioFoundry, `(unverified)` is not confirmed on a radio
or in a datasheet, and `(?)` is not determined. Unmarked values come from the radio's observed operation and the board's
markings.

## 1. Chips

| Part | Marking | Role |
| --- | --- | --- |
| SoC | HR_C7000 | C-SKY CK803S core and DMR baseband |
| RF transceiver | FD6818 | receive and transmit, on a two-wire bus (section 7) |
| Reference | 26 MHz TCXO | the FD6818's reference (unverified) |
| SoC crystal | unmarked | 24 MHz (unverified: the PLL settings give 168 MHz only from 24 MHz) |
| Program flash | GD25Q16C, JEDEC C8 40 15 (measured) | 2 MB, execute in place at 0x03000000 |
| Data flash | GD25Q16C | 2 MB on SPI0 (section 4) |
| Audio | NJM2904 dual op amp | analog, no control lines |
| Clock | none found | no RTC crystal or backup cell identified |

## 2. Boot and program flash

There is no vendor loader. After reset the boot ROM offers its serial download window on UART0 (the accessory jack's
programming cable; measured), then reads the flag at flash 0 and jumps to the address it names.

| Range (flash offset) | Content |
| --- | --- |
| 0x000000 | ROM flag `74 68 64 0C` (`thd\x0c`), then the entry address, little endian |
| 0x001000-0x00BFFF | unused by RadioFoundry |
| 0x00C000 | the RadioFoundry application (linked for 0x0300C000) |
| 0x0E8000-0x0FFFFF | the codec slot |
| 0x100000-0x1FFFFF | unused |

- RadioFoundry's flag names 0x0300C000. `stub/stubtool.py --write-app` writes the flag sector after the application when
  the board profile has `rom_entry` (board.toml `[flash.stub]`), so the radio keeps starting whatever was there until
  the new image is complete.
- The original image starts at 0x1000 and runs to about 0x78000, so installing RadioFoundry overwrites it. Only a full
  2 MB backup (`stubtool --progmem FILE` with the board profile) puts it back.
- The boot ROM's window is the recovery path: with the radio off, `stubtool PORT --write-app IMAGE`, then switch it on.

Start-up state: the boot ROM leaves the SoC on the 24 MHz oscillator and the XIP controller in single-bit mode. The
platform's clock set-up (platform/hrc7000/clock.c) starts the PLL at 168 MHz CPU and 84 MHz APB.

## 3. Pins

Index = GPIO pin index (0-31 PTA, 32-63 PTB, 64-95 PTC). The IO mux stays at its reset default except SPI0 (IO_DIPLEX1
bits 8:3 cleared, bits 29:27 set).

| Pin | Index | Use | Level |
| --- | --- | --- | --- |
| PTA0-PTA4 | 0-4 | JTAG (unverified: the unpopulated 7-pad header) | |
| PTA5 / PTA6 | 5 / 6 | a second two-wire bus, no device found | |
| PTA7 / PTA8 | 7 / 8 | FD6818 clock / data | bit-banged |
| PTA18 | 18 | data flash chip select | low = selected |
| PTA20-22 | 20-22 | SPI0 clock, MOSI, MISO (unverified) | |
| PTB0 | 32 | TX supply | high = on |
| PTB1, PTB2, PTB6, PTB21 | 33, 34, 38, 53 | channel knob code bits 2, 1, 3, 0 | |
| PTB3 | 35 | PA select | high = on |
| PTB4 | 36 | RX supply | high = on |
| PTB5, PTB7 | 37, 39 | LEDs (which color: ?) | high = on |
| PTB8 | 40 | voice prompt chip enable | high = on |
| PTB9, PTB10 | 41, 42 | side keys | low = pressed (unverified) |
| PTB11 | 43 | PTT; also the cable's serial line | low = pressed |
| PTB12 | 44 | power latch | high = stay on |
| PTB13 | 45 | audio amplifier | high = on (unverified) |
| PTB17 | 49 | power switch sense | level when off: ? |
| PTB26 | 58 | FD6818 GPIO6, its squelch output (measured) | not used |
| PTB27 | 59 | FD6818 GPIO0, the sub-audio compare result (measured) | high = the tone or code is heard |
| PTB28 / PTB29 | 60 / 61 | voice prompt chip clock / data | |
| DAC B | | PA control, after IO_DIPLEX2 bit 28 is cleared | 0 = off |
| DAC C | | routed to its pin (IO_DIPLEX2 bit 29) and set to 1762 at start-up; its use: ? | |
| ADC channel 2 | | battery | divider: ? |
| UART0 | | programming cable, 115200 8N1 under RadioFoundry | |

The transmitter is off with PTB0 and PTB3 low, PTB4 high and DAC B at 0 (`board_rf_safe`).

## 4. Data flash (2 MB, SPI0)

SPI0, mode 0, chip select PTA18 by GPIO. Read 0x03, page program 0x02, 4 KB erase 0x20, status 0x05, write enable 0x06.

| Range | Content |
| --- | --- |
| 0x001300-0x031FFF | the original firmware's settings and channels (kept, never written by RadioFoundry) |
| 0x100000-0x1FDFFF | RadioFoundry: settings, journal, open-firmware codeplug images, native codeplug (`boards/rt10/board.c`) |
| 0x1FE000-0x1FEFFF | activation block tied to the SoC's eFuse ID: never written |
| 0x1FF000-0x1FFFFF | factory calibration (section 4.1): never written |

### 4.1 Factory calibration

Byte tables of 15 entries, one per 10 MHz step. The step for a frequency is (frequency / 10 MHz) minus the base byte,
held between 0 and 14. Offsets are from 0x1FF000. 0xFF is an entry the factory didn't fill. RadioFoundry copies the
tables at start-up (`boards/rt10/calibration_rt10.c`) and transmits only with a valid base.

| Offset | Content |
| --- | --- |
| 0x1A9 | the modem's TX center (SIG_CENTER), one byte |
| 0x470 | base: the first step's frequency / 10 MHz (90 on the radio checked; 80-95 accepted) |
| 0x51D / 0x52C | PA control level / 16 (DAC B), high / low power |
| 0x559 | 15 pairs: the first byte the modulation balance (the modem's MOD_BIAS), the second not used |
| 0x5B5 / 0x5C4 | FD6818 PA gain (0x28), high / low power; the high one is capped at 7 |
| 0x5D3 / 0x5F1 | key-up delay / 10 us, high / low power: RX supply off to the TX state |
| 0x600 | key-down delay / 10 us, before the ramp down |

## 5. Power and transmitter

| Item | Value |
| --- | --- |
| Power latch | PTB12 high from start-up; low lets the radio turn off |
| Power switch | PTB17; the original firmware lets go of the latch after it reads "off" for 500 ms. Which level is off: ? |
| Battery | ADC channel 2; scale ? |
| PA | DAC B drives the PA's power control; levels per frequency step come from the calibration (section 4.1) |
| Power levels | L and H: the calibration's low and high sets |

FM key-up, in the original firmware's order. Measured: the chip's TX state and frequency and the pin levels; the signal
on air is untested.

1. The FD6818's TX words while it still receives (0x40 0x3AD0 with or without sub-audio, the sub-audio, PA gain 0x28
   with the gain in both 3-bit fields), the speaker off, PTB3 and PTB0 low.
2. The key-up delay, then PTB4 (RX supply) low.
3. The FD6818 to the TX frequency, idle, then TX state 0xC1FE (0xC1FA for a tone).
4. DAC B powered, PTB3 and PTB0 high, DAC B ramped to the calibration's level: 180 steps of 2 us on a smoothstep curve.

Key-down: the FD6818 back to receive, the key-down delay, the ramp down, DAC B 0, PTB0 and PTB3 low, DAC B powered
down, 100 us, the receiver retuned, PTB4 high.

## 6. Keys and knob

Two side keys and PTT are direct inputs. The 16-position channel knob gives a 4-bit code on four inputs; the code is not
the position in order (mapping ?).

## 7. FD6818 bus

A two-wire bus (I2C style) on PTA7 (clock) and PTA8 (data), device address 0xE2 write / 0xE3 read, 16-bit registers,
bit-banged (measured). The register map is the FD6818's (`drivers/rf/fd6818/fd6818sh.h`): state 0x03, page select
0x7F. On this bus page 1's registers are addressed from 0x80 (0x7F = 1, then register | 0x80).

| Item | Value |
| --- | --- |
| ID (0x00) | 0x6818 (measured) |
| States (0x03) | receive 0xBFF1 (reads back 0xBFF5), TX 0xC1FE, TX with a tone 0xC1FA; every change through 0 |
| Band | 902-928 MHz is the chip's band above 600 MHz: 0x1A = 0x1F80, 0x40 = 0x3AD0 (measured: -34 dBm read from a signal the band word 0xBF24 left at the floor) |
| Bandwidth (0x43) | 0x4048 at 12.5 kHz, 0x3028 at 25 kHz |
| Receive set | the power-on tables and the analog words in `boards/rt10/radio_rt10.c`, from the original firmware's |
| AF output (0x47) | 0x7140 receive, 0x7040 mute (TX) |
| GPIO | 0x25-0x27: GPIO0 the sub-audio compare, to PTB27; GPIO6 squelch, to PTB26 (measured) |
| Noise (0x65) | no signal: 38-44 raw at 904-927.5 MHz (measured), inside the core's squelch range, so the driver adds the FD6818 offset (`fd6818_noise`) |

Sub-audio decode (measured with CTCSS 100.0 Hz and DCS 023N): the right tone or code raises PTB27, a wrong one and no
carrier leave it low. The chip doesn't tell DCS polarity apart: 023N is heard with 023 set normal or inverted.

## 8. Codec

The AMBE+2 vocoder is part of the original image (code run from RAM, tables copied to modem SRAM at 0x1800420C,
12,152 bytes). Its build differs from the other HR_C7000 radios', so it needs its own codec map. RadioFoundry hosts it
in the codec slot like the other HR_C7000 radios (`boards/rt10/memory.ld`).

## 9. Bring-up state

| Item | State |
| --- | --- |
| Boot from the ROM flag at 0x0300C000 | written |
| Host link on UART0, stub entry, watchdog, crash log | written |
| Power switch level, battery scale, LED colors, key levels, knob map | open |
| FD6818 two-wire bus, FM receive, sub-audio decode | measured |
| Factory calibration read | measured (the radio's own sector) |
| FM transmit | written; the chip's TX state, frequency and the PA pins measured, the signal on air untested |
| DMR, codec map | open |
