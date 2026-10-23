<!-- SPDX-License-Identifier: MIT -->
# GD-77 family: the update file and the bootloader's update protocol

Status: written for the host side, tested against a model loader only, 2026-10-21. Nothing here has been tried on a
radio. Every line marked unverified needs a first run on a bench radio.

The radio's loader sits below flash 0x4000 and is never written. It takes the application from a PC over USB HID. The
file the radio's own update tool uses is a `.sgl` file. odrflash writes ODR into a copy of a `.sgl` file the user
already has, and sends it with the same protocol.

## 1. Entering update mode

Switch the radio off, hold both side keys, switch it on and connect USB. It enumerates as HID 15A2:0073 (the same IDs as
its programming interface; unverified which of the two enumerations the loader uses). Reports: OUTPUT id 1 and INPUT id
3, 41 data bytes each.

## 2. The update file

| Offset | Content |
|---|---|
| 0x00 | `SGL!` |
| 0x04..0x0F | 12 bytes XORed with the repeating key `SGL!`: `ENCV001`, pad (1 byte), info offset L (u16 LE), k0, k1 |
| L | 103-byte information block, XORed with k0 (even offsets) and k1 (odd offsets) |
| 0x400 + pad | the body: the flash image followed by one zero byte, scrambled |

Information block (offsets inside the block):

| Offset | Content |
|---|---|
| 2..5 | image start (u32 LE) |
| 6..9 | body length (u32 LE), the zero byte included |
| 10..13 | the complement of the 32-bit sum of the scrambled body bytes |
| 50..65, 66..73, 74..81 | company, model, version, 0xFF padded |
| 91 | high nibble: address width in bytes; bit 0: use the program/erase flow of 3 |
| 92 | bits 2:0 checksum width; bits 4:3 length width; bit 5: send the tag first |
| 93..94 | block size (u16 LE) |
| 95..102 | the tag |

Body scrambling: each byte is rotated right by 3, then XORed with a stream K. K follows
`K[j] = K[j-1] ^ K[j-8] ^ K[j-9] ^ K[j-15] ^ K[j-16]` (period 32767). The last 32 bytes of a file are zero before
scrambling, so they show 16 bytes of the stream and the rest follows. A file gives its own stream; no key is needed.

Writing an image: odrflash keeps the header of a file the user supplies (company, model, version, tag, widths,
length) and replaces the body and the sum. The image is padded with 0xFF to the file's length, the last 32 bytes stay
zero. An image longer than the file holds is refused.

## 3. The protocol

Frame, both directions: report id, 0, payload length (u16 LE), payload, zero padded. Each request is written, then one
reply is read (2 s; 10 s for the erase).

| Step | Host sends | Reply |
|---|---|---|
| 1 | `DOWNLOAD`, repeated until it answers | `#UPDATE?` |
| 2 | `A` | `A` |
| 3 | the tag, when information byte 92 bit 5 is set | the tag |
| 4 | `F-PROG\xFF\xFF`, when byte 91 bit 0 is set | `A` |
| 5 | company, model, version | `A` each |
| 6 | `F-ERASE\xFF` (else `ERASEROM`), then `A` | `A` |
| 7 | `PROGRAM\xFF` | `A` |
| 8 | blocks: address (big endian), length (big endian), data, never across a block-size boundary | `A` each |
| 9 | after every 1024 bytes and the last block: `END\xFF` and the u32 LE sum of the data sent since the last one | `A` |

The radio restarts by itself afterwards (unverified).

## 4. What the repository provides

| Piece | Where |
|---|---|
| File format, pack and unpack | `tools/odrflash/odrflash/updatefile.py` |
| The protocol, and a model loader for tests | `tools/odrflash/odrflash/hidboot.py` |
| Command line | `odrflash flash IMAGE --board gd77 --stock FILE.sgl`; `odrflash merge ... -o OUT.sgl` writes the update file |
| Web flasher | WebHID in the browser, same steps |

The codec comes from the same stock file: [codec-gd77.md](codec-gd77.md).

## 5. Unverified

- That the loader accepts a body scrambled with the donor's stream while the content is another image. The tag may
  select the stream; keeping the donor's tag is the safe choice. First run: the donor's own image, then a changed one.
- That the loader checks only the three names and the sums, not the version order.
- The radio's behavior after a failed run (the loader below 0x4000 is never written, so update mode should always remain).
- Timing and the exact HID enumeration.
