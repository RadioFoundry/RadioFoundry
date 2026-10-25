<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# TYT MD-UV380 hardware (MD-UV390, Retevis RT3S, MD-UV390 Plus 10 W)

The TYT MD-UV380, the MD-UV390 (the same radio in an IP67 case), the Retevis RT3S and the MD-UV390 "Plus" 10 W radio are
the DM-1701's board family: STM32F405VGT6, HR_C6000, one AT1846S for both bands, a 160 x 128 color LCD on the 8-bit
8080 bus, a 16 MB SPI NOR flash, a 3-row keypad matrix on the LCD data lines, a quadrature channel encoder, a volume
potentiometer with the power switch, and USB on OTG_FS. GPS is a factory option ("G" models).

This file lists **only what differs from the DM-1701**. Everything not mentioned here is as in
[hardware-dm1701.md](hardware-dm1701.md) (RadioFoundry's working board, `boards/dm1701`), and that file's section 17 already
holds a variants table this file expands.

Status: **nothing in this file has been checked on an MD-UV380.** The port is `boards/mduv380` on the family's shared
code in `boards/uv380_family`: every difference below is an entry of `uvf_variant` or `uvf_key_matrix` in
`boards/mduv380/board.c`, a pin in `boards/uv380_family/pins.h`, or the `POWER` build option. The order to check them on
a radio is [mduv380-bringup.md](../testing/mduv380-bringup.md).

Values marked `(unverified)` are not confirmed on a radio or in a datasheet; `?` means not found or contradictory. As nothing
in this file has been checked on an MD-UV380, treat every value as unverified until a radio confirms it. Sources: the
DM-1701 spec ([hardware-dm1701.md](hardware-dm1701.md)) and the codec spec ([codec-mduv380.md](codec-mduv380.md)).

---

## 1. Summary of differences from the DM-1701

| Area | DM-1701 | MD-UV380 / UV390 / RT3S 5 W | MD-UV390 Plus 10 W | Section |
|---|---|---|---|---|
| Visible LCD rows | 120 (8 hidden rows) | all 128, no row offset | as 5 W | 4 |
| LCD type mapping (sec. reg 3 byte 0x1D) | DM-1701 table; type 2 treated as 3 | MD-UV380 table, always RGB, type 2 kept | as 5 W | 4 |
| Key matrix | DM-1701 table, P1/P2 keys | MD-UV380 table, no P1/P2 | as 5 W | 5 |
| Top (orange) key | row 2 D5 | none: row 2 D5 unused | as 5 W | 5 |
| Side keys | D7 SK1, D6 SK2 | D7 SK1, D6 SK2 | as 5 W | 5 |
| PA band select | PC6 | PC6 | **PD8** | 3 |
| LCD backlight | PD8 | PD8 | **PC6** | 3, 4 |
| Battery ADC | raw / 37.5 per 0.1 V | raw / 40.3 | raw / 40.3 | 6 |
| Volume pot range | 29..2075, break 318 | UV380 22..2070 / 309; RT3S 23..2020 / 317 | 19..2054 / 308 | 6 |
| Power levels | 5 W | 5 W | 10 W (2, 3, 5, 10 W at the top four levels) | 7 |
| Power fractions | DM-1701 rows | 5 W rows | 10 W rows | 7 |
| Working-copy power conversion | DM-1701 row | 5 W row | 10 W row | 8 |
| AT1846S DMR AF gain | needs 0x44 = 0x07FF | less AF gain is enough ([hardware-dm1701.md](hardware-dm1701.md) 5.5) | | 2 |
| GPS | not fitted | G models: USART1 RX PA10, power PA9 | G models | 9 |
| DFU key | DM-1701 key | MD-UV380 key | same | 11 |
| Stock codec | V02.06 at 0x08074714 | V19.26 at 0x0807B830, same sectors | ? | 12 |
| Wake from the alarm / sleep | orange key | SK2 + PTT held together | as 5 W | 5 |
| Open-firmware CPS radio type | 8 (BGR) / 10 (RGB) | 6 | 6 | 10 |

The same between the two: the MCU, clock tree and HSE, internal flash map up to the codec, every
HR_C6000 pin and bus, the AT1846S bus and address, I2S3, the SPI flash bus, PTT pins, LEDs, the encoder pins, the
power latch, the RX supply switches, the PA enables, the APC DAC, the RX audio mux, USB pins.

---

## 2. MCU, clocks, RF chips

As the DM-1701. An 8 MHz crystal as the PLL reference and 72 / 36 / 72 MHz are reported
(unverified); the DM-1701 needed HSE bypass. Whether the MD-UV380's HSE is a crystal or an external clock is open (?):
the DM-1701's start-up code that falls back between the two covers both.

AT1846S: the same I2C pins (SCL PA8, SDA PC9), 7-bit address 0x2E. The I2C3 peripheral is on those pins, as on the DM-1701. AT1846S GPIO0 on PB1 and GPIO6 (squelch out in the DM-1701's 0x1F setting) on PB2 (unverified); neither pin is used
by the DM-1701 port, and the DM-1701 spec has no entry for them (?: may hold on the DM-1701 too).

All the AT1846S register tables of [hardware-dm1701.md](hardware-dm1701.md) apply. The DM-1701 needs more DMR AF gain than the MD-UV380
(5.5) keep 0x44 = 0x07FF unless bench tests show better BER with less.

HR_C6000: as the DM-1701, including the calibration-driven reference oscillator and modulation.

---

## 3. Pin differences

Only two pins differ, and only on the Plus 10 W:

| Pin | DM-1701 / RT84 / MD-UV380 5 W / RT3S | MD-UV390 Plus 10 W |
|---|---|---|
| PC6 | PA band select (low VHF, high UHF) | LCD backlight |
| PD8 | LCD backlight (PWM by DMA from TIM1) | PA band select |

The 5 W column is reported as PC6 for the VHF/UHF PA switch and PD8 for the backlight (unverified). The backlight
dimming of writes BSRR of the backlight pin's port, so on the 10 W radio the DMA target is GPIOC instead
of GPIOD.

Pins those notes list that the DM-1701 spec doesn't (unverified), not used by RadioFoundry today: PB1 AT1846S GPIO0, PB2
AT1846S GPIO6, PC3 not connected on a stock radio.

PB11 / PE14 are the channel selector: the same quadrature encoder (unverified, ?).

---

## 4. Display

Same bus, controller family, init tables and backlight as [hardware-dm1701.md](hardware-dm1701.md) 9, with these differences:

- **No hidden rows.** All 128 rows are visible: no 8-row offset in RASET, RASET end 128, no blank-row clearing. The
  CPS screen grab sends the frame buffer from row 0.
- **Panel type.** Read from SPI flash security register 3, byte 0x1D, low two bits, as on the DM-1701, but with its own
  meaning and **always RGB order**:

  | Type | MD-UV380 meaning | MADCTL written |
  |---|---|---|
  | 0 | rotated 180 degrees | 0xA0 |
  | 1 | normal | 0x60 |
  | 2 | horizontally flipped | 0xE0 (not remapped to type 3 as on the DM-1701) |
  | 3 | rotated 180 degrees | 0xA0 |

  Types 2 and 3 use the inter-command-unlock init table with 0xFD = 0x4F (type 2) or 0x40 (type 3) ([hardware-dm1701.md](hardware-dm1701.md) 9.3). The
  DM-1701 bench found type 2 with MADCTL 0xE0 mirrored top to bottom ([hardware-dm1701.md](hardware-dm1701.md) 20); whether MD-UV380 type 2 panels
  need 0xE0 is unverified (?).
- **10 W identification.** Security register 3 byte 0x1C = 0xF4 marks a Plus 10 W radio (unverified). RadioFoundry can use it to choose the 10 W pin swap and power table at
  run time (?: confirm on both kinds of radio before trusting it; a wrong guess swaps the backlight and the PA band
  select).
- Backlight: PD8 (5 W) or PC6 (10 W), active high.

[radios.md](radios.md) lists HX8302A (unverified).

---

## 5. Keypad, side keys, encoder, PTT

Matrix (rows PA6, PD2, PD3; columns D0-D7 as on the DM-1701):

| Row | D0 | D1 | D2 | D3 | D4 | D5 | D6 | D7 |
|---|---|---|---|---|---|---|---|---|
| Row 0 (PA6) | 1 | 2 | 3 | 4 | 5 | 6 | 0 | * |
| Row 1 (PD2) | green (menu) | up | down | 7 | 8 | 9 | # | red (back) |
| Row 2 (PD3) | - | - | - | - | - | - | side key 2 | side key 1 |

- No P1/P2 keys and no orange key. Up / down are front keys, not
  left / right.
- Side key 1 (D7) is the top side key, the one held with PTT to enter the bootloader.
- Wake from the alarm: SK2 + PTT held together.
- PTT PE11, external PTT PE12, encoder A PE14 / B PB11: as the DM-1701. Encoder direction and
  detent shape: assumed as the DM-1701's (?).

---

## 6. Analog inputs

| Item | MD-UV380 5 W | RT3S | Plus 10 W |
| --- | --- | --- | --- |
| Battery, 0.1 V | raw / 40.3 | raw / 40.3 | raw / 40.3 |
| Volume pot min / max / break | 22 / 2070 / 309 | 23 / 2020 / 317 | 19 / 2054 / 308 |

PA0 is reported at 0-1600 mV and PA1 through a 1:3 divider (unverified, ?). The RT3S reading is unstable at the bottom of the pot. Power-off threshold (5.5 V, switch sensed through the battery line), low-battery thresholds and the
temperature sensor: as the DM-1701.

---

## 7. TX power

Same DAC (PA4) and level scheme as, with these interpolation fractions (220 MHz rows are the old
firmware's untested estimates):

| Radio | Band | f0 | f1 | f2 | f3 |
|---|---|---|---|---|---|
| 5 W | VHF | 0.35 | 0.70 | 0.34 | 0.61 |
| 5 W | 220 MHz | 0.38 | 0.70 | 0.30 | 0.59 |
| 5 W | UHF | 0.40 | 0.70 | 0.25 | 0.55 |
| Plus 10 W | VHF / 220 MHz | 0.58 | 0.83 | 0.21 | 0.45 |
| Plus 10 W | UHF | 0.55 | 0.75 | 0.17 | 0.43 |

On the 10 W radio the top four levels are 2, 3, 5 and 10 W ([hardware-dm1701.md](hardware-dm1701.md) 7). Measured output (one radio) UHF 1.4 / 2.6 / 5.1 W,
VHF 1.1 / 2.1 / 4.3 W (unverified).

---

## 8. Calibration

Same location (security registers 1 and 2, working copy at 0x010000 with the same marker) and the same 512-byte layout
as Only the power conversion when deriving the working copy differs:

| Radio | Band | k0 | k1 | k2 | k3 |
|---|---|---|---|---|---|
| 5 W | VHF / UHF | 0.63 / 0.72 | 1.00 | 1.00 / 1.05 | 1.10 / 1.25 |
| Plus 10 W | VHF / UHF | 0.35 / 0.40 | 0.65 / 0.70 | 1.25 / 1.20 | 1.40 / 1.35 |

The open-firmware CPS calibration window (0x10000, 512 bytes) is served as on the DM-1701.

---

## 9. GPS

G models (MD-UV380G / MD-UV390 GPS / RT3S GPS) carry a JumpStar JS-H210 module on the display / keypad board; the
non-GPS boards have no footprint (unverified). Its NMEA output is on **USART1 RX, PA10**, and its power switch is
**PA9**, high = on: the same pins the DM-1701 spec lists for its unfitted GPS, not the MD-380's
USART3. 9600 8N1. Nothing is sent to the module.

Detection: the same board ships with and without the module (unverified). A listen-first probe as on the MD-380 ([hardware-md380.md](hardware-md380.md) 13.2), with PA9 driven
high for the listen window and back low if nothing is heard, avoids needing a model flag (?: on a non-GPS radio PA9
drives nothing that is known). The vendor firmware files differ between GPS and non-GPS radios (section 11).

---

## 10. USB and model reporting

Same OTG_FS core and descriptors as the DM-1701. The open-firmware CPS radio type is 6 (the DM-1701 reports 8 or 10 by panel color order). RadioFoundry's `tools/cps_client.py` already maps type 6 to "MD-UV380". A single RadioFoundry
image can tell a Plus 10 W radio by section 4's security register byte; it cannot tell an MD-UV380 from an RT3S or
UV390 (no known model byte) (?).

---

## 11. Bootloader and stock images

Entering the bootloader: hold PTT and the top side key (SK1) at power on (unverified). The TYT/AnyRoad DFU
loader, protocol and sector list are the DM-1701's ([hardware-dm1701.md](hardware-dm1701.md) 15.2). The image is XOR-encrypted with the MD-UV380 key, SHA-256
`739ce35cc8db6e72f95598effac1a01ae71dbe2a5792c1a69acaee5dbf634991`, first bytes 00 AA 89 89, the same key for the
MD-UV390 and the MD-2017 ([hardware-dm1701.md](hardware-dm1701.md) 15.3). `stockfw.derive_key` recovers exactly this
key from each of the four 19.26 vendor files (measured).

The initial stack pointer rule (: below the top of RAM) is documented for this radio too (unverified).
Whether the MD-UV380 bootloader checks the same GPIO combination (PE8 low and PE11 low) and the stack pointer word as
the DM-1701's dumped loader is unknown (?: needs a dump from an MD-UV380).

Stock files: OutSecurityBin, two segments (SPI flash resources at 0x00060000 + 0x5D400, the
application at 0x0800C000), blank model field. TYT names four builds per release:

| Prefix | Build | Radios |
|---|---|---|
| V | CSV (large contact list) | non-GPS |
| D | REC (recording) | non-GPS |
| P | CSV-GPS | GPS |
| S | GPS-REC | GPS |

The 19.26 file names and hashes are in
[codec-mduv380.md](codec-mduv380.md). Plus 10 W stock files exist but none was found at a reputable source (?).

---

## 12. Codec

The AMBE+2 codec is in every MD-UV380 stock application, the same library as the DM-1701's with the DM-1701's entry
offsets, relinked. In V19.26 CSV it runs from 0x0807B830 (code, sectors 7-8) and 0x080D8C94 (tables, sectors 10-11):
the DM-1701's sector use exactly, so the DM-1701 flash map (application in sectors 3-6, 336 KB; sector 9 free; codec
kept across updates) applies unchanged. Its output is bit-identical to the DM-1701 codec's in the emulator. Details,
the other three files and the extraction recipe: [codec-mduv380.md](codec-mduv380.md).

---

## 13. Variants

| Radio | Difference |
| --- | --- |
| MD-UV380 | the base 5 W board |
| MD-UV390 | the same radio in an IP67 case |
| Retevis RT3S | rebadged MD-UV380; its own volume pot curve (section 6) |
| G models | JS-H210 GPS on USART1 / PA9 |
| MD-UV390 Plus 10 W | PC6 / PD8 swapped, 10 W power tables, security register 3 byte 0x1C = 0xF4, board layout differs somewhat (unverified) |
| MD-2017 | trackball, two AT1846S; not covered here |

---

## 14. Unknowns that need a radio

| # | Unknown | How to settle it |
|---|---|---|
| 1 | HSE crystal or bypass | USB enumerates on HSE; record which mode started |
| 2 | Panel types 0-3 and their MADCTL, especially type 2 | the boot screen is upright and not mirrored; read sec. reg 3 byte 0x1D |
| 3 | 10 W detection by sec. reg 3 byte 0x1C | read it on a 5 W and a 10 W radio before trusting it for the pin swap |
| 4 | Key matrix and side key order | press each key, compare with section 5 |
| 5 | Encoder direction | clockwise = next channel |
| 6 | Battery divider (40.3) | compare against a meter |
| 7 | Volume pot ranges per model | read the raw value at both ends |
| 8 | GPS on PA10 / PA9, 9600 baud; PA9 on non-GPS radios | G model: sentences arrive; non-G: nothing breaks |
| 9 | AT1846S DMR AF gain | BER against a hotspot at 0x07FF and lower |
| 10 | Power fractions and k factors | power meter at each level, band edges |
| 11 | Bootloader entry check and stack rule | dump the loader (RDP 1 allows the application to read it) |
| 12 | Codec voice on air | DMR TX and RX through the vendor V19.26 codec |
| 13 | PB1 / PB2 (AT1846S GPIO0 / GPIO6) | scope while the squelch opens; not needed for a port |

---

## 15. Sources

This repo: [hardware-dm1701.md](hardware-dm1701.md) (sections cited) and [codec-mduv380.md](codec-mduv380.md). Everything else on this page is unverified.
