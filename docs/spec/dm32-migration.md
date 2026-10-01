<!-- SPDX-License-Identifier: MIT -->

# DM-32UV flashing and migration from stock firmware

How `tools/dm32_flash.py` writes firmware through the Baofeng bootloader, and how `tools/dm32_migrate.py` moves a
DM-32UV from its stock firmware to ODR and keeps the codeplug. The user guide is
[Moving from stock DM-32UV firmware](../user/19-moving-from-stock-dm32.md). The MD-380 counterpart, whose structure
and safety rules this follows, is [md380-migration.md](md380-migration.md).

**Status: untested on hardware.** Neither tool has talked to a radio. Facts marked (T) were tested with dmrconf
0.15.1 on files, or against the simulators in the self-tests. qdmr is GPL: ODR runs `dmrconf` as an external
program and contains none of its code.

## 1. The flasher (`dm32_flash.py`)

Implements [hardware-dm32.md](hardware-dm32.md) 13.2 on any pyserial port, 115200 8N1.

| Item | Handling |
|---|---|
| Input | A raw application image (linked for 0x0300C000), or a `BFUV32-V2` file: the 256-byte header is checked (`BFUV32-V2` at 0, `END` at 0xFD) and stripped (13.1). ELF files and other `BFU` versions are refused |
| Size | 1 to 0xF4000 bytes (0x0300C000..0x03100000). An image starting with 16 blank bytes is refused. The ODR codec slot ("DDRC" at 0x030E8000, `tools/dm32_codec.py`) is reported when present |
| Steps 1-6 | Fixed exchanges, 2 s each. Step 1 is sent up to three times. No answer: "is the radio in update mode? PTT + SK1 at power-on, green LED". A wrong answer names the step and both byte strings |
| Step 7 | `00 FF`, the 128-byte block (file name, NUL, decimal size), CRC-16/XMODEM big endian. The answer `06 43` comes after the erase: the wait is 30 s (13.2 says up to about 15 s) |
| Step 8 | Per 1 KB: `02`, wait `43`, then block number n (from 1, 0xFF wraps to 0x00), 0xFF - n, 1024 bytes (the last zero padded), CRC. `06` accepts. A `15` (NAK), another answer or a timeout resends the block from `02`, up to four times. Then the flash stops: the application area is erased, update mode is needed again |
| Step 9 | `04`; an answer within 0.5 s is read but not required |
| Ports | `--port`, or the only USB-serial bridge among CH340 / CH341 / CH9102 (1A86), FTDI (0403: 6001, 6010, 6014, 6015), CP210x (10C4:EA60) and PL2303 (067B:2303) |
| Recovery | `--stock FILE` flashes a stock `BFUV32-V2` file the same way |

Assumptions not in 13.2 (to check on the first radio): the bootloader answers a bad block with `15`, it accepts a
block sent again after a NAK, and it ignores the file name and size.

The self-test (`--self-test`, T) runs the flasher against `FakeBootloader`, the radio side of 13.2: every step's
bytes, the header CRC, block number, complement and CRC per block, and the image as written. Cases: a 301-block
image (numbers wrap) with three blocks damaged on the line and resent; a block refused every time; a silent radio; a
wrong step-2 answer; an erase that never ends. Plus the CRC check value, header and frame layout, file unwrapping and
size checks.

## 2. Where the codeplug goes on ODR

- ODR's DM-32 board keeps the legacy-layout codeplug (the EEPROM image and the flash image of
  [legacy-cps-interop.md](legacy-cps-interop.md) 7) at **+0x700000** in the 16 MB data flash: EEPROM image
  0x700000, flash image 0x720000 (`boards/dm32/flash_dm32.c`, `boards/dm32/board.mk`). The settings ring and the
  journal are at 0x711000..0x71AFFF, the native area at 0xF00000.
- Below 0x700000 is stock data: the stock codeplug pages and the factory calibration (0x001000..0x0C8FFF), the stock
  contact database and more ([hardware-dm32.md](hardware-dm32.md) 10.1). ODR's flash driver refuses every write and
  erase there. Nothing in this migration writes there either.
- The CPS shim takes **physical** addresses on area 1 and in `X 1` / `X 2` ([cps-protocol.md](cps-protocol.md)
  2.2-2.3). qdmr's open firmware drivers use the STM32 addresses (EEPROM image at 0, flash image at 0x20000). With
  the DM-32, they would read stock bytes and have every write refused. **dmrconf can't write ODR's DM-32 directly.**
- The board has no `cps` descriptor, so the radio info reports type 6 (MD-UV380), not 11.
- The shim speaks on UART0 through the programming cable. `dmrconf --device` would reach it, but the address
  problem above remains.

So the tool uses dmrconf only offline (encode, decode) and writes with `tools/cps_client.py`, which takes an
`offset` (`--offset 0x700000` on its command line) added to every area 1 address and `X 1` sector.

## 3. dmrconf facts

| Item | Fact |
|---|---|
| Radio keys (0.15.1) | `dm32uv` (stock Baofeng DM-32UV), `openuv380` (qdmr's driver for the OpenGD77-family STM32 radios, the legacy layout ODR serves) |
| Stock read | `dmrconf read --radio dm32uv [--device PORT] backup-stock.dfu` over the programming cable. Untested: whether `--device` wants the full path, and whether detection works on its own |
| dm32uv binary (T) | One DfuSe target "Baofeng DM32UV Codeplug", 4 KB elements at data flash addresses (0x4000, 0xB000, 0xF000..0x12000, 0x42000, ...) |
| openuv380 binary (T) | DfuSe with an empty target "OpenGD77 Codeplug EEPROM" and a target "OpenGD77 Codeplug FLASH" of four elements at the wire addresses 0x80 (0x5FE0 bytes), 0x7500 (0x3B00), 0x20000 (0x11A0), 0x9B000 (0x13E60). These are exactly qdmr's write regions ([cps-protocol.md](cps-protocol.md) 2.5) |
| DfuSe framing (T) | Prefix `DfuSe` 01, size, target count; targets with alternate setting 1, named; suffix FF FF FF FF FF FF 1A 01 "UFD" 10 and a CRC-32 that is the bitwise NOT of zlib's. The tool's writer reproduces dmrconf's files byte for byte |
| Conversion (T) | `decode --radio dm32uv` then `encode --radio openuv380` works on a dm32uv file; the round trip keeps channels, zones up to 68, contacts, RX group lists and the radio ID; scan lists are dropped |
| Callsign DB (T) | `encode-db --radio openuv380 --id ID [--database FILE] [--limit N]` gives one element at wire address 0x50000 (header "IdN..."), within ODR's flash image (0x750000 physical) |
| A dmrconf crash (T) | `encode --radio dm32uv` of a YAML with 1100 contacts ended in "Cannot resolve offset 49000h" (exit 139). Seen only on synthetic files |

## 4. Sequence (`all`)

0. Preflight: dmrconf with both drivers, pyserial, the helper scripts, the cable, the image (and the codec slot:
   without it the tool asks, or merges from `--codec-from DM32.01.01.049.bin` with `dm32_codec.py`).
1. Backup (stock firmware, normal mode): `dmrconf detect`, `read --radio dm32uv` to `backup-stock.dfu`, `decode` to
   `backup-stock.yaml` (or `--from FILE`). Then offline: `encode --radio openuv380` to `odr-codeplug.dfu` (the exact
   bytes restore writes), `decode` to `odr-codeplug.yaml`, and the conversion report (below). Over a limit: asks
   (`--accept-trim`). A users CSV becomes `users.json`, checked with `encode-db` (md380_migrate's code).
2. Flash: announce update mode, ask, `dm32_flash.Flasher` over the cable.
3. Wait for ODR (`odr_debug.py --port PORT ping`), `version` must say DM-32. `storage info`: `none` stops,
   `foreign` is claimed after a prompt (`storage claim --out` dumps it into the work directory first), `ours` goes on.
3b. Data flash backup through ODR's CPS reads (area 1, physical, 512 bytes a request): 0x000000..0x0C9000 by default
   (`dataflash-000000-0C9000.bin`, with a check that the calibration page, ID byte 2, is in it), all 16 MB with
   `--flash-backup full`, nothing with `none`. Skipped when the file is already in the work directory.
4. Codeplug: read the regions of `odr-codeplug.dfu` at +0x700000 into `legacy-window-before.bin` (whatever the stock
   firmware left there), write the sectors that differ (`X 1/2/3`, 32 bytes a request as qdmr), `C 6 0` (save and
   restart). Wait for ODR, read back, build `readback-odr.dfu` from the read and `decode` it; the counts must match
   what was written (after ODR's limits). Byte differences are counted and logged.
5. ID database: `encode-db` to `iddb.dfu` (around the codeplug's radio ID, or `--id`), the area saved to
   `iddb-area-before.bin`, then written the same way.

Every step stops on failure. The work directory (`~/ODR-DM32-migration/<date-time>/`: `migration.log`,
`dmrconf.log` with every dmrconf command and its output, `state.json`, the backups and reports) is never deleted.
`restore --workdir DIR` resumes from step 3; `restore --yaml FILE` converts and writes an edited qdmr file.

## 5. The conversion report

`conversion-report.txt` and `.json` in the work directory:

- item counts per section, stock and after the openuv380 round trip;
- what ODR's limits leave out (1024 channels, 68 zones, 1024 contacts, 76 RX group lists), by name;
- sections that shrink or vanish (scan lists, roaming, positioning, extra radio IDs, ...);
- names longer than 16 characters, names outside ASCII, and names that differ after the round trip;
- channels outside the DM-32's bands (136-174, 400-480 MHz, `boards/dm32/board.c`).

Never carried over: stock menu, key and display settings, encryption keys, the stock contact database.

## 6. Package

`dm32_migrate.py package OUTDIR --image odr_dm32_codec.bin` copies `dm32_migrate.py`, `dm32_flash.py`,
`cps_client.py`, `odr_debug.py`, `md380_migrate.py` (shared YAML, users-file and session code), `dm32_codec.py`,
`docs/testing/dm32-bringup.md`, the image(s) and `--include` files into a flat directory, and writes `README.txt`,
`requirements.txt` (pyserial) and `SHA256SUMS`. Every helper is loaded from the script's own directory, so the
kit runs as it is. The image is checked (size, header) and a missing codec slot is reported.

## 7. Self-test (`dm32_migrate.py --self-test`, in `tools/ci.sh`)

Offline, no dmrconf (T): DfuSe round trip, suffix, CRC and size checks; flat image and regions; a synthetic stock
YAML (70 zones, long and non-ASCII names, out-of-band channels, scan lists, roaming, two radio IDs) through the
report against its expected openuv380 round trip; `cps_client` with offset 0x700000 against `FakeCpsRadio` (the
shim's DM-32 map: physical addresses, writes only in 0x700000..0x710000 and 0x720000..0xF00000): the backup before
writing, the write lands at +0x700000, bytes outside the regions and everything below 0x700000 unchanged, the read
back matches and rebuilds the same DfuSe file, writes without the offset are refused, the stock data flash backup
finds the calibration page. Then the flasher's self-test.

## 8. Open points (need a radio)

- Everything in section 1 marked as an assumption, and the real erase time.
- `dmrconf read --radio dm32uv` with `--device`: the port name form, and the reading speed.
- Whether the shim's 32-byte requests over UART0 are reliable at 115200; a larger `CPS_BLOCK` would be faster.
- Whether the stock firmware leaves data at 0x700000 and up (voice recordings, 10.1). The tool saves those
  regions before writing; ODR's codeplug decoder ignores bytes outside qdmr's regions.
- Radio type: reporting 11 (as the legacy CPS expects) or 6. A shim-side offset for area 1 on the DM-32 would let
  qdmr program the radio directly; this tool would then need `CPS_OFFSET = 0`.
