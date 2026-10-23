<!-- SPDX-License-Identifier: MIT -->
# Radioddity GD-88 (Kydera DR-880UV) hardware

Status: facts only. RadioFoundry has no GD-88 board and no GD-88 radio. This page records whether the GD-88 programs
and updates over native USB or over a USB-serial cable, and what is known about its two SoCs and the update path. The
survey entry is in [radios.md](radios.md); the HR_C7000 facts it builds on (UART registers, USB core, clock gates) are
in [hardware-dm32.md](hardware-dm32.md). A value marked `(datasheet)` is from the HR_C7000 user guide (2016 user manual
and the V2 software application manual); `(unverified)` is not confirmed.

**Answer: a USB-serial cable.** The radio body has no USB connector. The PC link is a K1 (2.5 mm + 3.5 mm) jack, and
the cable carries a USB-to-UART converter chip. SoC A runs the PC protocol on its **UART1** at 115200 baud, and neither
SoC uses its USB controller.

The application images are plain and headerless and load at 0x0300C000, the same application base as the DM-32UV. Each
copies a RAM-code block to 0x10000 at start-up.

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

| Item | Finding |
| --- | --- |
| Radio connector | K1 jack on the right side of the GD-88. Radio RXD (data to radio) shares the 3.5 mm sleeve with PTT. Radio TXD (data from radio) is on the 2.5 mm ring. GND is on the 2.5 mm sleeve. The 3.5 mm tip carries +5 V through 100 Ω (not a supply) |
| Other connectors | SMA antenna socket and a 2-pin spring battery contact. The radio charges in a desktop charging base. Neither the product page nor the manual mentions a USB or Type-C port on the radio or the battery |
| Cable | Radioddity: "a small chip that converts signals to and from the USB-side into serial signals to and from the K1 / RJ45 style connector". It appears on the PC as a virtual COM port. The DB40-D box lists the cable as "FTDI-based"; the GD-88 box list says only "Programming Cable". Firmware and CPS release notes since 2022-10 add "support for programming cable based on FTDI-chip" |
| Which SoC | A, UART1 (0x14040000), interrupt vector 42, set to 115200. It serves the PC protocol: model handshake, 2 MB read and write blocks, record read, factory data, the B reset, and the command that hands over to the bootloader. The same UART also sends GPS NMEA out to the PC |
| Baud | 115200 8N1 in the application. The vendor's IAP updater offers 115200 and 38400 |
| Electrical level | Not stated for the K1 jack. The DB40-D's RJ-45 programming port, on the same platform, is documented as 3.3 V, 115.2 kbaud |
| B's own UART1 | Set to 115200 with a receive handler, but it carries only factory tuning and flash-copy commands. Whether it is wired to the K1 jack (directly or through a switch), to test pads, or to nothing is unknown |

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

| Item | Finding |
| --- | --- |
| Entry | Radio off, cable connected, CPS closed. Hold the upper side key P2 and turn the radio on with the volume knob. The status LED lights red and a screen shows "IAP_A" or "IAP_B" on the top line. Then release P2. It can take more than one try |
| A or B target | In IAP mode, long-press P2 to switch between IAP_A and IAP_B. The A file goes to IAP_A and the B file to IAP_B. Write the codeplug after updating both |
| Bootloader code | Not in the update files: they start at the 0x0300C000 application base, so the bootloader sits below it, as on the DM-32UV. How IAP_B reaches SoC B (A relaying over the UART3 link, or a switch moving the cable to B's UART) is not known |
| Software entry | The A application has a PC command that closes UART1 and hands over to the bootloader |
| ROM serial boot | The HR_C7000 boot ROM offers serial (UART0) boot or SPI-flash boot, no USB boot (datasheet). The datasheet names UART0 as the debug and boot-upgrade port. Where A's and B's UART0 balls go on the GD-88 board is unknown |

## 5. Open questions

1. Is the K1 cable wired to A's UART1 directly, or through a switch that the bootloader also points at B when
   IAP_B is selected? Needs a board photo or a continuity check with the radio opened (no live probing).
2. Which converter chip is in the GD-88 cable as shipped (FTDI per the DB40-D box list and the release notes;
   other batches unknown)? Plugging the cable into a computer and reading its VID:PID answers it, with no radio needed.
3. The K1 data level (3.3 V like the DB40-D's RJ-45, or something else).
4. Where the UART0 balls of A and B go (test pads?), for ROM serial-boot recovery.
5. Is there any USB data path at all, for example test pads on the F1/F2 nets? Nothing public suggests one. The
   FCC filing 2AN62-GD88 internal photos were not reachable.

## Sources

- Radioddity extended manual v3.4 (2025-05-01), sections 4.3, 11.1, 17.2, 17.5, 17.6:
  <https://radioddity.s3.amazonaws.com/2025-05-01_Extended_manual_for_Radioddity_DB25-D_PRO_-_DB40-D_-_GD-88_-_v3.4.pdf>
- Radioddity "How to update firmware on DB25-D and GD-88" v2.2 (2023-03-30):
  <https://radioddity.s3.amazonaws.com/2023-03-30%20How%20to%20update%20firmware%20on%20DB25-D%20and%20GD-88%20v2.2.pdf>
- Radioddity GD-88 product page (in the box: charging base, AC adaptor, programming cable; K1 connection):
  <https://www.radioddity.com/products/radioddity-gd-88>
- HR_C7000 user manual (2016) and software application manual V2: UART and USB memory map, pin list and pin mux,
  PIC table, CLK_MGR USB fields
