<!-- SPDX-License-Identifier: MIT -->

# The MMDVM modem protocol, as a DMR-only radio must speak it

What a radio acting as an MMDVM modem (a "hotspot") must send and accept so that every host that will talk to it
works: MMDVMHost (current master), RadioDesk `rfgw`, our bench tools (`tools/bench/`), and the hosts the old firmware
was tested with. Written from the public protocol, from observing what those hosts send and expect, and from our own
earlier hotspot work. Section 6 lists our own additions (the SFR debug command 0xD5 and the extra status bytes, and
the split duplex command 0xD6).
Section 7 is the test list for the host tests and `tools/mmdvm_probe.py`.

Conventions: byte values are hex. "Frame offset" counts from the 0xE0 start byte (offset 0). "Payload offset" counts
from the first byte after the command byte (payload offset 0 = frame offset 3). Frequencies are in Hz, little endian.
"Host" is the PC side, "modem" is the radio.

Related specs: `gotchas.md` section 8 (lessons from the old hotspot), `legacy-cps-interop.md` section 6 (how the
programming port switches to hotspot mode), `dmr-modem.md` (the burst layer under all of this), `capabilities.md`
("Hotspot and repeater").

## 1. Framing

| Item | Rule |
| --- | --- |
| Start byte | 0xE0. Anything that is not 0xE0 where a frame should start is skipped, one byte at a time, until 0xE0. |
| Length | One byte at frame offset 1: the length of the whole frame, start byte included. The shortest frame is 3 bytes (E0 03 cmd). |
| Command | Frame offset 2. |
| Payload | Frame offset 3 up to the length. |
| Long frames | Length byte 0 means a long frame: the real length is (frame offset 2) + 255, and the command moves to frame offset 3, the payload to offset 4. Total lengths 256 to 510. Only FM audio uses them; a DMR-only modem never sends one, but its parser must skip a whole long frame (consume LL + 255 bytes) and stay in sync. A frame of exactly 255 bytes uses the short form (length FF). |
| Checksum | None. No escaping, no byte stuffing. 0xE0 can appear inside a payload; resynchronizing on 0xE0 is only for after a bad length. |
| Byte order | Multi-byte numbers are little endian (frequencies), except the RSSI and DEBUG values, which are big endian. |
| Transport | A byte stream: USB CDC ACM (baud rate ignored; MMDVMHost and rfgw open it at 115200 8N1), a UART at 115200 8N1, or UDP (section 5.3). Frames can be split across USB packets or arrive several to a packet; never assume one frame per packet. |

Frames the modem sends must be complete and in order; two frames must never interleave (a status reply written in
the middle of a DMR frame corrupts both). The host parses one frame at a time and drops nothing it can't parse except
up to the next 0xE0.

## 2. Commands and replies for a DMR-only modem

### 2.1 Summary

| Cmd | Name | Direction | Reply from the modem | MMDVMHost needs it |
| --- | --- | --- | --- | --- |
| 00 | GET_VERSION | host to modem; modem's reply has the same command | the version frame | yes (startup fails without it) |
| 01 | GET_STATUS | host to modem; reply same command | the status frame | yes (2 s without a reply resets the modem) |
| 02 | SET_CONFIG | host to modem | ACK or NAK | yes (a NAK at startup is fatal) |
| 03 | SET_MODE | host to modem | ACK or NAK (MMDVMHost doesn't wait; rfgw does) | yes |
| 04 | SET_FREQ | host to modem | ACK or NAK | yes (a NAK at startup is fatal) |
| 08 | CAL_DATA | host to modem (calibration tools) | NAK (not supported) | no |
| 09 | RSSI_DATA | (calibration) | NAK | no |
| 0A | SEND_CWID | host to modem | ACK when accepted (rfgw waits for it), NAK 5 when not idle | only with CW ID enabled |
| 17 | DMR_ALOHA | host to modem, trunking only | NAK 2 or silence | no |
| 18 | DMR_DATA1 | both | none on success, NAK on refusal | duplex / SFR only |
| 19 | DMR_LOST1 | modem to host | (none) | duplex / SFR only |
| 1A | DMR_DATA2 | both | none or ACK on success, NAK on refusal | yes |
| 1B | DMR_LOST2 | modem to host | (none) | yes |
| 1C | DMR_SHORTLC | host to modem | none, or ACK; never NAK in simplex | sent on every call, simplex too |
| 1D | DMR_START | host to modem, duplex only | none or ACK | no (simplex) |
| 1E | DMR_ABORT | host to modem, duplex / rfgw SFR | none or ACK | no (simplex) |
| 10-13, 20-21, 30-32, 40-41, 45-49, 50 | other modes' data | host to modem | NAK 2 (not enabled) | no |
| 60-63 | FM_PARAMS1-4 | host to modem, only if FM is configured | ACK (ignored) | only if FM is configured |
| 70 | ACK | modem to host | | |
| 7F | NAK | modem to host | | |
| 80 | SERIAL | both (Nextion passthrough) | silently dropped | no |
| 90 | TRANSPARENT | both | silently dropped | no |
| 91 | QSO_INFO | host to modem (old hosts, CAST display) | silently dropped, or ACK | no |
| D5 | our SFR debug command | host to modem | ACK / NAK 4 (section 6) | no (ours) |
| D6 | our split duplex command (CONFIG_HOTSPOT_SPLIT builds only) | host to modem | one D6 reply with a result byte (section 6.5) | no (ours) |
| F1-F5, FA | DEBUG1-5, DEBUG_DUMP | modem to host | | no (host logs them) |
| anything else | | | NAK 1 | |

"None on success" for DMR_DATA: the MMDVM and MMDVM_HS firmwares stay silent and only NAK a refused frame; our old
firmware ACKed every DMR_DATA2. Every host here accepts both (MMDVMHost and rfgw ignore an ACK of DMR data; the bench
tools drop ACKs). Pick one and keep it; ACKing costs 4 bytes per 60 ms.

### 2.2 ACK and NAK

- ACK: `E0 04 70 <cmd>`.
- NAK: `E0 05 7F <cmd> <reason>`.

| Reason | Meaning (as used by the MMDVM firmwares and our old firmware) |
| --- | --- |
| 1 | unknown command |
| 2 | wrong mode: the mode isn't enabled, or the command doesn't apply now (other digital modes, DMR_DATA1 in plain simplex, SFR not on) |
| 3 | (unused by the hosts; "too long" in our numbering) |
| 4 | invalid: bad length, bad value (color code above 15, unknown state, TX delay out of range), a burst type the radio can't send, a frequency the radio can't tune |
| 5 | no space (TX buffer full); also SEND_CWID while not idle |
| 6 | MMDVM_HS: duplex not supported by this build. **Ours (SFR only): RF busy**, the inbound call is being repeated (section 6) |

MMDVMHost logs every NAK it receives outside startup as a warning and otherwise ignores it; the refused frame is lost
(no retry). rfgw raises an error for a NAK of a command it waited for, reports a NAK of DMR data to its owner (SFR
uses 6 and 2), and logs others.

### 2.3 GET_VERSION (00)

Request: `E0 03 00`. The reply must be a short frame (at most 255 bytes): MMDVMHost reads the reply at fixed frame
offsets and would misread a long frame.

**Protocol 1** (what our old firmware and MMDVM_HS send; what ODR sends, see 2.3.1):

| Frame offset | Content |
| --- | --- |
| 0-2 | E0, length, 00 |
| 3 | 01 (protocol version) |
| 4.. | description, ASCII, to the end of the frame. No terminating NUL needed. MMDVM_HS optionally appends a NUL and a hex UDID after the description; rfgw cuts the description at the first NUL. |

MMDVMHost with protocol 1 assumes the modem can do D-Star, DMR, YSF, P25, NXDN and POCSAG (there are no capability
bytes in protocol 1). It will therefore send SET_CONFIG enable bits and SET_MODE values for modes ODR doesn't
have if the user's configuration enables them; the radio must tolerate that (2.5, 2.6).

**Protocol 2** (the MMDVM repeater board firmware):

| Frame offset | Content |
| --- | --- |
| 3 | 02 |
| 4 | capabilities 1: 01 D-Star, 02 DMR, 04 YSF, 08 P25, 10 NXDN, 40 FM |
| 5 | capabilities 2: 01 POCSAG |
| 6 | CPU type: 0 Atmel ARM, 1 NXP ARM, 2 ST ARM (MMDVMHost prints 12 UDID bytes for ST, 16 otherwise) |
| 7-22 | UDID, 16 bytes (zero-padded) |
| 23.. | description, ASCII |

rfgw reads the description from frame offset 4 (protocol 1) or 23 (protocol 2). MMDVMHost refuses any version other
than 1 or 2 ("unsupported by this version of the MMDVM Host") and stops.

#### 2.3.1 Which version to answer

Answer **protocol 1**. Reasons: our bench tools (`hotspot_test.py`, `hs_normal.py` and their copies) send only the
23-byte protocol 1 SET_CONFIG and read the protocol 1 status layout (flags at frame offset 5, slot 2 space at offset
8); our status extension starts at the protocol 1 offset 13; the old firmware, which rfgw and the bench tools were
tested against, answered protocol 1. Current MMDVMHost and rfgw handle both versions. The cost of protocol 1: the
host can't learn that the radio is DMR only, so a misconfigured host may try other modes (they get NAK 2 / NAK 4).

#### 2.3.2 Description string rules

- ASCII, printable, no NUL, at most 120 bytes (keeps the frame short and under two 64-byte USB packets; the old
  firmware's buffer overflowed with a long build ID plus a suffix, gotchas 8.11).
- rfgw classifies the modem from the description, case-insensitively: it contains `OpenGD77` → type `opengd77`; one of
  `MMDVM_HS`, `ZUMspot`, `Nano_hotSPOT`, `NanoDV`, `SkyBridge`, `D2RG`, `LoneStar`, `ADF7021` → `mmdvm_hs`; `MMDVM` →
  `mmdvm`; otherwise rfgw refuses to start unless its config names the type. The `opengd77` type selects: 1 voice
  header and 1 terminator per call from rfgw (not 8 and 3), the radio's RSSI conversion (5.4), and simplex only.
  **A description without `OpenGD77` is detected wrongly by today's rfgw** (as `mmdvm` if it contains "MMDVM", which
  then needs a tuner and is refused), and the SFR mode is refused outright (next bullet). See section 4, row 1.
- SFR capability is announced by the suffix ` SFR TS<in>>TS<out> <framing>` (framing `MS`, `TS` or `BS`), present
  whenever the firmware has SFR, on or not, giving the slots and framing it would use: e.g. `... SFR TS1>TS2 TS`.
  rfgw finds it with the regular expressions `\bSFR\b`, `\bTS([12])\s*>\s*TS([12])\b` and
  `\bSFR\s+TS[12]\s*>\s*TS[12]\s+([A-Z]+)\b`, and today also requires `OpenGD77` in the string. It refuses to run
  an SFR config whose slots differ from the radio's, and warns unless the framing is `TS`.
- Split duplex capability is announced by the suffix ` SPLIT<version>` (` SPLIT1`), present whenever the firmware
  has the extension, role or not (section 6.5). It contains no `MMDVM`, `SFR` or slot pattern, so none of the
  detections above change.
- Avoid the substrings `MMDVM_HS`, `ADF7021`, `ZUMSPOT` and the like (they'd make rfgw take the radio for an MMDVM_HS
  board). The old firmware's description also carried `(Radio:<model>, Mode:MMDVM)`; nothing parses that part.

### 2.4 GET_STATUS (01)

Request: `E0 03 01`. MMDVMHost sends it every 250 ms, whether or not the last one was answered. rfgw sends it every
60 ms while it has bursts to hand over or is listening before talk, every 2 s otherwise. The reply must come within a
few tens of milliseconds even while transmitting (rfgw allows 1 s per request and gives up after 2 s of misses;
MMDVMHost resets the modem after 2 s without a status reply, 3.4).

**Protocol 1 layout** (send this one):

| Frame offset | Content |
| --- | --- |
| 0-2 | E0, length, 01 |
| 3 | modes enabled: 01 D-Star, 02 DMR, 04 YSF, 08 P25, 10 NXDN, 20 POCSAG (MMDVM_HS: 80 M17). MMDVMHost doesn't read it. Our old firmware sent 22 (DMR, POCSAG). |
| 4 | modem state: 00 idle, 02 DMR, 06 POCSAG, (MMDVM_HS: 61 CW ID). MMDVMHost stores it, doesn't act on it. |
| 5 | flags: 01 TX, 02 ADC overflow, 04 RX overflow, 08 TX overflow, 10 lockout, 20 DAC overflow, 40 carrier detect. 80: ours, SFR inbound busy (section 6). |
| 6 | D-Star space: 00 |
| 7 | DMR slot 1 space |
| 8 | DMR slot 2 space |
| 9 | YSF space: 00 |
| 10 | P25 space: 00 (MMDVMHost reads it only if the frame is longer than 10 bytes) |
| 11 | NXDN space: 00 (read only if longer than 11) |
| 12 | POCSAG space (read only if longer than 12): 00 or 01, never more (see 2.6) |
| 13.. | MMDVM_HS: M17 space at 13 (length 14). Ours: the SFR debug bytes, only in SFR mode (section 6) |

MMDVMHost (protocol 1) reads frame offsets 4 to 9 unconditionally, so the reply must be at least 10 bytes; send 13
(length 0D) like the old firmware, or 14 like MMDVM_HS. rfgw (protocol 1) reads offsets 5, 7 and 8 and refuses a
reply shorter than 9 bytes. The bench tools read offsets 5, 7, 8 and, from 13 on, the SFR debug bytes.

**Protocol 2 layout** (for reference): offset 3 state, 4 flags, 5 reserved 00, 6 D-Star space, 7 DMR slot 1 space,
8 DMR slot 2 space, 9 YSF, 10 P25, 11 NXDN, 12 reserved, 13 FM, 14 POCSAG, 15-19 reserved; length 20 (14 hex).

**Flags**:

| Bit | Meaning and what the hosts do with it |
| --- | --- |
| 01 TX | The modem is transmitting. Set from the moment it accepts traffic it will key for (buffering included) until its last burst, the terminator, has gone out, and while data is queued or on air; also while sending CW ID (gotchas 3.1, 8.6). MMDVMHost: sends SEND_CWID and DMR beacons only while it's clear; in duplex decides DMR_START. rfgw: waits for it to clear before retuning, sending CW ID or ending a call (and then waits 0.5 s more, a margin for the old firmware's early clear). Must not drop for a poll mid-call (gotchas 3.1). |
| 02, 20 overflow | MMDVMHost logs an error on every status that has them set. Report an event once, not as a sticky flag (every 250 ms would flood the log). ODR's modem passes bursts, not samples, so there is nothing to overflow: always clear. |
| 04 RX overflow | Uplink frames were lost because the queue to the host was full since the last status. Event, cleared once reported. |
| 08 TX overflow | The TX buffer was full and a frame was refused since the last status. Event. |
| 10 lockout | **Never set.** MMDVMHost switches to its lockout mode and sends SET_MODE idle while it is set (MMDVM's "COS as lockout" input). |
| 40 carrier detect | Something is on the channel (from the receiver's noise / RSSI, so analog and other color codes count too), evaluated only while not transmitting. rfgw uses it for listen before talk; MMDVMHost stores it. |
| 80 | Not defined by MMDVM. Ours, SFR mode only (section 6). |

**Space** (offsets 7 and 8): the number of further 33-byte DMR bursts the modem will accept for that slot right now
without a NAK. Unit: one burst, i.e. one DMR_DATA frame, i.e. 60 ms of air time in simplex. One byte, 0-255; never
negative, never wrapped (the old firmware once let it go below zero and reported 255, gotchas 8.4).

How the hosts use it (both MMDVMHost and rfgw do the same): take the value from each status reply, write a DMR_DATA
frame only while their copy is above 1 (so the last free place is never used), subtract 1 per frame written, and
overwrite the copy with the next status reply. Consequences for the modem:

- The value must be conservative. Frames written after the GET_STATUS was sent but before the reply was processed
  are still on the way when the host overwrites its copy, so with 250 ms polling the host can overcommit by several
  frames. Report the free count minus a margin of at least 2, or keep that slack in the buffer. A refused frame is
  lost for good (no host retries).
- Only frames that will occupy the buffer count. Frames the modem drops at once (idle bursts, PI headers in the old
  firmware) need not consume space.
- Simplex: slot 1's byte is the constant 10 (0A) in every firmware (MMDVM, MMDVM_HS, ours). Slot 2's byte is the
  real space. MMDVMHost may then write DMR_DATA1 frames if its network config enables slot 1; the modem refuses them
  (NAK 2) or drops them, and never transmits them.
- The initial value sets how much the host sends in one clump at the start of a network call: MMDVMHost queues 6 idle
  bursts (360 ms jitter, default) and 8 voice headers up front, so a space of at least 16 lets a call start without
  waiting for a status round trip. Our old firmware had 48 places, MMDVM_HS reports 25.
- In SFR mode both bytes carry the outbound slot's space (section 6).
- With no DMR mode (before SET_CONFIG, or in POCSAG / idle) the value may still be reported; MMDVM_HS reports 0 when
  DMR isn't enabled, our old firmware always reported its free places. Hosts only write after SET_MODE DMR.

### 2.5 SET_CONFIG (02)

MMDVMHost sends it once at startup (after SET_FREQ) and again after a modem reset; rfgw sends it on every tune
(after SET_FREQ, before SET_MODE). The modem answers ACK or NAK. **A NAK at MMDVMHost startup stops MMDVMHost** ("NAK
to the SET_CONFIG command"), so NAK only for values that make DMR impossible.

**Protocol 1 payload** (MMDVMHost sends 23 bytes, frame length 1A; MMDVM_HS refuses fewer than 23; our old firmware
accepted 13 or more; rfgw and the bench tools send 23):

| Payload offset | Content | DMR-only radio |
| --- | --- | --- |
| 0 | flags: 01 RX invert, 02 TX invert, 04 PTT invert, 08 YSF low deviation, 10 debug (the host wants DEBUG frames), 20 COS as lockout, 80 simplex (MMDVMHost `Duplex=0`) | must honor 80 (simplex / duplex, section 3.6 and SFR); 10 gates DEBUG frames; ignore 01, 02, 04, 08, 20 (no baseband path to invert; never report lockout) |
| 1 | modes enabled: 01 D-Star, 02 DMR, 04 YSF, 08 P25, 10 NXDN, 20 POCSAG | honor 02; ignore the others, never NAK for them |
| 2 | TX delay, 10 ms units (MMDVMHost default 100 ms = 0A) | MMDVM firmwares NAK above 50 (32 hex); our old firmware accepted up to 100 (1 s). Accept 0-255 and clamp; may use it as the key-up lead time |
| 3 | modem state to enter: 00 idle (MMDVMHost always sends 00), 02 DMR (rfgw and the bench tools send 02), 06 POCSAG | accept 00, 02, 06; NAK 4 for others |
| 4 | RX level, percent × 2.55 | ignore |
| 5 | CW ID TX level | ignore (or use for the CW tone level) |
| 6 | DMR color code 0-15 | honor; NAK 4 above 15 |
| 7 | DMR delay (duplex RX timing) | ignore in simplex |
| 8 | 80 (formerly oscillator offset) | ignore |
| 9 | D-Star TX level | ignore |
| 10 | DMR TX level (deviation) | may map to deviation; the old firmware ignored it |
| 11 | YSF TX level | ignore |
| 12 | P25 TX level | ignore |
| 13 | TX DC offset + 128 | ignore |
| 14 | RX DC offset + 128 | ignore |
| 15 | NXDN TX level | ignore |
| 16 | YSF TX hang | ignore |
| 17 | POCSAG TX level | ignore |
| 18 | FM TX level | ignore |
| 19 | P25 TX hang | ignore |
| 20 | NXDN TX hang | ignore |
| 21-22 | 00 (MMDVM_HS reads 21 as its M17 TX level) | ignore |

**Protocol 2 payload** (37 bytes, frame length 28; listed so the parser of a protocol 2 build or a probe knows it):
0 flags (as above), 1 modes (01 D-Star, 02 DMR, 04 YSF, 08 P25, 10 NXDN, 20 FM), 2 modes 2 (01 POCSAG), 3 TX delay,
4 state, 5 TX DC offset + 128, 6 RX DC offset + 128, 7 RX level, 8 CW ID level, 9 D-Star level, 10 DMR level,
11 YSF level, 12 P25 level, 13 NXDN level, 14 reserved, 15 POCSAG level, 16 FM level, 17-19 reserved, 20 YSF hang,
21 P25 hang, 22 NXDN hang, 23-24 reserved, 25 DMR flags (80 trunking), 26 color code, 27 DMR delay, 28 80, 29-36
reserved 00.

Effects to apply: color code (RX acceptance and TX framing), simplex / duplex, the state byte (as SET_MODE would),
debug flag. SET_CONFIG must not reset a transmission in progress unless the color code or duplex flag changed. In SFR
builds, re-evaluate the SFR trigger after it (section 6).

### 2.6 SET_MODE (03)

Payload: 1 byte, the state. `E0 04 03 <state>`. MMDVMHost doesn't wait for the reply; rfgw waits up to 1 s for an
ACK. The MMDVM firmwares and our old firmware ACK it.

| State | Action |
| --- | --- |
| 00 idle | Stop accepting new traffic for TX; a transmission in progress ends cleanly (terminator) within a superframe; the TX queue is cleared; TX flag clears once off air. The receiver keeps running (MMDVM firmwares pass up what they hear in idle; MMDVMHost switches itself to DMR mode on the first frame). CW ID is accepted only in idle. |
| 02 DMR | Normal hotspot operation. **Must be ACKed even before any SET_CONFIG** (rfgw sends SET_MODE DMR right after GET_VERSION, before its first SET_FREQ / SET_CONFIG; the old firmware treated DMR as always enabled). |
| 06 POCSAG | ACK and keep receiving DMR but don't pass it up (MMDVMHost drops DMR in POCSAG mode anyway); never transmit pages. Pi-Star enables POCSAG by default; MMDVMHost switches to POCSAG mode whenever a page arrives from DAPNET. Report POCSAG space 00 or 01 so the host never writes page data (it writes only while above 1). |
| same as now | ACK, no change. |
| 01, 03, 04, 05, 07, 0A and others | NAK 4. |

MMDVMHost sends SET_MODE idle when its mode hang expires (default 3 s after network traffic, 10 s after RF traffic),
when it enters its lockout or error modes, and at startup. It sends SET_MODE DMR before the first DMR frame of a call
(network) or after the first RF frame in idle.

### 2.7 SET_FREQ (04)

| Payload offset | Content |
| --- | --- |
| 0 | 00 (reserved) |
| 1-4 | RX frequency, Hz, little endian |
| 5-8 | TX frequency, Hz, little endian |
| 9 | RF level 0-255 (MMDVMHost `RFLevel` percent × 2.55; 100 % = FF) |
| 10-13 | POCSAG TX frequency, Hz, little endian (MMDVMHost sends 433000000 = 19CF0E40 when POCSAG is off; rfgw sends the TX frequency) |

Lengths: MMDVMHost and rfgw always send 14 payload bytes (frame length 11 hex). Accept 9 (very old hosts: no RF level,
take full power), 10 (with RF level) and 14. Protocol 1 and 2 use the same layout.

Rules:

- RX = TX is the normal simplex hotspot. RX ≠ TX in simplex is allowed (split simplex); tune both.
- Validate against the radio's hardware range only. Do not restrict to amateur bands: rfgw runs business-band
  channels (461-467 MHz), and the old firmware's band check broke that until removed (gotchas 8.7). A NAK at MMDVMHost
  startup stops it, which is the right outcome for a frequency the radio can't tune. Ignore the POCSAG frequency
  unless POCSAG TX is implemented (never NAK because of it).
- RF level: map 0-255 onto the radio's power steps. The old firmware kept the radio's own power setting when the level
  was FF (the user never touched it), and otherwise mapped below 50 as level / 12 and 50 and above as level / 50 + 3
  onto its power-level index. MMDVM_HS applies the new frequency only at the next SET_CONFIG; the old firmware
  retuned at once.
- SET_FREQ resets the hotspot's call state (gotchas 8.6): anything buffered for TX is dropped and the RF uplink call is
  closed. If an uplink call was open, send DMR_LOST2 before the ACK (recommendation: MMDVMHost has no RF watchdog,
  3.7). After the ACK, every frame sent up must have been heard on the new frequency: rfgw relies on this ("once the
  tune's last ACK is in, what follows was heard on the new channel") while it scans several channels a second.
- In SFR builds, re-evaluate the SFR trigger after it (section 6).

### 2.8 DMR_DATA1 / DMR_DATA2 (18 / 1A)

One DMR burst. Frame: `E0 25 1A <control> <33 bytes>` (length 37) for slot 2, 18 for slot 1. From the modem it may
carry 2 more bytes of RSSI: length 27 (39).

The 33 bytes are the whole 264-bit burst, first bit in the top bit of byte 0: 108 bits of payload, the 48-bit
sync / EMB / embedded-signaling field (bits 108-155: low nibble of byte 13, bytes 14-18, high nibble of byte 19),
108 bits of payload. For data-sync bursts the slot type (Golay(20,8), systematic) occupies the 10 bits on each side
of the sync: color code in bits 98-101 (frame byte 12, mask 3C), data type in bits 102-105 (byte 12 mask 03, then
byte 13 mask C0). Sync words (ETSI TS 102 361-1 table 9.2), 48 bits:

| Source | Voice | Data |
| --- | --- | --- |
| BS (repeater) | 755FD7DF75F7 | DFF57D75DF5D |
| MS (mobile) | 7F7D5DD57DFD | D5D7F77FD757 |
| Direct mode TS1 | 5D577F7757FF | F7FDD5DDFD55 |
| Direct mode TS2 | 7DFFD5F55D5F | D7557F5FF7F5 |

**Control byte** (frame offset 3):

| Value | Meaning |
| --- | --- |
| 40 + data type | a data-sync burst; low nibble = data type: 40 PI header, 41 voice LC header, 42 terminator with LC, 43 CSBK, 44 MBC header, 45 MBC continuation, 46 data header, 47 rate 1/2 data, 48 rate 3/4 data, 49 idle, 4A rate 1 data |
| 20 | voice burst A (voice sync) |
| 01-05 | voice bursts B-F (embedded signaling), the number is the position in the superframe |
| 00 | (host to modem) nothing said: classify from the burst |
| 80 + ... | (duplex MMDVM firmware, modem to host) what the idle receiver heard while the transmitter was off; rfgw's repeater reads it; a simplex radio never sends it |

**Host to modem**:

- MMDVMHost writes control byte **00 on every frame**: header, voice, terminator, idle, data alike. The modem must
  classify each burst from its own content: the sync word (any of the eight above; MMDVMHost puts MS sync on
  simplex traffic, BS on duplex traffic, and BS data sync on its idle bursts even in simplex), then for data sync the
  slot type's data type. Voice B-F carry an EMB instead of a sync: their position follows from the count since the
  last voice A (MMDVMHost always sends whole superframes, filling gaps with silence bursts itself) and is
  confirmed by the EMB's LCSS (B first fragment, C and D continuation, E last, F single / reverse channel).
  Classifying from the control byte sent data as voice, ignored terminators and NAKed the idle bursts before each
  call (gotchas 8.2). rfgw and the bench tools' `_ctl` cases set the control byte properly; when it is non-zero it
  may be trusted, but classification from the burst must give the same answer.
- What MMDVMHost sends for a network voice call in simplex: 6 idle bursts (its jitter buffer, 360 ms at the default
  `Jitter=360`; more for larger jitter), 8 voice LC headers, the voice superframes (A-F, silence bursts where the
  network lost frames), 3 terminators. A late-entry network call (no header) starts with the idle bursts and 8
  headers it builds itself. A PI header, if the network carries one, follows the headers. Data: 2 idle bursts, the
  data header, the blocks; CSBKs with up to 15 preamble CSBKs before them. A simplex host sends 8 headers and 3
  terminators; rfgw sends 1 header and 1 terminator to a radio it detects as `opengd77`, 8 and 3 to others.
- Idle bursts: accept (no NAK; the bench test `net_voice` fails on any NAK) and drop; the radio makes its own fill.
  MMDVM_HS transmits them as given.
- PI headers: the old firmware accepted and dropped them; a radio that transmits privacy calls should pass them on.
- Voice LC header: starts (or keys up) a call. Repeated headers with the same LC are the same call. A header with a
  different LC while the previous call is still going out is the next call: hold its frames behind the current call,
  end the current call with its terminator, then key again with the new headers (gotchas 8.3). A different header
  while still buffering (nothing on air yet) replaces the LC.
- Terminator: ends the call now; send what is buffered, then the radio's own terminator; no network timeout, no
  extra delay (gotchas 8.1). Further terminators of the same call are accepted and dropped.
- Start of transmission: start as soon as a few bursts are buffered (the old firmware: more than 4, or the terminator
  of a short call), never pad the start with silence superframes (gotchas 1.1). If nothing arrives for about 360 ms
  while buffering, give up and return to receive.
- Lone CSBKs and data: key, send, unkey (rfgw sends a radio check as a CSBK with preamble CSBKs, no header or
  terminator).
- Length must be exactly 37 (34 payload bytes): MMDVM firmwares NAK 4 otherwise.
- Full buffer: NAK 5 and drop the frame; never overwrite unsent frames (gotchas 8.4).
- Wrong mode: NAK 2 (state not idle / DMR, or DMR_DATA1 in plain simplex). A frame in idle state switches the modem to
  DMR (the MMDVM firmwares do this; our old firmware forced it for BlueDV).

**Modem to host** (what was heard on air), plain simplex:

- Every burst goes up as **DMR_DATA2**, whatever slot or sync the sender used (MMDVM_HS and MMDVM do the same;
  MMDVMHost simplex configs usually enable only network slot 2, and MMDVMHost drops slot 1 traffic for the network
  then). rfgw's simplex roamer listens only to DMR_DATA2. SFR mode uses the inbound slot's command (section 6).
- Control byte as in the table: 41 for each voice header heard, 20 for voice A, 01-05 for B-F, 42 for the
  terminator, 40 + type for PI headers, CSBK, data headers and blocks. MMDVMHost takes a frame whose control byte is
  exactly 42 as the end of the call.
- The 33 bytes must be a well-formed burst: MMDVMHost decodes the full LC of headers and terminators (BPTC(196,96)
  and RS(12,9) with the header / terminator masks; an undecodable header is dropped), regenerates the voice FEC,
  reads the EMB (its color code must match the configured one for late entry) and collects embedded LC, talker alias
  and GPS from B-E. Send the embedded signaling for B-E made from the call's LC (as the old firmware did) or as
  received if it decoded.
- Sequence: after voice A (20), MMDVMHost accepts B-F only as 01, 02, 03, 04, 05 in order; a repeated or skipped
  number makes it drop the rest of that superframe up to the next voice A. Fill a missed burst with a silence burst at
  its position rather than skip it (gotchas 8.5).
- Headers: send every header heard (MMDVM radios and Motorola send up to 8; MMDVMHost ignores repeats once the call
  is open). Late entry (voice heard without a header): MMDVMHost can start a call itself from voice A plus the
  embedded LC of B-E, but our clients were tested with the old firmware's behavior: once the embedded LC is known, a
  voice LC header made from it, then the superframe so far as silence from A so the sequence restarts cleanly.
  Either works with MMDVMHost; keep the old behavior for the bench tests (`rf_late`).
- One terminator per call (repeated terminators heard on air are the same call). Then nothing until the next call.
- RSSI: 2 bytes after the burst, big endian, raw. MMDVMHost maps raw to dBm with the user's RSSI table; rfgw
  converts with the radio formula of 5.4. The old firmware sent 00 and the AT1846S RSSI register (0x1B) high byte.
  Send the same scale on the AT1846S radios so rfgw's dBm stays right; on other RF chips, document the scale.

### 2.9 DMR_LOST1 / DMR_LOST2 (19 / 1B)

`E0 03 1B` (slot 2; 19 for slot 1). Sent when a call heard on air stops without a terminator. MMDVMHost has **no**
watchdog for RF calls: without DMR_LOST it keeps the call open until its timeout (default 180 s) and refuses network
traffic meanwhile. Timing: about 500 ms after the last burst of the call (the old firmware: 500 ms simplex, 300 ms in
SFR mode; MMDVM_HS: 13 missed frame times, about 780 ms; the bench test `rf_lost` requires 0.4-0.8 s). Never send it
during a long header run (8 headers take 480 ms: time the gap from the last burst of any kind) and never mid-call
because of one missed burst (one header followed by a 300 ms timer produced DMR_LOST mid-call and split calls in two,
gotchas 8.5).

### 2.10 DMR_SHORTLC (1C), DMR_START (1D), DMR_ABORT (1E), DMR_ALOHA (17)

- DMR_SHORTLC: `E0 0C 1C <9 bytes>`, the CACH short LC (activity per slot), encoded and interleaved. **MMDVMHost sends
  it in simplex too**, at the start and end of every call (it isn't gated on duplex). A direct-mode radio has no CACH:
  accept and ignore it, with no NAK (a NAK puts a warning in MMDVMHost's log per call). MMDVM_HS simplex builds ignore
  it silently; our old firmware ACKed it.
- DMR_START: `E0 04 1D 01` (key) / `E0 04 1D 00` (unkey). Duplex only; MMDVMHost never sends it in simplex. Ignore it
  (ACK or silence). rfgw doesn't send it in SFR mode.
- DMR_ABORT: `E0 04 1E <slot>` (01 or 02). Duplex only for MMDVMHost (sent when an RF call takes a slot from the
  network). rfgw sends it in SFR mode for the outbound slot whenever it starts a new network voice call or an RF call
  takes the slot: drop what is queued for TX (SFR). ACK (the old firmware) or silence. Never NAK.
- DMR_ALOHA: trunking only (MMDVMHost `[DMR] Trunking`); NAK 2 or ignore.

### 2.11 SEND_CWID (0A)

`E0 <len> 0A <ASCII callsign>`; MMDVMHost sends up to 200 characters, rfgw up to 40 upper case. Accepted only in idle
state (both MMDVMHost and rfgw put the modem in idle first and wait for TX to clear): ACK, then key FM and send the
Morse; TX flag set until done. In any other state NAK 5. rfgw waits for the ACK (1 s); MMDVM_HS sends no ACK on
success, our old firmware did. After CW, restore the DMR setup fully (gotchas 8.13: on the MD-9600 the HR_C6000 tone
configuration must be restored or DMR fails until a power cycle). The old firmware sent 60 ms dots (about 20 wpm).

Ours: the hotspot's `cw_id` op (`core/radio/radio_hotspot.c`) hands the callsign (up to 40 characters) to the radio
task, which keys it on boards whose `hal_radio_t` has `tx_tone` (only the DM-1701 so far; `tx_tone_key` switches
the tone without retuning): the DMR keying released, `set_mode(FM)`, a carrier on the hotspot's TX frequency, 150 ms
lead, the Morse (`core/radio/morse.c`: PARIS timing, dot = 1200 / wpm ms, `settings_t` `cw_wpm`, default 20 wpm,
tone `cw_tone_hz`, default 800 Hz), 150 ms tail, then `set_mode(DMR)` (the modem set up again, 8.13), the receiver
back on the hotspot's RX frequency and the DMR keying armed. The TX flag stays set meanwhile (`cw_busy`). Leaving
hotspot mode or SET_FREQ cuts the ID. Boards without `tx_tone` ACK and count it (`hs.cwid_unsent`, nothing keyed).
Debug link: `hs.cw_*` in the `dmr_ext` view section.

### 2.12 SERIAL (80), TRANSPARENT (90), QSO_INFO (91)

- SERIAL: MMDVMHost uses it to drive a Nextion display attached to the modem (`[Nextion] Port=modem`); it may arrive
  at any time. Drop silently (no NAK: it would flood the log). Never send it up.
- TRANSPARENT: drop silently.
- QSO_INFO: not sent by current MMDVMHost; older hosts / the CAST display send `E0 2F 91 02 <slot> <20 char source>
  <G|I> <20 char destination> <type>` for DMR, and `E0 <len> 91 FA <IP address text>` for the IP. The old firmware
  ACKed it and showed the source callsign and IP on its screen. Drop silently or ACK; never NAK.

### 2.13 DEBUG1-5 (F1-F5) and DEBUG_DUMP (FA), modem to host

| Cmd | Payload |
| --- | --- |
| F1 | text |
| F2 | text, then one signed 16-bit value, big endian |
| F3 | text, two values |
| F4 | text, three values |
| F5 | text, four values |
| FA | raw bytes (the host prints a hex dump) |

Text is not NUL-terminated (the host prints it bounded by the frame length). MMDVMHost logs each one. Send them only
when SET_CONFIG's debug flag (10) is set, never before the host has had its GET_VERSION reply (3.1), and never in a
burst (each costs host log I/O and serial time). Optional; not needed by any host.

### 2.14 FM parameters (60-63) and other modes

Hosts only send FM_PARAMS1-4 when FM is configured; MMDVMHost waits for their ACK at startup like SET_CONFIG. The
MMDVM firmware ACKs them even when built without FM; do the same (ACK, ignore) so a misconfigured host still starts.
D-Star, YSF, P25, NXDN, M17 and POCSAG data: NAK 2.

## 3. Handshake and timing

### 3.1 MMDVMHost startup (current master)

1. Opens the port (serial at the configured speed, 115200 usually; or UDP).
2. Waits **2 s**.
3. Sends GET_VERSION. Then reads for about 300 ms (30 reads 10 ms apart), consuming one frame per read; if a version
   reply is among them it continues, else it waits **1.5 s** and repeats, **6 attempts** in all (about 11 s), then
   gives up ("Unable to read the firmware version after six attempts") and stops. Any frame the modem sends before the
   version reply uses up one of the 30 reads: **the modem must not send unsolicited frames (DMR uplink, DEBUG) until
   it has been addressed** (our old firmware sent nothing up until the host had sent any command).
4. Sends **SET_FREQ** and waits for ACK / NAK.
5. Sends **SET_CONFIG** and waits for ACK / NAK.
6. (FM only) FM_PARAMS1-4, each waited on.
7. Starts polling GET_STATUS every 250 ms; sends SET_MODE idle.

"Waits for ACK / NAK" in steps 4-6 means: it reads one frame 10 ms after sending, then keeps reading while frames
arrive that aren't ACK or NAK (up to 30). If nothing has arrived at the first read, it carries on as if ACKed; a late
NAK is then only logged. So ACK quickly (within 10 ms is ideal), and in any case before any other frame.

The order is SET_FREQ before SET_CONFIG in current MMDVMHost. rfgw and the bench tools also send SET_FREQ, SET_CONFIG,
SET_MODE in that order, but rfgw sends a SET_MODE DMR right after GET_VERSION too, and the bench tools skip
GET_VERSION entirely. The modem must accept these commands in any order, repeated, at any time.

### 3.2 rfgw startup and tuning

GET_VERSION up to 6 times, each waited on for 1 s (no initial delay); then SET_MODE DMR; then per tune: SET_FREQ,
SET_CONFIG (with state 02 DMR), SET_MODE DMR, each waited on for 1 s (an error restarts the whole connection with
backoff 1, 2, 4 ... 30 s). Status every 60 ms while it has traffic or listens before talk, every 2 s when idle (to
keep the radio from leaving hotspot mode: the old firmware left after 20 s without host traffic). Scanning repeats the
tune sequence several times a second.

### 3.3 Bench tools

`hotspot_test.py` and `hs_normal.py` (and their copies `mmdvm_hotspot.py`, `hotspot_cases.py` in this repo): no
GET_VERSION; SET_FREQ, SET_CONFIG (protocol 1, 23 bytes), SET_MODE DMR, 150 ms apart, replies not waited on; then
GET_STATUS every 250 ms. Over UDP they treat each datagram as one frame (command at datagram byte 2): the modem must
send exactly one frame per datagram (5.3). `hs_normal.py` paces network calls like MMDVMHost (space above 1, minus one
per frame).

### 3.4 What makes MMDVMHost declare the modem dead

- Startup: no version reply in 6 attempts, a protocol version other than 1 or 2, a NAK to SET_FREQ or SET_CONFIG, or
  a port write failure: MMDVMHost exits.
- Running: **2 s without a GET_STATUS reply** (the timer starts at the first status reply and restarts on each): it
  logs "No reply from the modem for some time, resetting it", enters its error mode, closes the port, waits 2 s, and
  runs the whole startup (3.1) again, every 5 s until it succeeds. Only status replies restart this timer; DMR
  traffic does not.
- Port read / write errors are logged; a USB disconnect makes every read fail.

A modem that reboots (watchdog) while the host is running must answer the host's next GET_VERSION as at power on.

### 3.5 Playout and pacing (network to air)

MMDVMHost writes at most one DMR frame per slot per pass of its main loop, nothing within 10 ms of its last write
(playout timer), and only while its space copy is above 1 (2.4). A call therefore arrives as a clump (idle bursts +
headers, as far as space allows) and then at the network rate, about one frame per 60 ms, with jitter. rfgw: one
burst per 60 ms per slot after a prebuffer (6 by default), also gated on space above 1.

### 3.6 Simplex (Duplex=0) DMR behavior

- One timeslot on air. The host uses DMR_DATA2 / DMR_LOST2 only (slot 1 space is the constant 10).
- TX: MS-sourced framing on air regardless of the sync in the host's frames (MMDVMHost puts MS sync on simplex
  traffic; HBlink traffic forwarded by rfgw is regenerated to MS sync). The radio builds its own slot type, EMB and
  sync with the configured color code.
- The radio can't receive while it transmits; RF heard right after a transmission goes up normally.
- No DMR_START, no DMR_ABORT, short LC ignored, no CACH.
- After the host's terminator, the TX flag stays set until the radio's own terminator is on air (about 0.7 s after
  the host's terminator, measured on the DM-1701, gotchas 8.6), then clears.
- SET_CONFIG with the simplex flag clear and RX ≠ TX: MMDVMHost then expects a duplex repeater (both slots, DMR_START,
  BS framing). The old firmware ran as a plain simplex hotspot in that case; with RX = TX it turns SFR on (section 6).

### 3.7 Calls and "space" around back-to-back traffic

- Back-to-back network calls: MMDVMHost may deliver call 2's idle bursts and headers while call 1 is still in the
  modem's buffer. Call 1 must end with its terminator before call 2 keys up under its own header (gotchas 8.3);
  frames held for call 2 count against the space so the host doesn't overrun.
- Space must stay consistent: buffer occupied + held frames + free = capacity; report the smaller of voice buffer and
  data list space when they're separate (gotchas 8.6).
- RX and TX buffers are separate; RF frames don't survive into a later transmission and are cleared at a direction
  change (gotchas 8.8).
- Uplink (RF to host) frames go into a lock-free queue from the interrupt; a full queue drops and counts (RX overflow
  flag), never overwrites (gotchas 8.4).
- A DMR_LOST or terminator must close every uplink call before the next one's first frame; MMDVMHost has no RF
  watchdog (2.9).

## 4. Client compatibility matrix

Columns: **MMDVMHost** (current master, Linux / Pi-Star / WPSD class hosts), **rfgw** (RadioDesk, roamer `txvr` and
SFR `repeater`), **bench** (our `hotspot_test.py`, `hs_normal.py`, `run_sfr_call.sh` and copies), **old firmware**
(what our earlier radio did, which the other three were tested against).

| # | Point | MMDVMHost | rfgw | bench | old firmware |
| --- | --- | --- | --- | --- | --- |
| 1 | Version description | logged only | **must contain `OpenGD77`** to be typed `opengd77` (1 header / 1 terminator, radio RSSI formula); SFR needs `OpenGD77` + `SFR TSx>TSy <framing>` | not read | `OpenGD77_HS v0.1.18 GitID #<hash> (Radio:<model>, Mode:MMDVM\|BlueDV)` + ` SFR TS1>TS2 TS` in SFR builds |
| 2 | Protocol version | 1 or 2 | 1 or 2 | **1 only** (config and status offsets hard-coded) | 1 |
| 3 | GET_VERSION timing | 2 s after open, 6 tries, ~300 ms window each | 6 tries, 1 s each, no delay | never sent | answered from the first frame (that frame also switched the port to hotspot mode) |
| 4 | Status poll | 250 ms | 60 ms busy / 2 s idle | 250 ms | (answers each) |
| 5 | Dead modem | 2 s without status reply → reset and full re-handshake | 1 s per request, 2 s of misses → reconnect | none | left hotspot mode after 20 s without host traffic (MMDVM mode) |
| 6 | Status layout read | v1: offsets 4-9, 10-12 if present | v1: 5, 7, 8 (≥ 9 bytes) | 5, 7, 8; 13.. SFR debug | 13 bytes (22, state, flags, 0, 0A, space, 0, 0, 0, 01); 121 in SFR mode |
| 7 | Command order | GET_VERSION, SET_FREQ, SET_CONFIG, (status) SET_MODE | GET_VERSION, SET_MODE, then SET_FREQ, SET_CONFIG, SET_MODE per tune | SET_FREQ, SET_CONFIG, SET_MODE | any order |
| 8 | Waits for ACK of | SET_FREQ, SET_CONFIG, FM params (10 ms read, see 3.1) | SET_FREQ, SET_CONFIG, SET_MODE, SEND_CWID (1 s) | nothing | ACKed all of them |
| 9 | SET_CONFIG length | 23 payload (v1) | 23 (v1) / 37 (v2) | 23 | accepted ≥ 13 |
| 10 | SET_CONFIG state byte | 00 | 02 | 02 | accepted 00, 02, 06, 61 |
| 11 | Simplex flag | set (Duplex=0) | set (roamer); **clear + RX = TX for SFR** | set (`--simplex`) or clear + RX = TX (SFR) | SFR when clear and RX = TX (evaluated after either command) |
| 12 | SET_FREQ | 14 bytes, POCSAG 433 MHz filler | 14 bytes, POCSAG = TX freq; business band | 14 bytes, 461.6875 MHz | ≥ 9; refused 145.8-146.0 and 435-438 MHz only; reset hotspot state |
| 13 | DMR_DATA control byte to modem | **always 00** | set (41, 20, 01-05, 42, 43, 46 ...) | 00 (`net_*`) or set (`net_*_ctl`) | classified from the burst when 00 |
| 14 | Headers / terminators to modem | 8 / 3 (simplex), 6 idle bursts first | 1 / 1 to `opengd77`; SFR: 6 idle + 3 headers, terminator + (call hang / 60 ms − 1) more, about 50 | 6 idle, 8 headers, 3 terminators, 2 idle | dropped idle and PI headers; terminator ended the call |
| 15 | Space use | write while > 1, −1 per frame | same; SFR: slot 1 space from offset 7, slot 2 from 8 | same (`take_space`) | reported free of 48, smaller of voice / data |
| 16 | ACK of DMR data | ignored | ignored (NAK → callback) | ignored | ACKed every DMR_DATA2 |
| 17 | NAK of DMR data | logged, frame lost | SFR: 6 = RF busy (drops the stream), 2 = SFR off (error); others logged | `net_*` cases fail on any NAK | 5 full, 4 unusable burst, 2 wrong mode (DATA1 in simplex), 6 RF busy (SFR) |
| 18 | DMR_SHORTLC | sent every call, simplex too | duplex only, not SFR | never | ACKed |
| 19 | DMR_START / DMR_ABORT | duplex only | START duplex only; ABORT in SFR for the outbound slot | never | ACKed; ABORT dropped the SFR outbound queue |
| 20 | Uplink slot | DMR_DATA2 in simplex | roamer: only DMR_DATA2 is used; SFR: inbound slot's command (DATA1 for TS1) | `rf_*`: DATA2; SFR log: DATA1/2 | DATA2 simplex, inbound slot in SFR |
| 21 | Uplink control byte | 41 / 20 / 01-05 / 42 / 4x; 42 = end of call | same (rx.py reads data type and N) | modeled on MMDVMHost (sequence check) | all headers heard, late-entry header from embedded LC + silence fill, one terminator |
| 22 | RSSI after burst | optional; user RSSI table | optional; `opengd77`: dBm = raw − 151 (≥ 300 MHz), −164 + raw × 32 / 27 (VHF) | printed | 00, AT1846S reg 0x1B high byte |
| 23 | DMR_LOST | required (no RF watchdog) | used; own fallback 1 s (SFR) / 1.5 s | `rf_lost`: 0.4-0.8 s after last burst | 500 ms (300 ms SFR) |
| 24 | TX flag | gates CW ID and beacons | waits for clear + 0.5 s before retune / CW / end | `transmitting()` | set from buffering to own terminator on air, data busy, CW |
| 25 | Carrier detect 40 | stored | listen before talk | ignored | from noise level when not transmitting |
| 26 | SEND_CWID | only from idle, not waited on | idle first, waits for ACK, then waits for TX clear | never | ACK; NAK 5 unless idle |
| 27 | SET_MODE before SET_CONFIG | no | **yes (DMR, roamer)** | no | ACKed |
| 28 | POCSAG | SET_MODE 06 when pages arrive (if enabled) | never | never | ACKed mode 06; space 01; pages ACKed and not sent |
| 29 | Transport | serial or UDP (`Protocol=udp`, ModemAddress / ModemPort 3334, LocalPort 3335; accepts only datagrams from ModemAddress:ModemPort) | serial or UDP (same rule; local port 3335) | **UDP 12.47.82.93:3334 from 12.47.82.92:3335**, one frame per datagram (copies: serial or UDP) | USB CDC, or UDP 3334 over its USB network adapter (CDC-NCM), reply from radio:3334 to the host's last source port |
| 30 | DTR | Windows: **clears DTR**; Linux / macOS: raised by the OS on open | pyserial default (raised) | n/a (UDP) | ignored |
| 31 | 0xD5 / extra status bytes | never sent / ignored | not used | `--debug` sends 0xD5; parses 13.. | ACK (≥ 5 bytes) / NAK 4 |
| 31a | 0xD6 / ` SPLIT1` | never sent / suffix ignored | not used | not used (a split duplex host: `tools/split_duplex_host.py`) | D6 reply; NAK 1 without CONFIG_HOTSPOT_SPLIT |
| 32 | BlueDV (Windows client) | | | | "Mode:BlueDV" variant: no 20 s timeout, DMR_DATA2 in idle forced DMR mode, voice accepted without an embedded-LC call start (old firmware only; not verified against BlueDV itself) |

Row 1 is the one compatibility break a new name causes. Options for the lead: (a) change rfgw's detection to also
accept the new firmware's name (preferred: rfgw is ours; `detect()` and `sfr_slots()` in
`radiodesk_rfgw/mmdvm/radio.py`, plus its tests), or (b) put a compatibility token in the description. The old name
is the legacy project's, and `tools/license_grep.sh` fails on it in any source file outside `docs/`, so (b) is out
unless the rules change.

## 5. Transport details

### 5.1 USB CDC ACM

- Don't gate anything on DTR / RTS / line coding: MMDVMHost on Windows opens the port with DTR cleared, and many USB
  stacks' "connected" test is DTR. Check that the USB stack doesn't discard or block TX data while DTR is clear (test
  7, T-USB-2).
- Replies whose length is a multiple of 64 must be ended with a zero-length packet so the host sees them at once.
- Linux ModemManager probes new ACM ports with AT commands: the parser must skip those bytes (they aren't 0xE0) and
  answer the next real frame.
- The port may be shared with the programming protocol: the first byte 0xE0 selects MMDVM (`legacy-cps-interop.md`
  section 6). Once in hotspot mode, programming commands aren't answered.

### 5.2 UART

115200 8N1 (DM-32 programming cable). A 39-byte frame takes about 3.4 ms; status replies and uplink frames at 60 ms
intervals fit easily. Write from task context, not from an interrupt handler (gotchas 8.10). Stream parsing with
0xE0 resync as in section 1.

### 5.3 UDP (MMDVMHost `Protocol=udp`, rfgw `protocol: udp`)

- The modem listens on UDP port 3334 at its own address. The host sends from its LocalPort (3335 by default in rfgw;
  configured in MMDVMHost).
- Replies go from the modem's address, **source port 3334**, to the address and port the host's last datagram came
  from. MMDVMHost and rfgw both drop datagrams that don't come from exactly ModemAddress:ModemPort.
- One MMDVM frame per datagram in both directions (MMDVMHost writes each frame in one datagram; the bench tools need
  each reply in its own datagram). Still parse incoming datagrams as a stream.
- One host at a time (the last sender wins).
- The old firmware offered this over its USB network adapter (CDC-NCM, the radio's IP derived from the user's DMR ID;
  on the bench 12.47.82.93 with the Mac at 12.47.82.92). Gotchas 9.1-9.8 cover the network side.

### 5.4 RSSI scale

rfgw's `opengd77` type converts the raw RSSI to dBm as: dBm = raw − 151 at 300 MHz and above; dBm = −164 + raw × 32 /
27 below. The raw value is the 16-bit big-endian number after the burst; the old firmware put 00 in the high byte and
the AT1846S register 0x1B high byte in the low byte. MMDVMHost uses whatever RSSI table the user configured.

## 6. Our extensions (not part of MMDVM)

Both exist only for our single frequency repeater (SFR) in hotspot mode and for its on-air debugging. A standard host
can't trigger either: SFR turns on only when SET_CONFIG has the simplex flag **clear** and SET_FREQ has **RX = TX**
(both non-zero), evaluated after each of those commands in either order (gotchas 8.12). MMDVMHost with `Duplex=0`, and
Pi-Star, never get there. (An MMDVMHost configured `Duplex=1` with equal RX and TX frequencies would; that config makes
no sense for a real duplex board.)

### 6.1 Detecting them

- The GET_VERSION description's ` SFR TS<in>>TS<out> <framing>` suffix (2.3.2) says the firmware has SFR and with
  which slots and framing. It appears whether SFR is on or not.
- Sending 0xD5 to a modem without it gets NAK 1 (unknown command) from MMDVM, MMDVM_HS and any firmware without SFR;
  ours answers ACK (or NAK 4 if too short). Send it only to a modem whose description announces SFR.
- The extra status bytes appear only while SFR is on: a status reply longer than 13 bytes from a radio announcing
  SFR. A client must not assume them from the length alone (MMDVM_HS sends 14 bytes, with byte 13 its M17 space).

### 6.2 SFR mode behavior seen by the host

| Item | Rule |
| --- | --- |
| Uplink | What radios send on the inbound slot, burst by burst, never the radio's own repeat: DMR_DATA1 / DMR_LOST1 when the inbound slot is TS1, DMR_DATA2 / DMR_LOST2 for TS2. Every header, voice burst A-F, terminator, CSBK, data header and block, control byte and RSSI as in 2.8. A voice burst of a call whose header wasn't heard goes up after a header made from the embedded LC. DMR_LOST 300 ms after the last burst of a call without a terminator. |
| Downlink | The host sends whole streams for the outbound slot as DMR_DATA2 (DMR_DATA1 when the outbound slot is TS1). The radio keys itself, frames them with its own sync / slot type / EMB, fills gaps with idle bursts and runs its own TX hang. Each frame is ACKed or NAKed. Idle bursts from the host: ACK and drop. Repeated terminators: ACK. |
| NAKs for downlink frames | 6 RF busy: an inbound call is being repeated, and for the rest of a network stream an RF call cut into (until a new voice header, its terminator, or 1 s without it). 5 full. 4 a burst it can't use. 2 SFR not on. |
| RF priority | When an RF call starts, what the host had queued is dropped. Busy ends once the inbound call's terminator has been repeated (or the call is lost and what was heard has been repeated), not at the end of the hang. Once the inbound terminator is queued, a new stream from the host is accepted and goes out after it. |
| DMR_ABORT | Any slot: drops what the host had queued. ACK. |
| DMR_START, DMR_SHORTLC | ACK and ignore (the radio owns its keying and has no CACH). |
| GET_STATUS flag 01 | transmitting for any reason (the repeat, the host's traffic, the hang). |
| GET_STATUS flag 80 | **inbound slot busy**: an RF call is being repeated, until its terminator has been repeated. rfgw refuses new network streams while it is set. |
| GET_STATUS offsets 7 and 8 | both carry the outbound slot's free space (up to 16 bursts), so the host can write either slot's command. Outside SFR mode offset 7 is 10 as usual. |
| SEND_CWID | works while idle, as in simplex. |
| SFR settings | color code from SET_CONFIG, frequency and RF level from SET_FREQ; inbound / outbound slot, framing, hang and TOT from the radio's own channel settings (defaults TS1 in, TS2 out, TS framing, 1 s hang, 180 s TOT). No SET_CONFIG byte is used for them. |

### 6.3 GET_STATUS extra bytes (SFR mode only)

The protocol 1 status (frame offsets 0-12) unchanged, followed by 108 bytes of SFR counters and state, frame offsets
13 to 120; frame length 121 (79 hex). Counters are modulo 256. Layout: `drivers/modem/hrc6000/sfr_glue/hrcSfr.h`
(the `hrcSfrDebug_t` structure, carried over as our own code), first member at frame offset 13; the field list and
meanings are also in `tools/bench/mmdvm_hotspot.py`. A host that doesn't know them reads only up to offset 12 and is
not disturbed: MMDVMHost (protocol 1) reads at most offset 12; rfgw reads offsets 5, 7, 8.

Keep the length and member order stable; add new members only at the end, and never change a member's meaning, so
old bench tool versions stay correct.

The generic SFR layer (`core/hotspot/hotspotSfr.c` on `core/dmr/sfrRunner.c`, any modem behind `hal_dmr_modem_t`)
fills the members it knows: 14 bursts written, 15 bursts heard, 16 repeated, 17 send rejected, 24 state (01 keyed,
02 own timing, 10 RF call, 20 busy), 25 flags, 28 echo, 29 ignored, 30 duplicate, 31 last burst, 32 its color code,
35 reg10, 36 reg12, 38 ignored on the outbound slot, 39 bad checksum, 55 rssiMin, 60 raw area (FF: not used), 99 LC
rebuilt, 106 reg11, 107 flags2, 117 late entry held. The rest (register reads, RF switch timing, raw bursts) belong
to an HR_C6000 driver level SFR and stay 0 until one fills them.

### 6.4 Command 0xD5 (SFR debug)

`E0 <len> D5 <flags> <framing> [<reg10> [<reg12> [<rssiMin> [<reg11> [<flags2>]]]]]`, payload 2 to 7 bytes. ACK
(`E0 04 70 D5`) when at least 2 payload bytes are present, NAK 4 otherwise. Not stored (all zero at power on). Missing
optional bytes take their defaults: reg10 00 (the driver's), reg12 FF (the driver's), rssiMin 00 (default threshold),
reg11 00, flags2 00. Meanings of each bit: the `HRC_SFR_DEBUG_*` and `HRC_SFR_DEBUG2_*` definitions in
`drivers/modem/hrc6000/sfr_glue/hrcSfr.h`; framing 0 = the channel setting, 1 MS, 2 TS, 3 BS (GET_VERSION shows the
result). In a firmware without SFR, 0xD5 is an unknown command (NAK 1). The generic SFR layer acts on the framing and on flags2 0x40
(data calls repeated as heard, no hold); it keeps and reports the other switches for a driver that uses them.

### 6.5 Command 0xD6 (split duplex)

Two radios run as one duplex repeater, each an MMDVM modem (`docs/spec/split-duplex.md`, the full definition). Built
only with `CONFIG_HOTSPOT_SPLIT`; a firmware without it NAKs 0xD6 with 1, like any unknown command. Request
`E0 <len> D6 <sub> [<data>]`, reply `E0 <len> D6 <sub> <result> [<data>]` (result 0 done, 1 unknown sub-command,
2 wrong state, 4 invalid). Sub-commands: 00 INFO, 01 SET_ROLE, 02 TIME, 03 GET_EPOCH, 04 SET_EPOCH, 05 SET_AT,
06 STATUS. Nothing changes for a host that never sends 0xD6 except the ` SPLIT1` description suffix: a role starts
only with SET_ROLE 1 (TX half) or 2 (RX half) and ends with SET_ROLE 0, the end of the session (the port closed with
DTR, the 20 s timeout) or a 10 s lease that any 0xD6 refreshes. In a role the standard commands keep their framing
and change meaning as a duplex MMDVM board's (`split-duplex.md` 4); no other status bytes are added.

## 7. Conformance checklist and test list

For the host tests (a fake transport feeding the protocol code) and `tools/mmdvm_probe.py` (against the radio).
"→" is what the modem must answer; "(none)" means no frame within 100 ms. Payload bytes in examples are written in
full where they matter; `<33>` is a 33-byte burst.

### 7.1 Framing

| Id | Send | Expect |
| --- | --- | --- |
| F-1 | `E0 03 00` | version reply: `E0 LL 00 01 <ASCII>`, LL = 4 + description length, ≤ 7C |
| F-2 | `41 54 0D 0A` ("AT" CR LF) then `E0 03 00` | only the version reply |
| F-3 | `E0 03 01` split into three writes 20 ms apart | one status reply |
| F-4 | `E0 03 01 E0 03 01` in one write | two status replies |
| F-5 | long frame `E0 00 02 65 <253 bytes>` (total 257) then `E0 03 01` | NAK for 65 (or nothing), then one status reply; parser in sync |
| F-6 | `E0 02` then `E0 03 01` | status reply (a length below 3 is resynced) |
| F-7 | `E0 03 C3` (unknown) | `E0 05 7F C3 01` |
| F-8 | 1000 random bytes without E0, then `E0 03 01` | status reply |
| F-9 | power on, open port, wait 3 s, read | (none): nothing unsolicited before the first command |

### 7.2 Version and status

| Id | Check |
| --- | --- |
| V-1 | Protocol byte 01; description printable ASCII, no NUL, ≤ 120 bytes; frame ≤ 255 bytes. |
| V-2 | Description matches what the agreed rfgw detection expects (section 4 row 1) and, in SFR builds, contains ` SFR TS1>TS2 TS` (or the configured slots / framing) matching the three regular expressions in 2.3.2. |
| V-3 | Description contains none of `MMDVM_HS`, `ZUMspot`, `ADF7021`, `NanoDV`, `Nano_hotSPOT`, `SkyBridge`, `D2RG`, `LoneStar`. |
| S-1 | `E0 03 01` → length ≥ 0D, byte 2 = 01, byte 3 has bit 02, byte 7 = 0A in simplex, byte 12 ≤ 01. Fresh radio after SET_CONFIG simplex + SET_MODE DMR: e.g. `E0 0D 01 22 02 00 00 0A <space> 00 00 00 01` with space ≥ 10 (16+ recommended). |
| S-2 | 50 GET_STATUS 20 ms apart while a network call is being transmitted: 50 replies, each within 50 ms. |
| S-3 | Lockout bit 10 never set; bits 02 and 20 never set. |
| S-4 | TX overflow (08) appears in exactly one status after a NAK 5, then clears. |
| S-5 | In simplex the reply is never longer than 14 bytes (no SFR bytes). |

### 7.3 Configuration

Example frames (typical MMDVMHost defaults: levels 50 %, CC 1, TX delay 100 ms, simplex, DMR only):

- SET_FREQ 461.6875 MHz, RF 100 %: `E0 11 04 00 CC CA 84 1B CC CA 84 1B FF 40 0E CF 19` → `E0 04 70 04`
- SET_CONFIG v1: `E0 1A 02 80 02 0A 00 80 80 01 00 80 80 80 80 80 80 80 80 04 80 80 05 05 00 00` → `E0 04 70 02`
- SET_MODE DMR: `E0 04 03 02` → `E0 04 70 03`

| Id | Send | Expect |
| --- | --- | --- |
| C-1 | the three frames above, in that order, each within 2 ms of the last reply | three ACKs, each within 10 ms |
| C-2 | SET_MODE DMR before any SET_CONFIG (fresh power on) | `E0 04 70 03` |
| C-3 | SET_CONFIG with modes byte 3F (all modes) | ACK (unsupported mode bits ignored) |
| C-4 | SET_CONFIG with color code 10 hex (16) | `E0 05 7F 02 04` |
| C-5 | SET_CONFIG with TX delay 64 hex (1 s) | ACK |
| C-6 | SET_CONFIG with state 01 (D-Star) | `E0 05 7F 02 04` |
| C-7 | SET_CONFIG of 13 payload bytes (old hosts) | ACK, color code taken from payload 6 |
| C-8 | SET_FREQ of 9 and of 10 payload bytes | ACK each; 9 means full power |
| C-9 | SET_FREQ at 461.6875 MHz with POCSAG 433 MHz, and at 146.52 MHz (if the radio has VHF) | ACK (no amateur-band check, POCSAG frequency ignored) |
| C-10 | SET_FREQ outside the hardware range (e.g. 1 GHz) | `E0 05 7F 04 04` |
| C-11 | SET_MODE 06, then 00, then 02, then 02 again | four ACKs |
| C-12 | SET_MODE 03 (YSF) | `E0 05 7F 03 04` |
| C-13 | SEND_CWID `E0 08 0A 54 45 53 54 31` in idle | ACK, TX flag set while keying, clears after |
| C-14 | SEND_CWID in DMR state | `E0 05 7F 0A 05` |
| C-15 | FM_PARAMS1 `E0 0A 60 <7 bytes>` | ACK |
| C-16 | SET_FREQ during an uplink call | DMR_LOST2 (`E0 03 1B`) before the ACK; no uplink frame from the old frequency after the ACK |
| C-17 | the same three frames repeated 5 times a second for 10 s (rfgw scanning) | all ACKed, no crash, status still answered |

### 7.4 DMR downlink (network to air)

| Id | Send | Expect |
| --- | --- | --- |
| D-1 | MMDVMHost-style call, control 00: 6 idle (BS data sync, DT 9), 8 headers (MS data sync), 3 superframes, 3 terminators, 2 idle, paced by space | no NAK; on air (SDR / bench modem): one call, header LC as sent, ≥ 17 of 18 voice bursts, terminator, no voice after it |
| D-2 | the same with control bytes set (rfgw style) | same |
| D-3 | 1 header, voice, 1 terminator (rfgw `opengd77` profile) | one complete call on air |
| D-4 | call 2's idle + headers arriving while call 1 is still being sent | call 1 ends with its terminator, call 2 keys under its own LC; no NAK while the host respects space |
| D-5 | frames beyond the reported space (ignore space) | NAK 5 per refused frame (`E0 05 7F 1A 05`), the sent call not corrupted, space never reported above the true free count, never 255 |
| D-6 | DMR_DATA2 of length 36 or 38 | `E0 05 7F 1A 04` |
| D-7 | DMR_DATA1 in simplex | `E0 05 7F 18 02` or nothing; nothing on air |
| D-8 | DMR_SHORTLC `E0 0C 1C <9 bytes>`, DMR_START `E0 04 1D 01`, DMR_ABORT `E0 04 1E 02` in simplex | no NAK |
| D-9 | data: 2 idle, data header (DT 6), 3 rate 1/2 blocks (DT 7), control 00 | header and blocks on air in order; no NAK |
| D-10 | lone CSBK with 2 preamble CSBKs | three CSBKs on air, transmitter off after |
| D-11 | TX flag: set within one status after the first header is accepted; stays set through the call; clears within 1 s after the host's last terminator, only after the radio's terminator is on air | |
| D-12 | SET_MODE 00 mid-call | the call ends with a terminator within a superframe; TX flag clears; queue empty |
| D-13 | serial / transparent / QSO info (`E0 06 80 41 42 43`, `E0 05 90 00 00`, `E0 2F 91 02 ...`) during a call | no NAK, call unaffected |

### 7.5 DMR uplink (air to host)

Tests with the bench modem (`bench_tx.py`) or the host-test model of MMDVMHost's RF handling (`tests/host/hotspotUplinkTest.c`).

| Id | On air | Expect from the modem |
| --- | --- | --- |
| U-1 | 8 headers, 3 superframes, 3 terminators | DMR_DATA2 only; 41 for each header received (≥ 1), then 20, 01, 02, 03, 04, 05 per superframe, one 42; no DMR_LOST during the headers; ≥ 17 of 18 voice bursts in sequence |
| U-2 | late entry (no header, start mid-superframe) | a header (41) with the sender's LC once the embedded LC is known, then voice starting at 20 with no sequence gap; ≥ 10 of 22 bursts; ended by 42 |
| U-3 | 300 voice bursts | one call, ≥ 297 bursts, no DMR_LOST |
| U-4 | voice stopped without terminator | `E0 03 1B` 0.4-0.8 s after the last burst |
| U-5 | three back-to-back calls, alternating sources | three calls, each with its own header and terminator |
| U-6 | 4 CSBKs | ≥ 3 frames with control 43, no voice call |
| U-7 | unconfirmed data, header + 3 blocks | 46 then three 47 |
| U-8 | every frame | length 25 (no RSSI) or 27 (RSSI); if 27, the RSSI bytes give a sane dBm with the 5.4 formula |
| U-9 | a burst heard with a wrong color code | not sent up |
| U-10 | every header / terminator sent up | full LC decodes (BPTC + RS with the right mask); EMB color code = configured |

### 7.6 Timing and robustness

| Id | Check |
| --- | --- |
| T-1 | MMDVMHost-style startup (open, 2 s, GET_VERSION ...) succeeds on the first attempt. |
| T-2 | rfgw-style startup (GET_VERSION at once, SET_MODE DMR, tune) succeeds. |
| T-3 | 30 minutes of MMDVMHost-style polling with periodic calls: no "No reply from the modem" (no status reply gap ≥ 2 s). |
| T-4 | Host closes and reopens the port (or stops polling 25 s then restarts): the modem answers the new handshake; a call in progress is ended cleanly. |
| T-5 | Modem reboot while the host polls: the host's re-handshake works. |
| T-USB-1 | Status reply and uplink frames whose total is exactly 64 or 128 bytes arrive without delay (zero-length packet). |
| T-USB-2 | Port opened with DTR cleared (as MMDVMHost on Windows): full handshake and a call both ways work. |
| T-UDP-1 | Over UDP: replies come from port 3334 to the sender's port, one frame per datagram; datagrams with two frames from the host are both handled. |

### 7.7 Our extensions

| Id | Send | Expect |
| --- | --- | --- |
| X-1 | simplex config (flag 80) + RX = TX | status 13 or 14 bytes, no flag 80, offset 7 = 0A |
| X-2 | SET_CONFIG flag 00 + SET_FREQ RX = TX (either order) | SFR on: status length 79 hex (121), offsets 7 and 8 equal |
| X-3 | X-2 then SET_FREQ RX ≠ TX | SFR off, status back to 13 / 14 bytes |
| X-4 | `E0 05 D5 00 00` | `E0 04 70 D5` (SFR builds), `E0 05 7F D5 01` (non-SFR builds) |
| X-5 | `E0 04 D5 00` | `E0 05 7F D5 04` |
| X-6 | SFR on, RF call on the inbound slot, host sends DMR_DATA2 for the outbound slot | `E0 05 7F 1A 06`; status flag 80 set until the inbound terminator has been repeated |
| X-7 | SFR on, inbound TS1 | uplink as DMR_DATA1 / DMR_LOST1; DMR_LOST1 300 ms after a call without terminator |
| X-8 | SFR on, DMR_ABORT `E0 04 1E 02` during a network stream | ACK, queued outbound bursts dropped |
| X-9 | MMDVMHost (Duplex=0) full session with SFR firmware | never sees a status longer than 14 bytes, never gets NAK 6 |
