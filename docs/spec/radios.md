<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Radios

This page lists the main chips of every radio RadioFoundry supports or plans to support, and the state of each port.
For what each supported radio does, see [the radio reference](../user/23-radios.md) and
[the feature matrix](../feature-matrix.md). A board's drivers are in `boards/<id>/board.mk`.

Facts come from public sources (manufacturer pages, FCC filings, datasheets, retailer listings) and from the radios.
For a radio without a board, chip claims come from retailer listings, reviews and forums unless a source is named. `?`
means not found or contradictory. FCC IDs are listed per radio. The internal-photo exhibits on fccid.io and fcc.report
open in a browser.

## Summary table

The Port column says how far each radio's port is. A row that names a board in `boards/` gets its cell from the
board's `[status]` (`tools/boards.py docs` writes it). Every other row's cell is one of `Planned`, `Planned, hardware
needed`, `In progress, #N` (the pull request) or `Not planned`, then an optional `. note`. The README's radio table,
the user manual's planned radios and the web site list the planned and in-progress rows with that note, so it is
written for readers of all three ([tools/registry_docs.py](../../tools/registry_docs.py)).

An in-progress cell can say what the pull request already has before the note: `In progress, #N, builds, host tests,
on the radio`, any of the three. The README marks each ◐ until the board merges and its `[status]` takes over.

| Radio (and rebadges) | MCU | DMR chip | RF | Display | Ext. memory | GPS | Update path | Port |
|---|---|---|---|---|---|---|---|---|
| Baofeng DM-1701, Retevis RT84 | STM32F405VG | HR_C6000 | AT1846S | 160x128 color, 8080 bus | 16 MB SPI NOR | none from the factory; owners fit one on PA10 | USB DFU on the MCU, wrapped images | Supported ([radio reference](../user/23-radios.md#baofeng-dm-1701)) |
| Anytone AT-D868UV / AT-D878UV / AT-D878UVII Plus, BTECH DMR-6X2, Alinco DJ-MD5, Radioddity GD-AT10G | GD32F303VGT6; early 868UVs GD32F303VET6 | SCT3258 with built-in AMBE+2 | AT1846S | 160x128 color | 1-2 Gbit SPI NAND | 878: yes | USB CDC update mode | In progress, #291. A new platform (GD32F30x) and a new DMR chip (SCT3258, which runs the vocoder itself), so it starts the Anytone family |
| TYT MD-UV380 / MD-UV390, Retevis RT3S | STM32F405VG | HR_C6000 | AT1846S | 160x128 color, HX8302A | 25Q128FV, 16 MB | optional, JS-H210 | USB DFU on the MCU, XOR-keyed images | Supported ([radio reference](../user/23-radios.md#tyt-md-uv380)) |
| TYT MD-9600, Retevis RT90 (mobile) | STM32F405VG | HR_C6000 | 2x SKY72310 (V1-V3), "PLL1590" (V4), LMX2571 (V4A/V5) PLLs; AK2365A IF x2 on V1-V4 | 128x64 mono, ST7567 | 25Q128FV, 16 MB | optional, JS-M710 | USB DFU on the MCU | Supported ([radio reference](../user/23-radios.md#tyt-md-9600)) |
| TYT MD-380 / MD-390, Retevis RT3 | STM32F405VGT6 | HR_C5000 | SKY72310 PLL + GT3136 receiver | 160x128 color, ST7735 class; the panel type is read from the controller's ID | W25Q128FV 16 MB; some early VHF 1 MB | optional "G" models | USB DFU, encrypted images | Supported ([radio reference](../user/23-radios.md#tyt-md-380)) |
| Radioddity GD-77, TYT MD-760, GD-77S, Baofeng DM-1801 v1 / DM-860, RD-5R | NXP MK22FN512 | HR_C6000 | AT1846S | 128x64 mono, UC1701 | 25Q80BV 1 MB + AT24C512 64 KB EEPROM | none | USB HID bootloader, .sgl files | In progress, #111, builds, host tests, on the radio. A new platform: a different MCU (NXP MK22FN512), the same HR_C6000 and AT1846S, a 128x64 mono LCD. A DM-1801 bought new today is not v1, and the look-alike DR-1801 is unrelated |
| Radioddity GD-88 | HR_C7000 class SoC | HR_C7000 | ? (two transceivers) | 2" color TFT, controller ? | ? | yes, module ? | Radioddity updater over K-plug cable | In progress, #132, builds, host tests. Dual receiver on the HR_C7000 platform; it can reuse much of the DM-32UV port |
| Baofeng DM-32UV | HR_C7000 SoC, C-SKY CK803S core | HR_C7000 | FD6818 | 240x320 color, ST7789 class | 1 MB XIP program flash, 16 MB data flash | yes, ATGM336H | Baofeng bootloader over UART cable | Supported ([radio reference](../user/23-radios.md#baofeng-dm-32uv)) |
| Baofeng DM-36 | HR_C7000 SoC (photo) | HR_C7000 | FD6818B (photo) | 2.0" 240x320 color, ST7789 class | 16 MB data flash | no | Boot ROM serial stub over the cable | In progress, #607. Built on the DM-32UV's platform: FM, the display, keys and programming run; DMR voice on air and power levels open |
| Baofeng DM-1801 Pro, DM-1801 (current) | HR_C7000 class SoC (the HR_C7000 from the FCC photo; the MCU part ?) | HR_C7000 (FCC photo) | FD6818 | 1.77" 128x64 mono, ST7567 | SPI NOR | none | ? | Supported ([radio reference](../user/23-radios.md#baofeng-dm-1801-pro)) |
| Baofeng DR-1801 / DR-1801UV | ? (inside the AM6102 module ?) | Auctus AM6102 module (FCC photo) | ? | ? | ? | ? | ? | Not planned. Looks like a DM-1801 |
| Radioddity DB25-D / DB25-D PRO, Retevis RT73, Kydera CDR-300UV (mobile) | HR_C7000 SoC (FCC photo) | HR_C7000 | 2x BK4819 on the PRO; FD6818 on earlier batches | 1.77" color TFT, ST7735S class, serial bus | 2 Gbit SPI NAND, Kioxia TC58CVG1S3 (FCC photo) | yes, module ? | IAP loader over a UART cable, plain images | Supported ([radio reference](../user/23-radios.md#radioddity-db25-d-pro)) |
| Radioddity DB40-D, Kydera CDR-700UV (mobile) | two processors "A" and "B", likely two HR_C7000 (two, from the photos; ? for the part) | HR_C7000 | ? (separate VHF and UHF chains and PA modules) | 2.4" color TFT, controller ? | SPI NAND, TC58 family | yes, module ? | IAP loader over RJ-45 only, two plain images | Planned, hardware needed. Two HR_C7000-class processors (A and B), like the GD-88, so it follows the GD-88 port |
| TYT MD-2017 / Retevis RT82 | STM32 | HR_C6000 | two RF transceiver ICs | ? | ? | ? | ? | Planned. The DM-1701's STM32 and HR_C6000 pair with two receivers, so it can reuse the STM32F4 platform. The hardware can receive two channels at once |
| BTECH DMR-6X2 PRO | ? | ? | ? | ? | ? | yes | ? (built on the AT-D878UVII Plus, [miklor](https://miklor.com/COM/Review_DMR6X2.php)) | Planned. Built on the AT-D878UVII Plus, so it can follow the Anytone port |
| Anytone AT-D578UV (mobile) | GD32F303VKT6; AT32F435/437 in later units, with separate firmware | SCT3258 | 4x QA4818N, AT1846S-compatible; true dual receive | ? | ? | ? | as the 878 | In progress, #291. The 878's chips in a mobile, with a mid-run MCU change: two platforms |
| BridgeCom Maverick | ? | ? | ? | ? | ? | yes | ? (sold as an upgraded 878, takes the 868/878 battery, F, [BridgeCom](https://www.bridgecomsystems.com/products/bridgecom-maverick-dual-band-dmr-analog-ham-radio)) | Planned. Sold as an upgraded AT-D878UV, so it likely follows the Anytone port |
| Retevis RT10 (902-928 MHz) | HR_C7000 SoC (board photos) | HR_C7000 | FD6818, on a two-wire bus | none | 2 MB SPI NOR data flash, 2 MB program flash | none | the SoC's boot ROM over the UART cable | Supported ([radio reference](../user/23-radios.md#retevis-rt10)) |

## Per-radio notes

### Baofeng DM-1701 / Retevis RT84

- Bands 136–174 / 400–480 MHz, 1/2/5 W, 1.77" color TFT, no GPS from the factory
  ([baofengradio.com](https://baofengradio.com/products/dm-1701)).
- FCC ID 2AJGM-DM1701. The internal photos are too low resolution to read parts
  ([fccid.io](https://fccid.io/2AJGM-DM1701)).
- Chips: STM32F405, HR_C6000, AT1846S, a 160x128 color LCD on an 8080 bus, and 16 MB SPI NOR. It shares the
  MD-UV380 board family (`boards/uv380_family`).
- Programming: USB D+/D- on the K-plug go straight to the MCU. D+ and D- are swapped compared with the RT3S cable.
- Update images are wrapped.
- Board: `boards/dm1701` on `platform/stm32f4`.

The **Ailunce HD1** isn't a DM-1701 relative. It's a GD-77 class radio: MK22FN512, HR_C6000, AT1846S, 160x128 color
TFT, 25Q80BV + AT24C512, ST-26-U7L GPS, and a serial YMODEM bootloader over a PL2303 cable. A forum thread reports a
later HD1 v2 on the HR_C7000, but posters contradict each other.

### TYT MD-UV380 / MD-UV390 / Retevis RT3S

- STM32F405VG (Cortex-M4F, 1 MB flash, 192 KB SRAM including 64 KB CCM,
  [st.com](https://www.st.com/en/microcontrollers-microprocessors/stm32f405vg.html)), HR_C6000, AT1846S, HX8302A
  display controller, 25Q128FV SPI flash, and JS-H210 GPS on G models. 160x128 color TFT. The bootloader takes the
  first 48 KB of internal flash, and the application starts after it.
- The AT1846S is on I2C. The MD-UV390 Plus (10 W) board layout differs.
- The MD-UV390 is the same radio in a waterproof case. GPS is a hardware option; GPS firmware doesn't add GPS to a
  radio without the module.
- FCC IDs POD-MDUV380 and POD-MDUV390.
- DFU entry: hold PTT and the button above it at power-on.
- A repair-guide claim that the dual-band radios use an NXP MK22 contradicts every other source.
- Board: `boards/mduv380`, the same board family as the DM-1701 (`boards/uv380_family`). It differs in the key map,
  no LCD row offset, its own panel-type table, battery and volume ADC scaling, power tables (5 W and Plus 10 W, whose
  PA band select and backlight pins are swapped), its DFU key, and GPS on USART1 (PA10, power PA9).
- Codec: the TYT and Retevis files from 18.11 to 219.74 (V/D without GPS, P/S with GPS) carry the same codec library
  as the DM-1701, with the same entry offsets, in the same flash sectors. The RT3S takes the TYT key.

### TYT MD-9600 / Retevis RT90

- STM32F405VG, HR_C6000 (bit-banged SPI), an ST7567 128x64 mono LCD sharing SPI2 with a W25Q128 flash, and
  calibration in the flash security registers.
- Several PLL and IF chips were used in production. Board versions V2 (the first V1 boards were labeled V2), V3, V4,
  V4A (same as V5) and V5 exist, and a build for the wrong version boots but doesn't transmit or receive. V1-V3 have
  two SKY72310 synthesizers (one VHF, one UHF) with a 16.8 MHz TCXO and AK2365A IF chips. V4 has a "PLL1590"
  synthesizer (exact part ?) with AK2365A. V4A/V5 have an LMX2571 and no programmable IF chip. A retailer ties V4/V5
  to "PLL 2571" ([passion-radio](https://www.passion-radio.com/mobile/md9600-505.html)).
- Front 4x3 key matrix, channel encoder, and hand-mic keys read as analog levels. The mic has 16 keys on an RJ-45
  jack.
- 136–174 / 400–480 MHz. Spec power is 50 W VHF and 45 W UHF; the same retailer page also says 40 W UHF. One review
  measured 44 W at 146 MHz and 40 W at 446 MHz.
- Mini-USB goes to the MCU's own USB, which enumerates as STM32 DFU 0483:df11. A claim of a "UART chip inside"
  contradicts that.
- FCC ID POD-DMR3 (TYT), with class II changes for the MD-9600-5P GPS, and three internal-photo exhibits
  ([fccid.io](https://fccid.io/POD-DMR3)). No separate RT90 FCC ID was found.
- Board: `boards/md9600` on `platform/stm32f4`. The board version is a hardware option (`hal/hal_hwopt.h`).

### TYT MD-380 / MD-390 / Retevis RT3

- STM32F405VGT6 with 1 MB flash and 192 KB RAM, and an HR_C5000 DMR baseband.
- RF: SKY72310 synthesizer and a GT3136 receiver IC. There's no AT1846S. The schematic notes warn that the schematic
  "is known to be not correct".
- Display: 160x128 color TFT on the FSMC 8-bit bus. The controller is reported as HX8302A or ILI9481, and RT3 and
  MD-380 panels are "slightly incompatible". The board reads the controller's ID at start-up and picks the panel type
  from it.
- SPI flash: W25Q128FV 16 MB; some VHF radios are reported to have only 1 MB.
- One band per radio: UHF 400–480 or VHF 136–174 MHz, 1/5 W. GPS on "G" models. The module is reported as both JS-H210
  and JS-M710.
- Update path: a 48 KB Tytera bootloader at the start of flash, the application after it. USB DFU (0483:df11) with
  non-standard commands. Read-out protection level 1. Images are encrypted with a key held in the bootloader.
- FCC ID PODMD-380, with an 8-page internal-photos exhibit
  ([fccid.io](https://fccid.io/PODMD-380/Internal-Photos/Internal-Photos-2688727)).
- Board: `boards/md380` on `platform/stm32f4`, with UHF and VHF images. The HR_C5000 is a variant of
  `drivers/modem/hrc6000` (a chip flag selects the register differences). DMR voice uses the codec of the user's
  D002.032, D35.14 or S35.14 file, merged at build time and run in place ([codec.md](../dev/codec.md)).

### Radioddity GD-77 family

- MK22FN512 MCU, HR_C6000, AT1846S, UC1701 display controller, 25Q80BV SPI flash + AT24C512 I2C EEPROM, no GPS. The
  MK22FN512VLL12 is a 120 MHz Cortex-M4F with 512 KB flash and 128 KB SRAM
  ([Keil device page](https://www.keil.arm.com/devices/nxp-mk22fn512vll12)); the exact suffix on the GD-77 board is
  not known.
- 128x64 mono display, the size the UX specs assume ([information-architecture.md](../ux/information-architecture.md)).
  No public spec sheet states the panel resolution.
- 136–174 / 400–470 MHz, 1/5 W. USB cable with a K1 plug and no UART chip in the cable.
- FCC ID 2AN62-GD77 (manufacturer listing).
- Update mode: hold S1 + Fn at power-on. Firmware updates over USB HID, no driver. Images are .sgl files. No public
  description of the .sgl encryption was found.
- Rebadges: the TYT MD-760 is the GD-77 (three sources agree). The GD-77S is the TYT MD-750 (keyless, no display).
  The DM-1801 is also sold as the DM-860, and only its version 1 hardware is in this family. The 2022 DM-1801 and
  DM-1801A are reported different, and a DM-1801 sold new today is an HR_C7000 radio
  ([Baofeng DM-1801 Pro and DM-1801 (current)](#baofeng-dm-1801-pro-and-dm-1801-current)). The DR-1801 has the same
  case but no part of this platform ([Baofeng DR-1801](#baofeng-dr-1801--dr-1801uv)). The RD-5R is the DM-5R Tier II
  in a new case. Sources contradict each other on the DM-1801A.
- Shared with the STM32 radios: the HR_C6000 and AT1846S drivers and the 128x64 display class. Not in RadioFoundry:
  the MK22 platform, a UC1701 display driver, an I2C EEPROM driver, and an .sgl image tool. The 512 KB of internal
  flash is half the STM32 radios' 1 MB. The HD1 v1 is the same platform with a color display and a serial bootloader.

### Radioddity GD-88

- 136–174 / 400–480 MHz, 2.5/7 W, 2" color TFT, built-in GPS and APRS, dual VFO, 3000 mAh, FCC ID 2AN62-GD88
  ([radioddity.com](https://www.radioddity.com/products/radioddity-gd-88)).
- "2 separate transceivers both usable at same time, even on same band"
  ([Radioddity blog](https://www.radioddity.com/blogs/all/radioddity-gd-88-review)).
- Uses "the new C7000, DMR and combined CPU chip". No public source confirms two HR_C7000 chips. The RF chip and the
  display controller are not known; the FCC internal photos are the place to look.
- Shares a CPS with the Retevis RT73 / Kydera CDR-300UV / Radioddity DB25-D and DB40-D, which points to one OEM
  platform ([codeplugs.rats.net](https://codeplugs.rats.net/help/cps)).
- Programming and updates go over a USB-serial cable (FTDI-class chip in the cable) into the K1 jack, to SoC A's
  UART1 at 115200. The radio has no USB port.
- Shared with the DM-32UV: `platform/hrc7000` and `drivers/modem/hrc7000`. Not in RadioFoundry: the RF chips, the
  display, and core support for two simultaneous receivers.

### Baofeng DM-32UV

- TX 136–174 / 400–470 MHz. RX adds 400–480, AM air band 108–136 and FM broadcast 65–108 MHz. 10/4/1 W, 2.0" color
  screen, built-in GPS, USB-C charging (the socket is on the battery pack, no data). FCC ID 2AJGM-DM32UV
  ([baofengradio.com](https://www.baofengradio.com/products/dm-32uv)).
- Air band AM and FM broadcast receive: [airband-and-broadcast.md](airband-and-broadcast.md).
- K1-style programming cable; an FTDI cable is the better choice.
- The HR_C7000 integrates a C-SKY CK803S core.
- PCB revisions DM32_UV_V1.2 and V1.4. Factory firmware 048/049
  ([M7OCM/DM-32UV](https://github.com/M7OCM/DM-32UV)). Update mode is PTT + SK1 at power-on.
- Flash: a 1 MB XIP program flash and a 16 MB data flash.
- RF: FD6818. Display: ST7789-class 240x320 panel. GPS: ATGM336H.
- Board: `boards/dm32` on `platform/hrc7000`.

### Baofeng DM-1801 Pro and DM-1801 (current)

Two listings sold new under the DM-1801 name. Neither is the DM-1801 v1 of the GD-77 family.

| | DM-1801 Pro | DM-1801 (current listing) |
|---|---|---|
| Power | 7 / 4 / 1 W | 5 W |
| Memory | 4000 channels, 250 zones, 800 contacts plus 50,000 by CSV import | 1024 channels and zones, 1024 contacts |
| Programmable keys | 5: SK1, SK2, the top key and the left and right arrows | 3: SK1, SK2 and the top key |

- **DM-1801 Pro:**
  - TX 136–174 / 400–470 MHz; RX adds AM air band 108–136 MHz and FM broadcast 65–108 MHz.
  - 1.77" display, 7.4 V 2200 mAh battery, Kenwood 2-pin accessory jack, no GPS, no USB-C charging.
  - Records DMR calls (about 10 hours) and exports them as WAV through the CPS (V1.60).
  - FCC ID 2AJGM-DM1801PRO ([baofengradio.com](https://www.baofengradio.com/products/dm-1801-pro),
    [manual](https://baofeng.s3.amazonaws.com/Baofeng_DM-1801_Pro_User_Manual_EN_20260916.pdf),
    [fccid.io](https://fccid.io/2AJGM-DM1801PRO)).
- **Chips:** an HR_C7000 is on the PCB in the FCC internal photos.
- **Same hardware as the DM-1801:** the filing includes an attestation that the DM-1801 and the DM-1801 Pro are
  identical except for color and distributor (FCC filing). The listings' different power, memory and key counts
  are then firmware and marketing differences.
- **Which DM-1801 revision this is:** the hardware matches neither v1 (MK22 + HR_C6000) nor the 2022 redesign,
  described as a single module. It may be a third revision, or the 2022 redesign may already have used the HR_C7000.
  Buying "a DM-1801" new doesn't get the v1 board.
- **Telling them apart:** the model and FCC ID label under the battery is the reliable check. The case alone isn't
  (see the DR-1801 below), and v1 also has a 128x64 mono display.
- Board: `boards/dm1801pro` on `platform/hrc7000`, with the FD6818 driver (its own register set), the ST7567 display
  driver and the shared factory calibration reader (`drivers/cal/baofeng`).

### Baofeng DR-1801 / DR-1801UV

- FCC ID 2AJGM-DR1801 ([fccid.io](https://fccid.io/2AJGM-DR1801)).
- **Chips:** one large LCC module marked "Auctus AM6102". Under the module's RF shield is an IC marked
  "EP8277 ABE10" (FCC internal photos). What the AM6102 integrates (MCU, DMR baseband, RF) and what the
  EP8277 is are not documented. Auctus also makes the AT1846S.
- **Easily mistaken for a DM-1801:** the case looks identical (FCC photos). A forum post claims the DR-1801 is
  different hardware from the DM-1801, and that Baofeng renamed the redesigned DM-1801 to DR-1801.
- Nothing in RadioFoundry drives an Auctus DMR module, and no public documentation for it was found.

### Radioddity DB25-D / DB25-D PRO / Retevis RT73 / Kydera CDR-300UV

The DB25-D is Kydera's CDR-300UV under Radioddity's name. Radioddity's FCC ID 2AN62-DB25D is a change in ID of
VO6CDR-300UV (Fujian New Century Communications). That filing keeps its internal photos, schematics and parts list
confidential ([fccid.io 2AN62-DB25D](https://fccid.io/2AN62-DB25D),
[fccid.io VO6CDR-300UV](https://fccid.io/VO6CDR-300UV)).
The **DB25-D PRO** has its own filing, 2AN62-DB25DPRO, with public internal photos. Those photos are the source of
the "FCC photo" tags below, so they hold for the PRO. Its display board is carried over from the DB25-D, but its
control, RX and TX boards are redesigns ("CDR300UV-MCU-BT-AI-TDA-1.0", "C7DR300UV-RX-BT-AD-3.0",
"C7DR300UV-TX-RD35-BT-AD-3.5")
([2AN62-DB25DPRO internal photos](https://fccid.io/2AN62-DB25DPRO/Internal-Photos/Internal-photos-8316141)).
One firmware image covers the DB25-D and the DB25-D PRO (readme in the
[firmware package](https://radioddity.s3.amazonaws.com/2025-02-10_DB25-D_firmware_update_release_2025-02-10.zip)).
The Retevis RT73 is reported as the same radio
([kydera-ham forum](https://kydera-ham.boards.net/thread/315/thoughts-radioddity-db25-mini-mobile)). No RT73 FCC ID
was found.

- **MCU and DMR:** one HR_C7000, marked "HR_C7000 D56323MP1" (FCC photo), with a C-SKY CK803S core as on the
  DM-32UV. Radioddity's update guide is on [radioddity.com](https://radioddity.com/products/radioddity-db25d).
  Program flash part: ?. It's larger than 1 MB: data sits above 0x0310C000.
- **RF:** the PRO's RF board has **two Beken BK4819** transceivers with a TCXO (FCC photo). The receiver IF is
  51.550 MHz ([extended manual v3.4](https://radioddity.s3.amazonaws.com/2025-05-01_Extended_manual_for_Radioddity_DB25-D_PRO_-_DB40-D_-_GD-88_-_v3.4.pdf),
  section 18.2). The release notes' batches D64, D66 and D6T are the FD6818 boards; **the PRO is DBB**. On the PRO,
  one BK4819 is the LO synthesizer at f_RX + 51.55 MHz and the other works at the IF in receive. Radioddity images
  older than the first with BK4819 support don't drive a PRO's RF, so they must not be loaded on one. Both chips share
  one 3-wire bus with separate enables. The FD6818 batches pair an FD6818 main transceiver with a 0x7813 (D64),
  FD6818 (D66) or AT1141 (D6T) LO. RadioFoundry drives all four from one image.
- **Bands and power:** 136–174 / 400–480 MHz, 20 W or 5 W, 13.8 V ([radioddity.com](https://radioddity.com/products/radioddity-db25d)).
  The FCC grant lists 19.7 W conducted. The PA is a discrete RF MOSFET on the TX board (photo). DAC B sets the PA
  power with a ramp at each burst, as on the DM-32UV.
- **Display:** 1.77" color TFT ([radioddity.com](https://radioddity.com/products/radioddity-db25d)). An RT73 review
  says 2" diagonal. The controller is ST7735S class, in landscape on a 4-wire serial bus, not the DM-32UV's 8080 bus.
  An ADC strap picks bit-banged GPIO or the SoC's SPI2 on the same pins. Radioddity's "white screen after update" fix
  fits that (manual 14.4).
- **Controls:** front keys power, P1, P2, P3, VOL/SQL, TONE/T.SEL, B/E and Menu/OK, plus one push encoder. The
  16-key hand mic adds PTT and P4–P7 (DTMF A–D) (extended manual v3.4, section 5.5). The mic board ("CKS-10A-2")
  carries a small QFN, likely an MCU, so the keys likely reach the radio as serial codes on RJ-45 pin 2.
- **Mic connector:** RJ-45. 1 power off (to ground), 2 PTT / radio RXD, 3 mic audio, 4 shield, 5 radio TXD, 6 ground,
  7 +8 V, 8 speaker. 3.3 V logic, 115200 baud (extended manual v3.4, section 17.6). The side K1 jack has the same
  serial pair (3.5 mm sleeve RXD, 2.5 mm ring TXD) (section 17.5).
- **Audio:** NXP TDA8542AT (2 x 1.5 W BTL) speaker amplifier (FCC photo). An "AI" noise-reduction daughter module
  ("CC1157G") sits between the C7000 and the mic and speaker paths. Its pads read AFES_IN/OUT, MICES_IN/OUT, SCL, SDA,
  NR_ON/OFF, ES_SW (FCC photo). Mic and speaker otherwise go through the C7000's codec as on the DM-32UV.
- **Storage:** Kioxia TC58CVG1S3HRAIJ, a 2 Gbit (256 MB) 3.3 V SPI NAND on SPI0, with a bad-block table (FCC photo;
  part decode [Elnec](https://www.elnec.com/en/device/KIOXIA/TC58CVG1S3HRAIJ+%5BWSON8%5D)). **Calibration** is a 2 KB
  page at NAND byte address 0x280000 with a second copy in program flash.
- **Other:** RDA5802N FM broadcast receiver (FCC photo). GPS: built in, SMA antenna port, module ?. Bluetooth:
  none on the DB25-D per the product page. The PRO's board name says "BT", but no module was fitted in the FCC
  unit. Coin-cell RTC backup (photo).
- **Programming and update:** Radioddity's "IAP" Windows tool over a serial cable. Hold P1 and press power, and the
  display stays blank with the backlight on (update guide). The original cable has a Prolific PL2303
  ([addendum](https://radioddity.s3.amazonaws.com/2021-09-13%20Radioddity%20Addendum%20to%20DB25-D%20manual%20V1.1.pdf)).
  FTDI cables work since firmware 909E.D4.EARSAB.018 (release notes). A ".flash" file mode updates the IAP loader
  itself (manual 14.6).
- **Image format:** raw code with no header, checksum or encryption, linked at 0x0300C000 behind a 48 KB loader, as on
  the DM-32UV. The PC link and the debug output are both UART1 at 115200.
- **Codec:** the AMBE+2 vocoder is software on the CK803S core, and no vocoder chip shows in the photos. The DM-32UV's
  codec model carries over: the codec comes from the user's own DB25-D file, at a different address in every image,
  so each file needs its own map ([codec.md](../dev/codec.md)). AMBE frames pass through the CPU.
- **Codeplug tools:** qdmr has a support request for the radio
  ([qdmr #165](https://github.com/hmatuschek/qdmr/issues/165)).
- **Not DMR:** the DB25-G, DB20-G, DB40-G and DB35-G GMRS mobiles and the "DB25 Pro" (no D) are analog FM radios
  ([radioddity.com DB25-G](https://radioddity.com/products/radioddity-db25g)). They aren't RadioFoundry targets.
- Board: `boards/db25d` on `platform/hrc7000`, with the `st7735` driver on a bit-banged serial bus,
  `drivers/rf/bk4819` for both chips (receiver and LO synthesizer), `drivers/rf/fd6818` and `drivers/rf/at1141` for
  the FD6818 batches, and `drivers/flash/spi_nand` for the NAND. The calibration reader takes the newer of the NAND
  and program-flash copies and never writes either.

### Radioddity DB40-D / Kydera CDR-700UV

- The DB40-D's FCC ID is VO6CDR-700UV, the Kydera CDR-700UV, with Bluetooth (2402–2480 MHz) in the grant
  ([radioddity.com](https://radioddity.com/products/radioddity-db40-d), [fccid.io](https://fccid.io/VO6CDR-700UV)).
  The internal photos are public ([internal photos](https://fccid.io/VO6CDR-700UV/Internal-Photos/Internal-Photos-6738006)).
- **Processors:** the main board has two identical large packages marked "A" and "B" on the silkscreen. Their
  markings can't be read in the photos. Updates take two files, (A) and (B), written separately in IAP_A and IAP_B
  mode, and the B update shows no progress on the radio (extended manual v3.4, section 14.3). So the radio very
  likely has two HR_C7000 SoCs, one per VFO, like the GD-88. The DB40-D's B firmware shares the GD-88's B49C prefix
  (release notes).
- **RF:** separate VHF and UHF receive chains and two PA modules, one per band. Shielded cans hide the RF chips
  (photos). RF chip: ?. 136–174 / 400–480 MHz, 40 W or 25 W (extended manual v3.4, section 18.6). The product page
  says 20/40 W. Fan cooled.
- **Display and controls:** 2.4" color TFT ([radioddity.com](https://radioddity.com/products/radioddity-db40-d)),
  controller ?. The front has three push encoders (VFO A volume, VFO B volume, channel), PF1, PF2, on-hook and
  off-hook keys, Back/Zone and Menu/OK (extended manual v3.4, section 5.6; photo). The mic is the same 16-key
  RJ-45 mic as the DB25-D's, with P5–P8.
- **Storage:** TC58-family SPI NAND, part ?.
- **Other:** GPS (active antenna supported), Bluetooth 5.0 module with an external antenna, RDA5802 FM broadcast
  receiver, external speaker jack. The CDR-700UV board has a SIM footprint for an LTE option the DB40-D doesn't use
  (photo).
- **Programming and update:** only through the RJ-45 at 3.3 V and 115200 baud (extended manual v3.4, section 17.6).
  The box contains an FTDI cable. IAP mode: hold the on-hook key and press power. The images are raw, plain
  and headerless like the DB25-D's: A is 814 KB, B is 419 KB.
- **Codec:** software in each C7000, so the codec comes from the user's own files as for the DB25-D, once per image if
  both processors decode voice.
- Not in RadioFoundry: two cooperating images (A runs the UI, GPS and Bluetooth; B is a second modem and receiver),
  the A↔B link protocol (shared with the GD-88), the dual-SoC core support, the 2.4" panel, two PA power tables, and
  fan control.
