<!-- SPDX-License-Identifier: MIT -->

# Radios: hardware survey and port plan

Status: research, 2026-10-10. This page collects the main chips of every radio ODR supports or plans to support, plus
close relatives, from public sources only. It also says which ODR platforms and drivers each radio could reuse and
suggests an order for the ports. Register-level facts live in the per-radio specs
([hardware-dm1701.md](hardware-dm1701.md), [hardware-dm32.md](hardware-dm32.md)). This page is the overview.

Facts are not copyrightable. Every claim carries a source link and a confidence tag:

| Tag | Meaning |
|---|---|
| **C** | Confirmed: manufacturer page, FCC filing, datasheet, or our own bench work on the radio |
| **D** | Documented by a reputable reverse-engineering project (OpenRTX hardware docs, PoC\|\|GTFO, md380tools README, schematic notes) |
| **F** | Forum post, review or retailer claim. Treat as likely but unverified |
| **?** | Not found or contradictory. Needs checking before a port starts |

**FCC internal photos.** FCC IDs are listed per radio, but the fccid.io and fcc.report exhibit PDFs couldn't be
fetched by script for this survey. No chip below is confirmed from an FCC photo yet. Reading the internal-photo
exhibits by hand is the cheapest way to turn D and F tags into C, and it should happen before a port starts.

## Summary table

| Radio (and rebadges) | MCU | DMR chip | RF | Display | Ext. memory | GPS | Update path | ODR today |
|---|---|---|---|---|---|---|---|---|
| Baofeng DM-1701, Retevis RT84 | STM32F405VG (C, bench) | HR_C6000 (C, bench) | AT1846S (C, bench) | 160x128 color, 8080 bus (C, bench) | 16 MB SPI NOR (C, bench) | none (C) | USB DFU on the MCU, wrapped images (D) | First target, on the radio |
| TYT MD-UV380 / MD-UV390, Retevis RT3S | STM32F405VG (D) | HR_C6000 (D) | AT1846S (D) | 160x128 color, HX8302A (D) | 25Q128FV, 16 MB (D) | optional, JS-H210 (D) | USB DFU on the MCU (D) | Planned: board variant of the DM-1701 |
| TYT MD-9600, Retevis RT90 (mobile) | STM32F405VG (D) | HR_C6000 (D) | 2x SKY72310 PLL on older boards; later boards changed (D, F) | 128x64 mono, ST7567 (D) | 25Q128FV, 16 MB (D) | optional, JS-M710 (D) | USB DFU on the MCU (D) | Planned |
| TYT MD-380 / MD-390, Retevis RT3 | STM32F405VGT6 (D) | HR_C5000 (D) | SKY72310 PLL + GT3136 receiver (D, F) | 160x128 color, HX8302A (D, controller ?) | W25Q128FV 16 MB; some early VHF 1 MB (D) | optional "G" models (D, module ?) | USB DFU, encrypted images (D) | Planned |
| Radioddity GD-77, TYT MD-760, GD-77S, Baofeng DM-1801 v1 / DM-860, RD-5R | NXP MK22FN512 (D) | HR_C6000 (D) | AT1846S (D) | 128x64 mono, UC1701 (D) | 25Q80BV 1 MB + AT24C512 64 KB EEPROM (D) | none (D) | USB HID bootloader, .sgl files (F) | Planned: new platform |
| Radioddity GD-88 | HR_C7000 class SoC (F) | HR_C7000 (F) | ? (two transceivers, F) | 2" color TFT (C), controller ? | ? | yes (C), module ? | Radioddity updater over K-plug cable (C) | Planned |
| Baofeng DM-32UV | HR_C7000 SoC, C-SKY CK803S core (F, BIN) | HR_C7000 (F, BIN) | FD6818 (community pin map) | 240x320 color, ST7789 class (community, BIN) | 16 MB data flash (F); program flash size ? | yes (C), ATGM336H (community) | Baofeng bootloader over UART cable (BIN, F) | Builds, not on a radio yet |

BIN and "community" mean the facts come from our own disassembly and the community pin map recorded in
[hardware-dm32.md](hardware-dm32.md), not from a public teardown.

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
color TFT, 25Q80BV + AT24C512, ST-26-U7L GPS, serial YMODEM bootloader over a PL2303 cable (D)
([OpenRTX HD1](https://openrtx.org/#/hardware/hd1), [OpenRTX hardware table](https://openrtx.org/#/hardware/hardware)).
A forum thread reports a 2023+ HD1 v2 on the HR_C7000, but posters contradict each other (?)
([opengd77.com archive](https://www.opengd77.com/archive/viewtopic4b3f.html?f=7&t=932&start=20)).

### TYT MD-UV380 / MD-UV390 / Retevis RT3S

- STM32F405VG (Cortex-M4F, 1 MB flash, 192 KB SRAM including 64 KB CCM (C)
  [st.com](https://www.st.com/en/microcontrollers-microprocessors/stm32f405vg.html)), HR_C6000, AT1846S, HX8302A display controller, 25Q128FV SPI flash, JS-H210 GPS on G models (D)
  ([OpenRTX hardware table](https://openrtx.org/#/hardware/hardware)). 160x128 color TFT. The stock bootloader takes
  the first 48 KB of internal flash, and the app starts at 0x0800C000 (D)
  ([OpenRTX MD-UV380](https://openrtx.org/#/hardware/mduv380)).
- The AT1846S is on I2C. The MD-UV390 Plus (10 W) board layout "differs somewhat" (D)
  ([OpenRTX MD-UV380 mods](https://raw.githubusercontent.com/OpenRTX/openrtx.github.io/master/M17/mduv380_mods.md)).
- The MD-UV390 is the same radio in a waterproof case. GPS is a hardware option; GPS firmware doesn't add GPS to a
  non-GPS radio (F) ([miklor](https://miklor.com/COM/Review_UV380.php)).
- FCC IDs POD-MDUV380 (granted 2018-10-19) and POD-MDUV390 (F, search snippets) ([miklor](https://miklor.com/COM/Review_UV380.php)).
- DFU entry: hold PTT and the button above it at power-on (D)
  ([OpenRTX user guide](https://raw.githubusercontent.com/OpenRTX/openrtx.github.io/master/user_guide.md)).
- A repair-guide snippet claims the dual-band radios use an NXP MK22. That contradicts every other source and is
  almost certainly wrong (?).
- Open firmware: OpenRTX supports FM and M17 (M17 needs a hardware mod), but not DMR (D)
  ([OpenRTX status](https://openrtx.org/#/dev_status)). md380tools does not support it (D)
  ([md380tools README](https://raw.githubusercontent.com/travisgoodspeed/md380tools/master/README.md)). The
  OpenGD77 project supports it.
- **ODR:** the same board family as the DM-1701 ([hardware-dm1701.md](hardware-dm1701.md) section 17). It needs a key
  map, battery and volume ADC scaling, a power table, its own DFU key and GPS on USART3. No new drivers except a GPS
  NMEA reader.

### TYT MD-9600 / Retevis RT90

- STM32F405VG, HR_C6000 (bit-banged SPI), two SKY72310 PLLs (one VHF, one UHF) with a 16.8 MHz TCXO "at least in the
  older models", ST7567 128x64 mono LCD sharing SPI2 with a W25Q128 flash, calibration in the flash security
  registers (D) ([OpenRTX MD-9600](https://openrtx.org/#/hardware/md9600)).
- Several PLL and IF chips were used in production. Board versions V2 (the first V1 boards were labeled V2), V3, V4,
  V4A (same as V5) and V5 exist, and a build for the wrong version boots but won't transmit or receive (F)
  ([opengd77.org t=83](https://www.opengd77.org/viewtopic.php?t=83),
  [opengd77.org p=15](https://www.opengd77.org/viewtopic.php?p=15)). A retailer ties V4/V5 to "PLL 2571" (F)
  ([passion-radio](https://www.passion-radio.com/mobile/md9600-505.html)). **The PLL on V4/V5 boards is unidentified (?)**.
- Front 4x3 key matrix, channel encoder, and hand-mic keys read as analog levels. The mic has 16 keys on an RJ-45
  jack (D, F) ([OpenRTX MD-9600](https://openrtx.org/#/hardware/md9600), [miklor](https://www.miklor.com/COM/Review_MD9600.php)).
- 136–174 / 400–480 MHz (D). Spec power is 50 W VHF and 45 W UHF; the same retailer page also says 40 W UHF (F).
  Miklor measured 44 W at 146 MHz and 40 W at 446 MHz (F).
- Mini-USB goes to the MCU's own USB, which enumerates as STM32 DFU 0483:df11 (D). Miklor's claim of a "UART chip
  inside" is contradicted by that.
- FCC ID POD-DMR3 (TYT), granted 2017-12-28, with class II changes in 2020 and 2021 (MD-9600-5P GPS), three
  internal-photo exhibits (C) ([fccid.io](https://fccid.io/POD-DMR3)). No separate RT90 FCC ID was found.
- Open firmware: OpenRTX boots, with display, keys and RTC; RF isn't done (D)
  ([OpenRTX status](https://openrtx.org/#/dev_status)). The OpenGD77 project has per-version builds.
- **ODR:** reuses `platform/stm32f4`, `drivers/modem/hrc6000`, `drivers/flash/spi_nor`, the 128x64 display class
  already in the UI ([ui.md](ui.md)). New: a SKY72310 synthesizer driver plus the per-band receive chain, an ST7567
  display driver, analog mic-key decoding, and a board-version probe or per-version builds.

### TYT MD-380 / MD-390 / Retevis RT3

- STM32F405VGT6 with 1 MB flash and 192 KB RAM, and an HR_C5000 DMR baseband (D)
  ([PoC||GTFO 0x10 article 8](https://mcfp.felk.cvut.cz/publicDatasets/pocorgtfo/contents/articles/10-08.pdf),
  [holgerschurig/md380 schematic notes](https://github.com/holgerschurig/md380)).
- RF: SKY72310 synthesizer (D) ([OpenRTX hardware table](https://openrtx.org/#/hardware/hardware)) and a GT3136
  receiver IC. There's no AT1846S. The schematic notes warn the schematic "is known to be not correct" (F)
  ([holgerschurig/md380](https://github.com/holgerschurig/md380)).
- Display: 160x128 color TFT (D) ([OpenRTX MD-380](https://openrtx.org/#/hardware/md380)) on the FSMC 8-bit bus.
  The controller is reported as HX8302A ([OpenRTX table](https://openrtx.org/#/hardware/hardware),
  [DL4YHF](https://qsl.net/d/dl4yhf/RT3/md380_hw.html)) or "maybe an ILI9481" (holgerschurig). RT3 and MD-380
  panels are "slightly incompatible" (F). **Controller unconfirmed (?)**.
- SPI flash: W25Q128FV 16 MB, but md380tools says some VHF radios have only 1 MB (D)
  ([md380tools README](https://raw.githubusercontent.com/travisgoodspeed/md380tools/master/README.md)).
- One band per radio: UHF 400–480 or VHF 136–174 MHz, 1/5 W (F) ([miklor](https://miklor.com/MD380/380-Specs.php)).
  GPS on "G" models. OpenRTX names the module JS-H210 on its MD-380 page but JS-M710 in its hardware table (?).
- Update path: a 48 KB Tytera bootloader at 0x08000000, app at 0x0800C000. USB DFU (0483:df11) with non-standard
  commands. Read-out protection level 1. Stock images are encrypted with a key held in the bootloader, and the scheme
  was publicly broken (D) ([PoC||GTFO](https://mcfp.felk.cvut.cz/publicDatasets/pocorgtfo/contents/articles/10-08.pdf)).
- FCC ID PODMD-380, granted 2015-07-22, with an 8-page internal-photos exhibit (F, search snippet)
  ([fccid.io](https://fccid.io/PODMD-380/Internal-Photos/Internal-Photos-2688727)).
- Open firmware: OpenRTX supports FM and M17 but not DMR (D) ([OpenRTX status](https://openrtx.org/#/dev_status)).
  md380tools patches the stock firmware. The OpenGD77 project has a port.
- **ODR:** reuses `platform/stm32f4`, `drivers/flash/spi_nor`, `drivers/input/keymatrix`, probably
  `drivers/display/st7735` (if the panel really is HX8302A). New: an HR_C5000 modem driver (how much of
  `drivers/modem/hrc6000` carries over is unknown until the two datasheets are compared), a SKY72310 driver (shared
  with the MD-9600), and the analog receive chain.

### Radioddity GD-77 family

- MK22FN512 MCU, HR_C6000, AT1846S, UC1701 display controller, 25Q80BV SPI flash + AT24C512 I2C EEPROM, no GPS (D)
  ([OpenRTX hardware table](https://openrtx.org/#/hardware/hardware)). The MK22FN512VLL12 is a 120 MHz Cortex-M4F with
  512 KB flash and 128 KB SRAM (C) ([Keil device page](https://www.keil.arm.com/devices/nxp-mk22fn512vll12)); that
  exact suffix on the GD-77 board is not confirmed (?).
- 128x64 mono display, the size our UX specs assume ([information-architecture.md](../ux/information-architecture.md)).
  No public spec sheet found states the panel resolution (?).
- 136–174 / 400–470 MHz, 1/5 W (D, F) ([OpenRTX GD-77](https://openrtx.org/#/hardware/gd77),
  [miklor](https://www.miklor.com/COM/Review_GD77.php)). USB cable with a K1 plug and no UART chip in the cable (F).
- FCC ID 2AN62-GD77 (C, manufacturer listing).
- Update mode: hold S1 + Fn at power-on. Firmware updates over USB HID, no driver. Stock images are .sgl files (F)
  ([OpenGD77 user guide copy](https://www.scribd.com/document/619619664/OpenGD77-User-Guide2)). No public description
  of the .sgl encryption was found (?).
- Rebadges: the TYT MD-760 is the GD-77 (three sources agree). The GD-77S is the TYT MD-750 (keyless, no display).
  The DM-1801 is also sold as the DM-860, and only its version 1 hardware is in this family; the 2022 DM-1801 and
  DM-1801A are reported different. The RD-5R is the DM-5R Tier II in a new case (F)
  ([opengd77.org p=26](https://www.opengd77.org/viewtopic.php?p=26),
  [w0chp](https://w0chp.radio/ogd77-webcpsd/supported-radios/),
  [swenton.com](https://swenton.com/ham-radio/ham-radio-articles/opengd77)). Sources contradict each other on the
  DM-1801A (?).
- Open firmware: OpenRTX supports the GD-77 and DM-1801 with gaps, such as no settings persistence (D)
  ([OpenRTX status](https://openrtx.org/#/dev_status)). The OpenGD77 project supports the whole family.
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
  **No public source confirms two HR_C7000 chips, or names the RF chip or display controller (?)**. The FCC
  internal photos are the next thing to read.
- Shares a CPS with the Retevis RT73 / Kydera CDR-300UV / Radioddity DB25-D and DB40-D, which points to one OEM
  platform (F) ([codeplugs.rats.net](https://codeplugs.rats.net/help/cps)).
- **ODR:** reuses `platform/hrc7000` and `drivers/modem/hrc7000` from the DM-32UV. New: the RF chip(s), the display,
  and core support for two simultaneous receivers if the hardware really has two independent DMR paths.

### Baofeng DM-32UV

- TX 136–174 / 400–470 MHz. RX adds 400–480, AM air band 108–136 and FM broadcast 65–108 MHz. 10/4/1 W, 2.0" color
  screen, built-in GPS, USB-C charging. FCC ID 2AJGM-DM32UV (C)
  ([baofengradio.com](https://www.baofengradio.com/products/dm-32uv)).
- K1-style programming cable, with FTDI preferred (F) ([miklor](https://www.miklor.com/COM/Review_DM32.php)).
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

## Relatives worth supporting

| Radio | Chips | Reuse | Notes |
|---|---|---|---|
| TYT MD-2017 / Retevis RT82 | STM32 + HR_C6000, two RF transceiver ICs (F) | stm32f4, hrc6000, at1846s (if that's what the RF ICs are, ?) | True dual receive possible in hardware, not in stock firmware (F) ([radioreference](https://forums.radioreference.com/threads/is-the-md2017-true-dual-receive.414074/), [opengd77.org p=26](https://www.opengd77.org/viewtopic.php?p=26)) |
| Baofeng DM-1702 / DM-X | STM32 or MK22 + HR_C6000 (F) | hrc6000; platform ? | USB protocol reverse-engineered by MD1702-tools (F) ([dmrconfig issue 39](https://github.com/OpenRTX/dmrconfig/issues/39)) |
| Ailunce HD1 (v1) | MK22FN512, HR_C6000, AT1846S, 160x128 color, GPS (D) | GD-77 port + 160x128 class | Out of production, so used units only. Encrypted stock firmware, decoder exists (F) |
| Connect Systems CS7000-M17 | STM32F405VG, HR_C6000, SKY72310, HX8302A, 25Q128FV (D) ([OpenRTX table](https://openrtx.org/#/hardware/hardware)) | stm32f4, hrc6000, SKY72310 driver from the MD-9600 / MD-380 | Free once the SKY72310 driver exists. The Plus model has an STM32H743 |
| Retevis RT73 / Kydera CDR-300UV / Radioddity DB25-D, GD-73, TYT MD-9800 | HR_C7000 (F) ([opengd77.org p=26](https://www.opengd77.org/viewtopic.php?p=26)) | hrc7000 platform and modem | Popular mobiles. Encrypted vendor loaders (DB25-D, F). Need per-radio hardware facts |
| Anytone AT-D868UV / AT-D878UV / 878UVII, BTECH DMR-6X2, Alinco DJ-MD5, Radioddity GD-AT10G | GD32F303VGT6 (D) ([OpenRTX UV878](https://openrtx.org/#/hardware/uv878)), **SCT3258 DMR baseband with built-in AMBE+2, not an HR_C6000** (D, F), AT1846S, 160x128 color (D, F) ([VK7ZJA mods guide](https://pnwdigital.net/wp-content/files/documents/Anytone/Mods_868_878_878-II_information_VK7ZJA(sk)-4-2-23_.pdf)) | at1846s | No update encryption or signing (D). The vocoder lives in the SCT3258, so no codec merge is needed. New: a GD32F30x platform and an SCT3258 driver. The 878 and DMR-6X2 are the same hardware (F) |
| Anytone AT-D578UV (mobile) | GD32F303VKT6, AT32F43x after mid-2022, SCT3258 (F) | as the 878 | MCU change mid-run |

Not DMR candidates: the BK4819 and BK4829 are analog FM transceivers, and the BK4829 adds only an FSK modem (C)
([datasheet listing](https://www.alldatasheet.net/datasheet-pdf/pdf/2171272/BEKEN/BK4829.html)). The Radtel RT-4D does
DMR through a BK4819 plus an HF6853 vocoder module (D) ([OpenRTX table](https://openrtx.org/#/hardware/hardware)), a
different architecture from anything in ODR. The Retevis C62 and Quansheng-style radios are FM only.

## What exists and what's new

| Building block | In ODR now | Needed by |
|---|---|---|
| `platform/stm32f4` | yes | DM-1701, MD-UV380, MD-9600, MD-380, MD-2017, CS7000 |
| `platform/hrc7000` | yes | DM-32UV, GD-88, RT73 family |
| `drivers/modem/hrc6000` | yes | every HR_C6000 radio above |
| `drivers/modem/hrc7000` | yes | the HR_C7000 radios |
| `drivers/rf/at1846s` | yes | DM-1701, MD-UV380, GD-77 family, HD1, Anytone |
| `drivers/rf/fd6818` | yes | DM-32UV (GD-88 ?) |
| `drivers/display/st7735` | yes | 160x128 color radios (HX8302A compatibility per radio ?) |
| `drivers/display/st7789` | yes | DM-32UV |
| `platform/mk22` | **new** | GD-77 family, HD1 |
| `drivers/modem/hrc5000` | **new** | MD-380 |
| `drivers/rf/sky72310` (+ receive chain) | **new** | MD-380, MD-9600 (older boards), CS7000 |
| UC1701 / ST7567 mono display driver | **new** | GD-77 family (UC1701), MD-9600 (ST7567). Both are 128x64-class page-addressed controllers, so one driver may cover both (?) |
| I2C EEPROM | **new** | GD-77 family |
| GPS NMEA reader on a UART | **new** (as a board driver) | MD-UV380G, MD-9600G, MD-380G, DM-32UV, GD-88 |
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
4. **TYT MD-380 / MD-390 / RT3.** It reuses the STM32 platform and the MD-9600's SKY72310 driver. The HR_C5000 modem
   is the main new work, and the radios are older and single band. Effort: weeks.
5. **HR_C7000 family after the DM-32UV is on air**: GD-88, then RT73 / CDR-300UV / DB25-D. Start by reading the FCC
   internal photos, since the GD-88's chip list is mostly unknown.
6. **Opportunistic:** CS7000-M17 (nearly free after step 3), MD-2017 (dual receive) and DM-1702. The Anytone family
   is the biggest prize by popularity, but it needs a new MCU platform and a new DMR chip driver for the SCT3258, so
   it comes last.
