<!-- SPDX-License-Identifier: MIT -->
# DM-32UV: hosting the stock AMBE+2 codec

Status: research, 2026-10-09. Static analysis only (csky-elf-objdump on the stock images, function matching by
mnemonic sequence, literal-pool cross references). Nothing here has run on a radio yet.

Rule: the radio runs exactly the codec its own stock firmware shipped. The user's machine extracts it from a
stock file the user supplies. No codec bytes go in this repository or in released binaries.

## Stock files

| file | size | sha256 | header |
|---|---|---|---|
| `DM32.01.01.049.bin` | 869688 | `03107b6f600e7fa95105af8324cff349c0f17fe30db249c14d11e79baa8da97a` | 256-byte `BFUV32-V2 ... END` |
| `DM32.00.01.046HRVocoder.bin` | 890516 | `f8484c13c994e920649d7b7bc2b98a98a76cfd0836865e5995a00627fc0c2887` | same |

The files aren't encrypted. The image is linked at 0x0300C000 (XIP GD25Q80 at 0x03000000), so
file offset = address - 0x0300C000 + 0x100.

## Where the codec is

The stock crt0 copies a RAM code block from flash to RAM 0x10000 (049: flash 0x030BFBA0, length 0x1F098;
046: flash 0x030BF904, length 0x24490). The codec is part of that block. Its data tables are in the stock
modem RAM image, which crt0 copies to 0x18000800 (049: from flash 0x03071F80, 0x43FC bytes; 046: from 0x03071D60,
0x439C bytes).

| | 049 | 046 HRVocoder |
|---|---|---|
| codec code, RAM (run address) | 0x00016CDC..0x0002B2DC | 0x0001B838..0x0002F914 |
| length | 0x14600 | 0x140DC |
| flash address / file offset | 0x030C687C / 0xBA97C | 0x030CB13C / 0xBF23C |
| first two words (signature) | 0x152514D8, 0x74C776CD | same |
| sha256 of the carved code | `355ca906...6ceeafd1` | `02743cf4...5c5e64fb` |
| stock code inside the range (not codec, never called by it) | 0x17270..0x1732C, 0x1CDBC..0x1D4C8, 0x1E8BC..0x1FE64 | 0x21AEC..0x230A8 |
| codec tables, modem RAM | 0x18000EA8..0x18003E20 | 0x18000E48..0x18003DC0 |
| tables: flash address / file offset / length | 0x03072628 / 0x66728 / 0x2F78 | 0x030723A8 / 0x664A8 / 0x2F78 |
| sha256 of the carved tables | `10684c33...4eb0b0e1` | `cd5a4d27...3a270ef803` |

The table window holds two codec object groups (049: 0x18000EA8..0x18001198 and 0x180013C4..0x18003E20) with
stock data between them. Copying the whole window verbatim is simpler than splitting it, and it does no harm.

### Entry points (offsets from the codec start)

| function | offset | 049 RAM | 046 RAM | prologue |
|---|---|---|---|---|
| decode | +0x0000 | 0x16CDC | 0x1B838 | `push r4-r11, r15` |
| encode | +0x0260 | 0x16F3C | 0x1BA98 | `push r4-r11, r15` |
| encode ECC | 049 +0x1C60, 046 +0x1E34 | 0x1893C | 0x1D66C | |
| decoder init | +0x00B4 | 0x16D90 | 0x1B8EC | |
| decoder set (2 params) | +0x0084 | 0x16D60 | 0x1B8BC | |
| encoder init | +0x0314 | 0x16FF0 | 0x1BB4C | |
| encoder set (2 params) | +0x02E4 | 0x16FC0 | 0x1BB1C | |
| FEC/ECC state init | 049 +0x1BF8, 046 +0x1DCC | 0x188D4 | 0x1D604 | |

The ECC and FEC init offsets differ between versions because 049 links extra stock code before them.

### Self-containment and position dependence

* 152 functions are reachable from decode, encode and ECC. They make no calls outside the codec range and have
  no literals pointing at app RAM, flash or peripherals. The only indirect calls are `jsr r12` and `jsr r2`, and
  those use pointers the codec stores itself.
* The codec does reference the modem RAM tables above (049: 58 distinct addresses at 107 sites).
* Branches and calls are PC-relative. Exactly 3 literal-pool words hold absolute codec addresses. In 049 they are
  the pool words at RAM 0x187B0, 0x187B4 and 0x19B68, holding 0x19540, 0x22754 and 0x26CAC. 046 has the same 3.
* So the code runs unmodified only at its stock RAM address, with the tables at their stock modem RAM addresses.
  To move the contiguous block by a constant delta, patch those 3 words; ODR does (Hosting, step 2). To move the
  tables, you would patch every modem RAM literal.

### Calling convention

C-SKY V2 ABI: arguments in r0-r3, then the stack; r4-r11 and r15 are callee-saved; the return value is in r0.
The register and stack roles match `DM32_firmware/.../codec_interface.c`, so the dh4570 ABI applies unchanged.

* decode(r0 = wave out (80 samples), r1 = 80, r2 = bit buffer (49 x u16), r3, sp+0, sp+4, sp+8 = decoder state)
* encode(r0 = bit buffer, r1, r2 = wave in, r3 = 80, sp+0, sp+4 = 0/1 (first/second half), sp+8, sp+12 = encoder state)
* ECC(r0 = r1 = bit buffer, r2, r3 = ECC state)

The stock wrapper passes values it keeps in the state blocks where the aftermarket firmware passes constants:

* decode: r3 = D[0x1166], sp+0 = a flags word.
* encode: r1 = E[0x1736], sp+0 = the flags word (a literal 0x800 in three calls, E[0x172E] = 0x1840 in the other
  three; see [codec-dm1801pro.md](codec-dm1801pro.md) section 3), sp+8 = E[0x173A] (0x2000 after init).
* ECC: r2 = E[0x1738].

The aftermarket firmware instead uses r3 = 0, encode sp+0 = 0x1840 and then 0x800, and sp+8 = 0x2000. Stock sets
E[0x172C] = 0x1840 at init. Checked on the DM-1801 Pro build of the same wrapper code
([codec-dm1801pro.md](codec-dm1801pro.md) sections 3 and 5): the sequence 0x1840 once, then 0x800, matches none of the
wrapper's calls, because each call pattern passes a single value on every call. `boards/dm32/vocoder_dm32.c` follows the aftermarket
sequence, and since the first call has no measurable effect it behaves as the 0x800 pattern. Which pattern DMR voice
uses is open.

## Stock versions

Every DM-32UV stock build from 01.01.032 to 01.01.049 carries the same codec: the same length (0x14600), the same run
address (0x16CDC), the same entry points (the 049 column above) and byte-identical tables (`10684c33...`). Inside the
codec ranges the code differs only in its table pointers. The rest of the differences fall in the stock code that lies
inside the window but isn't codec. Each version links the codec to its own flash addresses, so `tools/hrc7000_codec.py`
keys each stock file by its SHA-256 and finds the codec in a program flash read by content, then by address.

| Stock build | Code, flash | Tables, flash | Tables, modem RAM | Same code as |
|---|---|---|---|---|
| 01.01.032 | 0x030C74B4 | 0x03071908 | 0x18000EA8 | |
| 01.01.037 | 0x030C5F08 | 0x03071D68 | 0x18000EA8 | 039, 040 |
| 01.01.039 | 0x030C6414 | 0x03072228 | 0x18000EA8 | 037, 040 |
| 01.01.040 | 0x030C6424 | 0x03072238 | 0x18000EA8 | 037, 039 |
| 01.01.046 | 0x030C65B0 | 0x03072378 | 0x18000EA8 | 01.02.046 |
| 01.02.046 | 0x030C6590 | 0x03072358 | 0x18000EA8 | 01.01.046 |
| 01.01.047 | 0x030C68B8 | 0x03072658 | 0x18000EA8 | 049 |
| 01.L01.048 | 0x030C4444 | 0x03070468 | 0x18000EA8 | |
| 01.01.049 | 0x030C687C | 0x03072628 | 0x18000EA8 | 047 |
| 01.01.049 NRF and fanti builds | 0x030C7914, 0x030C7954 | 0x03072E80, 0x03072EC0 | 0x18000EB0 | each other |

How the addresses were found:

| Value | Method |
|---|---|
| Run address | The three self-referencing pool words (section "Self-containment and position dependence") give it. Every build has 0x16CDC |
| Table RAM address | Every table literal in the codec ranges moves by the same constant against 049: none for 032 to 049, +8 for the NRF and fanti builds (81 literals). The DM-1801 Pro's +0x48 (codec-dm1801pro.md) comes out the same way |
| Table window | The 64-byte head CRC (`TABLES_HEAD_CRC`), then the SHA-256 |

The NRF and fanti tables end at 0x18003E28, 8 bytes above the 049 window, so the DM-32UV linker script reserves modem
SRAM up to 0x18003E28. The slot's stock tag is the version number (32 to 49), and the host reports it as
`dm32-<three digits>`.

## Comparison with the dh4570 pair and OpenDM32

* dh4570 section 1 (76577 bytes, XIP at 0x03061000) is the **same object code** as the stock codec, linked in a
  different order. Every one of its 165 functions has a mnemonic-identical copy in stock 046, and all but the two
  unreachable ones described below have one in 049. Byte for byte, 98.9% match
  (73970 of 74733 bytes against 049); the differences are only cross-gap branch offsets and literal addresses.
  Function order matches 046 up to +0x62B4 (046 has a single gap). These are the two unreachable library
  functions (+0x594 and +0x5F0); they touch peripheral 0x1415xxxx.
* dh4570 section 2 (30720 bytes at 0x18000800) is the whole Dec 2025 app's modem RAM image (it contains app
  pointers). The codec tables in it are byte-identical to stock: the window from 0x180013C4 to 0x18003E20 in stock
  sits 0x2470 higher in dh4570, and the window from 0x18000EA8 to 0x18001198 sits 0x2698 higher.
* The OpenDM32 codec (RAM 0x47900) is a different compile and doesn't matter for this design.

## C. Codec state init tables

The `codec.c` arrays (decode 0x116E, encode 0x2000, ECC 0x100 bytes, and the STM32 0x7EC / 0x2000 / 0x100 set)
**don't occur** in 049, 046 or the dh4570 blobs (none of their non-zero 16-byte chunks match). Stock builds the state
at run time with the codec's own init functions. The stock 049 init routine is at RAM 0x162A8. It uses a decoder
state D (049: 0x456C0) and an encoder state E (049: 0x48160), with the ECC state at E+0x16E0 and the encode bit
buffer at E+0x1898. It runs these steps in order:

1. It sets these 16-bit parameters:

   | field | value |
   |---|---|
   | E[0x172C] | 0x1840 |
   | E[0x173A] | 0x2000 |
   | E[0x173C] | 128 |
   | E[0x173E] | -20 |
   | E[0x1740] | -22 |
   | E[0x1742] | 20 |
   | E[0x1744] | -10 |
   | E[0x1746] | -40 |
   | E[0x1748] | 0x4000 |
   | E[0x174A] | 3276 |
   | E[0x174C] | 3276 |
   | E[0x174E] | 1310 |
   | E[0x1750] | 10 |
   | E[0x1752] | 20 |
   | D[0x1152] | 0 |
   | D[0x116A] | 128 |
   | D[0x116C] | -20 |

2. It calls the init functions:
   * encoder init(E, 1, 1)
   * FEC init(E+0x16E0, 1)
   * encoder set(E, E[0x173C], E[0x173E])
   * decoder init(D, 1)
   * FEC init(D+0xFD0, 1)
3. It runs one decode with r0 = D+0x12AE, r2 = D+0x1354, r3 = 0, sp = {8, 0, D}.
4. It calls decoder set(D, D[0x116A], D[0x116C]).

046 does the same (D = 0x4AAA8, E = 0x4D548). So our firmware should call these functions rather than copy
`codec.c` arrays. The state blocks can live anywhere, because they're passed by pointer. Minimum sizes from the
accesses are about D >= 0x1400 and E >= 0x1900; the exact sizes are open.

## Hosting

How ODR runs it ([codec-placement.md](codec-placement.md), "HR_C7000 radios"):

1. **At the user's machine**, `tools/hrc7000_codec.py` (or the web flasher) checks the sha256 of the stock file,
   carves the code and the table window, and writes them unchanged into the codec slot at 0x030E8000..0x03100000,
   after a 128-byte version 2 header and before the relocation list: the offsets of the 3 self-referencing words,
   from the build's codec map. The slot is load-only data in XIP flash after the application.
2. **At boot**, `platform/hrc7000/vocoder.c` copies the code to the board's codec window at the top of SRAM
   (0x43A00..0x58000), adds (window start - stock run address) to each listed word, checks the first two words, and
   copies the tables to their stock modem RAM address (049: 0x18000EA8).
3. **Memory map** (`boards/dm32/memory.ld`): one RAM region for ODR from 0x10000 to the codec window. The modem RAM
   window 0x18000E48..0x18003E28 (the union of the builds' tables) is reserved.
4. **Versions**: every DM-32UV build with a codec map (12 stock files, six code windows) runs in the same window,
   since only the 3 words depend on the run address.

## Open items

* Audio quality against stock. On the radio (2026-10-19) the 049 codec in the moved window decodes on-air speech
  with every frame (234 of 234, 78 of 78 bursts).
* Exact sizes of the D/E state blocks; whether the codec writes to its modem RAM tables (the boot copy covers
  both cases).
