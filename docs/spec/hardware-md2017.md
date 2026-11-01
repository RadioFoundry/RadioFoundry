<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# TYT MD-2017 hardware (Retevis RT82)

The TYT MD-2017 and the Retevis RT82 (the same radio) are the DM-1701's board family: STM32F405VGT6, HR_C6000, a
160 x 128 color LCD on the 8-bit 8080 bus, a 16 MB SPI NOR flash, a 3-row keypad matrix on the LCD data lines, a volume
potentiometer with the power switch, and USB on OTG_FS. Unlike the rest of the family it has **two AT1846S**, a
four-way navigation ring with a center press instead of a channel knob, three side keys, P1 and P2 front keys and an
orange top key. GPS is a factory option.

This file lists **only what differs from the DM-1701**. Everything not mentioned here is as in
[hardware-dm1701.md](hardware-dm1701.md) (RadioFoundry's working board).

Status: **nothing in this file has been run on an MD-2017.** Every value was read from the TYT 4.05 and 4.15 stock
images and the Retevis RT82 images ([codec-md2017.md](codec-md2017.md)): that settles what the vendor software does,
not what a radio does. The port is `boards/md2017` on the family's shared code in `boards/uv380_family`: each difference
below is an entry of `uvf_variant` or `uvf_key_matrix` in `boards/md2017/board.c` or a `UVF_PINS_MD2017` pin in
`boards/uv380_family/pins.h`. The order to check them on a radio is [md2017-bringup.md](../testing/md2017-bringup.md).

Tags: (images) read from the stock images, (derived) inferred from the images' use of a pin or value, `?` open.

---

## 1. Summary of differences from the DM-1701

| Area | DM-1701 | MD-2017 | Section |
|---|---|---|---|
| AT1846S | one, I2C3 on PA8 / PC9, 0x2E | two on I2C1 PB6 SCL / PB7 SDA: 0x2E receives and transmits, 0x71 is a second receiver | 2 |
| HR_C6000 config SPI | CLK PE3, MOSI PE4, MISO PE5 | CLK PC13, MOSI PE5, MISO PE3; CS PE2 as DM-1701 | 3 |
| RX supply switches | PA2 UHF, PA5 VHF | main receiver PC9 UHF, PA5 VHF; second receiver PA8 UHF, PA2 VHF | 3 |
| PA band select PC6 | high = UHF | **high = VHF** | 3 |
| Mic power | PA13 output | none: PA13 is an input with pull-up (headset detect, derived); SWD is free | 3 |
| Second receiver audio | none | PD8 gate, beside the PD9 mux | 3 |
| LCD backlight | PD8 | PB2 (stock never dims it) | 3, 4 |
| Visible LCD rows | 120 (8 hidden rows) | all 128, no offset | 4 |
| LCD type mapping | DM-1701 table | MD-UV380 table, always RGB | 4 |
| Key matrix | DM-1701 table | own table (section 5) | 5 |
| Channel knob | quadrature encoder | none; four-way ring on PB10 / PB11 / PE15 / PE14 plus center press | 5 |
| Top key | matrix row 2 D5 | PE13, active low | 5 |
| Side keys | SK1, SK2 | SK1, SK2, SK3 | 5 |
| CPS radio type | 8 / 10 | 9 | 10 |
| DFU key | DM-1701 key | MD-UV380 key | 11 |

The same as the DM-1701 (images): the MCU and clock tree with an 8 MHz crystal, the internal flash map, the HR_C6000
voice SPI2 (CS PB12) and I2S3, interrupts PC0-PC2, HR_C6000 power-down PE6, the SPI flash and its security registers,
PTT PE11, external PTT PE12, the PA enables PC5 and PC4, the APC DAC, beep PC8, sub-audio PC7, speaker mute PB8, amp
enable PB9, the RX audio mux PD9 (high = HR_C6000 line out), the power latch PA7, LEDs PE0 / PE1, the ADC channel list,
USB on PA11 / PA12.

---

## 2. AT1846S: two chips

Both chips share I2C1 on PB6 (SCL) and PB7 (SDA), bit-banged by the images. 7-bit addresses (images):

| Address | Role |
|---|---|
| 0x2E | main: receives and transmits on both bands |
| 0x71 | second receiver: receives only, never transmits |

0x2E and 0x71 are the AT1846S's two datasheet addresses, SEN high and low ([hardware-dm1701.md](hardware-dm1701.md)
5.1): the board straps one chip each way. RadioFoundry drives the bus with the I2C1 peripheral (AF4).

Each chip gets the DM-1701's init sequence and 23-pair register table unchanged ([hardware-dm1701.md](hardware-dm1701.md)
5.2), and the same register 0x30 values (images). The images transmit only on 0x2E, for either band. They power both
chips down once at init, then keep both awake while running; the chip not being listened to is muted by its own
registers (images):

| State | reg 0x41 | reg 0x44 |
|---|---|---|
| muted | 0x4022 | 0x00FF |
| unmuted | 0x4122 | 0x07FF |

Whether the images run both receivers at once (dual watch) or one at a time is set by a flag not yet decoded (?).

RadioFoundry initializes both chips, then holds 0x71 idle (RX and TX off) with its RX supply switches off: one receiver
at a time until the core can receive two channels.

---

## 3. Pin differences

| Pin | DM-1701 | MD-2017 | Source |
|---|---|---|---|
| PB6 / PB7 | | AT1846S I2C1 SCL / SDA | images |
| PA8 / PC9 | AT1846S I2C3 SCL / SDA | RX supply: PA8 second receiver UHF, PC9 main receiver UHF (1 = on) | images |
| PA2 | RX supply UHF | RX supply, second receiver VHF | images |
| PA5 | RX supply VHF | RX supply, main receiver VHF | images |
| PC6 | PA band select, high = UHF | PA band select, **high = VHF** | images |
| PC13 | | HR_C6000 config SPI CLK | images |
| PE3 | HR_C6000 config SPI CLK | HR_C6000 config SPI MISO | images |
| PE4 | HR_C6000 config SPI MOSI | not SPI: high in normal running, low after TX stop; role ? | images |
| PE5 | HR_C6000 config SPI MISO | HR_C6000 config SPI MOSI | images |
| PA13 | mic power (SWDIO) | input with pull-up; speaker muted while low (headset detect, derived) | images |
| PA14 | set high | never configured | images |
| PB2 | | LCD backlight, high = on | derived |
| PD8 | LCD backlight | second receiver audio gate | derived |
| PE13 | | orange top key, active low, pull-up | images |
| PB10 / PB11 / PE15 / PE14 | (PB11 / PE14: encoder) | navigation ring | images |

All RX supply pins are low while transmitting or off. TX start (images): RX supplies low, PC5 high, PC6 by band,
PC4 high; TX stop: PC5 low, then PC4 low.

**HR_C6000 config port** (images): CS PE2 low for the transfer; per bit CLK high, MOSI set (bit 7 first), CLK low,
MISO sampled. CLK idles low. The family's bit-bang timing applies; the clock line is on a different port from the other
three (`hrc_bus_t.clk_port`).

**Audio gates** (derived): PD9 and PD8 are written as a pair by the images. With the main receiver's digital audio on
the speaker they are (PD9, PD8) = (1, 0); the second receiver's is (0, 1). RadioFoundry holds PD8 low.
The full table per receiver mode is open (?).

The images configure no mic power pin: the mic is likely powered whenever the radio is on (?).

---

## 4. Display

Same bus, controller family, init tables and panel-type source as [hardware-dm1701.md](hardware-dm1701.md) 9, with the
MD-UV380's mapping (images):

| Type (sec. reg 3 byte 0x1D & 3) | Controller | MADCTL |
|---|---|---|
| 0 | ST7735 class | 0x08 |
| 1 | ST7735 class | 0xC8 |
| 2 | GC9106 class, FD = 0x4F | 0x88 |
| 3 | GC9106 class, FD = 0x40 | 0x08 |

ST7735 frame rate B1 / B2 = 05 3C 3C. All 128 rows are visible: the images draw 160 x 128 through CASET y, RASET
159 - x, with no offsets. Every type is RGB.

**Backlight** on PB2 (derived): the images switch it on with the screen and off at init. PB2 has no timer function on
the STM32F405 and no DMA stream writes GPIOB, so stock has no dimming. RadioFoundry dims it as on the DM-1701: DMA
writes the pin's BSRR from a timer, which works on any GPIO.

---

## 5. Keys, navigation ring, PTT

### 5.1 Matrix

The DM-1701's scan (rows PA6, PD2, PD3; columns D0-D7 on the LCD data lines PD14, PD15, PD0, PD1, PE7-PE10). The key
code table is decoded from the images; the decode was checked by reproducing the DM-1701 and MD-UV380 tables from
their images.

| Row | D0 | D1 | D2 | D3 | D4 | D5 | D6 | D7 |
|---|---|---|---|---|---|---|---|---|
| 0 (PA6) | 1 | 2 | 3 | 4 | 5 | 6 | 0 | * |
| 1 (PD2) | P1 | MENU | BACK | 7 | 8 | 9 | # | P2 |
| 2 (PD3) | | | | | SK3 | OK (ring center) | SK2 | SK1 |

Digits, `*` and `#` (images). MENU, BACK and OK by their function in the images (derived). SK1 and SK2 are the
MD-UV380's cells; SK3 is new (derived). Which side key is where on the case, and P1 against P2, need a radio (?).

### 5.2 Direct inputs

| Pin | Key | Source |
|---|---|---|
| PE11 | PTT, active low | images |
| PE12 | external PTT, active low | images |
| PE13 | orange top key, active low, pull-up | images |

### 5.3 Navigation ring

Four polled inputs, no interrupts (images). The images count one event for each level change (either edge) that holds
21 polls; the poll runs every main-loop pass, about every 2 RTOS ticks.

| Pin | Direction | Source |
|---|---|---|
| PB10 | up | derived: the images' up code |
| PB11 | down | derived: the images' down code |
| PE15 | left or right | images: frequency step; which way ? |
| PE14 | right or left | images |

RadioFoundry samples the four lines with the key scan, takes a level when two scans agree, and sends one key press per
change (`boards/uv380_family/input_uvf.c`).

### 5.4 Bootloader keys

TYT's update guide: hold PTT and the orange top key at power on. The images check PTT and SK1 (row 2 D7) for their own
test mode at boot. Which pair the bootloader checks needs a radio (?).

---

## 6. Flash layout

As the DM-1701: bootloader in sectors 0-2, the application from 0x0800C000. The codec placement is in section 12.

---

## 7. Analog inputs

The images set up ADC1 and ADC2 with the DM-1701's channel list (images). The battery divider and the volume pot range
were not found in the images. RadioFoundry uses the DM-1701's battery scale (37.5 per 0.1 V) and a pot range of
32..2048 with the break at 321 (unconfirmed).

---

## 8. Calibration

The images' calibration reader and derivation are instruction for instruction the DM-1701's (security registers 1 and
2, [hardware-dm1701.md](hardware-dm1701.md) 12). Band limits: security register 3 bytes 0x14 / 0x16 (first band low /
high, BCD), 0x18 / 0x1A (second band). RadioFoundry's power fractions and k factors are its own measured values, not
in any image: the MD-2017 uses the DM-1701's until measured.

---

## 9. GPS

GPS builds: USART1 RX on PA10, module power on PA9, as the MD-UV380 (derived from the image names and pin use).

---

## 10. USB and model reporting

OTG_FS on PA11 / PA12. Open-firmware CPS radio type 9 ([open-firmware-cps.md](open-firmware-cps.md)). The RT82 reports
the same type: no model byte tells the two apart (?).

---

## 11. Bootloader and stock images

The TYT / AnyRoad DFU loader (USB 0483:DF11, "AnyRoad Technology", "Digital Radio in USB mode") as the DM-1701's. Images
are XOR-encrypted with the MD-UV380 key, SHA-256
`739ce35cc8db6e72f95598effac1a01ae71dbe2a5792c1a69acaee5dbf634991` ([hardware-dm1701.md](hardware-dm1701.md) 15.3). The
application starts at 0x0800C000 (vector table SP 0x20003578). The bootloader itself has not been dumped from an
MD-2017.

---

## 12. Codec

[codec-md2017.md](codec-md2017.md).

---

## 13. Unknowns that need a radio

| # | Unknown | State | How to settle it |
|---|---|---|---|
| 1 | Backlight on PB2 | derived | the screen lights at boot |
| 2 | Panel types and orientation | images (section 4) | the boot screen is upright and the right colors |
| 3 | Physical key positions: SK1-SK3, P1 / P2, MENU / BACK | derived | press each key, read the key log |
| 4 | Ring left / right | open | press left: the cursor moves left |
| 5 | Second AT1846S held idle, PD8 low | derived | no audio from the second receiver; main receiver audio normal |
| 6 | PE4 role | open | scope it under stock |
| 7 | Battery scale, pot range | open | compare against a meter; read raw at both pot ends |
| 8 | Power fractions, k factors | open | power meter at each level, band edges |
| 9 | Bootloader entry keys | open | PTT + orange, else PTT + SK1 |
| 10 | HR_C6000 config SPI on PC13 / PE5 / PE3 | images | the diagnostics page reads HR_C6000 reg 0x10 |
| 11 | PA band select polarity | images | TX power on VHF and UHF |
| 12 | Mic power | open | TX audio on FM and DMR |
| 13 | Codec voice on air | open | DMR TX and RX |
