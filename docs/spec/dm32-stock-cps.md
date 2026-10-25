<!-- SPDX-License-Identifier: MIT -->
# DM-32UV vendor programming protocol (interop facts)

What the Baofeng firmware (DM32.01.01.049, same on 046) accepts from a PC, so a tool can read or set up a radio that
still runs the vendor firmware. RadioFoundry does not implement this protocol in its firmware
([open-firmware-cps.md](open-firmware-cps.md) covers what RadioFoundry speaks; the reasons are in
[dm32-migration.md](dm32-migration.md)). Values marked `(unverified)` are not confirmed on a radio.

## 1. Transport

| Item | Value |
|---|---|
| Port | The two-pin K1 accessory jack, wired to UART0 (0x14030000). There is **no USB device** on the radio: the firmware calls the link "USB" (task name "task usb", screen text "PC Program / USB Mode") because the cable is a USB-serial adapter. The USB-C socket is on the battery pack and only charges it ([hardware-dm32.md](hardware-dm32.md) 3.11) |
| Line | 115200 8N1, no flow control |
| Cable | Any K1 USB-serial cable reaches the firmware (CH340 and FTDI both reported); the official CPS insists on an FTDI cable |
| Session end | No exit command; closing the port ends the session (the radio's receive times out) |

## 2. Command words

The programming task waits for up to 7 bytes with a 500-tick timeout (RTOS ticks; 1 ms each, unverified) and compares
them with 7-letter ASCII words. Except for `PROGRAM`, a word is accepted only if exactly 7 bytes arrived in that
read, so send each word on its own and wait for the answer. While a command runs, the radio sets a "PC session"
flag that shows the PC screen text and holds off normal operation; it is cleared when the session ends.

| Word | Radio's answer | Then |
|---|---|---|
| `PSEARCH` | `06` followed by `DP570UV` (8 bytes). DP570UV is the internal model name of the DM-32UV | — |
| `PASSSTA` | 3 bytes: `50` (`P`) and two status bytes read from the radio settings (logical block 4, offset 0x439). `50 00 00` = no password set | — |
| `PASWORD` | `06` | The radio waits up to 50 000 ticks for one 10-byte frame: `50` (`P`), then `57` (`W`) or `52` (`R`), then 8 password bytes. `W` checks against the 8-byte password at settings offset 0x43B, `R` against the one at 0x443 (write and read passwords, unverified). A stored byte of 0x00 or 0xFF ends the stored password (an unset password reads as 0xFF). The reply is unverified (unverified) |
| `SYSINFO` | `06` | Information queries in 5-byte frames: `56` (`V`) `00 00 <hint> <id>` with id at most 0x40, answered with version, region map and capacity data; four `FF` bytes end the exchange (unverified) |
| `PROGRAM` | (see 3) | Programming mode. The CPS sends `FF FF FF FF 0C` `PROGRAM`, then `02`, then `06` |
| `RTCITEM` | `06` | Clock setting, repeated until a frame is bad or times out: a 5-byte header (`54` = `T`, three bytes, payload length below 8; 2000-tick timeout) and the payload (500-tick timeout). The radio answers `06` and sets the RTC; it computes the weekday itself. Payload order: year (16-bit little endian), month, day, hour, minute, second (unverified) |
| `PCTESTM` | `06` | PC test mode (the "TestMode OnLine" screen); frames unverified (unverified) |
| `DEL-RCD` | `06` | Deletes the stored voice recordings (from the name; 049 has voice recording, unverified); no further frames (unverified) |
| `IDCHECK` | `06` | nothing else |

## 3. Programming mode (`PROGRAM`)

As the official CPS V1.41 speaks it to firmware 046. The radio's 16 MB data flash appears as a flat 24-bit address space:

| Request | Bytes | Answer |
|---|---|---|
| Read | `52` addr (3 bytes, big endian) len (2 bytes) | the data |
| Write 4 KB | `57` addr (3 bytes) `00 10`, then 4096 data bytes | `06` |

No checksum. The codeplug occupies 0x001000–0x0C8FFF as 200 pages of 4 KB with a logical block ID in each page's
last byte; the full flash map and the never-write ranges (calibration is block ID 2) are in
[hardware-dm32.md](hardware-dm32.md) 10.1. A tool that writes must keep to those rules.

## 4. Not part of this protocol

- Firmware update: the Baofeng bootloader's own serial protocol on the same cable, entered with PTT + SK1 at power-on
  ([hardware-dm32.md](hardware-dm32.md) 13.2).
- The HR_C7000 ROM serial loader (13.3).
