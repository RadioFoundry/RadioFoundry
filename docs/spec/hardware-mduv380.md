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

Status: **nothing in this file has been run on an MD-UV380.** Where a value says "the TYT 19.26 images", it was read
from the four TYT 19.26 images ([codec-mduv380.md](codec-mduv380.md)): that settles what the vendor software does, not
what a radio does. The port is `boards/mduv380` on the family's shared
code in `boards/uv380_family`: every difference below is an entry of `uvf_variant` or `uvf_key_matrix` in
`boards/mduv380/board.c`, a pin in `boards/uv380_family/pins.h`, or the `POWER` build option. The order to check them on
a radio is [mduv380-bringup.md](../testing/mduv380-bringup.md).

Values marked `(unverified)` are not confirmed on a radio or in a datasheet; `?` means not found or contradictory. As nothing
in this file has been run on an MD-UV380, treat every value as unverified until a radio confirms it. Values marked
"(images)" are read from the TYT 19.26 images. Sources: the
DM-1701 spec ([hardware-dm1701.md](hardware-dm1701.md)) and the codec spec ([codec-mduv380.md](codec-mduv380.md)).

---

## 1. Summary of differences from the DM-1701

| Area | DM-1701 | MD-UV380 / UV390 / RT3S 5 W | MD-UV390 Plus 10 W | Section |
|---|---|---|---|---|
| Visible LCD rows | 120 (8 hidden rows) | all 128, no row offset (the vendor images use no offset on either radio, section 4) | as 5 W | 4 |
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
| Stock codec | V02.06 at 0x08074714 | four 19.26 builds, same codec, different addresses | ? | 12 |
| Wake from the alarm / sleep | orange key | SK2 + PTT held together | as 5 W | 5 |
| Open-firmware CPS radio type | 8 (BGR) / 10 (RGB) | 6 | 6 | 10 |

The same between the two: the MCU, clock tree and HSE, internal flash map up to the codec, every
HR_C6000 pin and bus, the AT1846S bus and address, I2S3, the SPI flash bus, PTT pins, LEDs, the encoder pins, the
power latch, the RX supply switches, the PA enables, the APC DAC, the RX audio mux, USB pins.

---

## 2. MCU, clocks, RF chips

As the DM-1701: an 8 MHz HSE, PLL M 8, N 288, P 4, Q 6, SYSCLK 72 MHz, AHB /1, APB2 72 MHz, APB1 36 MHz, flash latency
5 wait states with the caches on (images). The HSE is a **crystal**: the images start it with bypass off. The port
tries the crystal first and falls back to bypass when it never starts (`UVF_HSE_XTAL_FIRST`), and the diagnostics
page says which mode started.

The vendor clock library assumes a 12 MHz crystal (SYSCLK 108 MHz, APB2 108 MHz, APB1 54 MHz); the real clocks are the
72 / 72 / 36 MHz above. Only the I2C setup (it asks for 100 kHz, which comes out near 66.7 kHz on the real APB1) and
the GPS UART use the library's numbers; the timers use fixed constants (images). The GPS consequence is in section 9.

AT1846S: the same I2C pins (SCL PA8, SDA PC9), 7-bit address 0x2E. The I2C3 peripheral is on those pins, as on the DM-1701. AT1846S GPIO0 on PB1 and GPIO6 (squelch out in the DM-1701's 0x1F setting) on PB2 (unverified); neither pin is used
by the DM-1701 port, and the DM-1701 spec has no entry for them (?: may hold on the DM-1701 too).

The AT1846S register tables of [hardware-dm1701.md](hardware-dm1701.md) apply, with these differences in the images:

- **Init.** The same registers and values, plus one write, 0x0F = 0x8A24 (the DM-1701's init step 27 lists it too).
  0x44 is 0x07FF at init (DMR 100 %) and 0x04FF in a second mode function, as on the DM-1701. The DM-1701 needs more DMR
  AF gain than the MD-UV380 (5.5): keep 0x44 = 0x07FF unless bench tests show better BER with less.
- **Frequency set.** The routine writes reg 0x05 from 0x8443, 0x8543, 0x8643 or 0x8763, chosen by testing the frequency
  against multiples of 26 MHz and 182 MHz (spur avoidance), where the DM-1701 image writes 0x8643 once. It ends with
  reg 0x30 = 0x4026 (RX on, band bits 00); the DM-1701 image writes that only in its callers.

HR_C6000: as the DM-1701, including the calibration-driven reference oscillator and modulation. The register sequences
in the images equal the DM-1701's except: extra 0xE4 = 0x27 writes in the FM TX setup; no 0xE4 = 0x24 / 0x23 writes in
two other setups; a few page-4 0x10 = 0x6C / 0x40 writes in a different order; different page-4 0x35 and 0x37 table
values (images). Which of these matter on air is open.

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

PB11 / PE14 are the channel selector: the same quadrature encoder (section 5).

Other pins the images drive, as on the DM-1701 (images): backlight PD8 (high = on), PA band select PC6 (high above
200 MHz, else low; PC5 is set in the TX path), green LED PE0, red LED PE1, speaker mute PB8, audio amplifier PB9, mic
power PA13, RX switches PA2 and PA5, RX audio mux PD9. The beep is PWM on PC8 (TIM8 CH3; PC7 is set up as TIM8 CH2
too): the images set PC7 and PC8 to AF3 (inferred, not seen run).

---

## 4. Display

Same bus, controller family, init tables and backlight as [hardware-dm1701.md](hardware-dm1701.md) 9, with these differences:

- **No hidden rows.** All 128 rows are visible: no 8-row offset in RASET, RASET end 128, no blank-row clearing. The
  CPS screen grab sends the frame buffer from row 0.
- **Panel type.** Read from SPI flash security register 3, byte 0x1D, low two bits, as on the DM-1701. The MD-UV380
  image keeps all four types, where the DM-1701 image knows only one init path. What the images do per type (images):

  | Type | Init table | 0xFD parameter | MADCTL written |
  |---|---|---|---|
  | 0 | ST7735-class ([hardware-dm1701.md](hardware-dm1701.md) 9.3) | - | 0x08 |
  | 1 | same | - | 0xC8 |
  | 2 | the 0xFE / 0xEF unlock table (same section) | 0x4F | 0x88 |
  | 3 | same | 0x40 | 0x08 |

  The DM-1701 image has only the ST7735-class path (frame rate parameters 05 04 04, extra 0xF0 0x01 and 0xFC 0x80),
  MADCTL 0xC8 for type 1 and 0x08 for every other type, and no unlock table. In both images the pixel window is CASET =
  y and RASET = 159 - x, with no offset (so the images show no row offset difference between the two radios).

  The port maps each factory type to the same driver type (identity mapping) and always sets RGB order, with its
  driver's own MADCTL values (0xA0 / 0x60 / 0xE0 / 0xA0 for types 0-3). Open: the BGR bit (0x08) is set in all four
  vendor values and how that relates to the port's RGB setting; the DM-1701 bench found type 2 with MADCTL 0xE0
  mirrored top to bottom ([hardware-dm1701.md](hardware-dm1701.md) 20), and whether MD-UV380 type 2 panels need it is
  unverified (?).
- **10 W identification.** Security register 3 byte 0x1C = 0xF4 was reported as the Plus 10 W marker (unverified). The
  TYT 19.26 images **never read byte 0x1C**, so nothing in them confirms it. The port reads it at start-up: a 5 W image
  refuses to transmit when the marker is present (it would drive the PA band select through the backlight pin), and a
  10 W image does not require it, or a Plus without the marker could never transmit. A wrong guess swaps the backlight
  and the PA band select, so confirm on both kinds of radio before relying on it.
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

The matrix and the scan are the same in all four 19.26 images (images). Row 2 has no entries at D0-D5, and no matrix
position is an orange / emergency key (the DM-1701's row 2 D5 is unused).

Scan: **active low.** The rows are driven low with the columns read through pull-ups, then the columns driven low and
the rows read through pull-ups; two or more low columns or rows are rejected as ghosts. The scan runs only while the
LCD chip select (PD6) is high. This differs from the DM-1701 spec's description ([hardware-dm1701.md](hardware-dm1701.md) 10.1:
rows driven high, columns pulled down); the port uses the shared scan of the DM-1701 either way.

Confidence of the table (images): the digits, `*`, `#` and up / down are read from the key handlers. Red (row 1 D7) and
green (row 1 D0) rest on the same code as the DM-1701 image, where red is bench-confirmed, as do the two side keys;
their top / bottom order follows the DM-1701's and the image does not state it. The factory side-key functions are not in
the application (they come from the codeplug).

- No P1/P2 keys and no orange key. Up / down are front keys, not
  left / right.
- Side key 1 (D7) is the top side key, the one held with PTT to enter the bootloader.
- Wake from the alarm: SK2 + PTT held together.
- PTT PE11 (sampled three times, debounced), external PTT PE12, encoder A PE14 / B PB11: as the DM-1701.
- **Encoder.** Direction is the same as the DM-1701's; only the sampled edge differs (images). The MD-UV380 image
  interrupts on the B line (PB11) and the DM-1701 image on the A line (PE14), both on both edges, and each posts one
  virtual key per edge. With the same wiring, A leading B gives "next" in both, so the direction is not reversed.
  Which rotation is clockwise and the edges per detent are not in the images (?).
- **Power-on combinations** (images). With PTT released at start, a row-2 key plus D6 toggles a stored frequency-range
  preset (the marker is in the copy of security register 3): D6 and D7 together select 450-460 / 150-160 MHz, D6 alone
  446.00625-446.19375 / 149.025-149.1125 MHz. PTT held plus a row-2 key plus D6 enters a hidden test mode (it shows
  "TEST.."). The application has no bootloader entry (no jump, no magic RAM word): the PTT + side key entry is the
  bootloader's own. RadioFoundry implements none of these presets or the test mode.

---

## 6. Analog inputs

| Item | MD-UV380 5 W | RT3S | Plus 10 W |
| --- | --- | --- | --- |
| Battery, 0.1 V | raw / 40.3 | raw / 40.3 | raw / 40.3 |
| Volume pot min / max / break | 22 / 2070 / 309 | 23 / 2020 / 317 | 19 / 2054 / 308 |

PA0 is reported at 0-1600 mV and PA1 through a 1:3 divider (unverified, ?). The RT3S reading is unstable at the bottom of the pot. Power-off threshold (5.5 V, switch sensed through the battery line) and the
temperature sensor: as the DM-1701.

The images (images): ADC1 scans six ranks (IN9, IN1, IN0, IN3, IN0, IN1), as in the DM-1701 image; the battery is IN1
on PA1, the volume IN0. The battery value is the average of 8 samples in raw counts. The low and high battery levels
come from security register 1 table bytes 2 and 3, times 16 counts; the defaults, used when the byte is 0xFF or above
0xC9, are low 0x940 (2368) and high 0xC32 (3122) counts. The raw / 40.3 volt scale is not in the images (they compare
raw counts), so it is not confirmed.

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

The same 512-byte layout as the DM-1701's (security registers 1 and 2, working copy at 0x010000 with the same marker for
the open-firmware CPS). **The TYT images read only the factory registers** (images): security registers 1 and 2 (256 B
each, at 0x1000 and 0x2000) go into one 512-byte RAM table, and no working copy at flash 0x10000 is read.

- Power rows (UHF at 0x10, VHF at 0x19, then 0x20...) hold real points at the even offsets; the images overwrite the
  odd slots with the average of their neighbors.
- The DAC gets the row byte times 255, left-aligned (about the byte times 16 in 12 bits).
- Table bytes 0x02 and 0x03 are the battery level thresholds (section 6).

Only the power conversion when deriving the working copy differs:

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
USART3. Receive only, 8N1, a 100-byte interrupt-driven line buffer. **9600 baud on the wire** (images): the images ask the vendor
library for 14400, which assumes the 12 MHz crystal of section 2, and 14400 x 8 / 12 = 9600. The parser takes NMEA RMC
(talker GP or GN, status 'A'). PA9 is driven only in the GPS images (the module's power gate); the non-GPS images never
drive it. Nothing is sent to the module. The port tries 9600 first and keeps 14400 in the hunt until a G model confirms.

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

The application carries the text "Digital Radio in USB mode" and the string "AnyRoad Technology" and no USB VID / PID
literal (images). It is plain and executed in place from 0x0800C000, so a flash readout equals the decrypted
application by address.

The initial stack pointer rule (: below the top of RAM) is documented for this radio too (unverified).
Whether the MD-UV380 bootloader checks the same GPIO combination (PE8 low and PE11 low) and the stack pointer word as
the DM-1701's dumped loader is unknown (?: needs a dump from an MD-UV380).

The images read SPI flash at fixed addresses 0x2008, 0x2040, 0x2054, 0x2080, 0x20F0, 0x2100, 0x2154, 0x5A70, 0x416D0,
0x45100, 0x48B00, 0x48D00, 0x48F00 and 0x490A0, the same as the DM-1701 image; its other reads use computed addresses
(images). Security register 3 bytes read: 0x11 and 0x12 (the band limits, below), 0x14-0x1B, 0x1D (bits 0-1 LCD type;
bits 2, 5 and 6 are UI feature flags of unknown meaning), 0x20, 0x34 and 0x38.

Band limits: when security register 3 bytes 0x11 and 0x12 read 0xFF, the images use the defaults UHF 400-480 MHz and VHF
136-174 MHz (images).

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
offsets, relinked, at a different address in each of the four 19.26 builds. Its output is bit-identical to the
DM-1701 codec's in the emulator for all four. The merge places it like the DM-1701's: tables at their stock address,
the code after them, the descriptor at the end of sector 11; the application has sectors 3-9 and "keep codec" works.
Addresses, tags and checksums, and the extraction recipe: [codec-mduv380.md](codec-mduv380.md).

---

## 13. Variants

| Radio | Difference |
| --- | --- |
| MD-UV380 | the base 5 W board |
| MD-UV390 | the same radio in an IP67 case |
| Retevis RT3S | rebadged MD-UV380; its own volume pot curve (section 6) |
| G models | JS-H210 GPS on USART1 / PA9 |
| MD-UV390 Plus 10 W | PC6 / PD8 swapped, 10 W power tables, security register 3 byte 0x1C = 0xF4 reported but never read by the TYT images, board layout differs somewhat (unverified) |
| MD-2017 | trackball, two AT1846S; not covered here |

---

## 14. Unknowns that need a radio

The 19.26 images settled what the vendor software does; nothing has run on a radio, so every row is still unverified
there.

| # | Unknown | State | How to settle it |
|---|---|---|---|
| 1 | HSE crystal or bypass | images: crystal, bypass off; the port tries crystal first | USB enumerates on HSE; read which mode started |
| 2 | Panel types 0-3: MADCTL, RGB / BGR | images: init table and MADCTL per type (section 4); the BGR bit and type 2 orientation open | the boot screen is upright, not mirrored and the right colors; read sec. reg 3 byte 0x1D |
| 3 | 10 W detection by sec. reg 3 byte 0x1C | open: the images never read the byte, so 0xF4 is unconfirmed | read it on a 5 W and a 10 W radio before trusting it for the pin swap |
| 4 | Key matrix and side key order | images: matrix as section 5; red / green and the side key order inferred from the DM-1701 image | press each key, compare with section 5 |
| 5 | Encoder direction | images: same direction as the DM-1701; clockwise and edges per detent open | clockwise = next channel |
| 6 | Battery divider (40.3) | open: no scale in the images; thresholds are raw counts | compare against a meter |
| 7 | Volume pot ranges per model | open | read the raw value at both ends |
| 8 | GPS on PA10 / PA9, 9600 baud; PA9 on non-GPS radios | images: 9600 on the wire, PA9 driven only in the GPS images | G model: sentences arrive; non-G: nothing breaks |
| 9 | AT1846S DMR AF gain | open | BER against a hotspot at 0x07FF and lower |
| 10 | Power fractions and k factors | open; the images' power rows are averaged between points (section 8) | power meter at each level, band edges |
| 11 | Bootloader entry check and stack rule | open; the application has no entry code | dump the loader (RDP 1 allows the application to read it) |
| 12 | Codec voice on air | open: bit-identical in the emulator for all four builds | DMR TX and RX through the placed codec |
| 13 | PB1 / PB2 (AT1846S GPIO0 / GPIO6) | open; not needed for a port | scope while the squelch opens |
| 14 | HR_C6000 differences (section 2) and AT1846S reg 0x05 cases | images only | DMR and FM TX / RX at the band edges |
| 15 | USB VID / PID of the application | not in the images | enumerate on a radio |
| 16 | Model byte to tell an MD-UV380 from an RT3S or UV390 | not found | none known |
| 17 | Beep PWM on PC8 | images set the pin to TIM8 CH3; details not read | scope the pin |

---

## 15. Sources

This repo: [hardware-dm1701.md](hardware-dm1701.md) (sections cited) and [codec-mduv380.md](codec-mduv380.md). Everything else on this page is unverified.
