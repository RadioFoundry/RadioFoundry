<!-- SPDX-License-Identifier: MIT -->
# MD-UV380 / MD-UV390: hosting the AMBE+2 codec

Status: format description, against the reference build ([codec-placement.md](codec-placement.md)). The V19.26 CSV
codec was run in a host instruction set emulator (measured); nothing has run on a radio.

Rule: the radio runs exactly the codec its own TYT firmware shipped. The user's machine extracts it from a firmware
file the user supplies. No codec bytes go in this repository or in released binaries.

## Stock files

Published by TYT and mirrored by dealers. Older 18.11 and 18.16 packages exist; they are not described here.

| version | file | size | sha256 | radios |
|---|---|---|---|---|
| **V19.26 CSV (profiled)** | `MD-UV380(CSV)-V19.26.bin` | 1303040 | `75e4d470b451cb5d191a647c0836c71801433b826cc4a7fe2c48d750036bd98e` | non-GPS, large contact list |
| D19.26 REC | `MD-UV380(REC)-D19.26.bin` | 1315328 | `7dec4b751e8a0f44cf7d54d0ebc3b439a9f49899eadd7c8ff14d2804edbe8ac2` | non-GPS, recording build |
| P19.26 CSV-GPS | `MD-UV390(CSV-GPS)-P19.26.bin` | 1312256 | `84331dbc9bccf5021122b715dcb77d7589dba0745694bf3a249aae6e7c383ef1` | GPS, large contact list |
| S19.26 GPS-REC | `MD-UV390(GPS-REC)-S19.26.bin` | 1324544 | `77923d67aa75a07f7601a1c53ae1d5f5af37bd4367d52a50c8aeff89b834eceb` | GPS, recording build |

The four files are for the 5 W radios. A "Plus 10 W" file is not described (?). The header carries no model string, so a user must pick the file for their radio;
for RadioFoundry any of the four is only a codec donor and works on any MD-UV380 family radio, because the codec is the same
object code (below).

## Container and encryption

The "OutSecurityBin" two-segment form, as for the reference build:

| offset | contents |
|---|---|
| 0x00 | `OutSecurityBin`, two zero bytes |
| 0x10 | model field, blank (0xFF) |
| 0x30 | `02 17 0C 03 ...` in the non-GPS files, `02 16 0C 03 ...` in the GPS files (meaning unknown, ?) |
| 0x7C | u32 segment count, 2 |
| 0x80 | (0x00060000, 0x5D400): SPI flash resources; (0x0800C000, length): the application |
| 0x100 | payload |

Application lengths: V19.26 0xE0C00, D19.26 0xE3C00, P19.26 0xE3000, S19.26 0xE6000. The application starts at file
offset 0x5D500. The payload is XORed with the MD-UV380 bootloader key (SHA-256
`739ce35cc8db6e72f95598effac1a01ae71dbe2a5792c1a69acaee5dbf634991`, first bytes 00 AA 89 89, the `mduv380` model of
`tools/stm32_dfu.py`):

```
plain[i] = file[0x100 + i] ^ key[i % 1024]
```

For an application address A: file offset = A - 0x0800C000 + 0x5D500, key index = (file offset - 0x100) % 1024.

**Key recovery (measured).** `rfoundry/stockfw.derive_key(stock, <the hash above>)` recovers the key from each of
the four files and checks it by that hash: in about 0.2 s for V19.26, D19.26 and S19.26, and about 5 s for P19.26.
No key file is needed.

Decrypted application vector tables: SP 0x20002848 in all four; reset 0x080E8A09 (V19.26), 0x080EB999 (D19.26),
0x080EAE69 (P19.26), 0x080EDE39 (S19.26).

## Codec in the vendor image (V19.26 CSV)

| window | flash | length | file offset | key phase | sha256 (decrypted) |
|---|---|---|---|---|---|
| code | 0x0807B830..0x0808ACEA | 0xF4BA | 0xCCD30 | 0x030 | `5de390664c831412f37ae9ec1f8aadaf77d93112bded424e5bfc9d6b77a64aa1` |
| tables | 0x080D8C94..0x080EB580 | 0x128EC | 0x12A194 | 0x094 | `a5c80b51b638d38c714b3040526d89ab8ed1c193ed5524fa09558f560e7d35fa` |

First word of the code window: 0x4FF0E92D, as in the reference build.

* **Code window.** Execution covers 0x0807B830..0x0807C872 and 0x0807DD4C..0x0808ACEA; the gap between holds other
  code that the codec never executes, carried to keep the window contiguous.
* **Table window.** The constant tables at the reference table window's layout shifted by +0x72A4 (start) to +0x7340
  (most tables), plus one codec code island at 0x080DFF3C..0x080E0024 (the reference build's is at 0x080D8BFC). Starting it at 0x080D8C98 changes the output. Its end, 0x080EB580, is the reference window end + 0x7340 (not
  minimized). No small extra windows were needed.

Other files: the head is at 0x0807E3C0 (D19.26), 0x0807DBF0 (P19.26), 0x080807B4 (S19.26). Their windows have the same
shape at the same offsets from the head (not cut or checked).

## Entry points and calling convention

All AAPCS, Thumb, the reference build's argument lists and semantics ("Per-frame calls" in its codec page). The offsets
from the code start are **the reference build's** (another hosted family has different ones):

| call | offset | V19.26 CSV | D19.26 | P19.26 | S19.26 |
|---|---|---|---|---|---|
| encode | +0x130 | 0x0807B960 | 0x0807E4F0 | 0x0807DD20 | 0x080808E4 |
| encoder set | +0x1C0 | 0x0807B9F0 | 0x0807E580 | 0x0807DDB0 | 0x08080974 |
| encoder init | +0x284 | 0x0807BAB4 | 0x0807E644 | 0x0807DE74 | 0x08080A38 |
| FEC init | +0x4CC | 0x0807BCFC | 0x0807E88C | 0x0807E0BC | 0x08080C80 |
| encode ECC | +0x4E8 | 0x0807BD18 | 0x0807E8A8 | 0x0807E0D8 | 0x08080C9C |
| decode | +0x5D8 | 0x0807BE08 | 0x0807E998 | 0x0807E1C8 | 0x08080D8C |
| decoder set | +0x688 | 0x0807BEB8 | 0x0807EA48 | 0x0807E278 | 0x08080E3C |
| decoder init | +0x6D0 | 0x0807BF00 | 0x0807EA90 | 0x0807E2C0 | 0x08080E84 |
| encoder parameters | +0x848 | 0x0807C078 | 0x0807EC08 | 0x0807E438 | 0x08080FFC |
| FEC decode | +0xB54 | 0x0807C384 | 0x0807EF14 | 0x0807E744 | 0x08081308 |

Only the V19.26 CSV column has been executed. The init sequence is the hosted builds': encoder init (E, 1, 1),
FEC init (each FEC block, 1), decoder init (D, 1), encoder parameters (E, -22, 20, -10, stack: -40, 0x4000, 3276, 3276,
1310, 10, 20), encoder set (E, 128, -20), decoder set (D, 128, -20), one decode of an all-zero frame with flags 8. The
sequence is the reference build's, and the output matches it.

### State sizes

With every block ending at an unmapped page: encoder state 0x16E0, decoder state 0x7EC, FEC state 0x44 each run without
a fault (the sizes of the hosted builds; smaller sizes were not tried). Stack peak 3822 bytes from the entry.
No fixed RAM address is touched.

## Checks (measured)

In a host emulator (Thumb, Cortex-M4) with only the two windows, the state blocks, the I/O buffers and a stack mapped. For a 1 kHz tone
(30 frames) and a 300 frame stress run (noise, silence, full scale sweeps, injected bit errors, random frames, flags 8),
the V19.26 CSV codec's air bits and decoded PCM are **bit-identical** to the reference codec's, and the FEC decode
results agree (0 and 4).

## Hosting

Run in place at the vendor addresses, as for the reference build:

* The code window spans flash sectors 7 and 8 (0x08060000..0x0809FFFF) and the table window sectors 10 and 11
  (0x080C0000..0x080FFFFF), exactly the reference build's sector use. So its linker plan applies unchanged: the
  application gets sectors 3-6 (0x0800C000..0x0805FFFF, 336 KB), sector 9 is free, and an update that writes only
  sectors 3-6 keeps the codec ("keep codec" works).
* Boot check: first word 0x4FF0E92D at 0x0807B830, then the CRC or hash of both windows; otherwise FM only.
* One profile per firmware file, keyed by its SHA-256 (suggested ids `mduv380-v1926-csv`, `mduv380-d1926`,
  `mduv390-p1926`, `mduv390-s1926`). The windows move between files, so the application's sector budget is the same
  for all four (every head is in sector 7, every table window in sectors 10 and 11).

## Extraction recipe (V19.26 CSV)

1. Check the file's SHA-256 (`75e4d470...`) and the two-segment header.
2. Get the key: derive it from the file (above) or a key file checked by its SHA-256.
3. Decrypt file[0xCCD30 .. +0xF4BA] and file[0x12A194 .. +0x128EC]. Check the first word and both SHA-256 values.
4. Write both unmodified at 0x0807B830 and 0x080D8C94 in the image. The merge happens on the user's machine, never in a
   release.

## Open items

* Minimize the V19.26 window ends and the state sizes.
* Cut, hash and check D19.26, P19.26 and S19.26; add their profiles.
* The 18.11 / 18.16 packages and any Plus 10 W file.
* On a radio: TX and RX voice through the codec at its vendor addresses.
