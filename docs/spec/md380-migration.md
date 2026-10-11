<!-- SPDX-License-Identifier: MIT -->

# MD-380 migration from stock or md380tools firmware

How `tools/md380_migrate.py` moves a TYT MD-380, MD-390 or Retevis RT3 from its stock firmware (or the md380tools /
KD4Z toolkit patched firmware) to ODR and keeps the codeplug and the DMR ID database. The user guide is
[Moving from stock or md380tools firmware](../user/18-moving-from-stock-md380.md).

The codeplug conversion uses qdmr's command line tool `dmrconf` as an external program. qdmr is GPL: ODR runs it,
reads its manual and `--help`, and contains none of its code. Facts below marked (T) were tested with dmrconf
0.15.1 on files only. Everything that talks to a radio is untested (no radio at hand).

## 1. dmrconf and this radio family

| Item | Fact |
|---|---|
| Radio keys (`dmrconf --list-radios`, 0.15.1) | `md390` (TyT MD-390), `rt8` (Retevis RT8), `openuv380` (qdmr's open firmware driver for the STM32 family), and others. There is no `md380` key |
| MD-380, MD-390, RT3 | One codeplug format (the TYT MD-380 CPS programs all three; GPS settings are unused on the radios without GPS). The tool reads the stock radio with `--radio md390`. Whether `dmrconf detect` names an MD-380 by itself is untested, so the tool always passes the key |
| Commands used | `detect`, `read`, `decode`, `decode --manufacturer`, `encode`, `write --init-codeplug`, `encode-db`, `write-db` |
| TYT CPS files (.rdt) | `decode --manufacturer --radio md390 FILE.rdt OUT.yaml`. dmrconf requires exactly 262709 bytes (T). That is a 549-byte header, the 256 KB codeplug image and a 16-byte trailer. A synthetic .rdt with a zero header and trailer around an image decodes the same as dmrconf's own binary (T) |
| md390 binary (dmrconf `.dfu`) | One DfuSe element, codeplug addresses 0x2000-0x40000 of the 256 KB image (T) |
| Raw 256 KB image (md380tools codeplug dump) | Not read by dmrconf directly. The tool wraps it in a zero .rdt frame (`raw_to_rdt`) and decodes that |
| Over ODR's limits | `encode --radio openuv380` silently keeps the first 68 zones and 76 RX group lists (T). The tool reports what would be dropped before it flashes |
| Callsign DB, md390 | `encode-db --radio md390` fails: "Not implemented for 'md390'" (T). Not needed: the database goes to the radio once it runs ODR |
| Callsign DB, openuv380 | `write-db --radio openuv380 --id ID [--database FILE] [--limit N]`. Without `--database`, dmrconf downloads the public database itself |
| `--database` JSON | `{"users": [{"id": 3100001, "callsign": "N0CALL", "fname": "Alice", "surname": "Smith", "city": "Denver", "state": "CO", "country": "US"}]}`. `id` must be a number. Records come out as `CALL Fname City State Country`, format `N`, 27-byte records (T, decoded per [open-firmware-cps.md](open-firmware-cps.md) 7.5). Files keyed `radio_id`, or `{"results": [...]}`, load 0 entries (T) |

## 2. What ODR presents

- Radio info (area 9) radio type 7 ([open-firmware-cps.md](open-firmware-cps.md) 4, `boards/md380/board.c`):
  qdmr's OpenUV380 driver. USB 1FC9:0094 serial, like the DM-1701 ([cps-protocol.md](cps-protocol.md) 2).
- Limits: 1024 channels, 68 zones of 80 channels, 1024 contacts, 76 RX group lists of 32 contacts
  ([open-firmware-cps.md](open-firmware-cps.md) 7.2-7.3). A stock MD-380 holds 1000 channels, 1000 contacts,
  250 zones of 16 channels and 250 RX group lists. In practice only zones and RX group lists can overflow.
- SPI flash: on a 16 MB part the DM-1701 map applies ([hardware-md380.md](hardware-md380.md) 12.2). On a 1 MB part
  ODR gets no storage areas, so `storage info` says `region none`. ODR then runs from RAM and nothing can be
  restored. The tool stops there and keeps the backup.
- The stock firmware keeps its codeplug at the start of the SPI flash, where ODR's settings ring and journal go
  (0x011000-0x01AFFF). After the flash, that region is foreign ([settings-store.md](settings-store.md) 5), and every
  CPS commit is refused until `odr_debug.py storage claim` runs. The claim dumps the region and the calibration copy
  first. The tool runs the claim after asking, with the dump going into its work directory.
- The rest of the open-firmware image still holds stock bytes after the claim. The restore therefore writes with
  `--init-codeplug`: qdmr builds the image from its defaults and doesn't decode the junk first. The DMR ID database
  header at flash 0x50000 is past the 256 KB stock codeplug. md380tools keeps its user database at 0x100000 and up,
  inside ODR's open-firmware flash image. The ID database upload overwrites what it needs, and nothing else reads that
  area.

## 3. Firmware images

| Input | Handling |
|---|---|
| ODR image with the codec merged (`odr_md380_codec.bin`, about 988 KB from 0x0800C000) | XORed with the bootloader key, then written through the AnyRoad DFU path of `tools/stm32_dfu.py` |
| ODR image without the codec (`odr_md380.bin`) plus `--stock` | Codec merged with odrflash's `stockfw.extract` / `merge`, the same bytes as the build's merged image (checked against a local build) |
| TYT updater file (`OutSecurityBin`, one segment at 0x0800C000) | The payload is already XORed with the key, indexed from the payload start. That is the bootloader stream ([hardware-md380.md](hardware-md380.md) 14.2-14.3), so it goes out unchanged, without the key. With a key, the decrypted vector table is checked first. This also restores the stock firmware |
| Key | `--key FILE` (checked by SHA-256), or derived from `MD-380-D2.32(AD).bin` with `stockfw.derive_key` (about 6 s) |

The stock radio in normal programming mode also enumerates as 0483:DF11. The tool shows the USB strings and asks
the user to confirm the screen is dark (bootloader) before it writes. Which strings tell the two modes apart is not
known yet.

## 4. Sequence (`all`)

0. Preflight: dmrconf 0.12 or newer, pyusb + libusb, pyserial, the image checks, and the key. Nothing touches the
   radio.
1. Backup: `dmrconf read --radio md390 backup-stock.dfu` (or `--from FILE`), then `decode` to YAML. Encode for
   `openuv380` and decode again (`odr-codeplug.yaml`), compare the counts and report what ODR's limits drop. A users
   CSV becomes `users.json` and is checked with `encode-db`.
2. Flash: bootloader prompt, wait for 0483:DF11, confirm, write.
3. Wait for ODR (`odr_debug.py ping`). `version` must say MD-380. Then `storage info`: `none` stops, `foreign` is
   claimed after a prompt, and `ours` continues.
4. `dmrconf write --radio openuv380 --init-codeplug odr-codeplug.yaml`. Wait for the restart. Read back and compare
   the counts.
5. `dmrconf write-db --radio openuv380 --id <codeplug's radio ID>`, with `--database users.json` when a users file
   was given.

Every step stops on failure. The work directory (`~/ODR-MD380-migration/<date-time>/`, with a log and
`state.json`) is never deleted. `restore --workdir DIR` resumes from step 3.

## 5. Open points (need a radio)

- Does `dmrconf read --radio md390` read an MD-380 and an RT3? Does it read one running md380tools?
- The USB strings of the TYT bootloader compared with normal programming mode.
- Whether qdmr's OpenUV380 driver accepts radio type 7 from ODR without `--radio`. The tool always passes it.
- How long `write-db` takes over ODR's shim with a full database. Expect minutes.
- Windows: dmrconf's TyT access, and Zadig's WinUSB binding for the bootloader.
