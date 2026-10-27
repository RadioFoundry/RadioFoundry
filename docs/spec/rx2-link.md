# The second receiver's link

A radio with two SoCs, each with its own receiver, runs RadioFoundry on both. The main SoC (A) has the UI, PTT and
the transmitter; the other (B) is a second receiver. They talk over a UART with this protocol (`core/rx2/rx2_link.c`).
The radio core sees B as `board.rx2` ([radio-features.md 2.2](radio-features.md#22-a-second-receiver)).

## 1. Frames

```
A5 5A  len  type  seq  payload[len]  crc_hi crc_lo
```

- `len` is 0 to 96.
- `seq` counts each side's frames. B's `ACK` names the `seq` of the `TUNE` it answers.
- The CRC is CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over `len`, `type`, `seq` and the payload. It is sent
  big endian.
- A bad frame is dropped and counted. The parser looks for the next `A5 5A`.
- Payload numbers are little endian.

## 2. Messages

| Type | Name | From | Payload |
|---|---|---|---|
| 0x01 | `HELLO_REQ` | A | none. B answers `HELLO`, then stops and mutes: A has just started or lost it |
| 0x02 | `TUNE` | A | the settings (2.1). B answers `ACK` and a `STATUS` |
| 0x03 | `STOP` | A | none. B stops receiving and mutes |
| 0x04 | `AUDIO` | A | u8 on, u8 volume (percent) |
| 0x05 | `STUB` | A | u32 0x53545542. B enters the flash stub on the link UART |
| 0x06 | `RESET` | A | u32 0x53545542. B restarts |
| 0x07 | `LED` | A | u8: the LEDs on B's SoC that are on (bit 0 LED 1, bit 1 LED 2), on a board whose status LEDs are wired to B. A sends it on a change and again when B comes back; B turns them off at `HELLO_REQ` |
| 0x08 | `DL` | A | u8 tag, u8 part, then a debug link request: u8 command, its payload (2.3) |
| 0x81 | `HELLO` | B | u8 protocol version (1), u8 flags (bit 0: B decodes DMR), then B's firmware version as text |
| 0x82 | `STATUS` | B | 13 bytes (2.2) |
| 0x83 | `ACK` | B | u8 the `TUNE`'s seq, u8 result (0 = on it, else refused) |
| 0x84 | `LOG` | B | text for A's log |
| 0x85 | `DL_REPLY` | B | u8 tag, u8 part, then a piece of the debug link reply (2.3) |

### 2.1 TUNE

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | RX frequency (Hz) |
| 4 | 1 | mode (`radio_mode_t`) |
| 5 | 1 | bandwidth (`radio_bw_t`) |
| 6 | 1 | squelch level, 0 = open to 9 = tight |
| 7 | 1 | RX tone type (`tone_type_t`) |
| 8 | 2 | RX tone value (`subtone_t.value`) |
| 10 | 1 | DMR color code |
| 11 | 1 | DMR timeslot (1, 2) |
| 12 | 4 | our DMR ID |
| 16 | 4 | the channel's talk group, 0 = none |
| 20 | 1 | n, the RX group list's groups (0 to 16) |
| 21 | 4 | reserved, 0 |
| 25 | 4 n | the groups |

On DMR, B decodes the calls itself. It plays a call to one of the groups, to the talk group, or a private call to our ID.

### 2.2 STATUS

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | bit 0 on its settings, 1 active (FM: squelch open with the right tone; DMR: a call for us), 2 DMR, 3 a group call, 4 heard (active and its audio on) |
| 1 | 2 | RSSI (dBm, signed) |
| 3 | 1 | noise |
| 4 | 4 | DMR source ID |
| 8 | 4 | DMR destination ID |
| 12 | 1 | reserved |

B sends one on every change, and at least every 500 ms.

### 2.3 The debug link, through A

B serves debug link commands ([debuglink.md](debuglink.md) 3) with the same command numbers and reply formats as A,
for what it has: no display, keys, radio core or storage. The host reaches it through A's `DL_CMD_RX2` FORWARD
([debuglink.md](debuglink.md) 3.22), one command at a time, while both SoCs run on.

| Field | Meaning |
|---|---|
| tag | chosen by A for each request; B's reply parts carry it. A drops parts with another tag |
| part | bits 0-6 the part's index from 0, bit 7 the last part |

- A request is one part: the command byte and its payload, at most 94 bytes. Through A's `FORWARD` the payload is at
  most 62 bytes (debuglink.md 3.22).
- B's reply is the reply payload, status byte first, at most 1023 bytes (A's reply adds its own status), cut into parts
  of up to 94 bytes, sent in order. B answers one request at a time, in its link task, between its `STATUS` frames.
- A waits up to 1 s for the last part. A missing part, a wrong index or the timeout ends the request: A answers the
  host `DL_ST_UNAVAILABLE` (4).
- What B serves (`core/rx2/rx2_dl.c`): PING, VERSION, GET_DIAG, RESET_COUNTERS, GET_LOG, GET_VIEW (B's own sections),
  INFO_CHIP, READ_MEM, REBOOT (normal only) and CRASH. Anything else is `DL_ST_UNKNOWN_CMD` (1) from B.
- REBOOT takes the chip UID's low word as its token, as on A. When B can't read its UID (INFO_CHIP answers
  `DL_ST_UNAVAILABLE`), it takes the bridge's token `0x47445242` instead.
- A restart or crash test runs 50 ms after its reply has gone out.
- B's log (`dlog`) is its own ring, read with GET_LOG. The `LOG` frames B sends at start-up go to A's log as before.
- B's server is built in unless the board's build turns it off; without it, B answers every command
  `DL_ST_UNKNOWN_CMD`, and A's `FORWARD` passes that status on.

## 3. Timing

- A sends `HELLO_REQ` every second until B answers.
- A counts B as down after 1.5 s without a frame. The core then alternates A's receiver.
- A sends a `TUNE` again when it has no good `ACK` after 300 ms.
- When B comes back, A sends its settings and audio state again.
- A drops a `TUNE` with the same settings as the last one.

## 4. Updating B

B has no USB or PC connector of its own. Its first install goes through its IAP loader. Later ones go through A:

1. The host asks A's debug link for the bridge (`DL_CMD_RX2` BRIDGE, [debuglink.md](debuglink.md) 3.22).
2. After the reply, A sends `STUB`. B writes `STAY` (0x53544159) at 0x0004FFF8 and jumps to its first stage at
   0x0300C000 ([boot/README.md](../../boot/README.md)).
3. A copies bytes between the PC cable (UART1) and the link (UART3) both ways. Its transmitter stays off, and it feeds
   its watchdog.
4. B's first stage sees `STAY` and offers the boot ROM's handshake on the link for up to a minute. The host answers
   through A and loads the stub, which is told to stay on UART3.
5. The host writes B's image and restarts B (`--boot reset`).
6. The host ends the bridge with the exit sequence (4.1): A restarts at once. Without it, A restarts 10 s after the
   host's last byte, or after 90 s if the host never sends one. It finds B again with `HELLO_REQ`. Bytes from B don't
   count: its handshake offers go on until the host answers.

The link stays at 115200 baud through the bridge: the host does not switch speed.

### 4.1 Ending the bridge

The host sends `7E E7 81 18` with at least 2 ms of quiet on the cable before and after it, and nothing else. A sees
those 4 bytes as a burst of their own, does not pass them to B, and restarts after what is still on its way out has
gone. The bytes match no frame the bridge carries (the boot ROM's start `02` or `00`, the debug link's `A5 5A`), and a
burst of exactly these 4 bytes between two quiet gaps is not part of a longer message. A serial break does not end the
bridge: the cable's RX line is also the PTT line, and a press or a loose plug must not end a write half done.
`stubtool.py` and the web flasher send the sequence when they are done with B.
`stubtool.py --board <the second SoC's board>` does steps 1, 4 and 5. Without a host, B's first stage starts B's
application again after the minute, and A restarts.
