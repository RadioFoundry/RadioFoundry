<!-- SPDX-License-Identifier: MIT -->

# TFTP codeplug access on the USB network link

In USB network mode (`usb-network.md`) the radio runs a TFTP server on UDP port 69 at its own address:
`get codeplug.bin` reads the codeplug, `put codeplug.bin` writes it, `get` and `put iddb.bin` do the same for the DMR
ID database, `get info.txt` says who the radio is. Any stock TFTP client works (`tftp`, `curl tftp://`, tftpy).
Switches: `CONFIG_TFTP` (= `CONFIG_DMR_IP_GATEWAY`), `CONFIG_TFTP_IDDB` (= `CONFIG_TFTP`, section 5). Code:
`core/net/tftp.c` (the protocol, no RTOS), `core/net/tftp_codeplug.c` (codeplug.bin, the staging and the commit),
`core/net/tftp_iddb.c` (the ID database), `core/net/net_services.c` (the glue, info.txt). Written from RFC 1350, 2347, 2348 and 2349.

## 1. Protocol

| Item | Behavior |
|---|---|
| Requests | RRQ and WRQ on port 69, octet mode only (`netascii` and `mail`: error 4). File names are case insensitive, leading `/` ignored |
| Transfer port | Each transfer answers from a port of its own (49152 and up, its TID), bound in the gateway's port table while it lasts. Packets from another address or port to it get error 5 and the transfer goes on |
| One at a time | A request while a transfer runs: error 0 "busy". The same request again (a client's retry) is ignored: the first answer is resent by the timer |
| Options | `blksize` 8-554 (the gateway's 600-byte frames: 600 - 14 - 20 - 8 - 4). A bigger one is ignored, so the transfer uses 512: RFC 2348 lets the server answer a smaller value, but macOS `tftp` keeps the size it asked for and takes the first, shorter block for the last. `tsize` (a read: the file's size; a write: the size announced, checked by the file); `timeout` 1-255 s, echoed and used. Unknown options and malformed values are ignored. Any accepted option: OACK first (a read then waits for ACK 0) |
| Lock step | One DATA (read) or ACK (write) outstanding, resent after the timeout (1 s, or the client's), abandoned after 5 resends with error 0 "timed out". A duplicate ACK is never answered with a DATA (RFC 1123 4.2.3.1). A duplicate DATA is ACKed again |
| End | A read ends with the ACK of the block shorter than blksize (an empty one when the size is a multiple). A write's last ACK goes out only after the file was checked and stored, or an error instead, so the client learns the outcome. After it the transfer dallies 3 s and ACKs a resent last DATA again |
| Block numbers | Roll over from 65535 to 0 |
| Errors from the client | End the transfer (a write's staged data is dropped). During a commit they are ignored: the data is complete and the commit finishes |

## 2. Files

| Name | Read | Write | Content |
|---|---|---|---|
| `codeplug.bin` | yes | yes (section 4) | the codeplug image (section 3); a write may carry the ID database too (5.2) |
| `iddb.bin` | yes | yes (section 5) | the DMR ID database alone (5.1). A read of a radio without one: error 1 |
| `info.txt` | yes | no | `board`, `firmware`, `dmr_id`, `callsign`, `radio_ip`, `host_ip`, `mdns`, `codeplug` (open-firmware / builtin / none), `codeplug_write` (allowed / off), one `key: value` per line |

No `settings.bin`: the settings record holds the privacy keys (`settings-store.md` 3.1), which the radio doesn't give
out over a read (`cps-protocol.md` 2.7 has the same open question for the CPS's area 1).

## 3. The image: codeplug.bin

**Format: the flat image `tools/cps_client.py` reads and writes**, 716,384 bytes (0xAEE60): file offset = the
open-firmware CPS's STM32 wire address. Bytes 0x00000-0x0FFFF are the EEPROM image, 0x20000 on the flash image
(`open-firmware-cps.md` 7.1). On a read, the regions qdmr transfers hold the stored bytes and everything else is
0xFF:

| File range | Content |
|---|---|
| 0x00080-0x0605F | EEPROM image part 1: device info, general settings (call sign, DMR ID), APRS, DTMF, channels 1-128 |
| 0x07500-0x0AFFF | EEPROM image part 2: boot screen, quick-key slots (the side keys), VFOs, zones |
| 0x20000-0x2119F | custom data |
| 0x9B000-0xAEE5F | flash codeplug: channels 129-1024, contacts, RX group lists |

Why this one:

- It is what the CPS path already exposes. A TFTP read is byte for byte what `cps_client.py read` (default `qdmr`
  regions) writes to its file, apart from VFO A: the CPS read overlays the live VFO there, TFTP gives the stored
  record. So `cps_client.py write`, `verify` and the HIL tests' backups take a TFTP image and the other way round, and
  the record decoders in `cps_client.py` (`decode_channel`, `set_channel_name`) work on it.
- The file stops at the end of the last region, so a read moves 716 KB, not the 1.15 MB of a whole cps_client
  image. A write takes any size from 716,384 bytes up to the end of the open-firmware space (`openfw_end`, as an
  image address): a whole cps_client image (1,179,648 bytes), or an image that carries a DMR ID database (5.2).
  Bytes outside the regions and the database are ignored.
- Not the radio's raw flash: the settings ring, the journal, the calibration sector and the native area aren't part
  of a codeplug and stay out (privacy keys, calibration).
- Not a new container with a header: no tool reads one, and the open-firmware layout is still the radio's storage
  (`codeplug-native.md` 4). When the native codeplug comes, it gets a file of its own.

## 4. Writing codeplug.bin

The CPS writes over USB without asking, so TFTP over USB does too: writes are **allowed by default**, under the same
rules as the CPS shim (`cps-protocol.md` 2.3), plus a setting to turn them off: **Settings > Connections > TFTP
codeplug write** (`settings_t.net_flags` bit 0 set = off; read at boot and when changed). Refused (error 2) while off,
while the flash region is foreign (`settings-store.md` 5), or without a staging area.

| Step | What |
|---|---|
| WRQ | `tsize`, when given, must be from 716,384 up to the end of the open-firmware space (else error 3). A commit record left by an earlier failed commit is finished first |
| DATA | The bytes inside the regions go to the staging area (`hal_flash_map_t.staging_addr`, `staging_size`, set by the board: `hardware-dm1701.md` 13.2, `hardware-md380.md` 12.2; the DM-32 has none), back to back from its second sector, erased as the data reaches it; a CRC-32 runs over them. Bytes outside the regions are ignored. Nothing of the stored codeplug changes |
| Last DATA | The size must be in the same range. An ID database the file carries is finished first (5.2). The staged bytes are read back and checked against the CRC. Then the image check: the general settings' DMR ID is 8 BCD digits, and at least one channel marked used (any bank) decodes (`codeplug_openfw_decode`). The open-firmware layout has no header or version to check, so this is what tells a codeplug from a wrong file |
| Commit | The commit record (magic `ODRT`, version 1, length, the staged CRC, its own CRC) goes in the staging area's first sector. Then each 4 KB sector of the codeplug that a region touches is rebuilt (the stored sector, the staged bytes over it) and written through the sector journal (`settings-store.md` 4), one sector per tick so the host link keeps running; unchanged sectors are skipped. The quick-key slots (EEPROM 0x7524-0x7537, the side key assignment, `ui.md` 3.1) are written like the rest, as with the CPS. Then the record is erased, the codeplug re-read (`codeplug_reload`), and the last ACK sent |
| Failure | Any check failing: error 0 with the reason, the stored codeplug untouched. A transfer that stops, an error from the client, a timeout: the staged bytes are left and never used (no record) |

Power cuts: before the commit record exists the old codeplug is intact. After it, `tftp_codeplug_resume()`, called
at boot right after `storage_init()` and before the codeplug is read, checks the record and the staged CRC and copies
every sector again (each one journaled, so a cut inside a sector write is finished by the journal first), then erases
the record. A torn record fails its CRC and is erased. So a cut transfer or a cut commit leaves the old or the new
codeplug, never parts of both.

Not carried over from the CPS: the VFO A record is written as it is in the image, and the live VFO doesn't follow it
(the CPS decodes a changed VFO A into the settings).

## 5. The DMR ID database

The database (`open-firmware-cps.md` 7.5) runs to megabytes (100,000 users at 41 bytes is 4 MB; the STM32 radios
take up to 13 MB), against a 256 KB staging area. It doesn't need staging: a broken database costs names on the
receive screen, not a usable radio. So it is written in place, in an order that never shows a broken one:

1. The sector holding the header (image 0x50000) is erased, and the radio drops its cached header
   (`dmr_iddb_init`): from here it has no database.
2. The records are programmed as they arrive, area 1 then area 2, each sector erased as the data reaches it, each
   write read back and compared.
3. At the end of a complete transfer the header is programmed into its still erased bytes, read back, and the radio
   reads the database again. `codeplug_reload` bumps the codeplug serial, so the radio drops the names it cached.

A transfer that stops, an error from the client or a timeout leaves the header erased: no database, never a broken
one. No journal, no commit record and no staging area are involved, so `iddb.bin` works on radios without one (the
DM-32). Writes follow the same rules as codeplug.bin: refused while TFTP codeplug writes are off or the flash region
is foreign. They are not refused while transmitting, as codeplug.bin's aren't: the flash calls take the storage lock
like every other writer. A put isn't power-cut safe beyond that rule: a cut mid-transfer leaves no database.

Accepted headers: `Id`, a record size the radio reads (more than the ID, at most `DMR_IDDB_RECORD_MAX`), at most
0x400000 records, any format but `n` (whose area 2 sits in the voice prompts). The records must fit below
`openfw_end`. A header that fails these is refused before anything is erased.

### 5.1 iddb.bin

**Format: the database alone**, the 12-byte header and then the records back to back, as a CPS uploads them and as
the web editor builds them. The server splits them between area 1 (as many whole records as fit in 256 KB with the
header) and area 2. Chosen over an image-shaped file because it is the database's own format: a tool that writes one
needn't know the areas, and a read moves only the database (a 4 MB database, not 13 MB of mostly erased image).

| Step | What |
|---|---|
| RRQ | The stored database: size = 12 + count x record size. No database (or one whose records would run past `openfw_end`): error 1. A format `n` database reads too, from its voice prompt area 2 |
| WRQ | `tsize`, when given, must be at least 12 (else error 3). Nothing is erased yet |
| DATA | The header is taken first (it may span blocks at the smallest blksize), checked, and checked against `tsize` (12 + count x record size, else error 3); then step 1. Then the records, step 2. Bytes past the header's count: error 3 |
| Last DATA | The size must be 12 + count x record size (else error 0, no database). Then step 3, then the last ACK |

### 5.2 A combined codeplug.bin

A codeplug.bin written past 716,384 bytes can carry the database in its image layout: the header and area 1 at
0x50000, area 2 at 0xD8000 (as the editor's image and a cps_client image hold it). The bytes in those ranges go to
the same writer as iddb.bin's, while the codeplug's regions are staged as in section 4. The database is taken only
when both hold:

- the 12 bytes at 0x50000 are a header the radio reads (above), and
- the client announced a `tsize` that reaches the end of its last record.

Otherwise those bytes are ignored and the radio keeps its database. That covers every codeplug.bin made before
(0xFF at 0x50000: a TFTP read, `cps_client.py read` with its default regions), a client that doesn't send `tsize`,
and a whole cps_client image of a radio whose database runs past 1,179,648 bytes. Without `tsize` the server can't
know before it erases the old header whether the file covers the new database, so it doesn't start. Use iddb.bin
with such a client.

The header is programmed when the last DATA arrives, before the codeplug's own checks and commit. The database
arrived whole and checked, so it stays even if the codeplug is then refused (not BCD, no channel) or its commit is
cut short: the two don't roll back together. A transfer that stops leaves no database and the old codeplug.

A read of codeplug.bin stays 716,384 bytes without the database: read `iddb.bin` for it.

## 6. Tests

- Host: `tests/host/tftpTest.c` with the real gateway and a client in the test: reads with and without options, an
  oversized blksize ignored and the largest taken, tsize, the timeout option, the empty last block, roll-over past block 65535, retransmits and the
  give-up, duplicate ACKs, a foreign TID, busy, a repeated request, refusals (unknown file, netascii, wrong
  direction, junk), writes with a slow commit, duplicate DATA, the dally, a refused commit, a refused open, a block
  longer than blksize, a client's error, a write that never starts. Then codeplug.bin on a simulated flash: the read
  matches the flash, an edited image written back (the quick-key slots written too, the codeplug re-read, outside bytes untouched, no
  missing erases), a whole cps_client image, refusals (tsize, short, no channel, not BCD, writes off, a transfer
  that stops) leaving every byte as it was, and power cuts at 29 points through a commit, each followed by a restart
  and the resume: old or new codeplug, never a mix. Then the ID database: iddb.bin across areas 1 and 2 (lookups,
  the read back equal, nothing outside the areas touched, the header split over blocks at blksize 8), refusals
  before any erase (no header, format `n`, too big, `tsize` against the header, writes off), a transfer that stops,
  one too long and one too short leaving no database, a combined codeplug.bin past 1,179,648 bytes, the old-size
  image, a combined one without `tsize` and one cut short inside its database each keeping the radio's, and a
  refused codeplug that keeps its database. The short transfers go to `tftpTest.pcap`, checked by tshark.
- HIL: `tests/hil/test_net_services.py` (`--allow-usb-switch`): `info.txt`, two reads of `codeplug.bin` identical and
  equal to the stored image; with `--allow-codeplug-write` also: back up, write a channel rename, read it back, write
  the backup again and verify it byte for byte, as `test_cps.py` does over the CPS.
  Debug link view keys (section `net`): `tftp.state` (0 idle, 1 reading, 2 writing, 3 committing, 4 dallying)
  `tftp.reads tftp.writes tftp.errors tftp.timeouts tftp.retransmits tftp.last_size tftp.blksize tftp.write_allowed`.
