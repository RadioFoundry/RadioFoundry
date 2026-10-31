<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Fast codeplug protocol

A second way to read and write the codeplug, beside the open-firmware CPS (`cps-protocol.md` 2). It moves the same
bytes: the open-firmware image of `open-firmware-cps.md` 7, as the CPS shows it. It differs in how they travel:

- Whole 4 KB blocks, the flash's erase unit.
- Blocks of 0xFF are reported, never sent, in both directions.
- The host learns each block's CRC-32 first, so only blocks that changed move. A host with an image from an earlier
  read reads only the difference, and writes only the blocks it changed.
- Blocks travel as LZ4 streams in both directions.
- A write is staged and committed under a record a power cut can't break, where the board has a staging area.
- At the end the radio re-reads its codeplug, as after a CPS `C 5`. No restart.

The CPS stays: qdmr and the open-firmware CPS speak it. The fast protocol is for our own tools: `tools/cps_fast.py`
and the web Codeplug Editor (`site/static/js/codeplug/fastlink.js`), which use it when the radio's VERSION features
list `CPS_FAST`.

- Firmware: `core/cps/cps_fast.c` (the engine, no RTOS), `core/cps/cps_lz4.c` (the codec), the hooks in
  `core/debuglink/debuglink.c` and `core/stubentry/stub_entry.c`, the tick in `core/hostlink/hostlink.c`, the boot resume in `core/app/app.c`.
- Switch: `CONFIG_CPS_FAST` (`docs/build-config.md`), on by default with `CONFIG_CPS_OPENFW` and either listener:
  the debug link, or the stub entry's own listener in a build without it (`stub-entry.md` 2).
  Built without it, the command answers status 4 (unavailable).
- Hosts: `tools/cps_fast.py`, the editor's `fastlink.js` and `transport.js`.
- Tests: `tests/host/cpsFastTest.c` (the engine), `tests/host/cpsFastClientTest.py` (the client against `cpsSim`),
  `site/tests/codeplug.test.mjs` (the editor's client against `cpsSim`).

## 1. Transport

The protocol is a debug link command: `CODEPLUG`, command byte `0x1C` (`debuglink.md` 3.20). A build without the debug
link answers it with the stub entry's own listener, on the same framing (`stub-entry.md` 2). Each request is one debug
link frame, each answer one reply frame (`debuglink.md` 2). The first payload byte is the operation.

Why the debug link and not a mode of the CPS port:

| | Debug link frame | CPS byte protocol |
|---|---|---|
| Integrity | CRC-16 on every frame, both ways | none: a garbled byte in a write is written |
| Request boundary | length field | the header, or 3 ms of quiet |
| Lost answer | sequence number: the host tells a late answer from a new one | none |
| Status | a status byte and a reason | `-` for both "done" and "error" |
| Hosts | `rfoundry_debug.py`, `tests/hil`, the web flasher's `link.js` already speak it, over USB, a UART cable and UDP | needs its own parser |

The debug link's limits shape the operations: a request carries at most 64 payload bytes, a reply at most 1024. Both
come from buffers the firmware already has, so the protocol adds none. Writes therefore move in pieces of 61 stream
bytes. One piece per round trip would make the link the slow part on a programming cable, where a round trip costs
several milliseconds beyond the bytes; hosts send DATA pipelined instead (section 3.6).

## 2. The image and its blocks

Addresses on the wire are image offsets, as in the files `tools/cps_client.py` and the web editor keep: 0 is the
EEPROM image (`hal_flash_map_t.openfw_eeprom`), the flash image starts at `openfw_flash - openfw_eeprom` (0x20000 on
every radio so far). The radio adds `openfw_eeprom` (0x700000 on the HR_C7000 radios), so a host never needs it.

A block is 4096 bytes at an offset that is a multiple of 4096. The protocol reads and writes a block when all of it
lies inside:

- the EEPROM image, offsets 0x00000 to 0x0FFFF, or
- the flash image, from `openfw_flash - openfw_eeprom` up to `openfw_end - openfw_eeprom`,

and none of it overlaps the radio's own areas: the settings ring, the sector journal, the native codeplug area, the
staging area and the calibration window. Any other block is refused (reason 1, section 6), for reads too. The gap
between the two images (0x10000 to 0x1FFFF on the STM32 radios: calibration, settings, journal) is never served,
unlike the CPS's area 1, which reads it raw (`cps-protocol.md` 2.7, decision 2).

The image is the one the CPS shows. The VFO A record (EEPROM 0x7590) shows the live VFO when one is set, in sums and
reads alike (`open-firmware-cps.md` 2.2). A block written with a VFO A record that differs from the live VFO gives the
VFO its settings, as a CPS commit does.

## 3. Operations

All numbers are little endian. Every reply starts with the debug link status byte (`debuglink.md` 2); the tables give
the bytes after it.

| Op | Name | Request after the op byte | Reply after the status |
|---|---|---|---|
| 0 | INFO | none | the layout and limits (3.1) |
| 1 | SUMS | addr u32, count u8 (1 to 16) | addr u32, count u8, blank mask u16, CRC-32 u32 x count |
| 2 | READ | addr u32, offset u16 | addr u32, format u8, length u16, CRC-32 u32, offset u16, a reserved byte (0), up to 1000 stream bytes |
| 3 | BEGIN | mode u8: 0 direct, 1 staged | stage_max u16 (0 for direct) |
| 4 | BLOCK | addr u32, format u8, length u16, CRC-32 u32 | none |
| 5 | DATA | offset u16, 1 to 61 stream bytes | received u16 |
| 6 | END | none | none |
| 7 | COMMIT | none | none |
| 8 | STATUS | none | state u8, mode u8, blocks u16, copied u16, reason u8 |
| 9 | ABORT | none | none |

Formats: 1 an LZ4 stream (section 5), 2 a blank block (no stream: length 0). Format 0 is kept for raw blocks and not
offered: an LZ4 stream of incompressible data is 0.4 % longer than the data, and one format keeps the radio smaller.

### 3.1 INFO

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | protocol version: 1 |
| 1 | 1 | flags: bit 0 staged writes available, bit 1 the flash can be written (`storage_ready`) |
| 2 | 2 | block size: 4096 |
| 4 | 1 | blocks per SUMS: 16 |
| 5 | 1 | stream bytes per DATA: 61 |
| 6 | 2 | stream bytes per READ reply: 1000 |
| 8 | 4 | EEPROM image size: 0x10000 |
| 12 | 4 | flash image start (image offset) |
| 16 | 4 | image end (image offset) |
| 20 | 4 | `openfw_eeprom`, for information |
| 24 | 2 | stage_max: blocks one staged session holds (0: no staging area) |

A host takes the limits from INFO rather than from this page. A radio without the protocol answers status 1 (firmware
older than the command) or 4 (built without `CONFIG_CPS_FAST`): the host falls back to the CPS.

### 3.2 SUMS

Up to 16 consecutive blocks from addr. For each, the CRC-32 of its 4096 bytes (section 4) and, in the mask, bit i set
when block i is all 0xFF. The radio reads and hashes 64 KB at most per request (some 30 ms on an STM32F4 radio). The whole
request is refused when any of its blocks is outside the image.

A host compares the CRCs with its own image: a block whose CRC matches is not read (a delta read) or not written (a
delta write). The radio's CRCs travel to the host rather than the host's to the radio: the bytes on the wire are the
same either way, one operation serves reads and writes, and a reply holds 16 CRCs where a 64-byte request holds 14.

### 3.3 READ

The block's LZ4 stream from offset on, at most 1000 bytes per reply. The radio encodes the block again for every READ
(a few hundred microseconds), so READ keeps no state: a lost reply is asked for again, and the host asks for
offset + n next until it has length bytes. A blank block answers format 2, length 0 and no stream. The CRC-32 is the
block's: the host checks it after decoding, and a CRC or length that changes between two READs of one block means the
block changed meanwhile (read it again). An offset past the stream's end is a bad argument. A READ while a write holds
the storage sector buffer (a block between BLOCK and END, or a pending CPS sector) answers busy (3).

### 3.4 Writing: BEGIN, BLOCK, DATA, END, COMMIT

```
BEGIN mode
  BLOCK addr format length crc      for each block that changed
  DATA offset bytes ...             the stream in order, 61 bytes at a time
  END                               the radio decodes, checks the CRC, writes (direct) or stages (staged)
COMMIT                              staged: the copy starts, poll STATUS; direct: the session ends
```

- BEGIN opens a write session. Only one at a time: a second BEGIN is refused (reason 6), busy while a commit runs.
  Staged mode needs a staging area (reason 3 without), one nobody else holds (reason 5, section 7.3).
- BLOCK names the next block: format, stream length (1 to 4128 for LZ4, 0 for blank) and the CRC-32 of the decoded
  4096 bytes. It takes the storage sector buffer (busy while someone else has it). A BLOCK while another block is
  open drops that one: the host's way to start a block again.
- DATA carries the next stream bytes. Offset must be what came so far. A DATA whose bytes all came already (the
  host repeats one whose answer was lost) is answered with the count and not taken again. A gap is refused
  (reason 8) and the block stays open. Bytes that make the stream corrupt drop the block (reason 8).
- END checks the stream was whole and decodes to exactly 4096 bytes (reason 8), then the CRC-32 (reason 7). A good
  block is written or staged (section 7); a flash failure is reason 9. The buffer is let go either way.
- COMMIT ends the session. Direct: the codeplug is re-read if a block was written. Staged: the record is written and
  checked (reason 9 if it didn't take; the staged blocks stay and COMMIT may be sent again), then the host link task
  copies one block per pass. The host polls STATUS until DONE or FAILED.
- ABORT ends the session without a commit: staged blocks are forgotten (no record names them), a direct session
  that wrote blocks re-reads the codeplug. ABORT during a commit answers busy: a commit always runs to its end.
- A session ends by itself 10 s after its last request (not while committing), as an ABORT.

Every operation is safe to repeat after a lost answer: BLOCK starts the block again, DATA takes a repeat as a
repeat, END after a lost answer is refused (reason 6) and the host sends the block again. A direct rewrite of the same
bytes is skipped by the journal; a staged one reuses the block's slot.

### 3.5 STATUS

| Field | Meaning |
|---|---|
| state | 0 idle, 1 a session between blocks, 2 a block open, 3 committing, 4 done (the last session ended well), 5 failed (a commit could not finish: its record stays and the next boot finishes it) |
| mode | the session's mode |
| blocks | blocks ended in the session |
| copied | committing: blocks copied in so far |
| reason | the last refusal's reason (section 6), 0 since BEGIN if none |

The debug link's `storage` view section has `cpsf.state`, `cpsf.read`, `cpsf.written`, `cpsf.commits` and
`cpsf.refused` (`debuglink.md` 3.7).

### 3.6 Pipelined DATA

A host may send up to 8 DATA requests of one block before reading their answers. Nothing on the radio changes for it:
the requests queue and are answered in order. 8 frames are at most 576 bytes: a UART host port's 1 KB receive ring
holds them all, and USB holds back what its 512-byte CDC buffer can't take yet (an OUT packet isn't acknowledged
until there is room), so nothing is lost on either. BLOCK and END are still sent alone, and END's answer waited for, since END writes the flash.

A window that goes wrong (an answer missing, a refusal: a frame lost to an overrun leaves a gap, and the requests
after it are refused with reason 8) is not repaired piece by piece: the host sends the block again from BLOCK, one
DATA at a time. Every operation is safe to repeat (3.4), so this needs no state on the host beyond the block.

## 4. The hash: CRC-32

The block hash is CRC-32 (IEEE 802.3, reflected, the zlib `crc32`), over the 4096 bytes of the block as the image shows
it. The firmware already has it (`storage_crc32`, for the journal and the TFTP staging), so it costs no flash, and every
host language has it in its standard library. The STM32F4 flash stub and the TFTP staging use the same.

CRC-32 finds every change of up to 32 consecutive bits and misses a random change with probability 2^-32 per block. A
delta transfer over a 1 MB image (256 blocks) misses a changed block about once in 16 million transfers of a fully
changed image. The host is trusted (it writes the codeplug anyway), so a stronger hash against deliberate collisions
buys nothing. The cost of a miss is a block not moved; the next edit of that block moves it.

The CRC of a blank block is 0xF154670A, that of 4096 bytes of 0xFF. The blank flag says the same without the host
comparing.

## 5. Compression: LZ4 blocks

Streams are LZ4 blocks: the public LZ4 block format (sequences of a token, literals, a 2-byte offset and a match
length), one block per 4 KB, no frame header. The stub's sector reads use the same format, so the host decoders exist
already: `stub/stubtool.py` (`lz4_block`, `lz4_encode`) and `site/static/js/flasher/lz4.js`.

### 5.1 Which direction compresses on the radio

Both. The cost decided it:

| Part | Flash (STM32F4 radio) | RAM |
|---|---|---|
| Decoder (writes) | about 300 B | none: a streaming decoder writes each DATA's bytes straight into the storage sector buffer, matches copy from what it already decoded there |
| Encoder (reads) | about 300 B | 128 B of stack (64 hash entries); it writes the requested window of the stream straight into the debug link's reply buffer |

Neither needs a buffer of its own, which is why the protocol fits a small RAM (section 9). The encoder's table
is small because codeplug blocks repeat over short distances (records of 56, 24 or 80 bytes, padding runs): on a
33-block codeplug read from a bench radio (135,168 bytes), a 64-entry table gives 3,618 bytes of LZ4, a 4096-entry one
3,692, a full search about 3,500. Runs of one byte only (no table at all) give 17,397: the table pays for itself on
any link.

Reads gain most on the 115200 baud programming cable of the radios without USB (11.5 KB/s), writes on every radio:
the flash is slow, but a write session also reads blocks back (section 7) and a host's write is usually followed by a
verify.

### 5.2 Format rules

- The radio's encoder follows the format's end rules: the last 5 bytes of a block are literals, and no match starts in
  its last 12 bytes. Any LZ4 decoder takes its streams.
- The radio's decoder takes any valid LZ4 block stream: matches may reach back to the start of the block, overlap (a
  run), and be of any length. It refuses an offset of 0 or before the start, a literal or match run past the end of the
  block, and bytes after the end. The stream must end with a literals-only sequence and decode to exactly 4096 bytes.
- The longest stream a 4 KB block can need is 4096 + 4096 / 255 + 16 bytes: BLOCK refuses longer.

## 6. Errors and refusals

Status bytes are the debug link's (`debuglink.md` 2): 0 OK, 2 bad argument (a length that doesn't fit the operation,
a count outside its range, an offset past a stream), 3 busy (the storage sector buffer is taken, a commit is running),
4 unavailable (built without the switch, no flash map), 7 refused. A refusal carries a reason byte after the status:

| Reason | Name | When |
|---|---|---|
| 1 | RANGE | a block outside the image, misaligned, or touching the settings ring, the journal, the native area, the staging area or the calibration window |
| 2 | STORAGE | BEGIN while the flash can't be written: no usable flash, or the settings region holds another firmware's data (`settings-store.md` 5) |
| 3 | NO_STAGING | BEGIN staged on a radio without a staging area |
| 4 | STAGE_FULL | a new block when the session already holds stage_max blocks |
| 5 | STAGE_BUSY | BEGIN staged while a TFTP write holds the staging area, or a commit record waits for the next boot |
| 6 | STATE | an operation out of order: BLOCK, DATA or END outside a session, END without BLOCK, BEGIN in a session, COMMIT with a block open |
| 7 | CRC | the block decoded, but its CRC-32 is not the one BLOCK gave |
| 8 | STREAM | a corrupt LZ4 stream, a gap in the offsets, a stream that doesn't decode to 4096 bytes |
| 9 | FLASH | a flash erase, program or read-back failed |

These are the CPS's refusals and more: the CPS refuses the settings ring, the journal and the native area at `X 1`
and filters the calibration window byte by byte (`cps-protocol.md` 2.3); this protocol refuses those blocks whole,
the calibration window's and the staging area's included.

## 7. Writes and power cuts

### 7.1 Direct mode

Each block is written at END through the sector journal (`storage_sector_write`, `settings-store.md` 4): a copy of
the new block and a header go to the journal first, so a power cut during the erase or the program is finished at the
next boot. Each block is old or new after a cut. A session cut halfway leaves some blocks new and some old, as a CPS
write cut halfway does. Boards without a staging area write this way, and every radio
uses it for writes larger than the staging area. A DMR ID database goes in a direct session of its own, the block
holding its header sent first with the header erased and again last, so a cut leaves no database rather than a mix
(`iddb-compact.md` 4.3).

### 7.2 Staged mode

Where the board has a staging area (`hal_flash_map_t.staging_*`, typically 256 KB),
the whole session's blocks change together or not at all, as a TFTP codeplug write does (`tftp.md` 4):

1. END puts the block in its slot of the staging area (slot k at `staging_addr` + (k + 1) x 4096; a block sent
   again reuses its slot), erases and programs it, and reads it back against the CRC-32. Nothing in the codeplug
   changes.
2. COMMIT writes the record into sector 0 of the staging area and reads it back.
3. The host link task copies one slot per pass into the codeplug through the journal, then erases the record and
   re-reads the codeplug.

The record:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic 0x4652444F (bytes 4F 44 52 46 in memory order) |
| 4 | 4 | version: 1 |
| 8 | 4 | count: staged blocks (1 to stage_max) |
| 12 | 4 | CRC-32 of the staged slots, in slot order |
| 16 | 2 x count | each slot's block number (image offset / 4096) |
| 16 + 2 x count | 4 | CRC-32 of the bytes before it |

At boot (`cps_fast_resume`, after `storage_init` and before the codeplug is read) a record whose magic, version,
count, header CRC and data CRC all check, and whose blocks are all writable (section 2), is finished: every slot is
copied in again (the journal skips those already in) and the record erased. A record that doesn't check is erased:
it was torn while being written, so the copy never started. The VFO settings are not touched at boot (the settings
aren't up yet); the VFO A record is in the codeplug and takes effect as stored.

| Power cut | After the next boot |
|---|---|
| before COMMIT (staging) | the old codeplug; the staged slots are left, unused |
| while the record is written | the old codeplug; the torn record is erased |
| during the copy | the new codeplug: the boot copies every slot |
| during one block's copy | the journal finishes that block, then the boot copies every slot |

`tests/host/cpsFastTest.c` cuts the power at points all through a staged write of four blocks and checks the four are
all old or all new after the boot, never a mix.

Staged writes do twice the flash work of direct ones (each block is programmed into its slot and then copied), so they
take about 1.5 times as long (section 10). `tools/cps_fast.py` uses staged mode when the radio offers it and the
changed blocks fit, direct mode otherwise or with `--direct`.

### 7.3 The staging area is shared

The TFTP server's codeplug write (`tftp.md` 4) uses the same area. The first word of its first sector says whose
record is there (`STORAGE_STAGE_MAGIC_TFTP`, `STORAGE_STAGE_MAGIC_CPSF`):

- At boot `cps_fast_resume` runs first and leaves a TFTP record alone; `tftp_codeplug_resume` leaves ours alone.
- BEGIN staged is refused (reason 5) while a TFTP write is open or a record of either kind is there.
- A TFTP write is refused while a staged session is open, a commit runs, or our record waits for the boot.
- A record sector whose first word is erased but whose other bytes are not (a torn erase) is erased at BEGIN.

Both run in the host link task, so the checks don't race.

## 8. Sizes

| Item | Value |
|---|---|
| block | 4096 bytes, aligned |
| request payload | at most 64 bytes (the debug link's) |
| reply payload | at most 1024 bytes |
| SUMS | 16 blocks: request 6 bytes, reply 72 |
| READ | at most 1000 stream bytes per reply (15 header bytes) |
| DATA | at most 61 stream bytes |
| LZ4 stream | 1 to 4128 bytes per block |
| staged blocks | stage_max: (staging size / 4096) - 1, at most 64 (63 for a 256 KB area) |
| session time-out | 10 s after the last request |
| RAM | about 200 B of state (STM32F4 radio); the 4 KB storage sector buffer and the debug link's buffers are shared |
| flash | about 3.9 KB (STM32F4 radio) with the switch on, nothing with it off |

## 9. Example exchange

A host reads block 0x3000 (channel bank 0) of the simulator's codeplug (`tests/host/cpsSim --populate`), renames
channel 3 and writes the block back staged. Payloads in hex; each travels in a debug link frame, for example INFO:
`a5 5a 1c 31 01 00 00 10 f3` (command 0x1C, sequence 0x31, length 1, payload `00`, CRC-16).

| Dir | Payload | Meaning |
|---|---|---|
| > | `00` | INFO |
| < | `00 01 03 00 10 10 3d e8 03 00 00 01 00 00 00 02 00 00 00 e0 00 00 00 00 00 3f 00` | OK, version 1, staging and writable, block 4096, 16 sums, 61 data, 1000 read, EEPROM 0x10000, flash 0x20000, end 0xE00000, base 0, 63 slots |
| > | `01 00 30 00 00 01` | SUMS 0x3000, 1 block |
| < | `00 00 30 00 00 01 00 00 15 dd 07 05` | OK, not blank, CRC-32 0x0507DD15 |
| > | `02 00 30 00 00 00 00` | READ 0x3000 from offset 0 |
| < | `00 00 30 00 00 01 99 03 15 dd 07 05 00 00 00 ...` (936 bytes) | OK, LZ4, 921 stream bytes, the CRC, offset 0, the reserved byte, the whole stream |
| > | `03 01` | BEGIN staged |
| < | `00 3f 00` | OK, 63 slots |
| > | `04 00 30 00 00 01 d7 02 1b d9 60 28` | BLOCK 0x3000, LZ4, 727 bytes, CRC-32 0x2860D91B |
| < | `00` | OK |
| > | `05 00 00 1f ff 01 ...` (61 stream bytes) | DATA from offset 0 |
| < | `00 3d 00` | OK, 61 received |
| | ... | 11 more DATA |
| < | `00 d7 02` | OK, 727 received |
| > | `06` | END: decoded, CRC checked, staged in slot 0 |
| < | `00` | OK |
| > | `07` | COMMIT: the record is written |
| < | `00` | OK |
| > | `08` | STATUS |
| < | `00 04 01 01 00 01 00 00` | OK, done, staged, 1 block, 1 copied, no refusal |

The read took 2 requests (SUMS, READ) and about 1 KB on the wire, where the CPS's 32-byte reads of the same 4 KB take 128 requests and
5.5 KB. The CPS's chunked reads (the web editor's 32 KB on USB) move the full 4 KB plus the codeplug's blank blocks.

## 10. Speed

What a transfer costs, on a 33-block codeplug (the bench radio's, 135 KB in blocks that aren't blank):

| Transfer | Bytes on the wire | 115200 baud cable | USB |
|---|---|---|---|
| CPS, 32-byte reads of the qdmr regions (qdmr) | about 169 KB, 3,928 requests | about 15 s plus 3,928 round trips | several seconds (the round trips) |
| CPS, 4 KB / 32 KB reads (the web editor) | about 135 KB, blank blocks too | about 12 s | under 1 s |
| fast, full read | about 5 KB: 3 SUMS, 33 READs | about 0.5 s plus 36 round trips | about 0.1 s |
| fast, read from a cache, nothing changed | 3 SUMS, about 0.3 KB | a few round trips | a few ms |
| fast, write one changed channel | one block: about 0.8 KB of stream | under 0.1 s on the wire | |
| fast, verify after a write | SUMS of the written blocks, 72 bytes per 16 | a few round trips | a few ms |

Writes are limited by the flash, not the link. A 4 KB sector erase and program take some 60 ms on a
W25Q128 SPI NOR (the erase alone up to 400 ms at worst). A direct block costs two of them (journal and target), a staged block
three (slot, journal, target). Only changed blocks are written, where a CPS write rewrites every sector of its regions
(qdmr) or every sector with a change (the web editor).

## 11. Alternative considered: the RAM stub

The flash stubs (`stub/README.md`, `stub-stm32f4.md`) already offer CRC-compared sector writes, blank detection and,
on the HR_C7000 stub, LZ4 reads. The codeplug could be transferred through them instead, with no protocol code in
the application. It was not chosen:

| | In the application (this protocol) | Through the RAM stub |
|---|---|---|
| Flash (STM32F4 radio) | about 3.9 KB | about 0.8 KB for `CONFIG_STUB_ENTRY` (on by default) |
| RAM (STM32F4 radio) | about 0.2 KB | 8 KB: the stub's staging buffer (`CONFIG_STUB_ENTRY` takes RAM from 87 % to 94 %) |
| The radio meanwhile | keeps working; the codeplug is re-read at the end | stops (the stub owns the SoC); a restart at the end |
| Power-cut safety | the journal and the staged commit | the stub writes sectors in place: a cut mid-write leaves a broken sector |
| Refusals | the radio enforces its areas | the host must; the stub writes any sector |
| VFO and settings | as the CPS | not seen |

The stub stays the tool for firmware and whole-flash work.

## 12. Open points

- Not yet run on a radio: the HIL test (a read against the CPS, a staged write, a power cut during a commit) is to be
  written once the owner flashes a build with the switch. The web editor speaks the protocol
  (`site/static/js/codeplug/fastlink.js`) and is tested against the host simulator only; it keeps the CPS for
  radios that don't list `CPS_FAST`.
