<!-- SPDX-License-Identifier: MIT -->
# Radioddity GD-88 (Kydera DR-880UV) hardware

Status: research, 2026-10-11, facts only. ODR has no GD-88 board and no GD-88 radio. This page records whether the
GD-88 programs and updates over native USB or over a USB-serial cable. It also records what is known about the two
SoCs and the update path. The survey entry is in [radios.md](radios.md); the HR_C7000 facts it builds
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

Images examined (BIN): A `B49E.D6x.EOPSAB.021` (2025-01-19) and B `B49C.D6x.EHRNAB.012` (2025-01-17), plus
A `B49E.D66.EOPSAB.013` and B `B49C.D66.EHRNAB.008` (2023-03) as a cross-check, and a raw byte scan of the
2022-10 A/B pair. All are plain, headerless images that load at 0x0300C000, the same application base as the
DM-32UV (BIN). Each image copies a RAM-code block to 0x10000 at start-up. In image A it comes from flash 0x030B404C
and fills 0x10000–0x34763 (BIN).

---

## 1. SoC pair and roles

| Item | Finding | Tag |
|---|---|---|
| SoCs | Two HR_C7000 (C-SKY CK803S core each), called A and B. Both images are CK803S code with the same start-up as the DM-32UV stock image, and both link the HR_C7000 vocoder library | BIN |
| A | Master: display, keys, GPS, FM broadcast receiver, menus, the PC programming protocol, and resetting B ("Reset BandB") | BIN |
| B | Second transceiver (VFO B). It has no GPS or PC programming protocol. Its UART1 only carries a factory tuning / flash-copy command set | BIN |
| A↔B link | UART3 (0x14090000) at 115200 on both chips, interrupt vector 45 on both | BIN |
| GPS (A only) | UART2 (0x14050000), interrupt vector 43. It starts at 9600; the firmware sends the receiver a `$CFGPRT` port-configuration sentence | BIN |
| UART0 (0x14030000) | Not used by either application: no literal, no `movih`, and vector 44 is never registered. On the DM-32UV this is the programming-cable UART | BIN |
| Storage | One SPI NAND per SoC (TC58 family strings in both images); part per earlier notes: Toshiba TC58CVG1S3HRAIJ | BIN, COM |

UART vectors follow the PIC table: UART1 source 10 / vector 42, UART2 11 / 43, UART0 12 / 44, UART3 13 / 45,
USB 21 / 53 (DS).

## 2. Programming and update link

| Item | Finding | Tag |
|---|---|---|
| Radio connector | K1 jack on the right side of the GD-88. Radio RXD (data to radio) shares the 3.5 mm sleeve with PTT. Radio TXD (data from radio) is on the 2.5 mm ring. GND is on the 2.5 mm sleeve. The 3.5 mm tip carries +5 V through 100 Ω (not a supply) | COM (extended manual v3.4, 11.1 and 17.5) |
| Other connectors | SMA antenna socket and a 2-pin spring battery contact. The radio charges in a desktop charging base. Neither the product page nor the manual mentions a USB or Type-C port on the radio or the battery | COM |
| Cable | Radioddity: "a small chip that converts signals to and from the USB-side into serial signals to and from the K1 / RJ45 style connector". It appears on the PC as a virtual COM port, and a driver download is offered if Windows does not load one. The DB40-D box lists the cable as "FTDI-based"; the GD-88 box list says only "Programming Cable". Firmware and CPS release notes since 2022-10 add "support for programming cable based on FTDI-chip" | COM |
| Which SoC | A, UART1 (0x14040000), interrupt vector 42, set to 115200 by the start-up task. The A routines that read and write UART1 serve the PC protocol: model handshake strings "+DRS-880plus" and "+136-480MHZ+", 2 MB read/write blocks, record read, factory data, the B reset, and the command that hands over to the bootloader ("BLL_IAP_Flash"). The same UART also sends GPS NMEA out to the PC | BIN |
| Baud | 115200 8N1 in the application (BIN). The vendor's IAP updater offers "115200,n,8,1" and "38400,n,8,1" | BIN, COM |
| Electrical level | Not stated for the K1 jack. The DB40-D's RJ-45 programming port, on the same platform, is documented as 3.3 V, 115.2 kbaud | COM; INF for the GD-88 level |
| B's own UART1 | Set to 115200 with a receive handler, but it carries only factory tuning / flash-copy commands. Whether it is wired to the K1 jack (directly or through a switch), to test pads, or to nothing is unknown | BIN, INF |

### PC tools (installers inspected as data, never run)

| Item | Finding | Tag |
|---|---|---|
| IAP updater installers | `IAP(setup).exe` (2020 build, shipped 2022–2024) and `IAP 24.11.30(setup).exe` (v1.0.54): Setup Factory 6 run-time with a Setup Factory 7 project. Payload: a VB6 program `IAP+Flash.exe`, plus `mscomm32.ocx`, `comdlg32.ocx`, `msstdfmt.dll`, `ComCat.dll`, `stdole2.tlb`. **No driver**: no INF, no SYS, no CAT, and no VID/PID | COM |
| How the updater finds the radio | It uses the MSComm serial control, lists ports from `HARDWARE\DEVICEMAP\SERIALCOMM` and filters names with the substrings "COM", "USB", "VCP" and "SER". Up to v1.0.51 it only offered COM1–COM8; v1.0.54 removed that limit | COM |
| CPS installer | `DRS CPS_V3.3 25.1.15(Setup).exe`: the same installer family. Payload is the CPS program, radio .ini files, VB6 run-time and OCX controls. **No driver** either | COM |
| Updater messages | "IAP Successfully" after connecting; "Write/Read/IAP Checksum right/error", "Erase ok", "Write Flash Successfully"; per-page progress ("Current Page", "End Page", "Addr(Hex)"). It also takes a separate `*.flash` file for the data flash ("Open Flash File", "Write Flash", "Read Flash") | COM |

A native USB device would come with a vendor INF, or would rely on a class driver plus a known VID/PID. Neither
installer has either, and the manual says the cable holds the converter chip (INF).

## 3. USB controller use

**No.** Neither SoC's application uses the HR_C7000 USB core.

| Check (same method as the DM-32 in [hardware-dm32.md](hardware-dm32.md) 3.11) | A (2025, 2023) | B (2025, 2023) | Tag |
|---|---|---|---|
| `lrw` literal or `movih` building an address in 0x13000000–0x1303FFFF | None that is code. The `lrw` hits (three in A 2025, two in A 2023, one in B 2025) land in byte tables or mid-instruction decodes. No `movih` 0x1300–0x1303 exists | Same | BIN |
| USB interrupt (PIC source 21, vector 53) registered | No | No | BIN |
| Device descriptor (`12 01 ..`), configuration descriptor (`09 02 ..`), USB UTF-16 strings | None (all six images, 2022–2025) | None | BIN |
| "USB", "usb", "VCOM", "CDC-ACM" strings | None ("CDC" appears only in "CDCSS") | None | BIN |
| CLK_MGR 0x02C (USB clock gates, bits 16/17) | Start-up writes 0x00800200 (USB gates off). Some later clock paths write 0xFFFFFFFF (every gate on, USB included); one of them clears bits 16 and 17 again straight after. The USB core is never accessed either way | B: start-up 0x00800200; other paths write 0xFFF00000 (USB gates off) or 0xFFFFFFFF | BIN |
| CLK_MGR 0x028 (48 MHz USB divider) written | No | No | BIN |

### SoC pins (DS)

The USB data pins are the dedicated balls F1 (USB_DP) and F2 (USB_DM), with USB_AVDD33 on G3 (DS, as on the
DM-32UV). The 2016 user manual's pin-mux table also lists USB_DM / USB_DP as function 4 of PTA13 / PTA14 (balls
L16 / M14, which are UART3 RXD / TXD). But the IO-mux register for those pins (uart3_rxd_sel / uart3_txd_sel) marks
the matching value 2 as reserved (DS, conflicting). On the GD-88 those balls carry the A↔B link (BIN), so they are
not available for USB.

## 4. IAP bootloader entry

| Item | Finding | Tag |
|---|---|---|
| Entry | Radio off, cable connected, CPS closed. Hold the upper side key P2 and turn the radio on with the volume knob. The status LED lights red and a special screen shows "IAP_A" or "IAP_B" on the top line. Then release P2. It can take more than one try | COM (update guide v2.2, release notes 2024-06) |
| A or B target | In IAP mode, long-press P2 to switch between IAP_A and IAP_B. The A file goes to IAP_A and the B file to IAP_B. Write the codeplug after updating both | COM |
| Bootloader code | Not in the update files: they start at the 0x0300C000 application base, so the bootloader sits below it, as on the DM-32UV. No dump is available, so how IAP_B reaches SoC B (A relaying over the UART3 link, or a switch moving the cable to B's UART) is not known | BIN, INF |
| Software entry | The A application has a PC command that closes UART1 and hands over to the bootloader ("BLL_IAP_Flash") | BIN |
| ROM serial boot | The HR_C7000 boot ROM offers serial (UART0) boot or SPI-flash boot, no USB boot (DS). The datasheet names UART0 as the debug and boot-upgrade port. Where A's and B's UART0 balls go on the GD-88 board is unknown | DS, INF |

## 5. Open questions

1. Is the K1 cable wired to A's UART1 directly, or through a switch that the bootloader also points at B when
   IAP_B is selected? Needs a board photo or a continuity check with the radio opened (no live probing).
2. Which converter chip is in the GD-88 cable as shipped (FTDI per the DB40-D box list and the release notes;
   other batches unknown)? Plugging the cable into a computer and reading its VID:PID answers it, with no radio needed.
3. The K1 data level (3.3 V like the DB40-D's RJ-45, or something else).
4. Where the UART0 balls of A and B go (test pads?), for ROM serial-boot recovery.
5. Is there any USB data path at all, for example test pads on the F1/F2 nets? Nothing public suggests one. The
   FCC filing 2AN62-GD88 internal photos were not reachable this time.

## Sources

- Radioddity extended manual v3.4 (2025-05-01), sections 4.3, 11.1, 17.2, 17.5, 17.6:
  <https://radioddity.s3.amazonaws.com/2025-05-01_Extended_manual_for_Radioddity_DB25-D_PRO_-_DB40-D_-_GD-88_-_v3.4.pdf>
- Radioddity "How to update firmware on DB25-D and GD-88" v2.2 (2023-03-30):
  <https://radioddity.s3.amazonaws.com/2023-03-30%20How%20to%20update%20firmware%20on%20DB25-D%20and%20GD-88%20v2.2.pdf>
- Radioddity GD-88 product page (in the box: charging base, AC adaptor, programming cable; K1 connection):
  <https://www.radioddity.com/products/radioddity-gd-88>
- Radioddity GD-88 firmware packages 2022-10-28, 2023-03-30, 2024-06-20 and 2025-02-10 (support downloads; images,
  release notes, IAP updater and CPS installers)
- HR_C7000 user manual (2016) and software application manual V2: UART and USB memory map, pin list and pin mux,
  PIC table, CLK_MGR USB fields
