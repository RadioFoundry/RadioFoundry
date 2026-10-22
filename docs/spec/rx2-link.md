# The second receiver's link

A radio with two SoCs, each with its own receiver (the GD-88), runs ODR on both. The main SoC (A) has the UI, PTT and
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
| 0x81 | `HELLO` | B | u8 protocol version (1), u8 flags (bit 0: B decodes DMR), then B's firmware version as text |
| 0x82 | `STATUS` | B | 13 bytes (2.2) |
| 0x83 | `ACK` | B | u8 the `TUNE`'s seq, u8 result (0 = on it, else refused) |
| 0x84 | `LOG` | B | text for A's log |

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

## 3. Timing

- A sends `HELLO_REQ` every second until B answers.
- A counts B as down after 1.5 s without a frame. The core then alternates A's receiver.
- A sends a `TUNE` again when it has no good `ACK` after 300 ms.
- When B comes back, A sends its settings and audio state again.
- A drops a `TUNE` with the same settings as the last one.

## 4. Updating B

B has no USB or PC connector of its own. To update it, A sends `STUB`. B jumps to its flash stub, which listens on the
link UART. A then bridges its PC UART to the link UART (stub/README.md). `odrflash --board gd88b` does this.
