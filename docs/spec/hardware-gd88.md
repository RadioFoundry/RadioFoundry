<!-- SPDX-License-Identifier: MIT -->
# Radioddity GD-88 (Kydera DR-880UV) hardware

Status: bring-up, 2026-10-21. RadioFoundry builds for both SoCs (`boards/gd88`, `boards/gd88b`; section 13) but has not run
on a GD-88 yet. Sections 1-5 record how the GD-88 programs and updates (a USB-serial cable, not native USB). Sections
6-12 are the hardware facts the port is written from. The survey entry is in [radios.md](radios.md); the HR_C7000 facts it builds
on (UART registers, USB core, clock gates) are in [hardware-dm32.md](hardware-dm32.md).

**Answer: a USB-serial cable.** The radio body has no USB connector. The PC link is a K1 (2.5 mm + 3.5 mm) jack,
and the cable carries a USB-to-UART converter chip. In the stock firmware, SoC A runs the PC protocol on its
**UART1** at 115200 baud. Neither stock image accesses the SoC's USB controller. Confidence is high: the vendor
manual, the vendor's PC tools and the disassembly agree.

## Confidence tags

| Tag | Meaning |
|---|---|
| **DS** | HR_C7000 user guide (2016 user manual and the V2 software application manual) |
| **BIN** | Observed by disassembly of the official GD-88 firmware images (facts only: addresses, values, string references; no code carried over) |
| **COM** | Vendor or community public material: Radioddity manuals, release notes, product page, and the vendor's PC installers (inspected as data, never run) |
| **INF** | Inferred by us from the above; not seen directly |
| **HW** | Seen on a radio, running the stock firmware or RadioFoundry |

Images examined (BIN): A `B49E.D64.EOPSAB.021` (2025-01-19) and B `B49C.D64.EHRNAB.012` (2025-01-17), plus
A `B49E.D64.EOPSAB.013` and B `B49C.D64.EHRNAB.008` (2023-03) as a cross-check, and a raw byte scan of the
2022-10 A/B pair. All are plain, headerless images that load at 0x0300C000, the same application base as the
DM-32UV (BIN). Each image copies a RAM-code block to 0x10000 at start-up. In image A it comes from flash 0x030B404C
and fills 0x10000–0x34763 (BIN).

---

## 1. SoC pair and roles

| Item | Finding |
| --- | --- |
| SoCs | Two HR_C7000 (C-SKY CK803S core each), called A and B, with the same start-up as the DM-32UV and the HR_C7000 vocoder library in both |
| A | Master: display, keys, GPS, FM broadcast receiver, menus, the PC programming protocol, and resetting B |
| B | Second transceiver (VFO B). It has no GPS or PC programming protocol. Its UART1 only carries a factory tuning and flash-copy command set |
| A↔B link | UART3 (0x14090000) at 115200 on both chips, interrupt vector 45 on both |
| GPS (A only) | UART2 (0x14050000), interrupt vector 43. It starts at 9600 and the firmware sends the receiver a port-configuration sentence |
| UART0 (0x14030000) | Not used by either application. On the DM-32UV this is the programming-cable UART |
| Storage | One SPI NAND per SoC, TC58 family (Toshiba TC58CVG1S3HRAIJ) (unverified) |

UART vectors follow the PIC table: UART1 source 10 / vector 42, UART2 11 / 43, UART0 12 / 44, UART3 13 / 45,
USB 21 / 53 (datasheet).

## 2. Programming and update link

| Item | Finding | Tag |
|---|---|---|
| Radio connector | K1 jack on the right side of the GD-88. Radio RXD (data to radio) shares the 3.5 mm sleeve with PTT. Radio TXD (data from radio) is on the 2.5 mm ring. GND is on the 2.5 mm sleeve. The 3.5 mm tip carries +5 V through 100 Ω (not a supply) | COM (extended manual v3.4, 11.1 and 17.5) |
| Other connectors | SMA antenna socket and a 2-pin spring battery contact. The radio charges in a desktop charging base. Neither the product page nor the manual mentions a USB or Type-C port on the radio or the battery | COM |
| Cable | Radioddity: "a small chip that converts signals to and from the USB-side into serial signals to and from the K1 / RJ45 style connector". It appears on the PC as a virtual COM port, and a driver download is offered if Windows does not load one. The DB40-D box lists the cable as "FTDI-based"; the GD-88 box list says only "Programming Cable". Firmware and CPS release notes since 2022-10 add "support for programming cable based on FTDI-chip" | COM |
| Which SoC | A, UART1 (0x14040000), interrupt vector 42, set to 115200 by the start-up task. The A routines that read and write UART1 serve the PC protocol: model handshake strings "+DRS-880plus" and "+136-480MHZ+", 2 MB read/write blocks, record read, factory data, the B reset, and a command-word table shared with the DB25-D family (section 6). The same UART also sends GPS NMEA out to the PC | BIN |
| Baud | 115200 8N1 in the application (BIN). The vendor's IAP updater offers "115200,n,8,1" and "38400,n,8,1" | BIN, COM |
| Electrical level | Not stated for the K1 jack. The DB40-D's RJ-45 programming port, on the same platform, is documented as 3.3 V, 115.2 kbaud | COM; INF for the GD-88 level |
| Speaker with the cable in | The internal speaker still plays with the programming cable plugged in: the stock power-on tone sounds (2026-10-26). The plug doesn't switch the speaker off, so beeps and receive audio can be heard while the debug link runs. Whether the plug cuts the internal microphone is not known | HW; mic unknown |
| B's own UART1 | Set to 115200 with a receive handler, but it carries only factory tuning / flash-copy commands. Whether it is wired to the K1 jack (directly or through a switch), to test pads, or to nothing is unknown | BIN, INF |

### PC tools

The vendor's IAP updater and CPS are Windows programs that list the serial ports and need no driver of their own: they
use the cable's virtual COM port. The updater writes the application and takes a separate `*.flash` file for the data
flash. A native USB device would ship a vendor INF or rely on a class driver and a known VID/PID; neither installer has
one, and the manual says the cable holds the converter chip.

## 3. USB controller use

**None.** Neither SoC's application accesses the HR_C7000 USB core: no address in the USB register range is built, the
USB interrupt (source 21, vector 53) is never registered, and no USB descriptor or USB string exists in any image from
2022 to 2025. The clock manager's USB gates (CLK_MGR 0x02C bits 16/17) are switched off at start-up, and the 48 MHz USB
divider (0x028) is never written.

### SoC pins (datasheet)

The USB data pins are the dedicated balls F1 (USB_DP) and F2 (USB_DM), with USB_AVDD33 on G3 (as on the DM-32UV). The
2016 user manual's pin-mux table also lists USB_DM / USB_DP as function 4 of PTA13 / PTA14 (balls L16 / M14, which are
UART3 RXD / TXD), but the IO-mux register for those pins marks the matching value 2 as reserved (conflicting). On the
GD-88 those balls carry the A↔B link, so they are not available for USB.

## 4. IAP bootloader entry

| Item | Finding | Tag |
|---|---|---|
| Entry | Radio off, cable connected, CPS closed. Hold the upper side key SK1 (the vendor's P2) and turn the radio on with the volume knob. The status LED lights red and a special screen shows "IAP_A" or "IAP_B" on the top line. Then release SK1. It can take more than one try | COM (update guide v2.2, release notes 2024-06) |
| A or B target | In IAP mode, long-press SK1 to switch between IAP_A and IAP_B. The A file goes to IAP_A and the B file to IAP_B. Write the codeplug after updating both | COM |
| Bootloader code | Not in the update files: they start at the 0x0300C000 application base, so the bootloader sits below it, as on the DM-32UV. No dump is available, so how IAP_B reaches SoC B (A relaying over the UART3 link, or a switch moving the cable to B's UART) is not known | BIN, INF |
| How IAP_B may reach B | The updater's form has no A or B choice and sends the same commands for both: the target is picked on the radio only. Both applications use the same pads for UART1, PTA9 (RXD, also the K1 PTT line) and PTA10 (TXD), and both have the same release (IO_DIPLEX0 bits 9 and 10 set, both pins GPIO inputs). B releases them at boot. A tells B to release them at the start of every PC session (link id 0x6200) and to listen to PTA9 again at its end (0x6201). That points to the cable's two lines reaching both SoCs, with one owner at a time, so in IAP_B B's loader would answer the cable itself while A's lets go. A relaying (the applications already pass frames both ways over the link) is the other candidate. No reset or boot-strap line from A to B is in either application. RadioFoundry's B releases the pads at start-up as the original firmware does. A continuity check from the K1 pads to both SoCs, or a `Flash Read ` probe in IAP_A and in IAP_B, settles it | BIN, INF |
| Software entry | The string "BLL_IAP_Flash" is a function-name tag in a log call (line constant 1445). That the routine hands over to the bootloader is not shown | BIN, INF |
| ROM serial boot | The HR_C7000 boot ROM offers serial (UART0) boot or SPI-flash boot, no USB boot (DS). The datasheet names UART0 as the debug and boot-upgrade port. Where A's and B's UART0 balls go on the GD-88 board is unknown | DS, INF |

## 5. Open questions

1. Is the K1 cable wired to A's UART1 directly, or through a switch that the bootloader also points at B when
   IAP_B is selected? Section 4 points to both SoCs' PTA9 / PTA10 on the same lines. Needs a board photo or a
   continuity check with the radio opened (no live probing).
2. Which converter chip is in the GD-88 cable as shipped (FTDI per the DB40-D box list and the release notes;
   other batches unknown)? Plugging the cable into a computer and reading its VID:PID answers it, with no radio needed.
3. The K1 data level (3.3 V like the DB40-D's RJ-45, or something else).
4. Where the UART0 balls of A and B go (test pads?), for ROM serial-boot recovery.
5. Is there any USB data path at all, for example test pads on the F1/F2 nets? Nothing public suggests one. The
   FCC filing 2AN62-GD88 internal photos were not reachable.

## 6. Start-up, clocks and the PC command layer

All facts from the same A and B images (BIN). Detail and addresses are kept outside the repository.

| Item | Finding |
|---|---|
| Start-up | PSR 0x80000100, stack 0x00050000, VBR 0x00010000 (vector table in SRAM, copied from flash). Cache controller 0xE000F000 set as on the DM-32UV (cacheable 0x03000000, size code 0x2D). Start-up copies a fast block to 0x18000800, a code and table block to 0x10000, then clears bss |
| Clock modes | A has three: 24 MHz oscillator, 168 MHz (PLL 24 MHz / 1 x 28 / 2 / 2) and 192 MHz (24 MHz / 2 x 64 / 2 / 2). The XIP flash divider is 2 at 168 MHz and 4 at 192 MHz. B has the same register pattern |
| PC command words | `Erase`, `Flash Write`, `Flash Read `, `Record Read` (first 11 bytes), the same words as the DB25-D. `Record Read` returns an identification block: firmware version string, build date, build time, `+DRS-880plus`, `+136-480MHZ+` and two 6-byte fields |
| USMART console | Only the 2025 A build (`.021`) carries the USMART V3.1 debug console: the banner `USMART V3.1`, the `USMART_ENTIMX_SCAN` runtime message and an `@AT` string. The older A builds and the B images do not. The DB25-D's vendor firmware has the same console, and RadioFoundry loads its RAM stub through it without the bootloader ([hardware-db25d.md](hardware-db25d.md)). The GD-88 route is not traced yet: UART, frame format and hook address are unchecked |
| Other models | The A image carries the handshake strings of sibling models (`+DR-850`, `+DR-320`, `+DR-330`, `+DR-350`, `+DR-360`, `+DR-500Plus`, `+DR-F24`) |

## 7. Storage

| Item | Finding |
|---|---|
| NAND | SPI NAND on SPI0, chip select PTA18, standard SPI-NAND opcodes; geometry is read from the chip at run time. Each SoC has its own |
| Partition table | Names and NAND byte bases: font 0x040000, debug 0x100000, userop 0x280000, LinkMan 0x2C0000, xiepin1 0x380000, xiepin2 0x3C0000, App 0x400000 |
| Calibration | Primary copy is one 2 KB NAND page at 0x280000 starting with `TUNE` and a 32-bit counter (higher = newer). A backup copy sits in program flash at 0x031FE000 (4 KB). The firmware compares the two at boot and copies the newer over the older. Same layout and rule as the DB25-D: RadioFoundry reads both and never writes them |

## 8. Memory map and the codec

| Item | Finding |
|---|---|
| Program flash | 2 MB at 0x03000000 on each SoC. The IAP loader is below 0x0300C000. RadioFoundry's first stage takes 0x0300C000-0x0300D000 and the application starts at 0x0300D000 ([boot/README.md](../../boot/README.md)) |
| Data in program flash | The display record at 0x031FD000 (`lcd` and a type byte) and the calibration copy at 0x031FE000 (section 7) |
| Codec slot | 0x030E8000-0x03100000 on each SoC. Both SoCs run A's codec, merged on the user's machine from A's vendor image (`tools/hrc7000_codec.py`, boards `gd88` and `gd88b`). B's slot is byte for byte A's: the slot header names the run address and the tables address, and both are the same on the two SoCs. The user supplies only A's file, or the web flasher reuses the slot it wrote to A when it installs B through A's bridge. B's own vendor image is still accepted for B (`[codec] from` in `boards/gd88b/board.toml`) |
| Codec RAM | The code runs at 0x44E00-0x58000. Its tables go to modem SRAM: A's builds use 0x18001204-0x18004324, B's 0x18000B40-0x18003B10. A links with A's window. B links with the union, 0x18000B40-0x18004324, so that it takes A's codec or its own. The only other modem SRAM use is the crash record at 0x18007EF0 |
| SRAM | 288 KB at 0x10000 |

## 9. SoC A pins and chips

Pin index 0-31 = PTA, 32-63 = PTB, 64-95 = PTC.

| Function | Pin / bus |
|---|---|
| Key matrix | 6 rows PTB18-23 (outputs, idle low) by 3 sense lines PTB9-11 (inputs). Plus three keys read directly on the sense lines (9.2); 20-tick debounce |
| Power key / latch | PTB12 in (high = pressed); PTB13 out, high keeps power on (9.1) |
| Rotary encoder | PTB5, PTB8, both edges: one event +1 or -1 |
| TX / RX line | PTB2, high = transmit. The modem's RF TX and RX interrupts also switch it, so on DMR it follows every burst. It is wired to B's PTB2 too: B keys the PA on its edges (section 10) (BIN) |
| Speaker amplifier | PTC18 high, together with the serial chain's amplifier bit (9.3) |
| PTC20 | Low cuts the modem's audio line while a beep plays (INF). Low at start-up |
| PTT / K1 data | PTA9, shared with UART1 RXD; low = PTT |
| UARTs | UART1 PTA9/10 PC cable (115200); UART2 PTA11/12 GPS (9600); UART3 PTA13/14 link to B (115200) |
| GPS power | PTB17, high = on. On start-up the receiver gets `$CFGPRT,,0,9600,3,3` |
| LCD | 320x240 on the i8080 controller (0x12000000), reset PTC2 (low 10 ms, high 130 ms, twice). Four init variants (9.4) |
| Backlight | PWM channel 0 on PTA15, 1 kHz. The key light is PWM channel 1 on PTA16 |
| NAND | SPI0, CS PTA18 |
| RTC | internal I2C2, 7-bit 0x70 |
| IMU | bit-banged I2C PTA7 (SCL) / PTA8 (SDA), 7-bit 0x68, MPU-6050 class |
| FM receiver | RDA5807 class on the same bit-banged pair, 7-bit 0x11. No enable line; its audio is gated by PTC18 |
| RF chips | bit-banged 3-wire: SCLK PTB16, SDIO PTB24, main chip select PTB25, second chip select PTA19; status lines PTB26/27 |
| Output expander | 64-bit serial chain: clock PTB0, data PTB1, latch PTB3 (rising edge). Carries the audio routing only (9.3). PTB0 / PTB1 are not LEDs |
| Band path | The LO BK4819's GPIO4 / GPIO5 (register 0x33): VHF = GPIO5 high, GPIO4 low; UHF the reverse. A has no band, PA or TX supply GPIO |
| PA drive | None on A. Register 0x36 of the main BK4819 is written once at start-up (0x00BF) and the vendor's "ramp" on A writes only zeros. The PA, its supplies and its drive DAC are B's (section 10) (BIN) |
| ADC | Channel 0 microphone, 1 volume knob, 2 battery |
| Status LEDs | On SoC B (section 10). A switches them over the link |

### 9.1 Power and battery

| Item | Finding |
|---|---|
| Power key | PTB12 reads high while pressed. The radio is switched on with the volume knob, so PTB12 is most likely the knob's switch contact (INF). The vendor code latches on when it reads high, and switches off when the key is pressed again or the battery reading drops |
| Battery | ADC channel 2, 2S Li-ion. V x 10 = (raw + 5.7818) / 7.8551. Below 6.0 V critical, 6.0-6.9 V warnings, 7.0 V and up normal. A raw reading below 50 means the supply is going: switch off |
| Volume | ADC channel 1, 21 levels with hysteresis. The table values are not known |

### 9.2 Keys

The matrix code is the vendor's key number. The front panel has six keys besides the digits: two blank soft keys
under the display, green and red, and an up/down rocker. The original firmware draws the soft keys' labels on the
screen's bottom row, left and right (Menu and A/B at idle; Ok, Select, Unlock left; Back, Delete right). The sense
lines are the digit columns (PTB9 left: 1 4 7 `*`, PTB10 center, PTB11 right). The RadioFoundry key names (`KEY_*`) are
the front panel's. Each front key's code was checked on the radio (2026-10-26, HW). RadioFoundry makes the
soft keys OK and Back, as the original firmware labels them.

| Code | Vendor name | Key (RadioFoundry) | Where | Source |
|---|---|---|---|---|
| 8-17 | digits 0-9 (8 = 0) | `KEY_0`-`KEY_9` | matrix | BIN |
| 18, 19 | `*`, `#` | `KEY_STAR`, `KEY_HASH` | matrix | BIN |
| 5 | navigation | `KEY_SOFT_LEFT`: at idle opens the main menu; Menu + `*` unlocks the keypad | PTB9 x PTB23 | BIN |
| 6 | navigation | `KEY_SOFT_RIGHT`: at idle swaps VFO A and B | PTB11 x PTB23 | BIN |
| 1 | navigation | `KEY_UP`: at idle the channel up | PTB10 x PTB23 | BIN, HW |
| 2 | navigation | `KEY_DOWN`: at idle the channel down | PTB10 x PTB22 | BIN |
| 3 | P4 | `KEY_GREEN`: the vendor's programmable P4 (its function from the codeplug) | PTB9 x PTB22 | HW |
| 4 | mode key | `KEY_RED`: in a menu, back one level | PTB11 x PTB22 | HW |
| 22 | P2, the IAP key | `KEY_SK1`, upper left side key, above PTT | PTB9 direct |
| 23 | P1 | `KEY_SK2`, lower left side key, below PTT | PTB10 direct |
| 24 | P3 | `KEY_SOS`, the orange top key | PTB11 direct |
| - | PTT | `KEY_PTT`, left side between SK1 and SK2 | PTA9, low = pressed |

### 9.3 Audio routing in the serial chain

The chain is eight bytes. Byte 0 bit 0 is shifted first; a rising edge on PTB3 latches it. There are no RF bits.

| Bytes | Bit | Meaning |
|---|---|---|
| 0 (speaker), 1 (headset) | 0-3 | the source: bit 2 x (class - 1) + vfo. Class 1 is analog FM, class 2 DMR (INF). Vfo 0 is A's receiver, 1 is B's |
| 0, 1 | 5 | amplifier enable |
| 0, 1 | 6 | another source; RadioFoundry uses it for the broadcast FM tuner (INF) |
| 2-5 | 4 | the receive path in use, one-hot: byte 2 DMR A, 3 DMR B, 4 FM A, 5 FM B |
| 2-5 | 7 | the other VFO during TX |
| 6, 7 | 0-3 | the transmit route, the source bit of the transmitting class and vfo (byte 6 at volume 6 or less, else byte 7) |

B's audio reaches the speaker through these bits with vfo 1.

### 9.4 Display

The panel record at 0x031FD000 holds `lcd` and a type byte. A panel that answers ID 0xC505 to command 0xD3 is
variant 1. Without a record the vendor code uses variant 3.

## 10. SoC B pins

B has no display, keys, power latch or GPS. Pin index as in section 9.

| Function | Pins |
|---|---|
| RX band | UHF: PTB8 high, PTB7 low. VHF: the reverse |
| RX front end | UHF PTB15, VHF PTB14, high = on. **Shared**: A's receiver needs its band's front end too |
| RX path | **HW** A: PTB1, B: PTB0, high = on, in FM and DMR alike; neither receiver cares about the other's. B's reading of a bench signal rises 53 dB with PTB0 (-108 to -55 dBm), and without it B's floor is about -120 dBm. PTB13 and PTB5, the rest of the vendor's mode pairs (PTB1 + PTB13, PTB0 + PTB5), and PTB11 / PTB12 change neither receiver: held low. All off while transmitting |
| A's receiver | **HW** bench: with PTB15 + PTB1 low A hears nothing (noise floor -127 dBm); high, the floor is -86 dBm and a bench signal shows. VHF the same with PTB14. The band switch (PTB8 / PTB7) and PTB13 make no difference to A, nor to B. B sets them from A's `PATH` messages ([rx2-link.md](rx2-link.md)) |
| Audio source to A | mode 0: PTC18 high, PTB4 low; mode 1: the reverse. Held at mode 0 ("path 0", A's chip) while A transmits (BIN) |
| Audio band | VHF: PTB18 high, PTB22 low; UHF: the reverse. Set to A's TX band while A transmits (BIN) |
| TX path | UHF PTA17, VHF PTB10: B's own transmitter, which the vendor uses only for transmitting on B's chip |
| PA select, then supply | UHF PTB6, 200 us, then PTB20. VHF PTB3, then PTB21. TX off clears the supplies first |
| A's TX line | PTB2 in, both-edge interrupt: A's PTB2 (section 9) (BIN) |
| A's PA | **B keys the PA for A's transmitter** (BIN). A sends B a record first (vendor link id 0x6B00) with the drive and the page's delays, then raises PTB2. Rising edge: mode 0, the TX band pair, A's RX path PTB1 (and PTB13) off, 50 us, the TX-on delay, PA select, 200 us, supply, DAC B ramped up to the drive (180 steps of 4 us, S-shaped). Falling edge: the TX-off delay, the ramp down, supply then select off, 300 + 1000 + 300 us, the RX front end and A's RX path back, 200 us |
| PA drive and delays | From A's page: drive = item 0x045 (channel power byte 0) or 0x056 (otherwise), interpolated at the TX frequency; TX-on / TX-off delays from item 0x0AE, in us, over 2000 read as 0 (BIN). Which item is high power: INF |
| Start-up levels | Low: PTB20, PTB21, PTB6, PTB3, PTB1, PTB0, PTB15, PTB14, PTB13, PTB5, PTB12, PTB11, PTB8, PTB7, PTA17, PTB10, PTC18, PTB4. High: PTB22, PTB18 |
| Status LEDs | PTA16 (lit while A transmits) and PTB17 (receive), high = on. Colors not known. While A is in its flash stub they take turns at 2 Hz (`RX2_LED_STUB`: A's stub has no LEDs) |
| ADC | Channel 4 is a carrier level |
| RF chips | The same 3-wire bus pins as A (section 9) |

## 11. RF side (both SoCs)

| Item | Finding |
|---|---|
| Driver | A and B run the same RF driver code over the same 3-wire bus |
| Main chip ID (register 0) | 0x6818 (FD6818), 0x7813, 0x1846 (RDA1846/AT1846S, init is an empty stub), 0x4819 (BK4819) |
| Second chip ID | 0x6818, 0x7813, 0x1100 (AT1141), 0x4819 |
| Fitted parts | **HW** A on the bench: main 0x4819, LO 0x1100 (AT1141); the firmware's version string reads `DBT` (main BK4819 'B', LO AT1141 'T'). The firmware picks the driver by ID at start-up, so other units can differ |
| Superhet | The main chip receives at the IF and transmits at f; the second chip is the LO. **A's IF is 45 MHz** with either LO chip; B's is 51.55 MHz. DAC C = 1780 at RX set-up; DAC B drives the PA with a 180-step ramp |
| BK4819 LO | High-side, f + IF, in registers 0x38 / 0x39 |

### 11.1 The AT1141 LO

A PLL / VCO synthesizer on the LO's enable with the BK4819's bus format (8-bit address, bit 7 = read, 16-bit data). No
datasheet; the driver (`drivers/rf/at1141`) replays the firmware's set-up.

| Item | Finding |
|---|---|
| Banks | Register 0x7F = 1 selects a second bank; every table ends with 0x7F = 0 |
| Set-up | A 91-entry table, read back 0x15 / 0x16 / 0x19, the band for 400 MHz, the VCO start (waits 2, 5, 5 and 350 ms), 0x21 bits 10:5 = 10, output level 2 |
| Control (0x30) | 0x4006 standby, 0x4046 on, 0x4002 off for TX. Restart after TX: standby, 10 ms, standby, on |
| Tuning | Divider d (0x16 low byte) and multiplier m (0x15 bit 0: x3 when set, else x2) per octave from 550.4 MHz down, so d x f x (2 or 3) stays at 2.2-3.3 GHz. 0x3F = VCO / 406 250. PLL word in 0x62 (low) / 0x63 (high): VCO / 2 / 26 MHz in 6.26 fixed point, the reference halved when 0x19 bit 6 is set |
| Injection | f + 45 MHz, except **f - 45 MHz from 400 to 440 MHz** |
| Output level (0x26) | 0: 0x7F40; 1-7: one bit from bit 15 down, OR 0x0140; 8 and up: 0x0140 |
| GPIO | 0-3 in 0x2B, 4-7 in 0x2A, a nibble each (0xF = high). The band path is GPIO4 / GPIO5, as with a BK4819 LO |
| Baseband | Both images link the same HR_C7000 and vocoder library versions as the DB25-D. Each SoC has its own modem, RF pair and NAND |

## 12. The vendor's A to B link (UART3, 115200 8N1)

RadioFoundry does not use this protocol. It runs its own on the same UART ([rx2-link.md](rx2-link.md)).


| Byte | Meaning |
|---|---|
| 0 | 0x0A command or data, 0xAB acknowledge |
| 1-2 | command id, big endian |
| 3-4 | total frame length including header (7 = no payload), big endian |
| 5-6 | 16-bit sum of bytes 0-4 plus the payload, big endian |
| 7... | payload |

Header bytes time out after 100 ms and the payload after 1 s. Replies normally echo the id with no payload. Ids are
grouped by range (0x0000-0x9006 A to B, 0xB000-0xDB03 B to A). Most meanings are not decoded.

## 13. The RadioFoundry port

Two boards, one per SoC. Both use `boards/gd88/common`: the main BK4819 with a BK4819 or AT1141 LO, the calibration
page, DMR on the modem and the SPI NAND. Both run on the bench's radio (2026-10-26); receive is not yet checked on air.

| | A (`boards/gd88`) | B (`boards/gd88b`) |
|---|---|---|
| Runs | the radio core: UI, codeplug, scan, dual watch, host link on UART1, DMR, GPS | no core: a receiver task under `app_start` |
| Receive | FM and DMR on its own main BK4819 and LO (11.1) | FM and DMR, on what A asks for |
| Transmit | Off by default; `GD88_TX=1` builds it. A's main chip makes the signal; A arms B (`RX2_TX`, the drive and delays from A's page) and raises PTB2 | B's own chip never transmits. It keys the PA for A on PTB2's edges while armed (`pa_gd88b.c`) |
| Second receiver | B over UART3 (`board.rx2`, [rx2-link.md](rx2-link.md)). The core keeps B on the dual watch channel that is not selected | - |
| Audio | the chain's source bits (9.3) and PTC18. B's audio gets vfo 1 while A gives it the speaker | the source pair to A, muted until A sends `AUDIO` on |
| LEDs | sent to B (`RX2_LED`) | PTA16, PTB17 |
| Storage | settings on the SPI NAND, blocks 1920-1951 | none |
| Calibration | both copies read, never written | the program-flash copy |
| Update | the cable, through RadioFoundry's first stage | through A's bridge (rx2-link.md 4): `stubtool.py --board gd88b` |
| Broadcast FM | the RDA5807-class tuner (`CONFIG_BCAST_FM`, on by default, `bcast_gd88.c`): the shared driver on PTA7 / PTA8, its audio on chain bit 6 with the amplifier | - |

Not supported yet: the IMU, the headset path on its own, the RTC.

## 14. Remaining unknowns for bring-up

The code marks each with `TODO(gd88)`. The first-day plan that settles them is [gd88-bringup.md](../testing/gd88-bringup.md).

1. Which RF chips other units have (the bench's A: BK4819 and AT1141), and B's.
2. Transmit on the radio: the PA sequence on B is from the vendor firmware (section 10), not yet keyed on air.
3. FM against DMR for the chain's classes, chain bit 6 as the broadcast tuner's source, and B's audio source pair.
4. The front panel's navigation keys, the encoder's direction, whether PTB12 is the knob's contact.
5. The volume knob's level table, the LCD controller's timing word, DAC C on each SoC.
6. The NAND's size and B's NAND chip select.
7. Meaning of the link command ids and calibration items 0x602-0x60B.
8. Bootloader contents and the IAP packet format on this radio (not captured).

## Sources

- Radioddity extended manual v3.4 (2025-05-01), sections 4.3, 11.1, 17.2, 17.5, 17.6:
  <https://radioddity.s3.amazonaws.com/2025-05-01_Extended_manual_for_Radioddity_DB25-D_PRO_-_DB40-D_-_GD-88_-_v3.4.pdf>
- Radioddity "How to update firmware on DB25-D and GD-88" v2.2 (2023-03-30):
  <https://radioddity.s3.amazonaws.com/2023-03-30%20How%20to%20update%20firmware%20on%20DB25-D%20and%20GD-88%20v2.2.pdf>
- Radioddity GD-88 product page (in the box: charging base, AC adaptor, programming cable; K1 connection):
  <https://www.radioddity.com/products/radioddity-gd-88>
- HR_C7000 user manual (2016) and software application manual V2: UART and USB memory map, pin list and pin mux,
  PIC table, CLK_MGR USB fields
