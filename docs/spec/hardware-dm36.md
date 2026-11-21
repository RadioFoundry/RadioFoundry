<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Baofeng DM-36 hardware

## 1. Board

The board marked DM36-C7000-V2.0 uses an HR_C7000, an FD6818B and a 240 × 320 color panel. The owner’s handset reported stock version DM36.NRFB.01.020 and bootloader BFUV36-V1. The programming jack carries the wired host link. USB-C on the battery supplies charging power. [HR_C7000 platform](platform-hrc7000.md)

## 2. Reset and program flash

The application starts at 0x0300C000. The first 48 KiB of the 1 MiB program flash contain the factory bootloader. The linker reserves program flash from 0x030E8000 for the codec slot. A codec-free build boots and can exercise the user interface and debug link.

### 2.1 Codec windows

The NRFB.01.020 codec profile identifies the code and tables by hash. Tables load at 0x18000EFC and end at 0x18003E74. For DMR voice the owner can merge the codec from their matching program-flash backup. The repository contains the placement map and hashes. [Codec placement](codec-placement.md#hr_c7000-radios)

## 3. Wired programming

The programming jack uses UART0 at 115200 baud. Power up on the cable and use the debug link to read the board name and revision. The battery's USB-C socket provides no data link. For the first install, use the supervised boot-ROM stub procedure with verified backups of both flashes. The DM-36 board profile excludes the factory updater. [Board installation instructions](../../boards/dm36/board.toml)

## 4. GPIO

### 4.1 Boot state

The common initialization latches power, disables both PA selects, and mutes the amplifier. The board then applies its reset levels before setting the three final GPIO direction registers. The module enable at PTB16 is held low. The board preflash host test exercises initial GPIO values and verifies the resulting levels and directions. [Board initialization](../../boards/dm36/io_init.h)

## 5. Controls

### 5.1 Knobs

The top channel encoder uses PTB7 and PTB6, with two transitions per detent. The separate volume/power knob is a potentiometer on ADC0. The firmware maps its measured taper to the speaker volume steps.

### 5.2 Keys

The keypad shares pins with the display. The two side keys have their own active-low inputs. The board pin definitions give the matrix ordering and GPIO assignments. [Board pins](../../boards/dm36/pins.h)

## 6. Module power

PTB16 holds the fitted module off during reset. The board uses the programming cable for host traffic.

## 7. Display

The shared ST7789-class display driver drives the 240 × 320 panel over the i8080 bus. Verify the visible edges and backlight on the handset. [Panel reset and initialization](display-st7789.md#reset-and-initialization)

## 8. Radio

The FD6818B provides the RF path for the HR_C7000 baseband. The rated bands are 136–174 and 400–470 MHz. The board's UHF tuning table extends to 480 MHz. Airband, broadcast and extended coverage entries are receive-only. [FD6818 bus timing](rf-fd6818.md#bus-timing)

### 8.3 DMR receive and key-up

The measured UHF receive setting on the owner's handset is FD6818 register 0x48 = 0xB2F8 in DMR mode. The board has its own PA key-up parameters. Absolute output power at each setting requires a wattmeter. The earlier conducted measurements used a build with a software PA drive ceiling. A bench that relies on that ceiling requires the image that implements and reports it.

## 9. Audio and power

### 9.1 Speaker route

PTB15 selects the audio source and PTB14 follows the radio mode. The FD6818 AF output is gated during FM receive. FM volume uses register 0x48 bits 3–0. DMR listening volume uses the HR_C7000 line-out gain and preserves the modem input level.

### 9.3 ADC connections

ADC0 measures the volume knob and ADC2 measures the battery. This board does not supply a VOX hook. [Platform microphone level](platform-hrc7000.md#microphone-level)

### 9.4 Battery scale

The owner's handset gave 103 ADC counts per volt. Verify the reading against a meter on another handset.

### 9.5 Volume taper

The board uses nine points from sweeps of the volume knob to interpolate its position. The host test checks the FM gain steps and the mute end of the range.

### 9.6 Broadcast receiver

The receiver uses PTA25 and PTA26 for the control bus and an active-low enable on PTA29.

## 10. Data flash

The owner's 16 MiB SPI NOR reported JEDEC ID 85 20 18. Populated stock ranges extended through 0x6E7FFF.

### 10.1 Calibration

The factory calibration is in the page pool below 0xC9000. The owner's handset had one page tagged 2 at 0x78000. A calibration reading from an earlier build is evidence for that handset only.

### 10.3 RadioFoundry storage

The DM-36 uses the [16 MiB data flash layout](platform-hrc7000.md#data-flash-layout). Its open-firmware region ends at 0xE00000. The next MiB stays reserved before the native codeplug at 0xF00000.

## 11. Bring-up checks

Keep complete program- and data-flash backups before installing an image. The owner's integrated build has separate handset results. On this standalone build, check the wired link, display, keypad, both knobs, battery reading and calibration before any RF test. Check FM and DMR receive and transmit with a second receiver, voice with an owner-supplied codec, and power on a meter. [Handset results from the integrated build](https://github.com/RadioFoundry/RadioFoundry/pull/607#issuecomment-6942014561)
