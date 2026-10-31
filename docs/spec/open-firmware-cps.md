<!-- SPDX-License-Identifier: MIT -->

# Open-firmware CPS interoperability

This spec describes the serial protocol and the codeplug memory layout that the open-firmware CPS and qdmr's GD-77/UV380
open-firmware drivers use. A new implementation can use it to build an interop shim, so that existing PC tools can
read and write our radios. It covers wire formats and stored data formats only. How the new firmware stores its own data internally
is a separate decision. The shim can translate between the image described here and our own storage, or it can keep
this image as the storage.

Sources: qdmr's open-firmware drivers for the GD-77 and the UV380 family (GPL-3, github.com/hmatuschek/qdmr; only protocol
facts were taken from it) and the behavior of RadioFoundry's own implementation (`core/cps/cps_openfw.c`,
`core/codeplug/`). The open-firmware CPS is closed source. What it reads and writes is what the radio accepts and what
qdmr does.

| Contents | |
|---|---|
| 1, 2, 3 | Transports and framing, requests, memory areas |
| 4 | Radio info and radio types |
| 5, 6 | The `D` data service commands, the hotspot mode switch |
| 7 | The codeplug image: address mapping, EEPROM map, records, DMR ID database, voice prompts, settings block |
| 8, 9, 10 | Byte order, version differences, open questions |

Conventions: "BE" is big endian and "LE" is little endian. Offsets are hexadecimal unless they are plainly decimal
counts. "Image address" means the address the PC tool puts on the wire. Strings in the codeplug are padded with 0xFF,
not NUL terminated, unless stated otherwise.

## 1. Transports and framing

| Radio family | Transport | Framing of a request |
| --- | --- | --- |
| STM32F405 radios | USB full speed CDC-ACM, VID 0x1FC9 PID 0x0094; qdmr matches on the VID and PID only | USB bulk packets (64 bytes max), reassembled as below |
| NXP MK22 radios | USB full speed CDC-ACM, VID 0x1FC9 PID 0x0094 | as STM32 |
| HR_C7000 radios with no USB | Programming cable on a UART at 115200 8N1, through a USB-serial adapter. No USB on the radio | A request ends after 3 ms without a received byte, or when the header gives the length. Image at the STM32 addresses |

| STM32 radios in our USB network mode | USB CDC-NCM (VID 0x1FC9 PID 0x0096, no serial port). The serial protocol is carried in UDP datagrams to port 3334 at the radio's address | one datagram is one USB packet. Replies go back to the address and port that sent last. One client at a time |
| MK22 radios in our USB network mode | Composite device VID 0x1FC9 PID 0x0095: CDC-ACM (interfaces 0-1) plus CDC-NCM (interfaces 2-3) | as USB |

qdmr finds radios only by VID 0x1FC9 and PID 0x0094. A radio in network mode, or one behind a generic USB-serial adapter, has to be picked by port.

### 1.1 USB reassembly (STM32 and MK22)

Every request is handled to completion, and the reply is sent, before the next request is taken. Bytes that arrive
while a request is being handled are dropped. The PC waits for each reply before it sends again.

- A packet shorter than the endpoint size (64 bytes) completes a request, unless more bytes are expected (below).
- If the first packet of a request is exactly 64 bytes long, the radio works out the expected total length from it:
  - `R` (read): 8 bytes.
  - `W` or `X` with sub command 1: 5 bytes.
  - `W` or `X` with sub command 3: 2 bytes.
  - `W` or `X` with sub command 2, 4 or 7: 8 bytes plus the length field in bytes 6-7.
  - Anything else: the packet alone.
  Packets are then appended until the expected count is reached or a short packet arrives.
- The request buffer is 2048 bytes on the STM32 radios and 1536 bytes on the MK22. A request that would overflow it
  is discarded, and the radio answers with a single `-` (0x2D).
- The radio clears the request buffer before each new request. The text command (`C` 2) relies on this for NUL
  termination.

### 1.2 UART framing (radios without USB)

- The receive interrupt appends bytes to a 2048 byte request buffer. While a request is being handled, received
  bytes are dropped.
- When 3 ms have passed since the last byte, the main loop takes the bytes as one request. It handles the request,
  writes the reply on UART0 from the main loop, and clears the buffer.
- If the request overflowed the buffer, the reply is `-`.
- For 10 s after a request a "CPS session" is active. Debug text on UART0 (a build option) is suppressed during the
  session, so it can never land in the middle of a reply.
- In hotspot mode the bytes go to a separate 1024 byte ring instead, and are split into MMDVM frames by the frame
  length byte (section 6).
- The radio info reports the board's radio type (4); a board without a `radio_type` hook reports 6.

### 1.3 Addresses and lengths

All multi-byte fields in the request and reply headers are big endian. Data payloads are raw memory bytes. Most
structures inside them are little endian (section 8).

## 2. Requests

The first byte of a request selects the command.

| Byte 0 | Meaning | Radios |
| --- | --- | --- |
| `R` (0x52) | Read from a memory area | all |
| `W` (0x57) | Write. On the MK22 this is the real write command. On the STM32 radios and the HR_C7000 radios it is a "fake write": the radio acknowledges it (echoes bytes 0-1) and does nothing | all |
| `X` (0x58) | Write, on the STM32 radios and the HR_C7000 radios (sub commands as for `W`) | STM32, HR_C7000 |
| `C` (0x43) | Control command | all |
| `D` (0x44) | Our DMR data service host commands (section 5). A build with USB debug commands uses `D` for the debug console | all of our builds |
| 0xE0 | First byte of an MMDVM frame: switches to hotspot mode (section 6) | all, when a hotspot type is set |
| anything else | Answered with `-` | |

qdmr sends `W` to the MK22 family (its variant "GD77": radio types 0-4) and `X` to the STM32 family (its variant
"UV380": radio types 5-10). It reads and writes in 32 byte blocks.

### 2.1 `R`: read

| Offset | Size | Meaning |
| --- | --- | --- |
| 0 | 1 | `R` |
| 1 | 1 | Area (section 3) |
| 2 | 4 | Address, BE |
| 6 | 2 | Length, BE |

Reply on success:

| Offset | Size | Meaning |
| --- | --- | --- |
| 0 | 1 | `R` |
| 1 | 2 | Length actually returned, BE |
| 3 | n | Data |

On failure the reply is a single `-`. The original firmware clips the length to its reply buffer size minus 3 (2045).
RadioFoundry streams the reply in small pieces instead of building it in a buffer, so it clips at 32768: a client that reads
large areas (the web flasher's backup) asks for 32 KB at a time and takes what comes back, which is 2045 bytes from
the original firmware and older RadioFoundry builds. qdmr checks that the returned length equals the requested one, and asks
for 2045 or less. The radio info area always returns its own structure length, whatever length was asked for.

Send the header and the first data in one USB write. qdmr (0.15) reads a reply as soon as part of it has arrived and
doesn't wait for the rest. A 32-byte block sent as the header and then the data reached it short. qdmr then read the
late data byte as the next reply's first byte, reported an error and asked the radio to reboot. One write of 35 bytes
fits a single full-speed USB packet, as the original firmware sends it.

### 2.2 `W` / `X`: write

| Sub (byte 1) | Request after bytes 0-1 | Action |
| --- | --- | --- |
| 1 | Sector number, 3 bytes BE (image address / 4096) | Prepare a 4 KB flash sector: the radio reads the sector into a RAM buffer. Fails if a sector is already pending |
| 2 | Address 4 bytes BE, length 2 bytes BE, data | Merge data into the pending sector buffer. Bytes outside the pending sector are ignored. Fails if no sector is pending |
| 3 | none | Commit: erase the sector and program it as 16 pages of 256 bytes. Clears the pending state |
| 4 | Address 4 bytes BE, length 2 bytes BE, data | EEPROM write. Real only on the MK22 (I2C EEPROM). On the STM32 radios and the HR_C7000 radios the "EEPROM" is emulated in flash: the radio acknowledges and does nothing, and the PC writes the EEPROM image with sub commands 1-3 instead (section 7.1) |
| 7 | Address 4 bytes BE, length 2 bytes BE, data | Write into the audio (WAV) buffer at that offset (section 3). The buffered sample count becomes (address + length) / 160 |
| 9, other | | Refused: reply `-` |

The reply on success is 2 bytes: byte 0 and byte 1 of the request echoed. On failure it is a single `-`, and any
pending sector is dropped. Data lengths are clipped to the request buffer size minus 8.

Special cases the radio applies to sub commands 1-3 (and 4 on the MK22). An interop shim must reproduce them:

| Case | Behavior |
| --- | --- |
| Calibration window, flash image 0x10000..0x101FF (STM32 and HR_C7000 radios) | Preparing sector 0x10 succeeds without reading flash. Data inside the window goes to the radio's working calibration copy. The commit saves that copy. Reads of the window are served from the copy (2.1). On an HR_C7000 radio the copy is the UV380-format table: the radio's own factory calibration is not exposed, and the CPS has no calibration window of its own (the sector is plain data). The GD-77 has no window: its calibration is plain flash at 0xF000 (working copy) and 0x8F000 (factory) |
| Quick-key slots, EEPROM image 0x7524..0x7537 (10 entries of 2 bytes) | Written like the rest of the codeplug. RadioFoundry keeps the key assignment there (`ui.md` 3.1); values another firmware left are ignored |
| Channel bank 0 rewritten (a write that falls entirely inside EEPROM image 0x3780..0x538F) | Remembered. At "save and reboot" (`C` 6 0) the radio re-reads the zones and channels, selects the All Channels zone, and points it at its last used channel, or else the first channel in use |
| LUCZ rewritten (a write inside 0x6000..0x6049) | Remembered. The radio then doesn't overwrite the LUCZ with its own copy at "save and reboot" |
| Voice prompt header written | If a sub command 2 write starts at the voice prompt header address (new or old location, section 7.6) and its first 8 bytes carry a valid magic and version, the radio switches announcements to voice level 1 |
| DMR ID database upload | Preparing the sector at image 0x30000 marks a DMR ID upload. On Close (`C` 5) the radio re-reads the database header. The check uses the address without the 128 KB offset, so on STM32 radios it doesn't match the real database location (0x50000). A new implementation should re-read the database after any write that touches it |
| VFO overlay on read (STM32 and HR_C7000 radios) | When a flash read starts at or before 0x7590 and runs past the VFO pair, the radio replaces those 112 bytes in the reply with its live VFO settings in codeplug format. The check uses the radio's in-memory record size, so the read must extend somewhat beyond 0x75FF. A tool that reads 32-byte blocks never triggers it. A large-block CPS read does. The MK22 doesn't do this. The CPS sends `C` 6 2 first, so the VFOs are already saved |

### 2.3 `C`: control

The reply to every `C` request is a single `-`. It means "done", not "error". qdmr sends every `C` request as a
23-byte packet: the command, then zeros. The radio takes the command at its own length and drops the zeros, since no
request starts with a zero byte ([cps-protocol.md](cps-protocol.md) 2.1, `cps.junk`).

| Byte 1 | Arguments | Action |
| --- | --- | --- |
| 0 | | Enter programming mode: show the CPS screen. The radio also stops GPS NMEA output to the port (remembering the GPS mode) and leaves any satellite screen |
| 1 | | Clear the CPS screen buffer |
| 2 | byte 2 x, byte 3 y (pixels), byte 4 font (0 small, 1 small bold, 2 medium, 3 large, 4 extra large), byte 5 alignment (0 left, 1 center, 2 right), byte 6 inverted (0/1), bytes 7.. text (up to 16 characters, NUL terminated or zero filled) | Print one line on the CPS screen buffer |
| 3 | | Render the CPS screen |
| 4 | | Turn on the backlight |
| 5 | | Close: re-read the DMR ID database if one was uploaded, end AMBE compress mode, restore power saving, leave the CPS screen |
| 6 | byte 2 option, further arguments below | Miscellaneous (table below) |
| 7 | | Restore the GPS mode that `C` 0 / `C` 254 suspended |
| 254 | | Ping. Also suspends GPS NMEA output |
| `P` (debug builds only) | ASCII "lat lon" in 1/10000 degree | Set the radio's location |

`C` 6 options:

| Option | Action |
| --- | --- |
| 0 | Save settings and reboot. Apply the channel rewrite handling of 2.2. Save the settings (not the VFOs) and the LUCZ. Wait about 50 ms, then reset 500 ms later. The CPS sends this after writing a codeplug |
| 1 | Reboot (500 ms later), without saving |
| 2 | Save settings and the VFOs into the codeplug image. The CPS and qdmr send this before reading or writing, so the VFOs in the image are current |
| 3 | Flash the green LED (shown while reading) |
| 4 | Flash the red LED (shown while writing) |
| 5 | Enter AMBE compress mode: power saving off, encoder initialized |
| 6 | Reset the sound buffers and clear 960 bytes of the WAV buffer |
| 7 | Set the clock. Bytes 3-6 are Unix time in seconds UTC, LE (4 bytes). On the STM32 radios this also sets the RTC. Satellite predictions are cleared |
| 10 | Wait 10 ms |
| 8, 9, other | Ignored |

RadioFoundry takes `C` 6 0 and `C` 6 1 only in programming mode: after a `C` 0, with no quiet longer than the session time
(10 s) since. Without it the reply is `-` and nothing happens (`cps.reboots_ignored` on the debug link). On a radio
whose debug link shares the CPS's serial port, any three bytes "C 6 1" would otherwise reboot it (bench 2026-10-15).
qdmr sends `C` 0 first (the typical session below).

A typical session, as qdmr does it:
- Read: `C` 0, `C` 1, three `C` 2 lines, `C` 3, `C` 6 3, `C` 6 2, the reads, then `C` 5.
- Write: the same opening with `C` 6 4 instead of `C` 6 3, then `C` 6 2, the writes as prepare/data.../commit for
  each sector, then `C` 5, and finally `C` 6 0 (or `C` 6 1).

## 3. Memory areas (`R` byte 1)

| Area | Name | Address space | Notes |
| --- | --- | --- | --- |
| 1 | Flash | Image address in the external SPI flash (section 7) | STM32 and HR_C7000 radios: the calibration window is served from the working copy, and the VFO overlay applies (2.2). HR_C7000: image addresses are offset by +0x700000 physically (section 7.4) |
| 2 | EEPROM | EEPROM image address, 0x0000..0xFFFF | MK22: the 64 KB I2C EEPROM. STM32 and HR_C7000: emulated in the external flash at flash image 0x00000 (same bytes as area 1 at the same address) |
| 5 | MCU ROM | Offset into the MCU's program flash | STM32: offset from 0x08000000. MK22: absolute address from 0. HR_C7000: offset from 0x03000000, bounds-checked to 1 MB (other builds don't check bounds) |
| 6 | Display buffer | Byte offset into the frame buffer | MK22: 128x64 mono, 1 bit per pixel (1024 bytes, 8-pixel pages). STM32 and HR_C7000: RGB565, 2 bytes per pixel (160x128 on the STM32 radios). Some panels have an 8-row top offset: the first 8 rows read 0xFF (white). Used for screenshots |
| 7 | WAV buffer | Byte offset into the audio / hotspot buffer (2400 bytes on STM32) | Read or write raw PCM (8 kHz, 16 bit) |
| 8 | AMBE compress | ignored | Encodes the PCM in the WAV buffer (960 bytes = 3 voice frames) and returns 27 bytes of AMBE+2, zero-padded to the requested length (the CPS asks for 32). Then clears 960 bytes of the WAV buffer. Used by the CPS to make voice prompts. **The new firmware carries no AMBE codec, so the shim should answer `-`** |
| 9 | Radio info | ignored | Section 4 |
| 10 | Flash security registers | Register address in the SPI flash's OTP security register space | Not on the MK22. Used by the CPS for identification. The shim may answer `-` or return the chip's registers |

A client that reads a radio running the original firmware (the web flasher's backup before an install,
[web-flasher.md](../dev/web-flasher.md)) keeps to what that firmware is known to take:

- Ask at most 2045 bytes per `R`. The reply buffer holds no more.
- Area 5 has no bounds check on the STM32 builds. A read past the end of the MCU flash can fault the radio. Read
  exactly the MCU flash, from its base to its end, as the board data gives them.
- Area 10 may not be answered at all: one STM32 build sent nothing, not even `-`. Don't ask it, or don't wait on it.
- Area 1 at 0x7590-0x75FF is the live VFO overlay (2.2). Read that range in 32-byte requests to get the flash's own
  bytes.
- Send no `C` command when only reading. `C` 6 2 saves settings, and once left a radio's USB hung.
- Long runs of reads hung one radio once. Reads change nothing, so a read that goes unanswered can be asked again, and
  a backup can stop and continue later.

These points come from one radio and this spec, not from the original firmware's source. Not all are confirmed on
every build.

## 4. Radio info (area 9)

The structure is little endian, packed, 46 bytes in struct version 3.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | structVersion: 3 |
| 4 | 4 | radioType (table below) |
| 8 | 16 | Git revision, ASCII, NUL padded |
| 24 | 16 | Build date and time, ASCII `YYYYMMDDhhmmss`, NUL padded |
| 40 | 4 | Flash chip JEDEC capacity / part id: 0x4014 for an 8 Mbit (GD-77), 0x4018 for a 128 Mbit (STM32 radios), 0x4017 for a 64 Mbit (modified GD-77s) |
| 44 | 2 | Features, bit field: bit 0 inverted (night) display; bit 1 the DMR ID database extends into the voice prompt area (header format byte `n`, section 7.5); bit 2 voice prompts are loaded |

| radioType | Radio | qdmr's driver |
| --- | --- | --- |
| 0 | GD-77 | qdmr (MK22 driver) |
| 1 | GD-77S | qdmr (MK22 driver) |
| 2 | MK22 radio (DM-860 family) | qdmr (MK22 driver) |
| 3 | RD-5R / DM-5R Tier2 | qdmr (MK22 driver) |
| 4 | MK22 radio (single band) | qdmr (MK22 driver) |
| 5 | a dual-band mobile radio on the STM32 and HR_C6000 pair | qdmr (UV380 driver) |
| 6 | UV380 family. Also what a board reports when it sets no radio type, such as the HR_C7000 boards | qdmr (UV380 driver) |
| 7 | the HR_C5000 radio and its sibling model | qdmr (UV380 driver) |
| 8 | a UV380-class radio with a BGR panel | qdmr (UV380 driver) |
| 9 | MD-2017 / RT-82 | qdmr (UV380 driver) |
| 10 | the same radio as 8 with an RGB panel (the type follows the detected panel color order) | qdmr (UV380 driver) |
| 11 | an HR_C7000 radio (known to the open-firmware CPS since E2026.07.13) | **not known to qdmr**: it fails with an "Unknown ... variant" error |

qdmr chooses its "extended callsign DB" layout when features bit 1 is set and bit 2 is clear.

## 5. `D`: DMR data service host commands (ours)

This is our protocol: it has no open-firmware counterpart and no other users yet. It is available in programming mode on
the serial port, and on the STM32 radios also over UDP 3334 in network mode. It doesn't run in hotspot mode.

Each request is one USB packet (64 bytes at most) or one UART frame: `D`, the sub command, then the arguments. Every
reply starts with `D`, the sub command, and a result byte: 1 means OK; 0 means failed, busy or nothing available.

| Sub | Request arguments | Reply after the result byte |
| --- | --- | --- |
| 1 Clear | none | none. Fails while a transmission is in progress |
| 2 Append bytes | len (1 byte), then len bytes (up to 61) | none. Appends to a 300 byte message buffer; fails when full |
| 3 Append burst | data type (1), flags (1: bit 0 = the payload already holds its CRC / mask, send it verbatim), len (1, at most 24), payload | none. Up to 64 bursts; fails while busy |
| 4 Send | kind (1), flags (1), destination (3 bytes BE), port (2 bytes BE; for kind 3: byte 0 = DPF, low 4 bits of byte 1 = SAP) | none. Result 1 means queued |
| 5 Status | none | The result byte is the TX status (0 idle, 1 running, 2 done, 3 failed); then the received bursts waiting (1 byte) and the inbox messages not yet read by the host (1 byte) |
| 6 Pop RX burst | none | data type, flags (bit 1 = the receiver reported a CRC error), len, payload (oldest first; the queue holds the last 8) |
| 7 Pop message | none | source ID (3 BE), destination ID (3 BE), group flag (1), text length (1), text (Latin-1). The oldest message the host hasn't read. The inbox holds 4 |
| 8 Network mode | 1 on, 0 off, 0xFF query | saved mode (1 byte), network link up (1 byte). Saved with the other settings (for example by `C` 6 0). Takes effect at the next boot |

Send kinds:

| Kind | Meaning |
| --- | --- |
| 0 | Send the appended bursts as they are, at whatever rate they were built |
| 1 | TMS text message from the appended bytes (Latin-1 text) |
| 2 | UDP datagram from the appended bytes, to and from the given port |
| 3 | Unconfirmed packet from the appended bytes, with the DPF and SAP given in the port bytes |

Send flags:
- bit 0: group destination (a talkgroup, not an individual).
- bit 1: request a TMS acknowledgement (kind 1).
- bits 2-3: block rate for kinds 1-3 (0 Rate 1/2, 1 Rate 3/4, 2 Rate 1). The value 3 is refused.

Data types are the ETSI TS 102 361-1 values: 0 PI header, 1 voice LC header, 2 terminator with LC, 3 CSBK, 4 MBC
header, 5 MBC continuation, 6 data header, 7 Rate 1/2 data, 8 Rate 3/4 data, 9 idle, 10 Rate 1 data.

## 6. Hotspot mode switch (0xE0)

The MMDVM protocol itself is public and is specified separately. The switch works as follows:

- **Entering.** In programming mode, a request whose first byte is 0xE0 and that is at least 3 bytes long switches
  the radio to hotspot mode, if a hotspot type is configured (MMDVM or BlueDV) and DMR isn't disabled. There is no
  CPS reply. The request is queued as the first MMDVM frame. The radio also stops GPS NMEA output, so MMDVMHost
  doesn't see stray text.
- **USB (STM32, MK22).** From then on, every USB packet starting with 0xE0 and at least 3 bytes long is taken as one
  MMDVM frame. The radio does not parse the MMDVM length byte. A packet that doesn't fit in the frame ring is dropped
  and counted.
- **UART (radios without USB).** Bytes are split into frames by the MMDVM length byte, resynchronizing on 0xE0. MMDVMHost may
  send frames back to back, so gaps can't be used. Bytes that came with the first request are split the same way.
- **UDP 3334 (network mode).** A datagram is treated like a USB packet.
- **Leaving.** In MMDVMHost mode the radio leaves after 20 s without traffic from the host. In BlueDV mode the user
  leaves. The radio then returns to programming mode. While in hotspot mode, `R`, `W`, `X`, `C` and `D` are not
  answered.

## 7. The codeplug image

The PC tools see two images, "EEPROM" and "flash", in the GD-77's terms. The GD-77 has a 64 KB I2C EEPROM and a 1 MB
SPI flash. The STM32 radios and the HR_C7000 radios have no EEPROM: both images live in one large SPI flash. The tools address
it with the per-family offsets below. A new firmware can store the data any way it likes, as long as the shim
presents these addresses.

### 7.1 Per-family address mapping

| Family | EEPROM image | Flash image (codeplug regions, DMR ID DB, voice prompts) | Wire commands used for the EEPROM image |
| --- | --- | --- | --- |
| MK22 (GD-77 etc.) | Real EEPROM, address = EEPROM image address | Flash address = flash image address (offset 0) | `R` 2 / `W` 4 |
| STM32 (radio types 5-10) | Flash address = EEPROM image address (offset 0; this occupies flash 0x00000..0x0FFFF) | Flash address = flash image address + 0x20000 (the "128 KB offset") | `R` 1 or 2 / `X` 1-3 (flash sector writes). qdmr's OpenUV380 driver puts everything in its flash bank |
| HR_C7000 | As STM32 at the physical offset EEPROM image + 0x700000 (7.4) | flash image + 0x20000 + 0x700000, the STM32 mapping | `R` 1 or 2 / `X` 1-3 |

The rest of this section gives flash image addresses without the offset. For the STM32 family, add 0x20000 to get
the address on the wire.

Flash regions that qdmr transfers:

| Region | MK22 wire address | STM32 wire address | Size |
| --- | --- | --- | --- |
| EEPROM image part 1 | EEPROM 0x00080 | flash 0x00080 | 0x5FE0 (to 0x6060) |
| EEPROM image part 2 | EEPROM 0x07500 | flash 0x07500 | 0x3B00 (to 0xB000) |
| Custom data ("additional settings") | flash 0x00000 | flash 0x20000 | 0x11A0 used by qdmr (boot melody plus satellites) |
| Flash codeplug | flash 0x7B000 | flash 0x9B000 | 0x13E60 (to 0x8EE60 / 0xAEE60) |
| DMR ID DB | flash 0x30000 (+ 0xB8000) | flash 0x50000 (+ 0xD8000) | section 7.5 |

EEPROM image part 1 ends at 0x6060. It therefore includes the first 0x15 bytes of the radio's own settings block at
0x604B (7.2). A tool that writes back what it read restores those bytes unchanged. A shim must accept writes to that
range without letting them corrupt the live settings. It can ignore the range, or validate it against the magic.

### 7.2 EEPROM image map

| Address | Size | Content |
| --- | --- | --- |
| 0x0000 | 0x80 | Unused by the firmware |
| 0x0080 | 0x60 | Device info (7.3.1) |
| 0x00E0 | 0x28 | General settings: callsign at 0x00E0 (8 bytes), DMR ID at 0x00E8 (7.3.2) |
| 0x1400 | 0x78 | DTMF signaling settings (7.3.9). The DTMF durations are at 0x1472 (5 bytes) |
| 0x1588 | 8 x 0x40 | APRS configurations (7.3.8), to 0x1788 |
| 0x2F88 | 63 x 0x20 | DTMF contacts (7.3.7), to 0x3768 |
| 0x3780 | 0x10 + 128 x 0x38 | Channel bank 0: a 16 byte in-use bitmap, then channels 1-128 at 0x3790 (to 0x538F) |
| 0x6000 | 4 + 70 | LUCZ, last used channel in each zone (7.3.10) |
| 0x604B | 0x70-0x80 | The radio's own settings block. Not part of the CPS codeplug (7.7) |
| 0x7500 | | Start of "CPS section 2" |
| 0x7518 | 1 | Boot screen type: 0x01 text, 0x00 picture. This byte is also the start of the 7 byte boot PIN block read from 0x7518 |
| 0x7519 | 1 | Bit 0: boot password (PIN) enabled |
| 0x751C | 3 | Boot PIN as BCD digits, high nibble first. A nibble of 0xF ends the PIN. Up to 6 digits |
| 0x7524 | 10 x 2 | Quick-key slots (LE). The CPS's function ids there aren't documented here. RadioFoundry uses the slots for its key assignment: slot 0 the marker 0x4B02, slots 1-9 one per assignable key, the press's `keyfn_t` in the low byte and the long press's in the high byte (`core/input/keyassign.h`, `ui.md` 3.1). Layout 1 (marker 0x4B01, a slot each for the press and long press of three keys) is read and rewritten as layout 2 on the first change |
| 0x7540 | 16 | Boot text line 1 |
| 0x7550 | 16 | Boot text line 2 |
| 0x7590 | 0x38 | VFO A, channel record format (7.3.3) |
| 0x75C8 | 0x38 | VFO B |
| 0x8000 | 0x10 | Reserved ("extended zone basic"), not read by the firmware |
| 0x8010 | 0x20 | Zone in-use bitmap: bit j of byte i means zone slot (i x 8 + j) is in use. 68 slots are used |
| 0x8030 | 68 x 0xB0 | Zone records (7.3.4), 80 channels each, to 0xAEF0. Open-firmware 16-channel layout: 68 x 0x30 |

Zone layout detection: the firmware reads the 16 bytes at 0x8060 and looks at the last one (0x806F). In the 16-channel
layout, 0x806F is the last byte of the second zone's name, so it is 0xFF. In the 80-channel layout, it is the high byte
of a channel number (at most 0x04). A value 0x00-0x04 therefore selects 80 channels per zone. Current tools always
write the 80-channel layout.

### 7.3 Records

#### 7.3.1 Device info (EEPROM 0x0080, 96 bytes)

| Offset | Size | Field |
| --- | --- | --- |
| 0x00 | 2 | Minimum UHF frequency |
| 0x02 | 2 | Maximum UHF frequency |
| 0x04 | 2 | Minimum VHF frequency |
| 0x06 | 2 | Maximum VHF frequency |
| 0x08 | 6 | Last programming time |
| 0x0E | 2 | Reserved |
| 0x10 | 8 | Model |
| 0x18 | 16 | Serial number |
| 0x28 | 8 | CPS software version |
| 0x30 | 8 | Hardware version |
| 0x38 | 8 | Firmware version |
| 0x40 | 24 | DSP firmware version |
| 0x58 | 8 | Reserved |

The firmware treats all of these as informational (inherited from the vendor layout). The shim should store and return
them unchanged.

#### 7.3.2 General settings (EEPROM 0x00E0, 40 bytes)

| Offset | Size | Field |
| --- | --- | --- |
| 0x00 | 8 | Callsign / radio name, ASCII, 0xFF padded |
| 0x08 | 4 | The radio's DMR ID: 8 BCD digits stored big endian (bytes in reading order, for example ID 2345678 is 02 34 56 78) |
| 0x0C | 1 | Codeplug version byte, written by the CPS. Not used by the firmware |
| 0x0D | 3 | Reserved |
| 0x10 | 1 each | ARS init delay, TX preamble duration, monitor type, VOX sensitivity, RX low battery, call alert duration, response timer, reminder timer, group hang, private hang (0x10..0x19) |
| 0x1A | 4 | flag1..flag4 |
| 0x1E | 2 | Reserved |
| 0x20 | 8 | Programming password |

Only the callsign and the DMR ID matter to the firmware. The rest is vendor layout and should be kept.
The group and private hang bytes (0x18, 0x19) have no unit here. Our firmware reads them as the reply window,
assuming 500 ms steps (`codeplug.c` `GEN_HANG_UNIT_MS`, `dmr-voice.md` 7); that unit is not confirmed.

#### 7.3.3 Channel (56 bytes; EEPROM 0x3790 + 56 x (n-1) for n = 1..128, flash banks for 129..1024; VFOs too)

| Offset | Size | Field |
| --- | --- | --- |
| 0x00 | 16 | Name, 0xFF padded. A record is empty when its in-use bit is clear |
| 0x10 | 4 | RX frequency: BCD of the frequency in 10 Hz units, stored as a LE 32-bit word (438.500 MHz = 43850000 -> 0x43850000 -> bytes 00 00 85 43) |
| 0x14 | 4 | TX frequency, same encoding |
| 0x18 | 1 | Mode: 0 FM, 1 DMR |
| 0x19 | 1 | Power: 0 = use the radio's master power; n = power level n-1. qdmr maps 1 min, 3 low, 6 mid, 9 high, 10 max; the levels themselves are per radio. Radios with 9 or more levels read n as level n-1; radios with fewer spread the scale over their levels (2 levels: 1-5 low, 6-10 high) |
| 0x1A | 1 | Fixed location latitude, low byte |
| 0x1B | 1 | TX timeout in 15 s units; 0 = the radio's own TX time-out (the TOT setting), which zone sharing's imported channels use |
| 0x1C | 2 | Latitude, middle and high bytes |
| 0x1E | 2 | Longitude, low and middle bytes |
| 0x20 | 2 | RX tone (LE): 0xFFFF or 0 = none. CTCSS: BCD of tenths of Hz (0x0885 = 88.5 Hz), bits 14-15 clear. DCS: bit 15 set, bit 14 set = inverted, the low bits are the code's octal digits written as hex digits (D023 = 0x8023, D023I = 0xC023) |
| 0x22 | 2 | TX tone, same encoding |
| 0x24 | 1 | Longitude, high byte |
| 0x25 | 1 | SFR byte A (ours; was unused, see below) |
| 0x26 | 1 | flag byte 1 (bits below) |
| 0x27 | 3 | Per-channel DMR ID override, 24-bit BE, valid when flag 1 bit 7 is set. When unset, the CPS writes 00 16 00 (the vendor defaults of the fields these bytes replaced) |
| 0x2A | 1 | SFR byte B (ours; was unused, see below) |
| 0x2B | 1 | RX group list (TG list) index, 1-based, 0 = none |
| 0x2C | 1 | Color code 0-15 |
| 0x2D | 1 | APRS configuration index, 1-based, 0 = none |
| 0x2E | 2 | TX contact index, LE, 1-based, 0 = none |
| 0x30 | 1 | flag1: bits 0-1 talker alias TX on TS1, bits 2-3 on TS2 (0 off, 1 APRS, 2 text, 3 both) |
| 0x31 | 1 | flag2: bit 6 = timeslot 2 (clear = TS1) |
| 0x32 | 1 | flag3: bits 6-7 = 0b11 STE, bit 5 non-STE.  |
| 0x33 | 1 | flag4: bit 7 power (vendor; superseded by 0x19), bit 6 VOX, bit 5 zone scan skip, bit 4 all-channel scan skip, bit 3 talkaround (unused), bit 2 RX only, bit 1 bandwidth 25 kHz (clear = 12.5 kHz), bit 0 squelch (vendor) |
| 0x34 | 2 | VFO offset frequency (vendor; qdmr uses it as the VFO TX offset) |
| 0x36 | 1 | VFO flags: bits 4-7 step index (2.5, 5, 6.25, 10, 12.5, 25, 30, 50 kHz); bits 2-3 offset mode (qdmr) |
| 0x37 | 1 | Squelch: 0 = the radio's default, 1-21 = level. Values above 21 are read as 10 |

Location: each coordinate is 24 bits, assembled from its high, middle and low bytes. Bit 23 is the sign (1 = south or
west). Bits 22-15 are whole degrees. Bits 14-0 are the fraction in 1/10000 degree (0..9999). The location is used for
distance sorting and the like when flag 1 bit 3 is set.

Flag 1 (offset 0x26):

| Bit | Meaning |
| --- | --- |
| 7 | Per-channel DMR ID override present (0x27..0x29) |
| 6 | No beep |
| 5 | No power saving on this channel |
| 4 | Out of band (set at run time on boards that have it, never saved) |
| 3 | Use the fixed location |
| 2 | Force DMO (direct mode on a duplex channel; qdmr calls it "simplex") |
| 1 | SFR enabled for this channel (ours) |
| 0 | Roaming (ours reads it: in a roaming zone, flagged channels narrow the sites to themselves; `dmr-roaming.md` 5.2) |

SFR bytes (ours; the CPS doesn't know them and leaves them as read; they are only read when flag 1 bit 1 is set):

| Byte | Bits | Meaning |
| --- | --- | --- |
| A (0x25) | 2-0 | Role: 0 off, 1 user of a standard SFR, 2 user of a Motorola ERDM repeater, 3 this radio is a standard SFR repeater, 4 this radio is an ERDM-style repeater |
| A | 4-3 | Outbound framing: 0 auto (BS for the Motorola role, else TS), 1 MS sync, 2 TS1/TS2 sync, 3 BS sync |
| A | 5 | Inbound slot: 1 = TS2, 0 = TS1. The outbound slot is the other one |
| A | 7-6 | Hang time index: 1 s, 2 s, 4 s, none |
| B (0x2A) | 7 | Outbound color code set |
| B | 3-0 | That color code (else the channel's) |

The repeater's timeout is the channel TOT, or 180 s when the TOT is 0.

Channel banks in the flash image: banks 1-7 (channels 129-1024) start at flash image 0x7B1B0. Each bank is a 16 byte
in-use bitmap followed by 128 channels of 56 bytes (0x1C10 bytes per bank), so channel n (n >= 129) is at
0x7B1C0 + 16 x ((n-129) / 128) + 56 x (n-129). Bitmaps: bit (k % 8) of byte k / 8 marks the bank's channel k (0-based)
in use. The flash image from 0x7B000 to 0x7B1AF is unused by the firmware; keep it as read.

#### 7.3.4 Zone (EEPROM 0x8030 + 0xB0 x slot)

| Offset | Size | Field |
| --- | --- | --- |
| 0x00 | 16 | Name, 0xFF padded |
| 0x10 | 80 x 2 | Channel numbers (LE, 1-based). The first 0 ends the list |

The firmware counts zones by walking the in-use bitmap. Slots need not be packed. The radio adds a virtual "All
Channels" zone after the real ones.

#### 7.3.5 Contact (flash image 0x87620 + 24 x (n-1), n = 1..1024)

| Offset | Size | Field |
| --- | --- | --- |
| 0x00 | 16 | Name, 0xFF padded. 0xFF in the first byte means empty |
| 0x10 | 4 | Talkgroup or ID: 8 BCD digits stored big endian (as the DMR ID in 7.3.2) |
| 0x14 | 1 | Call type: 0 group, 1 private, 2 all call. 0xFF means empty |
| 0x15 | 1 | Call receive tone (vendor) |
| 0x16 | 1 | Ring style (vendor) |
| 0x17 | 1 | Timeslot override: bit 0 set = no override; otherwise bit 1 is the timeslot minus 1. 0xFF = no override |

#### 7.3.6 RX group lists (TG lists)

| Address | Size | Content |
| --- | --- | --- |
| Flash image 0x8D620 | 76 bytes, padded to 0x80 | Per list: 0 = unused, otherwise the member count plus 1 |
| Flash image 0x8D6A0 + 80 x (n-1) | 80 bytes each, 76 lists, to 0x8EE60 | Name 16 bytes (0xFF padded), then 32 contact indices (LE, 1-based; the first 0 ends the list) |

#### 7.3.7 DTMF contact (EEPROM 0x2F88 + 32 x (n-1), n = 1..63)

The record is a name (16 bytes, 0xFF padded; a first byte of 0xFF or 0x00 means empty) followed by a code of 16 bytes:
one digit per byte (0x00-0x09 the digits, 0x0A-0x0D A-D, 0x0E `*`, 0x0F `#`), ended by 0xFF. qdmr assumes 64 contacts,
but a 64th would overlap the channel bank 0 bitmap at 0x3780. Use 63.

#### 7.3.8 APRS configuration (EEPROM 0x1588 + 64 x (n-1), n = 1..8)

| Offset | Size | Field |
| --- | --- | --- |
| 0x00 | 8 | Name (0xFF in the first byte means empty) |
| 0x08 | 1 | Sender SSID |
| 0x09 | 3 | Fixed latitude, 24-bit (as for channels) |
| 0x0C | 3 | Fixed longitude |
| 0x0F | 6 + 1 | Path 1: callsign, SSID |
| 0x16 | 6 + 1 | Path 2: callsign, SSID |
| 0x1D | 1 | Symbol table |
| 0x1E | 1 | Symbol index |
| 0x1F | 24 | Comment (qdmr uses 23 characters) |
| 0x37 | 4 | TX frequency (unaligned, LE) |
| 0x3B | 2 | Reserved |
| 0x3D | 1 | Flags: bit 0 300 baud (else 1200); bit 1 use the fixed position; bit 2 transmit QSY info; bit 3 beacon silently; bits 5-7 position ambiguity |
| 0x3E | 2 | Magic / version, LE: 0x4153 now. 0x4152 was the older layout, whose ambiguity field sat one bit lower; RadioFoundry accepts both and reads the ambiguity field at the matching position |

#### 7.3.9 DTMF signaling (EEPROM 0x1400, 120 bytes)

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | Self ID |
| 8 | 16 | Kill code |
| 24 | 16 | Wake code |
| 40 | 1 each | Delimiter, group code, decode response, auto reset timer, flag1 (40..44) |
| 45 | 3 | Reserved |
| 48 | 30 | PTT ID up code |
| 78 | 2 | Reserved |
| 80 | 30 | PTT ID down code |
| 110 | 2 | Reserved |
| 112 | 1 | Response hold time |
| 113 | 1 | Decode time |
| 114 (0x72) | 1 | First digit delay, in 100 ms units |
| 115 | 1 | First digit duration, in 100 ms units |
| 116 | 1 | `*` and `#` duration, in 100 ms units |
| 117 | 1 | Rate, 1-10 digits per second |
| 118 | 1 | TX tail, in 100 ms units |
| 119 | 1 | Reserved |

The firmware uses only the five duration bytes at 114-118. qdmr reads this block starting at 0x1470.

#### 7.3.10 LUCZ (EEPROM 0x6000)

| Offset | Size | Content |
| --- | --- | --- |
| 0 | 4 | ASCII `LUCZ`. Without it, every zone starts at its first channel |
| 4 + z | 1 | For real zone slot z = 0..67: the index within that zone (0-based) |
| 4 + 68 | 2 | All Channels: channel number minus 1, LE |



### 7.4 Custom data, calibration and the data flash offset

Flash image 0x00000 holds the custom data area:

| Offset | Size | Content |
| --- | --- | --- |
| 0 | 8 | the 8 ASCII bytes `4F 70 65 6E 47 44 37 37`. If missing, there is no custom data |
| 8 | 4 | Version (LE) |
| 12 | | Blocks, each a header of type (4 bytes LE) and length (4 bytes LE), followed by the data. The walk ends at an empty header (type or length 0xFFFFFFFF) or a length of 0, and never goes past 0x10000 |

| Type | Content |
| --- | --- |
| 1 | Boot image. On mono radios a 128x64 1-bit bitmap (the RD-5R uses fewer rows) |
| 2 | Boot melody. qdmr writes 0x208 bytes: magic 2, then 256 notes of 2 bytes (pitch index, duration) |
| 3 | Satellite TLE set. qdmr writes 0x9E0 bytes: magic 3, then 25 records of 0x64 bytes (name, epoch, orbit elements as packed BCD, FM and APRS up/downlinks, CTCSS, beacon, APRS path) |
| 4 | Day theme colors (color radios) |
| 5 | Night theme colors |

A block's length can't change once written; the firmware rewrites a block only when the length is the same.

Calibration: on the STM32 and HR_C7000 radios, the window at flash image 0x10000..0x101FF is the working calibration
copy (2.2). This address is physical, not offset. On the GD-77, calibration is at 0xF000 (working copy) and 0x8F000
(factory; 0x6F000 on type 2), read raw.

HR_C7000 specifics:
- **Physical offset.** Every flash address the CPS uses (the EEPROM image, the flash image, the DMR ID DB, voice
  prompts) is placed at +0x700000 in the 16 MB data flash (`board.openfw_eeprom` 0x700000, `board.openfw_flash`
  0x720000, end 0xF00000). Below that is the radio's own data (settings, contacts, fonts, voice prompts,
  recordings, factory calibration), which is never touched. The window therefore covers 9 MB (to 0xFFFFFF).
- **Collision with the radio's own firmware.** The window sits inside a region (0x6DC000..0xFFFFFF) that the original
  firmware may use for recording and extended contacts. Going back to the original firmware and recording can
  overwrite the codeplug.
- **Flash image offset.** The build uses the STM32 mapping: the EEPROM image at 0x700000 and the flash image at
  +0x20000 from it, so the custom data area and the EEPROM image don't overlap.

### 7.5 DMR ID database

| Family | Header and area 1 | Area 2 |
| --- | --- | --- |
| MK22 | Flash 0x30000, 256 KB | Flash 0xB8000 (0x48000 bytes) |
| STM32 | Flash 0x50000 | Flash 0xD8000. qdmr allows up to 0xD28000 bytes, to the 14 MB mark |
| Format `n` | | Area 2 starts at the voice prompt header address instead, overwriting the prompts (features bit 1) |

Header (12 bytes):

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 2 | ASCII `Id`. Anything else means no database. The old `ID` tag is rejected |
| 2 | 1 | Format. `N` (78): compressed text with 3-byte binary IDs. `n`: the same, with area 2 in the voice prompt area. Any other value: 4-byte BCD IDs with plain text. qdmr's "uncompressed" format is 45 (`-`) |
| 3 | 1 | Record size + 0x4A |
| 4 | 4 | Version, ASCII (qdmr writes `001`) |
| 8 | 4 | Record count, LE |

Records follow the header in area 1, sorted by ascending ID, with no gaps. Area 1 holds as many whole records as fit
in 256 KB minus the header. The rest continue at the start of area 2. Each record is:
- An ID: 3 bytes LE binary (formats `N` and `n`) or 4 bytes BCD.
- The text. With `N` and `n`, the text is packed at 6 bits per character, 4 characters in 3 bytes, most
  significant first. The 64-character alphabet is, in order: space, `0`-`9`, `A`-`Z`, `a`-`z`, `.` (codes 0-63).
  Trailing spaces are trimmed after unpacking. The text is "callsign name [city state country]" (qdmr's
  convention). The record size is 3 + 3/4 of the text length. qdmr's default text length is 16 characters (15 byte
  records) on the GD-77 and 32 (27 byte records) on the UV380 family.

The radio looks IDs up by binary search, using a cache of 14 evenly spaced IDs once there are more than 40 records.

RadioFoundry also reads format `C` (header bytes 2-3 `C`, 0): a dictionary-coded database about half the size of
`N`, laid out so a refresh changes few 4 KB blocks ([iddb-compact.md](iddb-compact.md)). Byte 3 is 0, so firmware
that doesn't read it sees no database.

### 7.6 Voice prompts file

| Family | Header address (flash image) | Old address, still read | Version | TOC entries |
| --- | --- | --- | --- | --- |
| Color radios (STM32, HR_C7000) | 0x8F400 (+0x20000) | 0xE0000 (+0x20000) | 9 | 368 |
| Mono radios (MK22) | 0x8F400 | 0xE0000 | 8 | 331 |

Layout:

| Offset | Size | Content |
| --- | --- | --- |
| 0 | 4 | Magic 0x5056 (`VP`), LE |
| 4 | 4 | Version (table above) |
| 8 | 4 x N | Table of contents: prompt i plays the bytes from TOC[i] to TOC[i+1], as offsets from the end of the TOC. A zero entry means the prompt is absent (silence is played instead) |
| 8 + 4N | | AMBE+2 data in 27-byte units (three 9-byte voice frames) |

Prompt numbering, fixed part:
- 0 silence, 1 "point", 2-11 digits 0-9, 12-37 letters A-Z, 38 an alternate Z.
- 39 onward: channel, contact, milliseconds, megahertz, kilohertz, talkgroup, degrees, VFO, seconds, minutes, volts,
  watt, watts, milliwatts, percent, receive, transmit, mode, DMR, FM, plus, minus, the three VFO exchange and copy
  phrases, power, channel mode, scan mode, timeslot mode, filter mode, zone mode, power mode, color code mode,
  hertz, settings update, star, hash, VOX, sweep scan mode, eco mode, duration, reverse repeater, distance, km/h,
  kilometers, then three unused slots.
- That gives 88 fixed prompts. Entries from 88 onward follow the order of the language strings of the firmware the CPS targets; the
  mono builds skip the theme strings.

The voice prompt files are made with the AMBE codec, and the new firmware carries no codec. Whether the new firmware
reads this file at all is a product decision. If it does, the language-string index list has to be supplied as interop data, which is an open question.

### 7.7 The radio's own settings block

At EEPROM image 0x604B, right after the LUCZ, the firmware keeps its own non-volatile settings. This block is not a
CPS codeplug structure, and the CPS doesn't interpret it. The first field is a 32-bit LE magic, which changes whenever
the layout changes; on a mismatch the radio resets the block to defaults.

| Family | Magic | Size |
| --- | --- | --- |
| STM32 base | 0x4780 | 0x74 |
| STM32 with the DMR data settings | 0x4781 | 0x80 |
| MK22 base | 0x4784 (0x4785 with GPS logging) | 112 |
| HR_C7000 | 0x477C | |


The new firmware should keep its settings elsewhere, and treat bytes 0x604B..0x605F of a codeplug write as noise
(7.1).

## 8. Byte order summary

| Data | Encoding |
| --- | --- |
| Request and reply headers (address, length) | BE |
| Radio info, zone and group member lists, contact indices, quick keys, LUCZ All Channels entry, custom data headers, DMR ID DB count, voice prompt header and TOC | LE |
| Channel frequencies | BCD in 10 Hz units, as a LE 32-bit word |
| CTCSS | BCD in tenths of Hz, as a LE 16-bit word. DCS: flags in bits 14-15, octal digits as hex |
| Contact numbers and the radio's DMR ID | BCD, big-endian byte order |
| Per-channel DMR ID override | 24-bit binary, BE |
| DMR ID DB IDs | 24-bit binary LE (formats `N` and `n`) or 32-bit BCD |
| Strings | ASCII, 0xFF padded (CPS text command: NUL terminated) |

## 9. Version differences to handle

- **structVersion.** Version 3 (46 bytes, with features). qdmr reads version 1 (44 bytes, no features field) as well.
- **radioType 11 (HR_C7000).** Known to the open-firmware CPS since E2026.07.13, unknown to qdmr (checked against its current source). RadioFoundry reports 6 on HR_C7000 boards, which qdmr accepts because the image mapping matches (7.4); qdmr then shows the UV380 model.
- **Panel types 8 and 10.** The type follows the panel's color order. The CPS needs it only for screenshots.
  qdmr treats the two alike.
- **APRS magic.** 0x4153 now, 0x4152 in older records (7.3.8).
- **Voice prompt versions.** 8 for mono radios, 9 for color radios. A version mismatch makes the radio fall back to
  beeps.
- **Our additions.** The SFR bytes and flag 1 bit 1, the `D` commands, and the network mode USB identities
  (PID 0x0095 / 0x0096) don't exist in R20260131. They are invisible to the CPS, which round-trips the bytes it read.

## 10. Open questions

1. Which flash image offset does the open-firmware CPS use for radio type 11? RadioFoundry reports type 6 and uses the STM32 mapping (7.4), which works with qdmr.
2. Which radio type the HR_C7000 boards should report: 11 for the open-firmware CPS, or an STM32 type (today 6) so that qdmr works, or a qdmr change to add type 11.
3. Exactly which EEPROM and flash ranges the closed open-firmware CPS reads and writes, beyond the qdmr ranges above
   (themes, boot image, calibration, security registers). This needs a USB capture of a full CPS read and write.
4. Whether to support voice prompt files (they need the language-string index list and AMBE playback).
5. Whether the shim answers area 8 (AMBE compress) and area 10 (security registers) at all, or always with `-`.
