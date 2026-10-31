<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# SCT DMR basebands

Sicomm's SCT3258 and SCT3288 run DMR layers 1 and 2 and an AMBE+2 vocoder inside the chip. The MCU sends and
receives packets of typed fields; the voice goes between the chip and the audio codec without passing through the
MCU. `drivers/modem/sct` drives both chips: a packet layer (`sct_pkt`), the transport and boot (`sct_host`) and a
layer 2 engine that implements `hal_dmr_modem_t` (`sct_dmr`). The board supplies the bus and the reset sequence.
[feature-matrix.md](../feature-matrix.md) lists which radios have one.

Sources: the chip's datasheet and its "Packet Interface" document (cite them, don't copy them into this repository),
and what the stock firmware of the radios that carry the chips sends. `(unverified)` marks what no radio has
confirmed yet.

## 1. Chips and host link

| Item | SCT3258 | SCT3288 |
| --- | --- | --- |
| Host link | 8-bit asynchronous parallel port (HPI), Intel or Motorola timing; strap PIO1 low at reset = Intel | SPI mode 0, 16-bit transfers |
| Word order | 16-bit mode, low byte first | 16-bit transfers |
| Data waiting (HOBIB) | active low | active high |
| Boot | the host sends a 1024-word boot block after every reset | boot ROM in the chip |
| Firmware | application and vocoders in the chip's flash, enabled per chip by activation files | the same |

Both speak the packet protocol in section 3. The board says which link and which boot its chip takes
(`sct_host_t`): `write` sends an even number of bytes, `read` returns what is waiting (0 while HOBIB says nothing
is), `reset` runs the board's power and reset sequence, `boot` is `SCT_BOOT_BLOCK` or `SCT_BOOT_ROM`.

## 2. Boot

`sct_host_boot` resets the chip and waits for its application, once at start-up. Packets that arrive with the boot
answer are dropped.

| Boot | Sequence | Success |
| --- | --- | --- |
| `SCT_BOOT_BLOCK` | the board's reset, then exactly 2048 bytes (the boot block) | `ACK_MESSAGE` 0x01 (reset ready) |
| `SCT_BOOT_ROM` | the board's reset, then wait for the ROM's greeting. 0x01: the chip started its application itself. 0x0E (not loaded): send `74 A9 01 00 01 00`, wait for 0x01 | `ACK_MESSAGE` 0x01 |

Then the voice set-up, when the board gives one (`sct_host_t.voice`, an `sct_voice_t`). Each packet waits for its
answer before the next:

| Step | Packet | Answer, wait |
| --- | --- | --- |
| 1 | TYPE 0x00, CODEC_SELECT (the board's `codec`; 0 skips the step) | `2D 00`, 1 s |
| 2 | TYPE 0x00, VOCODER_SEL (the board's `vocoder`; 0 skips the step) | `10 00`, 3 s |
| 3 | TYPE 0x00, one packet per entry of the board's `fields`, in order (control fields such as DIGI_SPEAKER_GAIN and DIGI_MIC_GAIN) | `ID 00`, 200 ms |
| 4 | TYPE 0x40, `page register value`, one packet per entry of the board's `codec_writes`, in order | `17 00`, 200 ms |

The waits are the Packet Interface document's field timeouts; the control field's and the codec write's are what
the stock firmware allows. A field of the wrong length is not sent and fails the set-up.
An `ACK_MESSAGE` with an error code answers any step; 0x06 (processing) is waited through.

Results (`sct_boot_result_t`): `OK`, `NO_BLOCK` (no block given), `BUS` (a write failed), `NO_APP` (the chip
answered, but its application didn't start), `SILENT` (no answer in time), `NO_VOICE` (the application runs, but a
voice set-up step was refused or not answered).

The boot block is the chip vendor's code. RadioFoundry never ships it: the flashing tool takes it from the user's
own stock firmware file and writes it to the board's modem boot slot ([codec-placement.md](codec-placement.md)
describes the slot mechanism). The board checks the slot's CRC before it boots; without a valid block the radio runs without DMR.

## 3. Packets

```
84 A9 61  LEN_hi LEN_lo  TYPE  fields...  [2F parity]  [00]
```

- LEN counts the bytes after TYPE, the parity marker and byte included when parity is on.
- An odd-length packet gets one `00` pad that LEN doesn't count.
- A field is its ID byte and its data. Most IDs have a fixed data length (`sct_field_len`); a few carry a length
  byte. The engine sends parity off.

TYPE:

| Value | Meaning |
| --- | --- |
| 0x00 | control |
| 0x03 / 0x05 | dPMR / DMR fields |
| 0x04 | program (flash) |
| 0x10 | bit: to the far end |
| 0x20 | bit: read (a query, and every report from the chip) |
| 0x40 | codec bypass: the body (page, register, value) goes to the codec chip on the baseband's I2C; answered by `ACK_MESSAGE` |
| 0x80 | AMBE bypass: the packet goes to an external vocoder chip. Not a path to the baseband's own vocoder frames (section 7) |

A write is answered with a packet without the read bit whose fields are `ID status`. A report has the read bit and
the fields as written. `sct_fields_next` walks either; an unknown ID or a field cut short stops the walk with bytes
left over, and the engine counts the packet as bad.

Fields the driver uses:

| ID | Name | Data |
| --- | --- | --- |
| 0x10 | VOCODER_SEL | the vocoder: 4 AMBE+2 (the chip's default), 16 analog |
| 0x17 | ACK_MESSAGE | status code (0x00 OK, 0x01 reset ready, 0x02-0x05 errors, 0x06 processing, 0x0A far-end message sent, 0x0C-0x0E not activated / unsupported / not loaded) |
| 0x18 | WORK_MODE | 0x01 receive, 0x02 transmit |
| 0x1A | PROCESS_MODE | 0x02 layer 2 |
| 0x1B | NEW_EVENT | 1 data sync with FEC error, 4 data sync, 5 voice sync, 8 sync lost, 12 carrier, 13 carrier lost |
| 0x19 | CARRIER_READY | bits 1-0: 0 carrier lost (the default), 1 carrier ready, 2 ready with the demodulated phase reversed; bit 2 mute on a code mismatch (not sent by the driver) |
| 0x1C | DEMOD_GAIN | 2 bytes, MSB first: the demodulator's gain in Q16.11 (2048 = 0 dB); 0 auto calibration (the default) |
| 0x1D | REPORT_FIELD | which fields the chip reports. In DMR: bit 7 NEW_EVENT, bit 6 DMR_FLC, bit 5 CSBK, bit 0 slot type and EMB, bits 4-1 reserved. The chip's default is 0x70 |
| 0x2D | CODEC_SELECT | the codec chip: 1 TLV320AIC3204, 2 WM8758B (the chip's default), 3 ES8338 |
| 0x41 | MODOUT_CONFIG | 5 bytes: the delay between the two outputs of two-point modulation (signed, 10.4 µs steps, ±40), then 4 reserved bytes. Default all 0 |
| 0x43 | DIGC_DATA_FRAME | PROP, LEN, data (section 4) |
| 0x68 | DIGI_MIC_GAIN | 2 bytes, MSB first: the microphone gain in digital mode, Q16.11 (2048 = 0 dB) |
| 0x69 | DIGI_SPEAKER_GAIN | 2 bytes, the speaker gain in digital mode, the same format |
| 0x6C | SQ_LEVEL | 12 bytes, the chip's squelch: gain adjustment (dB, signed), external LNA, forced RF gain, OOBE interval (1.333 ms units), then four u16 OOBE thresholds MSB first: digital high and low, analog high and low. Default 0, 0, 0, 90, 0x4000, 0x0400, 0x4000, 0x0400. Read with the read bit, it reports RSSI and OOBE instead |
| 0x6F | DMR_CALL_SLOT | bit 7 transmit, bit 6 aligned to the received slot timing, bits 1-0 slot (3 = both) |
| 0x77 | DMR_CC | color code |
| 0x79 | DMR_CALL_OPTION | 5 bytes, below |
| 0x7A | DMR_OFFSET | 8 bytes. The Packet Interface document gives 4: the RF_TIMING output's offset against the slot timing (signed, MSB first, 41.7 µs units, ±720) and its length (240-1200, default 720). The firmware seen sends 8 |
| 0x7B | DMR_SLOT_TYPE | the received slot type |
| 0x7E | DMR_OWN_ID_BCD | 8-digit BCD |
| 0x7F | DMR_SLOT_FOUND | sync family and slot of a received burst |

DMR_CALL_OPTION, from the Packet Interface document (default all 0):

| Byte | Bits |
| --- | --- |
| 0 | 7 unused slot filled with random bits (else 0); 6 squelch (carrier) detection by the chip; 5 IF IQ receive (else baseband); 4 the chip controls the RF chip; 3 RF slot timing control; 2 RF_TIMING also in receive; 1-0 traffic mode: 0 continuous, 1 direct without the reverse channel, 2 direct with it |
| 1 | 7 IQ transmit; 6 reserved; 5-0 voice delay, 40 ms steps |
| 2 | 7-4 good CACH before DMR_SLOT_FOUND reports a verified slot; 3-0 LC header repeats |
| 3 | 7-6 reserved; report every one, not only the first, of identical: bit 4 SLC, 3 idle frames, 2 CSBKs, 1 LC headers, 0 terminators |
| 4 | reserved |

Without squelch detection by the chip (byte 0 bit 6), the chip looks for sync only after CARRIER_READY says the
carrier is there. The engine never sends CARRIER_READY, so a board sets bit 6 unless it adds that. The chip's
squelch then works with SQ_LEVEL's thresholds, its defaults unless the board sends its own.

Byte 3: an engine at the burst level takes every repeat as a burst modem gives it, so a board sets bits 1 and 0
(LC headers and terminators). With them clear the chip reports a header identical to the last one once; if it keeps
the last one from the previous call, a second call with the same LC is not reported (section 9).

## 4. Layer 2 engine

`sct_dmr` gives the core `hal_dmr_modem_t`. The chip does FEC, burst building, sync and slot timing; the stack hands
over each burst's information bits as one `DIGC_DATA_FRAME`.

PROP byte: bit 4 a voice burst (the data is the 9-byte LC for the embedded signaling), bits 3-0 the data type
(the TS 102 361-1 slot type, the same numbers as the stack's data burst kinds).

Frame lengths by kind:

| Kind | Bytes |
| --- | --- |
| voice LC header, terminator, PI header (LC kinds) | 9 |
| CSBK, MBC header, data header | 10 |
| MBC continuation, rate 1/2 data, idle | 12 |
| rate 3/4 data | 18 |
| rate 1 data | 24 |

USBD, empty and short bursts are not sent.

Start (`start`), all packets TYPE 0x05 (the voice set-up of section 2 ran once at boot):

| Field | Value |
| --- | --- |
| PROCESS_MODE | 2 |
| REPORT_FIELD | 0xE1 (NEW_EVENT, FLC, CSBK, slot type and EMB) OR the board's `report_field` |
| DMR_CALL_OPTION | the board's `call_option`, byte 0 bits 1-0 set to traffic mode 1 |
| DMR_CC, DMR_OWN_ID_BCD | the channel's |
| the board's `start_fields` | in order, as given (MODOUT_CONFIG, DMR_OFFSET, SQ_LEVEL, DEMOD_GAIN and the like) |

Then a second packet: WORK_MODE receive and DMR_CALL_SLOT 3 (or the locked slot). A start field of the wrong
length fails `start`.

Reports with IDs the engine doesn't use (DMR_CALLED_ID, DMR_OWN_ID, DMR_EMB) are walked past. An ID the length table
doesn't know stops the walk: the rest of that packet is lost and counted as bad.

Every entry point takes the board's recursive lock (`sct_dmr_port_t.lock`), so the stack's task and the board's
modem task can both call in. No RTOS calls and no waits: the host tests run the engine against a fake chip
(`tests/host/sctTest.c`).

## 5. Transmit

1. `tx_begin`: the board's `codec_tx` (when given), then WORK_MODE transmit, DMR_CALL_SLOT
   `0x80 | aligned 0x40 | slot`. The queue holds 8 bursts.
2. Each frame is sent, then the engine waits for its answer (`43 00` or ACK 0x0A). The next frame goes at least
   60 ms after the one before.
3. A frame unanswered for `SCT_DMR_ACK_MS` (300 ms) is dropped and counted; a NAK frees the slot.
4. Voice: the first voice burst of a call (and burst A after `set_embedded_lc`) sends `43 11 09 LC`. Later voice
   bursts carry nothing for the chip; they are taken one per 60 ms and reported as sent.
5. `TX_DONE` comes on ACK 0x0A after the last frame, or `SCT_DMR_DONE_MS` (600 ms) after it. Then WORK_MODE
   receive. `tx_abort` goes back to receive without `TX_DONE`.
6. After `codec_tx`, the return to receive (`TX_DONE`, `tx_abort`, `stop`) sends the board's `codec_rx` once,
   after WORK_MODE.

A codec write is answered by a TYPE 0x40 packet holding an `ACK_MESSAGE`. The engine skips TYPE 0x40 packets, so
the answer is never taken as a frame's answer or a NAK, OK or not.

## 6. Receive

| Report | Engine |
| --- | --- |
| DMR_CC | the burst's color code |
| DMR_SLOT_FOUND | sync family and slot: base station sync gives the slot from the TC bit, mobile sync from TDMA slot 1 or 2 |
| NEW_EVENT 1 | the next frame is reported with its CRC failed |
| NEW_EVENT 8 or 13 | `SYNC_LOST`; receive voice stops |
| DIGC_DATA_FRAME | a burst event to the stack |

- LC kinds are reported with `lc_valid` and 9 bytes. A voice burst's LC (PROP bit 4) is reported as a voice LC
  header.
- PI header, CSBK, MBC header and data header come with the CRC the chip already checked. The engine rebuilds the
  CRC-CCITT with its mask, so the stack sees 12 bytes as from a burst-level modem.
- The chip reports a voice call by its header (each repeat, when the board has DMR_CALL_OPTION report them), or by
  the first superframe's LC on a late entry. From there the engine reports a voice burst without payload every
  60 ms, from burst A (B for a late entry), at most 4 catching up at once, until the terminator, sync lost or
  carrier lost. A repeated header starts the count again, burst A 60 ms after it; a superframe's LC during the call
  doesn't. The stack holds the call as on other modems.

## 7. Voice on the baseband (`CAP_MODEM_VOICE`)

A board whose baseband plays and records the voice sets `CAP_MODEM_VOICE` (`hal/board.h`). The core then gives the
DMR call a voice sink and source that do nothing: no PCM, no vocoder, no `board.vocoder`. Consequences:

| Feature | With `CAP_MODEM_VOICE` |
| --- | --- |
| Voice calls, data, CSBK, SMS | work: the modem takes and reports bursts in their slots |
| Privacy (voice bit scrambling) | nothing to work on |
| Call recording, voice prompts in a call | nothing to work on |
| Hotspot voice, the store-and-forward repeater | need the AMBE frames on the MCU (not written, below) |

The board turns those switches off in its `config.mk` ([build-config.md](../build-config.md)).

The vocoder's frames can be routed through the MCU, but the driver doesn't do it yet: what the chip's frames hold is
not known well enough (section 9).

| Item | What is known |
| --- | --- |
| Routing | `VOCODER_IO_SET` (0x3E, control, one byte): bit 6 the modulator takes frames from the MCU, bits 5-4 the encoder's output to the modem (00), the MCU (01) or both (10), bit 3 the decoder takes frames from the MCU, bit 2 the demodulator's frames also go to the MCU. 0 restores the normal path. Sent at any time, so it can be set per call |
| Transport | `CHAN_D` (0x01, control): `01 LEN data`, a whole number of 9-byte frames, one frame per 20 ms. The chip sends its frames as reports and wants no answer; frames from the MCU are answered |
| Block size | 36 bytes (four frames, 80 ms) in the manual; 27 bytes (three frames, one DMR burst) is also used in DMR mode |
| Frame content | 49 bits in the first bytes of the 9 (bytes 0-5 and the top bit of byte 6); the meaning of the other 23 bits is unknown. So the chip does the FEC and the MCU sees the vocoder's parameter bits, not the 72 coded bits |
| Audio | With bit 2 alone the chip still plays the call. With bit 3 set the speaker plays what the MCU sends back |

## 8. Board glue

| Board part | Notes |
| --- | --- |
| Bus | the parallel port is bit-banged on GPIO where the stock firmware does so: data direction per transfer, strobes with a short settle, reads while HOBIB says data waits |
| Reset | supply on, strobes idle, boot strap, RSTN pulse, then the chip's start-up time (about 0.7 s) |
| Boot block | from the modem boot slot, checked by CRC-32 before use |
| Modem task | polls the chip every 2 ms under the lock, feeds each packet to `sct_dmr_packet`, then calls `sct_dmr_poll` |
| `call_option` | DMR_CALL_OPTION, all five bytes but the traffic mode: how the chip is wired (who keys the RF, baseband or IQ, squelch by the chip), CACH count and LC header repeats, which repeats are reported (every LC header and terminator, section 3). Otherwise as the stock firmware sends it |
| `report_field` | REPORT_FIELD bits added to the engine's 0xE1, as the stock firmware sends them |
| `start_fields` | more start fields, such as MODOUT_CONFIG, DMR_OFFSET and the squelch's SQ_LEVEL, with calibrated values where the stock firmware takes them from calibration. Without calibration the board sends the stock factory default |
| `codec_tx`, `codec_rx` | a codec write at the start of a transmission and its undo on the return to receive, such as an output the stock firmware mutes while transmitting |
| Voice set-up | `sct_voice_t`: CODEC_SELECT and VOCODER_SEL as the stock firmware sends them, control `fields` (the digital mode microphone and speaker gains), then the codec writes (gains and routing for the board's audio wiring). Calibrated values where the stock firmware takes them from calibration |

## 9. Open

- Whether the SCT3288 boot ROM starts the application by itself after reset, or always waits for the start command
  (both are handled) (unverified).
- DMR_CALL_OPTION byte 3 with bits 1 and 0 clear reports identical LC headers and terminators once. Whether the
  chip forgets the last one between calls is open (unverified); boards set both bits (section 3). What the reserved
  bits the stock firmware sets (bit 6 of bytes 1 and 3, byte 4, REPORT_FIELD bits 4-1) do is unknown.
- The SQ_LEVEL thresholds and the digital mode gains a board sends are the stock firmware's; whether the chip's
  squelch opens on weak signals with them, and the audio levels they give, are unverified.
- EQUALIZER_FILTER: the chip enables no filter by default, which is what the stock firmware's "no equalizer" setting
  sends, so the driver sends nothing. A user equalizer preset is not written.
- The vocoder's frames on the MCU (section 7). Needed before the driver can route them:
  - the order of the 49 bits in a `CHAN_D` frame, against the parameter order the core's FEC and privacy use, and
    what the other 23 bits carry;
  - which block size the chip uses in DMR mode, what selects it (a `CHAN_D` with length 0xFF and a count byte is
    seen, meaning unknown), and whether a block lines up with a burst and its place in the superframe (A-F);
  - with the modulator fed by the MCU: how many blocks the chip buffers, when it asks for the next, and what it sends
    when a block is late. A hotspot or repeater needs the frames of a burst on air in that burst's slot;
  - whether frames from the MCU are accepted while the encoder isn't running (a network call with the microphone
    idle).
  Each needs the chip: a received call logged with the demodulator's output routed to the MCU, then frames fed back
  on the SDR bench.
- Timing of `TX_DONE` against the last burst on air; checked against a fake chip only.
