<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The OpenDigitalRadio Authors -->

# TYT MD-UV380 hardware (MD-UV390, Retevis RT3S, MD-UV390 Plus 10 W)

The TYT MD-UV380, the MD-UV390 (the same radio in an IP67 case), the Retevis RT3S and the MD-UV390 "Plus" 10 W radio are
the DM-1701's board family: STM32F405VGT6, HR_C6000, one AT1846S for both bands, a 160 x 128 color LCD on the 8-bit
8080 bus, a 16 MB SPI NOR flash, a 3-row keypad matrix on the LCD data lines, a quadrature channel encoder, a volume
potentiometer with the power switch, and USB on OTG_FS. GPS is a factory option ("G" models).

This file lists **only what differs from the DM-1701**. Everything not mentioned here is as in
[hardware-dm1701.md](hardware-dm1701.md) (ODR's working board, `boards/dm1701`), and that file's section 17 already
holds a variants table this file expands.

Status (2026-10-12): **nothing in this file has been checked on an MD-UV380.** No radio is at hand.

Confidence tags as in [radios.md](radios.md) and [hardware-md380.md](hardware-md380.md):

| Tag | Meaning |
|---|---|
| **C** | Confirmed: manufacturer data, a datasheet, our own reading of a stock file, or bench work on the radio |
| **D** | Documented by the reference port ([ref]) or the vendor page ([dealer]), or by our own tool or reading. Facts that rest only on public notes we cannot name are marked (unverified) instead |
| **unverified** | Reported in public notes, a review or a forum we cannot name or cannot check, with no independent basis. Needs a radio |
| **F** | A value carried over from a sibling radio's notes ([dm1701]), or a forum or retailer claim |
| **?** | Not found, contradictory, or our own assumption. Needs a radio |

Sources (section 15): [ref] the reference port's facts (the aftermarket firmware tree's MD-UV380 build variant: pin set-up, per-variant
constants, facts only), [dealer] the Passion Radio MD-UV380 download page, [stock] our own reading of the
TYT 19.26 stock files ([codec-mduv380.md](codec-mduv380.md)), [dm1701] [hardware-dm1701.md](hardware-dm1701.md).

---

## 1. Summary of differences from the DM-1701

| Area | DM-1701 | MD-UV380 / UV390 / RT3S 5 W | MD-UV390 Plus 10 W | Section |
|---|---|---|---|---|
| Visible LCD rows | 120 (8 hidden rows) | all 128, no row offset (D) [ref] | as 5 W | 4 |
| LCD type mapping (sec. reg 3 byte 0x1D) | DM-1701 table; type 2 treated as 3 | MD-UV380 table, always RGB, type 2 kept (D) [ref] | as 5 W | 4 |
| Key matrix | DM-1701 table, P1/P2 keys | MD-UV380 table, no P1/P2 (D) [ref] | as 5 W | 5 |
| Top (orange) key | row 2 D5 | none: row 2 D5 unused (D) [ref] | as 5 W | 5 |
| Side keys | D7 SK1, D6 SK2 | D7 SK1, D6 SK2 (D) [ref] | as 5 W | 5 |
| PA band select | PC6 | PC6 (D) [ref] | **PD8** (D) [ref] | 3 |
| LCD backlight | PD8 | PD8 (D) [ref] | **PC6** (D) [ref] | 3, 4 |
| Battery ADC | raw / 37.5 per 0.1 V | raw / 40.3 (D) [ref] | raw / 40.3 | 6 |
| Volume pot range | 29..2075, break 318 | UV380 22..2070 / 309; RT3S 23..2020 / 317 (D) [ref] | 19..2054 / 308 | 6 |
| Power levels | 5 W | 5 W | 10 W (2, 3, 5, 10 W at the top four levels) | 7 |
| Power fractions | DM-1701 rows | 5 W rows (D) [ref] | 10 W rows (D) [ref] | 7 |
| Working-copy power conversion | DM-1701 row | 5 W row (D) [ref] | 10 W row (D) [ref] | 8 |
| AT1846S DMR AF gain | needs 0x44 = 0x07FF | less AF gain is enough (F) [dm1701] 5.5 | | 2 |
| GPS | not fitted | G models: USART1 RX PA10, power PA9 (D) [ref] | G models | 9 |
| DFU key | DM-1701 key | MD-UV380 key (C) [stock] | same | 11 |
| Stock codec | V02.06 at 0x08074714 | V19.26 at 0x0807B830, same sectors (C) [stock] | ? | 12 |
| Wake from the alarm / sleep | orange key | SK2 + PTT held together (D) [ref] | as 5 W | 5 |
| USB product name (aftermarket firmware) | "OpenDM1701 Transceiver" | "OpenMDUV380 Transceiver" | "OpenMDUV380Plus 10W Transceiver" | 10 |
| Open-firmware CPS radio type | 8 (BGR) / 10 (RGB) | 6 (D) [ref] | 6 | 10 |

The same between the two (D) [ref]: the MCU, clock tree and HSE, internal flash map up to the codec, every
HR_C6000 pin and bus, the AT1846S bus and address, I2S3, the SPI flash bus, PTT pins, LEDs, the encoder pins, the
power latch, the RX supply switches, the PA enables, the APC DAC, the RX audio mux, USB pins.

---

## 2. MCU, clocks, RF chips

As the DM-1701 (D) [ref]. Public notes we cannot name report an 8 MHz crystal as the PLL reference and 72 / 36 / 72 MHz
(unverified); the DM-1701 needed HSE bypass. Whether the MD-UV380's HSE is a crystal or an external clock is open (?):
the DM-1701's start-up code that falls back between the two covers both.

AT1846S: the same I2C pins (SCL PA8, SDA PC9), 7-bit address 0x2E (D) [ref]. The aftermarket firmware tree uses the I2C3 peripheral on those pins, as on the DM-1701 (D) [ref]. The same notes also list the
AT1846S GPIO0 on PB1 and GPIO6 (squelch out in the DM-1701's 0x1F setting) on PB2 (unverified); neither pin is used
by the DM-1701 port, and the DM-1701 spec has no entry for them (?: may hold on the DM-1701 too).

All the AT1846S register tables of [dm1701] 5 apply. The DM-1701 needs more DMR AF gain than the MD-UV380 (F) [dm1701]
5.5: keep 0x44 = 0x07FF unless bench tests show better BER with less.

HR_C6000: as the DM-1701, including the calibration-driven reference oscillator and modulation (D) [ref].

---

## 3. Pin differences

Only two pins differ, and only on the Plus 10 W (D) [ref]:

| Pin | DM-1701 / RT84 / MD-UV380 5 W / RT3S | MD-UV390 Plus 10 W |
|---|---|---|
| PC6 | PA band select (low VHF, high UHF) | LCD backlight |
| PD8 | LCD backlight (PWM by DMA from TIM1) | PA band select |

Public notes we cannot name are reported to agree with the 5 W column (PC6 VHF/UHF PA switch, PD8 backlight) (unverified). The backlight
dimming of [dm1701] 9.4 writes BSRR of the backlight pin's port, so on the 10 W radio the DMA target is GPIOC instead
of GPIOD.

Pins those notes list that the DM-1701 spec doesn't (unverified), not used by ODR today: PB1 AT1846S GPIO0, PB2
AT1846S GPIO6, PC3 not connected on a stock radio.

PB11 / PE14 are the channel selector: the same quadrature encoder (unverified, ?).

---

## 4. Display

Same bus, controller family, init tables and backlight as [dm1701] 9, with these differences (D) [ref]:

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

  Types 2 and 3 use the inter-command-unlock init table with 0xFD = 0x4F (type 2) or 0x40 (type 3) [dm1701] 9.3. The
  DM-1701 bench found type 2 with MADCTL 0xE0 mirrored top to bottom ([dm1701] 20); whether MD-UV380 type 2 panels
  need 0xE0 is unverified (?).
- **10 W identification.** Security register 3 byte 0x1C = 0xF4 marks a Plus 10 W radio (D) [ref]; the aftermarket firmware
  reads but doesn't use it (it is a build variant there). ODR can use it to choose the 10 W pin swap and power table at
  run time (?: confirm on both kinds of radio before trusting it; a wrong guess swaps the backlight and the PA band
  select).
- Backlight: PD8 (5 W) or PC6 (10 W), active high.

[radios.md](radios.md) lists HX8302A (unverified).

---

## 5. Keypad, side keys, encoder, PTT

Matrix (rows PA6, PD2, PD3; columns D0-D7 as on the DM-1701) (D) [ref]:

| Row | D0 | D1 | D2 | D3 | D4 | D5 | D6 | D7 |
|---|---|---|---|---|---|---|---|---|
| Row 0 (PA6) | 1 | 2 | 3 | 4 | 5 | 6 | 0 | * |
| Row 1 (PD2) | green (menu) | up | down | 7 | 8 | 9 | # | red (back) |
| Row 2 (PD3) | - | - | - | - | - | - | side key 2 | side key 1 |

- No P1/P2 keys and no orange key. Up / down are front keys (KEY_INCREASE / DECREASE on the aftermarket firmware), not
  left / right.
- Side key 1 (D7) is the top side key, the one held with PTT to enter the bootloader (D) [ref].
- The aftermarket firmware lets SK1 + green stand in for the missing orange key (D) [ref]: a UI choice, not hardware.
- Wake from the alarm: SK2 + PTT held together (D) [ref].
- PTT PE11, external PTT PE12, encoder A PE14 / B PB11: as the DM-1701 (D) [ref]. Encoder direction and
  detent shape: assumed as the DM-1701's (?).

---

## 6. Analog inputs

| Item | MD-UV380 5 W | RT3S | Plus 10 W | Tag |
|---|---|---|---|---|
| Battery, 0.1 V | raw / 40.3 | raw / 40.3 | raw / 40.3 | (D) [ref] |
| Volume pot min / max / break | 22 / 2070 / 309 | 23 / 2020 / 317 | 19 / 2054 / 308 | (D) [ref] |

The aftermarket firmware builds the RT3S pot values for every non-10 W MD-UV380 build, with 7 counts of margin at the bottom
because the RT3S reading is unstable there (D) [ref]. Public notes we cannot name give PA0 at 0-1600 mV and PA1 through a 1:3 divider
(unverified, ?). Power-off threshold (5.5 V, switch sensed through the battery line), low-battery thresholds and the
temperature sensor: as the DM-1701 (D) [ref].

---

## 7. TX power

Same DAC (PA4) and level scheme as [dm1701] 7, with these interpolation fractions (D) [ref] (220 MHz rows are the old
firmware's untested estimates):

| Radio | Band | f0 | f1 | f2 | f3 |
|---|---|---|---|---|---|
| 5 W | VHF | 0.35 | 0.70 | 0.34 | 0.61 |
| 5 W | 220 MHz | 0.38 | 0.70 | 0.30 | 0.59 |
| 5 W | UHF | 0.40 | 0.70 | 0.25 | 0.55 |
| Plus 10 W | VHF / 220 MHz | 0.58 | 0.83 | 0.21 | 0.45 |
| Plus 10 W | UHF | 0.55 | 0.75 | 0.17 | 0.43 |

On the 10 W radio the top four levels are 2, 3, 5 and 10 W ([dm1701] 7). The aftermarket firmware's calibration screen labels
the four calibrated points 250 mW, 1, 2, 5 W (5 W) or 250 mW, 1, 5, 10 W (10 W), and offers a "force 10 W" setting on
5 W builds for 10 W radios flashed with a 5 W image (D) [ref]. Measured stock output (one radio) UHF 1.4 / 2.6 / 5.1 W,
VHF 1.1 / 2.1 / 4.3 W (unverified).

---

## 8. Calibration

Same location (security registers 1 and 2, working copy at 0x010000 with the same marker) and the same 512-byte layout
as [dm1701] 12 (D) [ref]. Only the power conversion when deriving the working copy differs (D) [ref]:

| Radio | Band | k0 | k1 | k2 | k3 |
|---|---|---|---|---|---|
| 5 W | VHF / UHF | 0.63 / 0.72 | 1.00 | 1.00 / 1.05 | 1.10 / 1.25 |
| Plus 10 W | VHF / UHF | 0.35 / 0.40 | 0.65 / 0.70 | 1.25 / 1.20 | 1.40 / 1.35 |

The open-firmware CPS calibration window (0x10000, 512 bytes) is served as on the DM-1701.

---

## 9. GPS

G models (MD-UV380G / MD-UV390 GPS / RT3S GPS) carry a JumpStar JS-H210 module on the display / keypad board; the
non-GPS boards have no footprint (unverified). Its NMEA output is on **USART1 RX, PA10**, and its power switch is
**PA9**, high = on (D) [ref]: the same pins the DM-1701 spec lists for its unfitted GPS, not the MD-380's
USART3. 9600 8N1 (D) [ref]. Nothing is sent to the module.

Detection: the same board ships with and without the module, and stock GPS firmware doesn't add GPS to a non-GPS radio
(unverified). A listen-first probe as on the MD-380 ([hardware-md380.md](hardware-md380.md) 13.2), with PA9 driven
high for the listen window and back low if nothing is heard, avoids needing a model flag (?: on a non-GPS radio PA9
drives nothing that is known). The stock files differ between GPS and non-GPS radios (section 11), and a forum report
says a GPS stock image hangs at the boot screen on a non-GPS radio (F); whether that is a GPS probe or something else is
unknown (?).

---

## 10. USB and model reporting

Same OTG_FS core and descriptors as the DM-1701. The aftermarket firmware reports radio type 6 to the open-firmware CPS (the
DM-1701 reports 8 or 10 by panel color order) and names the product "OpenMDUV380 Transceiver" or
"OpenMDUV380Plus 10W Transceiver" (D) [ref]. ODR's `tools/cps_client.py` already maps type 6 to "MD-UV380". A single ODR
image can tell a Plus 10 W radio by section 4's security register byte; it cannot tell an MD-UV380 from an RT3S or
UV390 (no known model byte) (?).

---

## 11. Bootloader and stock images

Entering the bootloader: hold PTT and the top side key (SK1) at power on (unverified). The TYT/AnyRoad DFU
loader, protocol and sector list of [dm1701] 15.2 (D) [ref]. The image is XOR-encrypted with the MD-UV380 key, SHA-256
`739ce35cc8db6e72f95598effac1a01ae71dbe2a5792c1a69acaee5dbf634991`, first bytes 00 AA 89 89, the same key for the
MD-UV390 and the MD-2017 (D) [dm1701] 15.3. **Confirmed (C, 2026-10-12):** `stockfw.derive_key` recovers exactly this
key from each of the four 19.26 stock files.

The initial stack pointer rule ([dm1701] 1.5: below the top of RAM) is documented for this radio too (unverified).
Whether the MD-UV380 bootloader checks the same GPIO combination (PE8 low and PE11 low) and the stack pointer word as
the DM-1701's dumped loader ([dm1701] 15.4) is unknown (?: needs a dump from an MD-UV380).

Stock files (C) [stock], [dealer]: OutSecurityBin, two segments (SPI flash resources at 0x00060000 + 0x5D400, the
application at 0x0800C000), blank model field. TYT names four builds per release (D) [dealer]:

| Prefix | Build | Radios |
|---|---|---|
| V | CSV (large contact list) | non-GPS |
| D | REC (recording) | non-GPS |
| P | CSV-GPS | GPS |
| S | GPS-REC | GPS |

(The file names in the 19.26 packages follow the dealer's table.) The 19.26 file names and hashes are in
[codec-mduv380.md](codec-mduv380.md). Plus 10 W stock files exist but none was found at a reputable source (?).

---

## 12. Codec

The AMBE+2 codec is in every MD-UV380 stock application, the same library as the DM-1701's with the DM-1701's entry
offsets, relinked. In V19.26 CSV it runs from 0x0807B830 (code, sectors 7-8) and 0x080D8C94 (tables, sectors 10-11):
the DM-1701's sector use exactly, so the DM-1701 flash map (application in sectors 3-6, 336 KB; sector 9 free; codec
kept across updates) applies unchanged. Its output is bit-identical to the DM-1701 codec's in the emulator (C). Details,
the other three files and the extraction recipe: [codec-mduv380.md](codec-mduv380.md).

---

## 13. Variants

| Radio | Difference | Tag |
|---|---|---|
| MD-UV380 | the base 5 W board | (unverified) |
| MD-UV390 | the same radio in an IP67 case | (unverified) |
| Retevis RT3S | rebadged MD-UV380; its own volume pot curve (section 6) | (D) [ref] |
| G models | JS-H210 GPS on USART1 / PA9 | (unverified) |
| MD-UV390 Plus 10 W | PC6 / PD8 swapped, 10 W power tables, security register 3 byte 0x1C = 0xF4, board layout differs somewhat (unverified) | (D) [ref] |
| MD-2017 | trackball, two AT1846S; not covered here | (D) [dm1701] 17 |

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
| 12 | Codec voice on air | DMR TX and RX through the stock V19.26 codec |
| 13 | PB1 / PB2 (AT1846S GPIO0 / GPIO6) | scope while the squelch opens; not needed for a port |

---

## 15. Sources

| Short name | Source | Kind |
|---|---|---|
| [ref] | The reference port's MD-UV380 build variant (old tree, `MDUV380_firmware`, which builds the DM-1701, RT84, MD-UV380 and MD-UV390 Plus 10 W from one source): pin set-up and per-variant constants, facts only | (D) |
| [dealer] | Passion Radio MD-UV380 page and its firmware downloads, https://passion-radio.com/dmr-equipment/tyt-md380-321.html | (F) |
| [stock] | Our own reading of the TYT 19.26 stock files (headers, key derivation, codec windows) | (C) |
| [dm1701] | [hardware-dm1701.md](hardware-dm1701.md), sections cited | This repo |

No code was copied for this file; the reference port was read for facts only.
