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
| 0x07 | `LED` | A | u8: the LEDs on B's SoC that are on (bit 0 LED 1, bit 1 LED 2), on a board whose status LEDs are wired to B. A sends it on a change and again when B comes back; B turns them off at `HELLO_REQ`. Bit 7 (`RX2_LED_STUB`): A is jumping to its flash stub, which has no link; B blinks its LEDs until the next `HELLO_REQ` or `LED` |
| 0x08 | `DL` | A | u8 tag, u8 part, then a debug link request: u8 command, its payload (2.3) |
| 0x09 | `PATH` | A | u8 on, u32 Hz: where A's own receiver is, on a board whose receive path for A runs through B's pins. A sends it on a change and again when B comes back; B turns A's path off at `HELLO_REQ` |
| 0x0A | `TX` | A | u32 Hz (0 = disarm), u16 PA drive (0-4095), u16 TX-on delay (us), u16 TX-off delay (us): on a board whose PA for A's transmitter is wired to B. A sends it before every transmission and waits for the `ACK`; it doesn't transmit without one. While armed, B keys the PA on the edges of A's TX line, with this drive and these delays. A disarms after the transmission; B also disarms at `HELLO_REQ`, and a disarm while keyed ramps the PA down at once |
| 0x0B | `DMR` | A | u8 op, then its fields: a request to B's transmitter as A's second DMR transmitter (2.4), on a board whose B can transmit (`HELLO` flag bit 2). No `ACK`: B's state comes back in `DMR_EV` |
| 0x0C | `SPEAKER` | A | u8 on: A's loudspeaker is in use (squelch open, a beep, DMR voice), on a board whose speaker path has a switch on B (`rx2_ops_t.speaker`). Sent on a change, and again after `HELLO` while on; A holds it on 1 s after the last sound, so a beep doesn't wait for the link. No `ACK`. B opens the switch at `HELLO_REQ` |
| 0x81 | `HELLO` | B | u8 protocol version (1), u8 flags (bit 0: B decodes DMR; bit 1: `STUB` reaches B's first stage, 4; bit 2: B transmits DMR for A, 2.4), then B's firmware version as text |
| 0x82 | `STATUS` | B | 13 bytes (2.2) |
| 0x83 | `ACK` | B | u8 the `TUNE`'s or `TX`'s seq, u8 result (0 = on it, else refused) |
| 0x84 | `LOG` | B | text for A's log |
| 0x85 | `DL_REPLY` | B | u8 tag, u8 part, then a piece of the debug link reply (2.3) |
| 0x86 | `DMR_EV` | B | 5 bytes: an event or the state of B's transmitter (2.4) |

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

### 2.4 B as A's second DMR transmitter

On a board where B has a transmitter of its own, A can send DMR on it while its own modem goes on receiving: a single
frequency repeater's repeat ([sfr.md](sfr.md) 3 item 7). The modem A's core gets is A's own with the transmit calls
(`tx_begin`, `tx_queue`, `tx_listen`, `tx_end`, `tx_abort`, `set_embedded_lc`) sent to B while the split is on, and
B's transmit events passed back to the core's callback (`core/rx2/rx2_dmr.c`, both ends).

| Op | Name | Fields | B |
|---|---|---|---|
| 0x01 | `SPLIT` | u8 on, i8 color code | on: stops its own receiving and takes the role; off: back to A's last `TUNE` |
| 0x02 | `ARM` | u32 Hz (0 = disarm), u8 power level | receives there in DMR (the modem's slot timing comes from the inbound bursts) and keys its PA per burst there, with its own calibration |
| 0x03 | `BEGIN` | u8 timing, i8 slot, u8 access | `tx_begin`; refused: a `TX_REJECTED` event |
| 0x04 | `BURST` | u8 kind, u8 voice index, u8 flags (bit 0 host CRC), u8 sync override, u8 length, the payload | `tx_queue` |
| 0x05 | `END` | none | `tx_end` |
| 0x06 | `ABORT` | none | `tx_abort` |
| 0x07 | `EMB` | 9 bytes | `set_embedded_lc` |
| 0x08 | `LISTEN` | none | `tx_listen` |

`DMR_EV`: u8 kind, i8 slot, u8 queue space, u8 state (bit 0 ready: split and armed; bit 1 transmitting), u8 bursts
taken. Kind is the modem's event for `TX_SLOT_STARTED`, `TX_DONE` and `TX_REJECTED`, or 0x80 for the state alone,
which B sends on a change and every 200 ms while split. Bursts taken counts every `BURST` B received (mod 256): A
counts the bursts still on the link against the space B reported.

- No `tx_fill`: its answer can't wait for the link, so A's caller queues the burst instead.
- A's `tx_begin` waits (returns false) until B reports ready. Not ready 1.5 s after the split, or the link down, and A
  turns the split off and transmits itself.
- A transmission B goes quiet on for 600 ms, or the link down, ends at A with `TX_DONE` (after `tx_end`) or
  `TX_REJECTED`, so the core never waits for an event that won't come.
- `HELLO_REQ` ends the role at B (A restarted).
- A's radio keys B's transmitter instead of its own while the split is on (`ARM` from its `tx()`). Its `tx_off()`
  leaves B armed: B goes on receiving there for the slot timing and keys nothing without a `BEGIN`. The split's end
  disarms it.
- At the split A arms B at once on its receive frequency (the board's `listen`), so B's modem has the inbound slot
  timing before the first repeat is begun. An aligned `BEGIN` with nothing heard takes the modem's own timing (the
  repeat landed 12 ms off the grid).
- A's `set_cc_filter` goes to B too (`SPLIT` with the new code, then `ARM` again): with a stale code B heard nothing
  and fell back to its own timing.
- A's SFR doesn't check its inbound bursts for its own echo while split: A's chip sends nothing, and the repeat runs
  about 180 ms behind (no `tx_fill`), close enough that a call of silence matched its own copy from the superframe
  before (gotchas 14.2).
- One `BURST` frame is 40 bytes, about 3.5 ms at 115200: well inside a repeat's 90 ms.

## 3. Timing

- A sends `HELLO_REQ` every second until B answers.
- A counts B as down after 1.5 s without a frame. The core then alternates A's receiver.
- A sends a `TUNE` again when it has no good `ACK` after 300 ms.
- When B comes back, A sends its settings and audio state again.
- A drops a `TUNE` with the same settings as the last one.

## 4. Updating B

B has no USB or PC connector of its own. Its first install goes through its IAP loader. Later ones go through A:

1. The host asks A's debug link for the bridge (`DL_CMD_RX2` BRIDGE, [debuglink.md](debuglink.md) 3.22).
2. After the reply, A sends `STUB`. B writes `STAY` (0x53544159) at 0x0004FFF8, puts UART3 back to unconfigured and
   jumps to its first stage at 0x0300C000 ([boot/README.md](../../boot/README.md)). A sends `RESET` instead when B's
   `HELLO` lacks flag bit 1: a B application from before the flag kept its own UART divisor, wrong at the loader's
   clock. B then restarts, and its first stage makes its ten start-up offers.
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
