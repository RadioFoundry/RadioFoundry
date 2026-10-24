<!-- SPDX-License-Identifier: MIT -->

# Radios: hardware survey and port plan

Status: research, 2026-10-10. This page collects the main chips of every radio ODR supports or plans to support, plus
close relatives, from public sources only. It also says which ODR platforms and drivers each radio could reuse and
suggests an order for the ports. Register-level facts live in the per-radio specs
([hardware-dm1701.md](hardware-dm1701.md), [hardware-dm32.md](hardware-dm32.md),
[hardware-md380.md](hardware-md380.md), [hardware-mduv380.md](hardware-mduv380.md), [hardware-gd88.md](hardware-gd88.md), [hardware-db25d.md](hardware-db25d.md), [hardware-dm1801pro.md](hardware-dm1801pro.md), [hardware-anytone.md](hardware-anytone.md)). This page is the overview.

Facts are not copyrightable. Every claim carries a source link and a confidence tag:

| Tag | Meaning |
|---|---|
| **C** | Confirmed: manufacturer page, FCC filing, datasheet, or our own bench work on the radio |
| **D** | Documented by our own reference port notes, a still-cited project such as radio_tool, or our own reading of an image (IMG, BIN). Facts that rest only on public notes we cannot name are marked (unverified) instead |
| **unverified** | Reported in public notes, a review or a forum we cannot name or cannot check, with no independent basis. Check before relying on it |
| **F** | A forum post, review or retailer claim that this page still cites. Treat as likely but unverified |
| **?** | Not found or contradictory. Needs checking before a port starts |

**FCC internal photos.** FCC IDs are listed per radio, but the fccid.io and fcc.report exhibit PDFs couldn't be
fetched by script for this survey. Opened in a browser, they download fine. So far the DB25-D PRO, DB40-D, DM-1801 (2026) and
DR-1801 chips below are confirmed from FCC photos. Reading the internal-photo exhibits by hand is the cheapest way to turn D
and F tags into C, and it should happen before a port starts.

## Summary table

The Port column says how far each radio's port is. A row that names a board in `boards/` gets its cell from the
board's `[status]` (`tools/boards.py docs` writes it). Every other row's cell is one of `Planned`, `Planned, hardware
needed`, `In progress, #N` (the pull request) or `Not planned`, then an optional `. note`. The README's radio table,
the user manual's planned radios and the web site list the planned and in-progress rows with that note, so it is
written for readers of all three ([tools/registry_docs.py](../../tools/registry_docs.py)).

| Radio (and rebadges) | MCU | DMR chip | RF | Display | Ext. memory | GPS | Update path | Port |
|---|---|---|---|---|---|---|---|---|
| Baofeng DM-1701, Retevis RT84 | STM32F405VG (C, bench) | HR_C6000 (C, bench) | AT1846S (C, bench) | 160x128 color, 8080 bus (C, bench) | 16 MB SPI NOR (C, bench) | none (C) | USB DFU on the MCU, wrapped images (D) | Supported ([hardware-dm1701.md](hardware-dm1701.md)) |
| TYT MD-UV380 / MD-UV390, Retevis RT3S | STM32F405VG (D) | HR_C6000 (D) | AT1846S (D) | 160x128 color, HX8302A (unverified) | 25Q128FV, 16 MB (unverified) | optional, JS-H210 (unverified) | USB DFU on the MCU, XOR-keyed images (C, key derived from stock 19.26) | Planned. Same board family as the DM-1701 (STM32F405, HR_C6000, AT1846S), so it should be the next port ([hardware-mduv380.md](hardware-mduv380.md)) |
| TYT MD-380 / MD-390, Retevis RT3 | STM32F405VGT6 (unverified) | HR_C5000 (unverified) | SKY72310 PLL + GT3136 receiver (unverified) | 160x128 color, HX8302A (unverified, controller ?) | W25Q128FV 16 MB; some early VHF 1 MB (unverified) | optional "G" models (unverified, module ?) | USB DFU, encrypted images (unverified) | Supported ([md380-bringup.md](../testing/md380-bringup.md)) |
| Radioddity GD-77, TYT MD-760, GD-77S, Baofeng DM-1801 v1 / DM-860, RD-5R | NXP MK22FN512 (unverified) | HR_C6000 (unverified) | AT1846S (unverified) | 128x64 mono, UC1701 (unverified) | 25Q80BV 1 MB + AT24C512 64 KB EEPROM (unverified) | none (unverified) | USB HID bootloader, .sgl files (F) | In progress, #111. Hardware needed. A new platform: a different MCU (NXP MK22FN512), the same HR_C6000 and AT1846S, a 128x64 mono LCD. A DM-1801 bought new today is not v1, and the look-alike DR-1801 is unrelated |
| Radioddity GD-88 | HR_C7000 class SoC (F) | HR_C7000 (F) | ? (two transceivers, F) | 2" color TFT (C), controller ? | ? | yes (C), module ? | Radioddity updater over K-plug cable (C) | In progress, #132. Hardware needed. Dual receiver on the HR_C7000 platform (chip count not confirmed yet); should reuse much of the DM-32UV port |
| Baofeng DM-32UV | HR_C7000 SoC, C-SKY CK803S core (F, BIN) | HR_C7000 (F, BIN) | FD6818 (community pin map) | 240x320 color, ST7789 class (community, BIN) | 16 MB data flash (F); program flash size ? | yes (C), ATGM336H (community) | Baofeng bootloader over UART cable (BIN, F) | Supported ([dm32-bringup.md](../testing/dm32-bringup.md)) |
| Baofeng DM-1801 Pro, DM-1801 (current) | HR_C7000 class SoC (C for the HR_C7000, FCC photo; the MCU part ?) | HR_C7000 (C, FCC photo) | ? | 1.77" (C), type and controller ? | ? | none (C) | ? | Supported ([dm1801pro-bringup.md](../testing/dm1801pro-bringup.md)) |
| Baofeng DR-1801 / DR-1801UV | ? (inside the AM6102 module ?) | Auctus AM6102 module (C, FCC photo) | ? | ? | ? | ? | ? | Not planned. Looks like a DM-1801 |
| Radioddity DB25-D / DB25-D PRO, Retevis RT73, Kydera CDR-300UV (mobile) | HR_C7000 SoC (C, FCC photo) | HR_C7000 (C) | 2x BK4819 on the PRO (C, IMG); FD6818 on earlier batches (IMG) | 1.77" color TFT (C), ST7735S class, serial bus (IMG) | 2 Gbit SPI NAND, Kioxia TC58CVG1S3 (C, FCC photo) | yes (C), module ? | Vendor IAP loader over a UART cable, plain images (C, IMG) | Supported ([hardware-db25d.md](hardware-db25d.md)) |
| Radioddity DB40-D, Kydera CDR-700UV (mobile) | two processors "A" and "B", likely two HR_C7000 (C for two, ? for the part) | HR_C7000 (IMG) | ? (separate VHF and UHF chains and PA modules, C) | 2.4" color TFT (C), controller ? | SPI NAND, TC58 family (IMG) | yes (C), module ? | Vendor IAP loader over RJ-45 only, two plain images (C, IMG) | Planned, hardware needed. Two HR_C7000-class processors (A and B), like the GD-88, so it follows the GD-88 port |

BIN and "community" mean the facts come from our own disassembly and the community pin map recorded in
[hardware-dm32.md](hardware-dm32.md), not from a public teardown. IMG means our own look at a vendor's public update
image: its container layout, its readable strings and the range of its address constants, with no disassembly.
Treat IMG like D.

## Per-radio notes

### Baofeng DM-1701 / Retevis RT84

- Bands 136–174 / 400–480 MHz, 1/2/5 W, 1.77" color TFT, no GPS (C)
  ([baofengradio.com](https://baofengradio.com/products/dm-1701)). A search snippet from the same site said
  400–520 MHz; the product page itself says 400–480 MHz.
- FCC ID 2AJGM-DM1701, granted 2018-12-22. Internal photos exist but are too low resolution to read parts (C)
  ([fccid.io](https://fccid.io/2AJGM-DM1701)).
- STM32F405 + HR_C6000 + AT1846S + 160x128 color LCD + 16 MB SPI NOR: confirmed by our own bring-up
  ([hardware-dm1701.md](hardware-dm1701.md)). No public teardown names the DM-1701's chips. Public sources only say
  it shares the MD-UV380 platform (F) ([w0chp supported radios](https://w0chp.radio/ogd77-webcpsd/supported-radios/)).
- Programming: USB D+/D- on the K-plug straight to the MCU. D+ and D- are swapped compared with the RT3S cable (F)
  ([opengd77.org t=81](https://opengd77.org/viewtopic.php?t=81)).
- Stock images are wrapped. radio_tool can write and wrap DM-1701 firmware (D)
  ([radio_tool](https://github.com/v0l/radio_tool)).
- Reuses `platform/stm32f4`, `drivers/modem/hrc6000`, `drivers/rf/at1846s`, `drivers/display/st7735`,
  `drivers/flash/spi_nor`, `drivers/input/keymatrix`.

The **Ailunce HD1** is *not* a DM-1701 relative. It's a GD-77 class radio: MK22FN512, HR_C6000, AT1846S, 160x128
color TFT, 25Q80BV + AT24C512, ST-26-U7L GPS, serial YMODEM bootloader over a PL2303 cable (unverified).
A forum thread reports a 2023+ HD1 v2 on the HR_C7000, but posters contradict each other (?)
([opengd77.com archive](https://www.opengd77.com/archive/viewtopic4b3f.html?f=7&t=932&start=20)).

### TYT MD-UV380 / MD-UV390 / Retevis RT3S

- STM32F405VG (Cortex-M4F, 1 MB flash, 192 KB SRAM including 64 KB CCM (C)
  [st.com](https://www.st.com/en/microcontrollers-microprocessors/stm32f405vg.html)), HR_C6000, AT1846S, HX8302A display controller, 25Q128FV SPI flash, JS-H210 GPS on G models (unverified). 160x128 color TFT. The stock bootloader takes
  the first 48 KB of internal flash, and the app starts at 0x0800C000 (unverified).
- The AT1846S is on I2C. The MD-UV390 Plus (10 W) board layout differs somewhat (unverified, ?).
- The MD-UV390 is the same radio in a waterproof case. GPS is a hardware option; GPS firmware doesn't add GPS to a
  non-GPS radio (unverified).
- FCC IDs POD-MDUV380 (granted 2018-10-19) and POD-MDUV390 (unverified).
- DFU entry: hold PTT and the button above it at power-on (D), see [hardware-mduv380.md](hardware-mduv380.md).
- A repair-guide snippet claims the dual-band radios use an NXP MK22. That contradicts every other source and is
  almost certainly wrong (?).
- Open firmware: the OpenGD77 project supports it.
- **ODR:** the same board family as the DM-1701 ([hardware-mduv380.md](hardware-mduv380.md) lists every difference).
  It needs a key map, no LCD row offset and its own panel-type table, battery and volume ADC scaling, power tables
  (5 W and Plus 10 W, whose PA band select and backlight pins are swapped), its own DFU key and GPS on USART1 (PA10,
  power PA9; an earlier note here said USART3, which is the MD-380's). No new drivers.
- Stock codec (C, 2026-10-12): TYT 19.26 files (V/D non-GPS, P/S GPS) carry the DM-1701's codec library with the same
  entry offsets, in the same flash sectors; V19.26 runs bit-identical to the DM-1701 codec in an emulator
  ([codec-mduv380.md](codec-mduv380.md)).

### TYT MD-9600 / Retevis RT90

- STM32F405VG, HR_C6000 (bit-banged SPI), two SKY72310 PLLs (one VHF, one UHF) with a 16.8 MHz TCXO "at least in the
  older models", ST7567 128x64 mono LCD sharing SPI2 with a W25Q128 flash, calibration in the flash security
  registers (D), from the reference port ([hardware-md9600.md](hardware-md9600.md)).
- Several PLL and IF chips were used in production. Board versions V2 (the first V1 boards were labeled V2), V3, V4,
  V4A (same as V5) and V5 exist, and a build for the wrong version boots but won't transmit or receive (F)
  ([opengd77.org t=83](https://www.opengd77.org/viewtopic.php?t=83),
  [opengd77.org p=15](https://www.opengd77.org/viewtopic.php?p=15)). A retailer ties V4/V5 to "PLL 2571" (F)
  ([passion-radio](https://www.passion-radio.com/mobile/md9600-505.html)). The reference port's register writes give: V1-V3
  SKY72310 x2 with AK2365A IF chips, V4 a "PLL1590" synthesizer (TYT's V4 firmware folder name; exact part ?) with AK2365A, V4A/V5 the LMX2571 register
  set (F) and no programmable IF chip. Details, pin map and per-revision programming: [hardware-md9600.md](hardware-md9600.md);
  codec: [codec-md9600.md](codec-md9600.md) (V26.45 is the radio's own donor).
- Front 4x3 key matrix, channel encoder, and hand-mic keys read as analog levels. The mic has 16 keys on an RJ-45
  jack (unverified).
- 136–174 / 400–480 MHz (unverified). Spec power is 50 W VHF and 45 W UHF; the same retailer page also says 40 W UHF (F).
  One review measured 44 W at 146 MHz and 40 W at 446 MHz (unverified).
- Mini-USB goes to the MCU's own USB, which enumerates as STM32 DFU 0483:df11 (D). A claim of a "UART chip
  inside" is contradicted by that.
- FCC ID POD-DMR3 (TYT), granted 2017-12-28, with class II changes in 2020 and 2021 (MD-9600-5P GPS), three
  internal-photo exhibits (C) ([fccid.io](https://fccid.io/POD-DMR3)). No separate RT90 FCC ID was found.
- Open firmware: the OpenGD77 project has per-version builds.
- **ODR:** reuses `platform/stm32f4`, `drivers/modem/hrc6000`, `drivers/flash/spi_nor`, the 128x64 display class
  already in the UI ([ui.md](ui.md)). New: a SKY72310 synthesizer driver plus the per-band receive chain, an ST7567
  display driver, analog mic-key decoding, and a board-version probe or per-version builds.

### TYT MD-380 / MD-390 / Retevis RT3

- STM32F405VGT6 with 1 MB flash and 192 KB RAM, and an HR_C5000 DMR baseband (unverified).
- RF: SKY72310 synthesizer and a GT3136
  receiver IC (unverified). There's no AT1846S. The schematic notes warn the schematic "is known to be not correct" (unverified).
- Display: 160x128 color TFT (unverified) on the FSMC 8-bit bus.
  The controller is reported as HX8302A (unverified) or possibly an ILI9481 (unverified, ?). RT3 and MD-380
  panels are "slightly incompatible" (unverified). **Controller unconfirmed (?)**.
- SPI flash: W25Q128FV 16 MB, but some VHF radios are reported to have only 1 MB (unverified).
- One band per radio: UHF 400–480 or VHF 136–174 MHz, 1/5 W (unverified).
  GPS on "G" models. The module is reported as both JS-H210 and JS-M710 (?).
- Update path: a 48 KB Tytera bootloader at 0x08000000, app at 0x0800C000. USB DFU (0483:df11) with non-standard
  commands. Read-out protection level 1. Stock images are encrypted with a key held in the bootloader, and the scheme
  was publicly broken (unverified).
- FCC ID PODMD-380, granted 2015-07-22, with an 8-page internal-photos exhibit (F, search snippet)
  ([fccid.io](https://fccid.io/PODMD-380/Internal-Photos/Internal-Photos-2688727)).
- Open firmware: the OpenGD77 project has a port.
- **ODR:** `boards/md380` builds (UHF and VHF images), passes its host tests and has run on a radio
  ([hardware-md380.md](hardware-md380.md), [md380-bringup.md](../testing/md380-bringup.md)). It reuses
  `platform/stm32f4`, `drivers/flash/spi_nor` and `drivers/display/st7735` (HX8302A assumed compatible, ?). The
  HR_C5000 is a variant of `drivers/modem/hrc6000` (a chip flag selects the register differences). New:
  `drivers/rf/sky72310` (shared with the MD-9600 later), `drivers/gps/nmea_line`, run-time GPS detection
  (`board.gps_present`). DMR voice uses the codec of the stock D002.032 file, merged at build time and run in place
  ([codec-md380.md](codec-md380.md)); untested on a radio.

### Radioddity GD-77 family

- MK22FN512 MCU, HR_C6000, AT1846S, UC1701 display controller, 25Q80BV SPI flash + AT24C512 I2C EEPROM, no GPS (unverified). The MK22FN512VLL12 is a 120 MHz Cortex-M4F with
  512 KB flash and 128 KB SRAM (C) ([Keil device page](https://www.keil.arm.com/devices/nxp-mk22fn512vll12)); that
  exact suffix on the GD-77 board is not confirmed (?).
- 128x64 mono display, the size our UX specs assume ([information-architecture.md](../ux/information-architecture.md)).
  No public spec sheet found states the panel resolution (?).
- 136–174 / 400–470 MHz, 1/5 W (unverified). USB cable with a K1 plug and no UART chip in the cable (unverified).
- FCC ID 2AN62-GD77 (C, manufacturer listing).
- Update mode: hold S1 + Fn at power-on. Firmware updates over USB HID, no driver. Stock images are .sgl files (F)
  ([OpenGD77 user guide copy](https://www.scribd.com/document/619619664/OpenGD77-User-Guide2)). No public description
  of the .sgl encryption was found (?).
- Rebadges: the TYT MD-760 is the GD-77 (three sources agree). The GD-77S is the TYT MD-750 (keyless, no display).
  The DM-1801 is also sold as the DM-860, and only its version 1 hardware is in this family; the 2022 DM-1801 and
  DM-1801A are reported different, and the DM-1801 sold in 2026 is an HR_C7000 radio
  ([Baofeng DM-1801 Pro and DM-1801 (current)](#baofeng-dm-1801-pro-and-dm-1801-current)). The DR-1801 has the same
  case but no part of this platform ([Baofeng DR-1801](#baofeng-dr-1801--dr-1801uv)). The RD-5R is the DM-5R Tier II in a new case (F)
  ([opengd77.org p=26](https://www.opengd77.org/viewtopic.php?p=26),
  [w0chp](https://w0chp.radio/ogd77-webcpsd/supported-radios/),
  [swenton.com](https://swenton.com/ham-radio/ham-radio-articles/opengd77)). Sources contradict each other on the
  DM-1801A (?).
- Open firmware: the OpenGD77 project supports the whole family.
- **ODR:** reuses `drivers/modem/hrc6000`, `drivers/rf/at1846s`, `drivers/flash/spi_nor` and the 128x64 display
  class. New: a `platform/mk22` (Kinetis start-up, clocks, SPI, I2C, USB device for TinyUSB), a UC1701 display driver,
  an I2C EEPROM driver for settings, and an .sgl image tool. The 512 KB of internal flash is half the STM32 radios'
  1 MB, so check ODR's image size with codec early. The HD1 v1 is the same platform with a color display and a serial
  bootloader.

### Radioddity GD-88

- 136–174 / 400–480 MHz, 2.5/7 W, 2" color TFT, built-in GPS and APRS, dual VFO, 3000 mAh, FCC ID 2AN62-GD88 (C)
  ([radioddity.com](https://www.radioddity.com/products/radioddity-gd-88)).
- "2 separate transceivers both usable at same time, even on same band" (C)
  ([Radioddity blog](https://www.radioddity.com/blogs/all/radioddity-gd-88-review)).
- Uses "the new C7000, DMR and combined CPU chip" (F) ([opengd77.org p=26](https://www.opengd77.org/viewtopic.php?p=26)).
  No public source confirms two HR_C7000 chips; the stock firmware does (two images, A and B, linked a UART apart:
  [hardware-gd88.md](hardware-gd88.md)). The RF chip and display controller aren't confirmed (?); the FCC internal
  photos are the next thing to read.
- Shares a CPS with the Retevis RT73 / Kydera CDR-300UV / Radioddity DB25-D and DB40-D, which points to one OEM
  platform (F) ([codeplugs.rats.net](https://codeplugs.rats.net/help/cps)).
- Programming and updates go over a USB-serial cable (FTDI-class chip in the cable) into the K1 jack, to SoC A's
  UART1 at 115200. The radio has no USB port, and neither stock image uses the HR_C7000 USB core (COM, BIN):
  [hardware-gd88.md](hardware-gd88.md).
- **ODR:** reuses `platform/hrc7000` and `drivers/modem/hrc7000` from the DM-32UV. New: the RF chip(s), the display,
  and core support for two simultaneous receivers if the hardware really has two independent DMR paths.

### Baofeng DM-32UV

- TX 136–174 / 400–470 MHz. RX adds 400–480, AM air band 108–136 and FM broadcast 65–108 MHz. 10/4/1 W, 2.0" color
  screen, built-in GPS, USB-C charging (the socket is on the battery pack, no data). FCC ID 2AJGM-DM32UV (C)
  ([baofengradio.com](https://www.baofengradio.com/products/dm-32uv)).
- The stock firmware receives air band AM (C, owner). Which chip demodulates it (the FD6818's AF output is the
  working guess) and the broadcast chip (an RDA5807M on the pin map) are open: [airband-and-broadcast.md](airband-and-broadcast.md).
- K1-style programming cable, with FTDI preferred (unverified).
- The HR_C7000 integrates a CK803S MCU, according to a vendor quote about a different radio (F)
  ([opengd77.org p=114](https://www.opengd77.org/viewtopic.php?p=114)).
- PCB revisions DM32_UV_V1.2 (2024-08) and V1.4 (2024-12). Factory firmware 048/049 (F)
  ([M7OCM/DM-32UV](https://github.com/M7OCM/DM-32UV)). Update mode is PTT + SK1 at power-on (F).
- Flash: [hardware-dm32.md](hardware-dm32.md) has a 1 MB XIP program flash and a 16 MB data flash. A forum snippet
  names a 25Q16 (2 MB) and a 25Q128 (16 MB) as the chips community tools back up (F, snippet only). **The program
  flash part and size are unconfirmed (?)**.
- FD6818 RF, ST7789-class 240x320 panel and ATGM336H GPS come from the community pin map and disassembly
  ([hardware-dm32.md](hardware-dm32.md)). No public teardown ties them to the radio (?).
- Open firmware: OpenDM32 (credited to RA4FHE) is binary-only (F)
  ([radioreference](https://forums.radioreference.com/threads/opendm32-software.495804/post-4244981)).

### Baofeng DM-1801 Pro and DM-1801 (current)

Two listings sold new in 2026 under the DM-1801 name. Neither is the DM-1801 v1 of the GD-77 family.

| | DM-1801 Pro | DM-1801 (current listing) |
|---|---|---|
| Power | 7 / 4 / 1 W (C) | 5 W (F, retailer) |
| Memory | 4000 channels, 250 zones, 800 contacts plus 50,000 by CSV import (C) | 1024 channels and zones, 1024 contacts (F, retailer) |
| Programmable keys | 5: SK1, SK2, the top key and the left and right arrows (C) | 3: SK1, SK2 and the top key (F, retailer) |

- **DM-1801 Pro:**
  - TX 136–174 / 400–470 MHz; RX adds AM air band 108–136 MHz and FM broadcast 65–108 MHz (C).
  - 1.77" display, 7.4 V 2200 mAh battery, Kenwood 2-pin accessory jack, no GPS, no USB-C charging (C).
  - Records DMR calls (about 10 hours) and exports them as WAV through the CPS (V1.60) (C).
  - FCC ID 2AJGM-DM1801PRO; the English manual is dated 2026-09-16 (C)
    ([baofengradio.com](https://www.baofengradio.com/products/dm-1801-pro),
    [manual](https://baofeng.s3.amazonaws.com/Baofeng_DM-1801_Pro_User_Manual_EN_20260916.pdf),
    [fccid.io](https://fccid.io/2AJGM-DM1801PRO)).
- **Chips:** an HR_C7000 is on the PCB in the FCC internal photos (C, owner). The RF chip, the broadcast receiver,
  the flash and the display controller aren't identified yet (?).
- **Same hardware as the DM-1801:** the filing includes an attestation that the DM-1801 and the DM-1801 Pro are
  identical except for color and distributor (C, FCC filing). The listings' different power, memory and key counts
  don't fit "identical" unless they're firmware and marketing differences (?).
- **Which DM-1801 revision this is:** the hardware matches neither v1 (MK22 + HR_C6000) nor the 2022 redesign
  described as a single module (F) ([OpenGD77 user guide](https://github.com/LibreDMR/OpenGD77_UserGuide/blob/master/OpenGD77_User_Guide.md)).
  It may be a third revision, or the 2022 redesign may already have used the HR_C7000 (?). Buying "a DM-1801" new
  doesn't get the v1 board.
- **Telling them apart:** v1 has a 128x64 mono display (D). The model and FCC ID label under the battery is the
  reliable check; the case alone isn't (see the DR-1801 below).
- **ODR:** reuses `platform/hrc7000`, `drivers/modem/hrc7000`, the FD6818 driver (own register set) and the shared
  factory calibration reader. New: the ST7567 display driver and the board. The hardware is in
  [hardware-dm1801pro.md](hardware-dm1801pro.md); the radio is not yet run on a bench. Needs: the key map, the update
  path and the codec's location.

### Baofeng DR-1801 / DR-1801UV

- FCC ID 2AJGM-DR1801 ([fccid.io](https://fccid.io/2AJGM-DR1801)).
- **Chips:** one large LCC module marked "Auctus AM6102". Under the module's RF shield is an IC marked
  "EP8277 ABE10" (C, owner, FCC internal photos). What the AM6102 integrates (MCU, DMR baseband, RF) and what the
  EP8277 is are not documented (?). Auctus also makes the AT1846S (C).
- **Easily mistaken for a DM-1801:** the case looks identical (C, FCC photos). The OpenGD77 FAQ calls the DR-1801
  completely different hardware from the DM-1801 (F) ([OpenGD77 FAQ](https://opengd77.org/viewtopic.php?t=7)). A
  forum claim that Baofeng renamed the redesigned DM-1801 to DR-1801 is unverified (F).
- **ODR:** not planned. Nothing in ODR drives an Auctus DMR module, and no public documentation for it was found.

### Radioddity DB25-D / DB25-D PRO / Retevis RT73 / Kydera CDR-300UV

The DB25-D is Kydera's CDR-300UV under Radioddity's name. Radioddity's FCC ID 2AN62-DB25D is a change in ID of
VO6CDR-300UV (Fujian New Century Communications, granted 2021-08-19). That filing keeps its internal photos,
schematics and parts list confidential (C) ([fccid.io 2AN62-DB25D](https://fccid.io/2AN62-DB25D),
[fccid.io VO6CDR-300UV](https://fccid.io/VO6CDR-300UV)). The 2025 **DB25-D PRO** has its own filing, 2AN62-DB25DPRO,
with public internal photos. Those photos are the source of the C tags below, so they hold for the PRO. Its display
board is dated 2020.08.21 and carried over, but its control, RX and TX boards are 2024 redesigns
("CDR300UV-MCU-BT-AI-TDA-1.0", "C7DR300UV-RX-BT-AD-3.0", "C7DR300UV-TX-RD35-BT-AD-3.5") (C)
([2AN62-DB25DPRO internal photos](https://fccid.io/2AN62-DB25DPRO/Internal-Photos/Internal-photos-8316141)).
One firmware image covers the DB25-D and the DB25-D PRO (C, readme in the
[2025-02-10 package](https://radioddity.s3.amazonaws.com/2025-02-10_DB25-D_firmware_update_release_2025-02-10.zip)).
The Retevis RT73 is reported as the same radio (F) ([kydera-ham forum](https://kydera-ham.boards.net/thread/315/thoughts-radioddity-db25-mini-mobile)). No RT73 FCC ID was found (?).

- **MCU and DMR:** one HR_C7000, marked "HR_C7000 D56323MP1" (C, FCC photo). The HR_C7000 integrates a C-SKY CK803S
  core, as on the DM-32UV ([hardware-dm32.md](hardware-dm32.md)). The stock update file is named
  `C7000_DR300UV_Ham_DB25D_20210902.bin` (C) ([Radioddity update guide](https://radioddity.s3.amazonaws.com/2023-03-30%20How%20to%20update%20firmware%20on%20DB25-D%20and%20GD-88%20v2.2.pdf)).
  Program flash part: ?. It's larger than 1 MB: the firmware keeps data above 0x0310C000 (IMG).
- **RF:** the PRO's RF board has **two Beken BK4819** transceivers with a TCXO (C, FCC photo). The receiver IF is
  51.550 MHz (C) ([extended manual v3.4](https://radioddity.s3.amazonaws.com/2025-05-01_Extended_manual_for_Radioddity_DB25-D_PRO_-_DB40-D_-_GD-88_-_v3.4.pdf),
  section 18.2). The firmware reads register 0 of both RF chips at boot and writes the result into the version
  string's batch code (IMG): the main transceiver is an FD6818 ('6', the DM-32's chip) or a BK4819 ('B'); the LO
  chip is an FD6818, an AT1141 PLL ('T'), a part with ID 0x7813 ('4') or a BK4819. The release notes' batches D64,
  D66 and D6T are the FD6818 boards; **the PRO is DBB**. On the PRO, one BK4819 is the LO synthesizer at f_RX +
  51.55 MHz and the other works at the IF in receive (IMG). "rda1846" is only the RF task's name. BK4819 support first
  appears in the 2024-05-09 build, so older stock images must not be loaded on a PRO (IMG). Both chips share one
  3-wire bus with separate enables (IMG).
- **Bands and power:** 136–174 / 400–480 MHz, 20 W or 5 W, 13.8 V (C) ([radioddity.com](https://radioddity.com/products/radioddity-db25d)).
  The FCC grant lists 19.7 W conducted (C). The PA is a discrete RF MOSFET on the TX board (C, photo). PA power is set
  by DAC B with a ramp at each burst, as on the DM-32 (IMG).
- **Display:** 1.77" color TFT (C) ([radioddity.com](https://radioddity.com/products/radioddity-db25d)). An
  RT73 review says 2" diagonal (unverified). The init sequence is an ST7735S-class controller in landscape on a 4-wire serial
  bus, not the DM-32's 8080 bus. An ADC strap picks bit-banged GPIO or the SoC's SPI2 on the same pins (IMG).
  Radioddity's "white screen after update" fix fits that (C, manual 14.4).
- **Controls:** front keys power, P1, P2, P3, VOL/SQL, TONE/T.SEL, B/E and Menu/OK, plus one push encoder. The
  16-key hand mic adds PTT and P4–P7 (DTMF A–D) (C) (extended manual v3.4, section 5.5). The mic board ("CKS-10A-2")
  carries a small QFN, likely an MCU, so the keys probably reach the radio as serial codes on RJ-45 pin 2 (?).
- **Mic connector:** RJ-45. 1 power off (to ground), 2 PTT / radio RXD, 3 mic audio, 4 shield, 5 radio TXD, 6 ground,
  7 +8 V, 8 speaker. 3.3 V logic, 115200 baud (C) (extended manual v3.4, section 17.6). The side K1 jack has the same
  serial pair (3.5 mm sleeve RXD, 2.5 mm ring TXD) (C, section 17.5).
- **Audio:** NXP TDA8542AT (2 x 1.5 W BTL) speaker amplifier (C, FCC photo). An "AI" noise-reduction daughter module
  ("CC1157G") sits between the C7000 and the mic and speaker paths. Its pads read AFES_IN/OUT, MICES_IN/OUT, SCL, SDA,
  NR_ON/OFF, ES_SW (C, FCC photo). Mic and speaker otherwise go through the C7000's codec as on the DM-32 (?).
- **Storage:** Kioxia TC58CVG1S3HRAIJ, a 2 Gbit (256 MB) 3.3 V SPI NAND (C, FCC photo; part decode
  [Elnec](https://www.elnec.com/en/device/KIOXIA/TC58CVG1S3HRAIJ+%5BWSON8%5D)). The firmware's storage code is
  NAND with a bad-block table on SPI0 (IMG). **Calibration** is a 2 KB page at NAND byte address 0x280000 with a
  second copy in program flash; the firmware repairs one from the other at boot (IMG). The layout is decoded
  ([hardware-db25d.md](hardware-db25d.md) 6).
- **Other:** RDA5802N FM broadcast receiver (C, FCC photo). GPS: built in, SMA antenna port, module ? (C). Bluetooth:
  none on the DB25-D per the product page (C). The PRO's board name says "BT", but no module was fitted in the FCC
  unit (?). Coin-cell RTC backup (C, photo).
- **Programming and update:** the vendor "IAP" Windows tool over a serial cable. Hold P1 and press power, and the
  display stays blank with the backlight on (C) (update guide). The original cable has a Prolific PL2303 (C)
  ([2021 addendum](https://radioddity.s3.amazonaws.com/2021-09-13%20Radioddity%20Addendum%20to%20DB25-D%20manual%20V1.1.pdf)).
  FTDI cables work since firmware 909E.D4.EARSAB.018 (C, release notes). A ".flash" file mode updates the IAP loader
  itself (C, manual 14.6).
- **Image format:** raw code with no header, checksum or encryption, linked at 0x0300C000 behind a 48 KB loader, as on
  the DM-32 (IMG, confirmed from the start-up copy addresses). The PC link and the debug output are both UART1 at
  115200 (IMG).
- **Codec:** the image links **`libc7000_hrvocoder_V1.9.6.a`** beside `libHRC7000Lib_V1.0.16.a` (IMG). The AMBE+2
  vocoder is software on the CK803S core, and no vocoder chip shows in the photos (C). The DM-32's
  [own-stock codec](codec-dm32.md) model carries over: extract the codec from the user's own DB25-D file. It's the
  same code as the DM-32's 046 "HRVocoder" build, at a different address in every DB25-D image, so each stock file
  needs its own recipe (IMG). AMBE frames pass through the CPU, as on the DM-32.
- **Open firmware:** none. OpenGD77 doesn't list it, and a 2021 forum thread calls it unlikely without
  C-SKY tools (F) ([kydera-ham forum](https://kydera-ham.boards.net/thread/315/thoughts-radioddity-db25-mini-mobile)).
  qdmr's support request has been open since 2021 (C) ([qdmr #165](https://github.com/hmatuschek/qdmr/issues/165)).
  MM7DBT's CPEditor edits stock codeplugs (F) ([codeplugs.rats.net](https://codeplugs.rats.net/help/cps)).
- **Not DMR:** the DB25-G, DB20-G, DB40-G and DB35-G GMRS mobiles and the "DB25 Pro" (no D) are analog FM radios
  (C, [radioddity.com DB25-G](https://radioddity.com/products/radioddity-db25g)). They aren't ODR targets.
- **ODR:** bring-up started, `make BOARD=db25d` ([hardware-db25d.md](hardware-db25d.md) 9): FM receive, nothing run
  on a radio yet. It reuses `platform/hrc7000` (shared with the DM-32), the `st7735` driver on a bit-banged serial
  bus, `drivers/rf/bk4819` for both chips (receiver and LO synthesizer), the shared calibration curve, and
  `drivers/flash/spi_nand` (read side) for the NAND. The calibration reader takes the newer of the NAND and
  program-flash copies and never writes either. Still to do: TX (refused until a real calibration page has been seen),
  DMR (the DM-32's modem glue shared, plus a codec recipe for a stock image from 2024-05 or later), the NAND's write
  side and a storage adapter (ODR's stores assume NOR sectors), the mic keypad protocol, the FD6818 boards, and an
  unbrick path (the JTAG pins look untouched by the app; the ROM serial boot needs checking on this board).

### Radioddity DB40-D / Kydera CDR-700UV

- The DB40-D's FCC ID is VO6CDR-700UV, the Kydera CDR-700UV, granted 2023-08-16 with Bluetooth (2402–2480 MHz) in the
  grant (C) ([radioddity.com](https://radioddity.com/products/radioddity-db40-d), [fccid.io](https://fccid.io/VO6CDR-700UV)).
  The internal photos are public (C) ([internal photos](https://fccid.io/VO6CDR-700UV/Internal-Photos/Internal-Photos-6738006)).
  The firmware names its siblings "CDR-300UV(DB25-D)", "DR880UV(GD-88)" and an unreleased "DB45-D" (IMG).
- **Processors:** the main board has two identical large packages marked "A" and "B" on the silkscreen. Their
  markings can't be read in the photos (C for two, ? for the part). Updates take two files, (A) and (B), written
  separately in IAP_A and IAP_B mode, and the B update shows no progress on the radio (C) (extended manual v3.4,
  section 14.3). Both images link the HR_C7000 library and vocoder (IMG). So the radio very likely has two HR_C7000
  SoCs, one per VFO, like the GD-88. The DB40-D's B firmware shares the GD-88's B49C prefix (C, release notes).
- **RF:** separate VHF and UHF receive chains and two PA modules, one per band. Shielded cans hide the RF chips (C,
  photos). RF chip: ?. 136–174 / 400–480 MHz, 40 W or 25 W (C) (extended manual v3.4, section 18.6). The product page
  says 20/40 W (C). Fan cooled (C).
- **Display and controls:** 2.4" color TFT (C) ([radioddity.com](https://radioddity.com/products/radioddity-db40-d)),
  controller ?. The front has three push encoders (VFO A volume, VFO B volume, channel), PF1, PF2, on-hook and
  off-hook keys, Back/Zone and Menu/OK (C) (extended manual v3.4, section 5.6; photo). The mic is the same 16-key
  RJ-45 mic as the DB25-D's, with P5–P8 (C).
- **Storage:** TC58-family SPI NAND per the firmware strings (IMG), part ?.
- **Other:** GPS (active antenna supported), Bluetooth 5.0 module with an external antenna, RDA5802 FM broadcast
  ("rda5802" string), external speaker jack (C, IMG). The CDR-700UV board has a SIM footprint for an LTE option the
  DB40-D doesn't use (C, photo).
- **Programming and update:** only through the RJ-45 at 3.3 V and 115200 baud (C) (extended manual v3.4, section 17.6).
  The box contains an FTDI cable (C). IAP mode: hold the on-hook key and press power (C). The images are raw, plain
  and headerless like the DB25-D's: A is 814 KB, B is 419 KB (IMG).
- **Codec:** `libc7000_hrvocoder_V1.9.6.a` in both images (IMG). It's software in each C7000, so own-stock extraction
  works as for the DB25-D, once per image if both processors decode voice.
- **Open firmware:** none.
- **ODR:** needs two cooperating images (A runs the UI, GPS and Bluetooth; B is a second modem and receiver) and an
  A↔B link protocol. That's the GD-88's problem too, so it should follow the GD-88 port. New beyond the DB25-D:
  the dual-SoC core support, the 2.4" panel, two PA power tables and fan control. Effort: months. Blockers: the two
  chips' identity, the A↔B protocol and hardware.

## Relatives worth supporting

| Radio | Chips | Reuse | Notes |
|---|---|---|---|
| TYT MD-2017 / Retevis RT82 | STM32 + HR_C6000, two RF transceiver ICs (F) | stm32f4, hrc6000, at1846s (if that's what the RF ICs are, ?) | True dual receive possible in hardware, not in stock firmware (F) ([radioreference](https://forums.radioreference.com/threads/is-the-md2017-true-dual-receive.414074/), [opengd77.org p=26](https://www.opengd77.org/viewtopic.php?p=26)) |
| Baofeng DM-1702 / DM-X | STM32 or MK22 + HR_C6000 (F) | hrc6000; platform ? | USB protocol reported reverse-engineered by third parties (F) |
| Ailunce HD1 (v1) | MK22FN512, HR_C6000, AT1846S, 160x128 color, GPS (unverified) | GD-77 port + 160x128 class | Out of production, so used units only. Encrypted stock firmware, decoder exists (F) |
| Connect Systems CS7000-M17 | STM32F405VG, HR_C6000, SKY72310, HX8302A, 25Q128FV (unverified) | stm32f4, hrc6000, SKY72310 driver from the MD-9600 / MD-380 | Free once the SKY72310 driver exists. The Plus model has an STM32H743 |
| TYT MD-9800 | HR_C7000 (F) ([opengd77.org p=26](https://www.opengd77.org/viewtopic.php?p=26)) | hrc7000 platform and modem | Relation to the Kydera platform unverified (?). The RT73 / CDR-300UV / DB25-D and the DB40-D have their own sections above. The GD-73 is a handheld, not part of that family |
| Anytone AT-D868UV / AT-D878UV / 878UVII, BTECH DMR-6X2, Alinco DJ-MD5, Radioddity GD-AT10G | GD32F303VGT6; early 868UVs GD32F303VET6 (512 KB, 64 KB RAM) (unverified), **SCT3258 DMR baseband with built-in AMBE+2, not an HR_C6000** (D, F), AT1846S, TLV320AIC3204 codec, 1-2 Gbit SPI NAND, 160x128 color (D, F) ([VK7ZJA mods guide](https://pnwdigital.net/wp-content/files/documents/Anytone/Mods_868_878_878-II_information_VK7ZJA(sk)-4-2-23_.pdf)) | at1846s | Native USB CDC update mode; encryption and signing not documented, images not yet examined (F). SWD with no read-out protection on documented units (F). The SCT3258 runs DMR layers 1-2 and the vocoder, so the MCU never sees AMBE bits (no privacy or hotspot voice unless a bypass exists) and needs a Sicomm boot block at every reset. New: a GD32F30x platform, an SCT3258 driver, SPI NAND. Details: [hardware-anytone.md](hardware-anytone.md). The 878 and DMR-6X2 are the same hardware (F) |
| Anytone AT-D578UV (mobile) | GD32F303VKT6, AT32F435/437 after 2022-07 (separate firmware per MCU), SCT3258, 4x QA4818N (AT1846S-compatible), true dual receive (F) | as the 878 | MCU change mid-run: two platforms. [hardware-anytone.md](hardware-anytone.md) |

Not DMR candidates: the BK4819 and BK4829 are analog FM transceivers, and the BK4829 adds only an FSK modem (C)
([datasheet listing](https://www.alldatasheet.net/datasheet-pdf/pdf/2171272/BEKEN/BK4829.html)). The Radtel RT-4D does
DMR through a BK4819 plus an HF6853 vocoder module (unverified), a
different architecture from anything in ODR. The Retevis C62 and Quansheng-style radios are FM only.

## What exists and what's new

| Building block | In ODR now | Needed by |
|---|---|---|
| `platform/stm32f4` | yes | DM-1701, MD-UV380, MD-9600, MD-380, MD-2017, CS7000 |
| `platform/hrc7000` | yes | DM-32UV, GD-88, DB25-D family, DB40-D, DM-1801 Pro / DM-1801 (2026) |
| `drivers/modem/hrc6000` | yes | every HR_C6000 radio above |
| `drivers/modem/hrc7000` | yes | the HR_C7000 radios |
| `drivers/rf/at1846s` | yes | DM-1701, MD-UV380, GD-77 family, HD1, Anytone |
| `drivers/rf/fd6818` | yes | DM-32UV (GD-88 ?) |
| `drivers/display/st7735` | yes | 160x128 color radios (HX8302A compatibility per radio ?) |
| `drivers/display/st7789` | yes | DM-32UV |
| `platform/mk22` | **new** | GD-77 family, HD1 |
| `drivers/modem/hrc5000` (with `hrc_common`) | yes (`hrc5000_attach`) | MD-380 |
| `drivers/rf/sky72310` (+ receive chain) | yes (MD-380 board glue) | MD-380, MD-9600 (older boards), CS7000 |
| UC1701 / ST7567 mono display driver | **new** | GD-77 family (UC1701), MD-9600 (ST7567). Both are 128x64-class page-addressed controllers, so one driver may cover both (?) |
| I2C EEPROM | **new** | GD-77 family |
| GPS NMEA reader on a UART | yes (`drivers/gps/nmea_line`; the UART in the board) | MD-UV380G, MD-9600G, MD-380G, DM-32UV, GD-88, DB25-D, DB40-D |
| BK4819 transceiver | **new** | DB25-D PRO (two per radio) |
| SPI NAND + storage adapter | **new** | DB25-D, DB40-D (Kioxia TC58CVG1S3, bad-block handling) |
| `drivers/bcast/rda5807m` | yes | DM-32UV; the RDA5802 in the DB25-D and DB40-D is a close relative (register compatibility ?) |
| Dual-SoC (A/B) core support | **new** | GD-88, DB40-D |
| GD32F30x platform + SCT3258 modem | **new** | Anytone family |

## Suggested port order

By effort, then by how many radios a port unlocks and how popular they are:

1. **TYT MD-UV380 / MD-UV390 / RT3S.** It's a board variant of the DM-1701, very popular, and needs no new chip
   drivers. Effort: days.
2. **Radioddity GD-77 family** (GD-77, MD-760, DM-1801 v1, RD-5R, GD-77S, then HD1 v1). It has the largest user base
   of any open-firmware DMR radio. It reuses the HR_C6000 and AT1846S drivers and the existing 128x64 UI class, but
   needs a new MK22 platform and a mono display driver, and has a tight 512 KB of flash. Effort: weeks.
3. **TYT MD-9600 / RT90.** STM32 + HR_C6000 again. New are the SKY72310 driver, the ST7567 display (shared work with
   the GD-77's mono driver) and the board-version problem. Effort: weeks, and the V4/V5 PLL needs identifying first.
4. **TYT MD-380 / MD-390 / RT3.** Builds now, with the SKY72310 driver the MD-9600 can reuse, and runs the stock
   D002.032 codec in place for DMR voice ([codec-md380.md](codec-md380.md)); it needs a radio for bring-up.
5. **HR_C7000 family after the DM-32UV is on air**: the DM-1801 Pro (air band and broadcast like the DM-32UV, chip
   list still open), then the DB25-D (one HR_C7000, but a new RF
   chip and SPI NAND), then the GD-88, then the DB40-D (two SoCs, reusing the GD-88's A/B work). All four are wish
   list items until hardware is on the bench. Start by reading the FCC internal photos, since the GD-88's chip list is
   mostly unknown.
6. **Opportunistic:** CS7000-M17 (nearly free after step 3), MD-2017 (dual receive) and DM-1702. The Anytone family
   is the biggest prize by popularity, but it needs a new MCU platform and a new DMR chip driver for the SCT3258, so
   it comes last.
