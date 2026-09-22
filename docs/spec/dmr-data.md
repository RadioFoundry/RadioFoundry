# DMR data services

Text messages, packet data answers, CSBK call alert and radio check, ARS registration and LRRP location answers on
the DMR call stack. Status: built and host-tested on the DM-1701 (`dmrDataServiceTest`, `dmrDataChainTest`); not yet
checked on air (`tests/hil/test_dmr_data.py`).

Sources: ETSI TS 102 361-1 (bursts, data headers, blocks, CRCs, CSBK), TS 102 361-2 (call alert, radio check),
TS 102 361-3 V1.3.1 (packet data, responses, the IP text application of 5.7.1), the public Motorola TMS / ARS / LRRP
descriptions (the formats the service
already decoded), `docs/spec/gotchas.md` 3.6-3.9, 5, 7, 14.1.

## 1. Switches

| Switch | What it adds |
|---|---|
| `CONFIG_DMR_SMS` | text messages: ETSI IP text (5.3, the default) and ETSI defined short data (5.2): receive (inbox, ETSI response, duplicate suppression), send |
| `CONFIG_DMR_TMS` | Motorola TMS format support (5.1), needs `CONFIG_DMR_SMS`: TMS receive (inbox, TMS ACK), send with ACK and retries |
| `CONFIG_DMR_ARS` | ARS registration with a presence server |
| `CONFIG_DMR_LRRP` | LRRP answers (immediate and triggered location requests) |
| `CONFIG_DMR_SIGNALING` | call alert and radio check CSBKs: answered, and sent by the user |
| `CONFIG_DMR_EMERGENCY` | the emergency alarm CSBK, sent and heard (`dmr-emergency.md`; the rest of the emergency is in the radio core) |
| `CONFIG_DMR_DATA_CORE` | derived, on when any of the above is: the packet data core (`core/dmr/dmrData.c`), the service, the ETSI responses to confirmed data |

All default to on with DMR (`CONFIG_DMR_TMS` follows `CONFIG_DMR_SMS`). A feature switched off is compiled out of the service (`if (!CONFIG_...)` guards,
the linker drops what is unused); its API calls return false. With all five off nothing of the data path is built,
and `core/dmr/dmr_data_api.h` turns into inline no-ops so UI code needs no `#if`.

## 2. Architecture

```
HR_C6000 engine (drivers/modem/hrc6000)      one burst per our slot from its lookahead queue; data bursts
   |  DMR_EV_BURST_RX (12 / 18 / 24 byte          reported with their CRC bytes and crc_ok
   v  payloads)                ^ tx_begin / tx_queue / tx_end
dmr_call (core/dmr/call)       |                 data / CSBK dispatch to the registered data RX hook;
   |  dmr_call_set_data_rx     | dmr_call_data_tx    burst list TX (phase TX_DATA), wake-up through a repeater
   v                           |
radio core glue (core/radio/radio_sm.c)          channel facts, PA keying as for PTT, dmrBurst_t <-> dmr_burst_t
   |  dmrDataServiceRxBurst / Tick / TxDone
   v
data service (core/dmr/dmrDataService.c)        reassembly, replies, requests, retries, ARS, LRRP
   ^
   |  mailbox + lock
dmr_data_api.h (UI, debug link)                  sms_inbox_*, sms_send, dmr_signal_*, events
```

- The service runs only in the radio task (`radio_sm_tick`: `dmr_call_poll`, then `dmrDataServiceTick` every
  10 ms). It never touches the modem: it hands burst lists to its host (`dmrDataEnv_t.txStart`) and gets the
  result back (`dmrDataServiceTxDone`).
- `dmr_data_api.h` may be called from any task. Requests go into a mailbox the radio task empties at its next tick;
  reads are copies under the lock given to `dmrDataServiceSetLock` (the firmware: scheduler suspend, a few
  microseconds).
- The call stack hands every burst heard on our slot whose kind is CSBK, MBC, data header, rate 1/2 / 3/4 / 1 data
  or USBD to the data hook. A voice call in progress is unaffected.
- TX (`dmr_call_data_tx`): refused while not running, while another transmission (voice or data) is on, while PTT
  is held, or while a list is already given. Through a repeater it wakes the repeater first (BS activation, as for
  voice), then sends aligned to its timing. Direct mode: aligned to the station just heard when it was heard less
  than 300 ms ago (the modem keeps a reception's slot timing for 390 ms; within that window an answer stays in step
  with the sender), else on our own timing. Access is impolite: the service has done its own listen before talk.
  One burst per frame; a list ends after its last burst has gone (TX_DONE), or after 10 s plus 60 ms per burst
  (gotcha 3.9) with `DMR_TX_ERR_TIMEOUT`. A channel change or the stack stopping drops the list (`REJECTED`).
- PTT pressed during a data list: the list finishes first (it is at most a few hundred ms), then the voice call
  starts if PTT is still held.
- No reception while transmitting (gotcha 14.1): the modem is half duplex; the service only expects answers after
  its list is over.

## 3. Receive

- CSBKs and data headers: the service checks the CRC-CCITT with its mask (CSBK 0xA5A5, data header 0xCCCC) itself
  and counts disagreements with the modem's flag (`data.rx_crc_flag_mismatch`); its own check decides.
- Blocks count only within 400 ms of the previous header, CSBK or block (gotcha 5.4); others are dropped
  (`data.rx_blocks_stale`). Rate 1/2, 3/4 and 1 blocks, confirmed (serial number and CRC-9 skipped) and unconfirmed;
  the packet's CRC-32 decides.
- Addressed to us: our ID, or a group that is the channel's talkgroup or in its listen list.
- Confirmed data to us gets an ETSI response (ACK with its N(S)) before anything else (gotcha 5.6).
- Text to us (any format, 5): kept in the inbox (4 messages, newest first, the oldest replaced). A TMS ACK request is
  answered with a TMS ACK. The same message again from the same radio within 60 s (sender, format, sequence number,
  text) is a retry: answered again, not kept twice (gotcha 5.8).
- Control station passthrough (`usb-network.md` 5.2): IP based packets to us are left to the PC; only the ETSI
  response above is still sent.
- Replies (responses, TMS ACKs, CSBK answers) wait in a 4-entry queue; one still queued after 5 s, or not started
  within 3 s for want of a clear channel, is dropped: the sender has given up (gotcha 5.7).

## 4. CSBK signaling

Formats (10 octets before the CRC; octets 4-6 and 7-9 are 24-bit radio IDs):

| CSBK | Octet 0 | FID | Octet 3 | 4-6 | 7-9 |
|---|---|---|---|---|---|
| call alert | 0x80 \| 0x1F | 0x00 | 0 | the alerted radio | the caller |
| call alert ack | 0x80 \| 0x20 | 0x00 | 0 | the caller | the alerted radio |
| radio check request | 0x80 \| 0x24 | 0x10 (Motorola) | 0x80 | the checked radio | the checker |
| radio check answer | 0x80 \| 0x24 | 0x10 | 0x00 | the checked radio | the checker |
| radio check, ETSI form | 0x80 \| 0x24 | 0x00 | 0x80 request, 0x00 answer | as above | as above |
| emergency alarm | 0x80 \| 0x27 | 0x10 or 0x00 | 0 | the group or radio | the radio in emergency (`dmr-emergency.md` 2) |

- Received call alert to us: event `CALL_ALERT` (the UI shows "Call alert from N"), acknowledged when the setting
  allows. A caller repeats until it hears the ack, so the same caller within 10 s rings once.
- Received radio check to us: counted; answered (event `RADIO_CHECK_ANSWERED`) only when the setting allows.
- Both are answered for FID 0x00 and 0x10, with the request's FID.
- Our radio check goes with FID 0x10 (Motorola), or 0x00 with the setting `radioCheckEtsi` (the "ETSI form": the
  same opcode and octets with the ETSI FID; an assumption, no ETSI text we have names it). An answer is taken with
  the IDs in either order (octets 4-6 the checked radio as the Motorola answer has it, or swapped).
- Ours (`dmr_signal_call_alert`, `dmr_signal_radio_check`): up to 3 tries, each waiting 1.5 s after the CSBK has
  gone for the answer; events `CALL_ALERT_ACKED` / `RADIO_CHECK_OK` or `..._FAILED`. One call alert or radio check
  at a time.

## 5. Text messages

Three formats. Which one a message goes in: per message (`sms_send(..., DMR_SMS_FORMAT_ETSI_IP / _ETSI / _TMS)`,
`dmrDataServiceSendSMSFormat`) or the radio's default (`DMR_SMS_FORMAT_DEFAULT`: the setting `smsFormat`, used by
the UI, Settings > Connections > Message format). A reply uses the received message's format
(`dmrDataMessage_t.format`). A per-contact format is not stored yet (the codeplug's contact has no field for it).
All three are read whatever the setting.

| Format | `dmr_sms_format_t` | Carried as | Delivery confirmed by |
|---|---|---|---|
| ETSI IP text (5.3), the default | `ETSI_IP` (2) | UDP 5016, UTF-16BE, full IPv4 header, SAP 4 | nothing yet (unconfirmed packet) |
| ETSI defined short data (5.2) | `ETSI` (1) | DD_HEAD, SAP 10, ISO 8859-1 | the ETSI response (A bit) |
| Motorola TMS (5.1), `CONFIG_DMR_TMS` | `TMS` (0) | UDP 4007, UTF-16LE with the TMS header | the TMS ACK |

### 5.1 Motorola TMS (`CONFIG_DMR_TMS`)

- Format: IPv4 / UDP port 4007 to 4007, user data SAP 4, unconfirmed data. TMS header 0xE0 (ACK requested) or 0xA0,
  sequence number in 5 bits (with bit 7 set), encoding 4, the text as UTF-16LE (Latin-1 characters only on the
  radio side). Radio IDs map to 12.x.x.x, groups to 225.x.x.x.
- Ours (`sms_send`): rate 1/2 blocks (gotcha 5.3), 2 preamble CSBKs. With an ACK requested: waits 8 s for the TMS
  ACK with our sequence number, 2 tries, then `SMS_FAILED`. To a group or without an ACK: `SMS_SENT` once on air.
  One message at a time, at most 140 characters.
- Built without `CONFIG_DMR_TMS` (the TMS builder, decoder and ACK are left out): a TMS heard is not decoded, so it
  is neither kept nor acknowledged; as confirmed data it still gets its ETSI response (3), and the IP gateway still
  forwards it to the PC like any datagram. `sms_send(..., TMS)` is refused; a stored default of TMS (a record
  written by a build with it) sends ETSI IP text instead. The UI's format list has no Motorola entry. Host test:
  `dmrDataServiceTestNoTms`.

### 5.2 ETSI short data text

- Format (defined short data, the DD_HEAD PDU of TS 102 361-1 V2.7.1 Figure 8.9 and 9.2.12): data header with DPF 13 (octet 0 bits 3:0), SAP 10 (octet 1 bits 7:4),
  the blocks to follow in octet 0 bits 5:4 and octet 1 bits 3:0, octet 0 bit 7 group, bit 6 response requested;
  octet 8 the defined data format in bits 7:2 (bit 1 S, bit 0 F: one complete message), octet 9 the bit padding;
  CRC-CCITT masked 0xCCCC. Rate 1/2 blocks follow, the last ending with the CRC-32 (B.3.9, least significant octet
  first).
- DD formats read (`dmrData.h`): 0x02 7-bit (ISO/IEC 646, packed, the bit padding gives the length), 0x03-0x11 ISO
  8859-x (all read as 8859-1), 0x12 UTF-8, 0x13 UTF-16 (with a byte order mark; big endian without one), 0x14
  UTF-16BE, 0x15 UTF-16LE; characters outside Latin-1 become '?'. Binary and BCD are not text: counted, not shown.
  The codes are TS 102 361-1 V2.7.1 Table 9.50 (9.3.38); the bit padding is 9.3.44. A host test holds one message
  computed from those clauses apart from this code (`dmrDataChainTest`, "ODR ETSI 541"); a commercial radio is still
  to be tried (open point). HIL 2026-10-10: the bench tool sent ISO 8859-1 text marked 0x02 (its bug, fixed), and the
  radio showed it, correctly for that code, as 7-bit characters.
- Also read: UDT text (TS 102 361-4, UDT header formats 3 ISO 7-bit, 4 ISO 8-bit, 7 UTF-16BE; CRC-CCITT
  unmasked, the pad nibbles in octet 8).
- Received to us: into the inbox with format ETSI (`dmrDataMessage_t.format`); with the response bit set the ETSI
  response (ACK: class 0, type 1, SAP 10) goes back. Duplicates: the same source, format and text within the TMS
  window.
- Ours: ISO 8859-1, response requested when individual (`ack`), 2 preamble CSBKs. Done (`SMS_SENT`) when the
  response ACK from the destination arrives, waiting 4 s per try, 2 tries, then `SMS_FAILED` (a NACK response is not acted on: the message
  fails when the tries run out).
  To a group: `SMS_SENT` once on air.

### 5.3 ETSI IP text

TS 102 361-3 V1.3.1 5.7.1: text messaging over the IP bearer uses UTF-16BE (plane 0) on the default UDP port 5016.
The compressed UDP/IPv4 header (5.6, 7.2.4) has a port index for it: SPID / DPID 1 = 5016 (Table 7.17 / 7.18).

- Ours (`dmrDataBuildIPText`): the text as UTF-16BE (Latin-1 on the radio side: one code unit per character, no byte
  order mark, no terminator), UDP from and to port 5016, the full IPv4 / UDP header with the 12.x.y.z / 225.x.y.z
  addresses of 5.1, SAP 4, unconfirmed, rate 1/2, 2 preamble CSBKs, at most 140 characters (29 bursts). The full
  header and not the compressed one (SAP 3): every receiver of the IP bearer reads SAP 4, the compression is an
  option a receiver may lack. 5.7.1 defines no acknowledgment of its own, so the message is done (`SMS_SENT`) once on
  air; the UI says "Sent to" rather than "Delivered to" (`sms_send_confirmed()` false). Open: sent as confirmed data
  (C_HEAD), the ETSI response would confirm delivery; the TX side of confirmed data is not built yet.
- Received (SAP 4 or SAP 3, destination port 5016): the payload decoded as UTF-16BE (`dmrDataDecodeIPText`; a byte
  order mark first is honored, either way), into the inbox with format `ETSI_IP`. No reply of its own; confirmed
  data gets its ETSI response (3). Duplicates as in 5.2.
- Through the IP gateway the PC sends and receives the same datagrams on UDP 5016 (`usb-network.md` 5).

### 5.4 The default: ETSI IP text

The default for messages the radio sends is ETSI IP text. Why this one of the two ETSI forms:

- It is the text application TS 102 361-3 defines (5.7.1). Defined short data (5.2) is a bearer with a character set
  code; no ETSI clause defines a text messaging service around it, and receivers differ in which DD formats they
  show (open point in 5.2).
- It rides the IP bearer, which any radio that takes packet data decodes for its other services too (location,
  registration, the vendor text formats): a full IPv4 / UDP header on SAP 4. A receiver needs no DPF 13 header
  support, only the UDP port and UTF-16 text. Radios and dispatch software that do ETSI text over IP read it; radios
  configured for "DMR standard" text in their CPS are expected to (to confirm on air, open point).
- Group and individual messages are sent the same way; nothing waits for an answer that a receiver may never send.

Defined short data stays available (Settings > Connections > Message format: "ETSI short") for radios that only show
that form, and it confirms delivery. Motorola TMS ("Motorola") is the format for MOTOTRBO fleets.

## 6. ARS

- Off until a server ID is set (`dmrDataSettings_t.arsServer`). Then: registers 5-15 s after power-up (spread by
  our ID so radios powered together don't collide), and again when the channel (frequency, slot, color code), our
  ID or the server has been unchanged for 5 s.
- Unacknowledged or refused: retried 5 times, 30 s apart and doubling; the state shows `no_answer` or `refused`
  meanwhile and after the last try, until something changes.
- Registered: again after the refresh time the server's answer gives (never when it says 0), else every 30 min.
- Power-off deregistration: `dmrDataServicePowerOff()` sends it and returns true until it has gone (gotcha 7.4).
  The power-off path calls it through the radio core: `radio_req_power_off()` (`RREQ_POWER_OFF`), then
  `core/app/app.c` waits while `radio_power_off_busy()`, at most 3 s, before the settings flush and the latch.

## 7. LRRP

- Immediate location requests to us are answered; triggered requests (periodic, distance) start a trigger (2 at
  most, reports at most every 5 s) with a start response, then reports.
- Without a position source (the DM-1701 has no GPS, `hasGps` false) every immediate request is answered with
  result 0x10 (no fix) and a triggered request is refused with 0x10: no trigger is started, so nothing is sent
  later on its behalf.
- `lrrpReply` off: requests are counted, not answered.
- `dmrDataServiceGetPosition` is weak: a platform with a GPS overrides it and sets `hasGps`.
- Positions elsewhere: the ETSI GPS Info LC in voice calls is in `dmr-voice.md` 3.1, positions to an APRS gateway ID
  (LRRP triggered report or UDT location) in `aprs.md` 8. Neither feeds the LRRP answers yet (open: answer with the
  APRS position when there is no GPS).
- Not ported: the UDT location reports and the GPS power management (`dmrDataServiceLocationNeededInMs` exists for
  it).

## 8. Settings and defaults

| Setting | Default | Why |
|---|---|---|
| `arsServer` | 0 (off) | no transmission the user didn't ask for (`docs/ux/principles.md`) |
| `radioCheckReply` | off | the same: a radio check answer keys the radio |
| `callAlertAck` | on | the ack is what makes a call alert work for the caller |
| `lrrpReply` | on | answers only a dispatcher's request; without GPS it says "no fix" |
| `hasGps` | false | the DM-1701 has none |
| `smsFormat` | ETSI IP text | the standard text application (5.4); ETSI short data and Motorola TMS (`CONFIG_DMR_TMS`) for radios that need them. A settings record from before this default (no format bit in `ds_flags`, `settings-store.md` 3) keeps TMS: only the default changed |
| `radioCheckEtsi` | off | the Motorola form is what most radios answer |

The settings are kept in the settings store (the `ds_*` fields, `settings-store.md` 3): the radio core loads them
at start and, from its data tick, saves any change made by `dmr_data_set_settings` or the debug link's DMR_DATA
SETTINGS. `hasGps` is the board's, not saved. The codeplug does not hold them. Only committed settings are applied, never an edit in
progress (gotcha 3.7).

Transmission rules: nothing is sent unless the channel is a DMR channel we may transmit on (not RX only, in band,
not a VFO, not in hotspot mode, an own ID set), the channel has been quiet (`dmr_call_channel_clear`: 120 ms in
direct mode, 180 ms and no repeater hang time through a repeater, gotcha 3.6), and PTT is not held. A list still
waiting after 10 s (3 s for replies) is given up (`data.tx_gave_up`).

## 9. API (core/dmr/dmr_data_api.h)

| Call | |
|---|---|
| `sms_inbox_count()`, `sms_inbox_unread()` | inbox size and unread count |
| `sms_inbox_get(i, &msg)`, `sms_inbox_mark_read(i)` | message i, 0 = newest: source, destination, group, time, unread, text |
| `sms_send(dst, group, text, ack, format)`, `sms_send_state()`, `sms_send_confirmed()` | queue a message (format ETSI_IP, ETSI, TMS or DEFAULT = the setting); state idle / queued / sending / ok / failed; whether the last one done was acknowledged |
| `dmr_signal_call_alert(dst)`, `dmr_signal_radio_check(dst)`, `dmr_signal_state(&kind, &dst)` | start one; the last one's state |
| `dmr_data_serial()`, `dmr_data_last_event()` | bumped on every event and inbox change; the last event (kind, ID, time) |
| `dmr_data_ars_state()` | off / registering / registered / refused / no_answer |
| `dmr_data_get_settings()`, `dmr_data_set_settings()` | the settings above |

The UI's minimal hook (`core/ui/ui.c`): a toast on each event ("Message from N", "Call alert from N",
"N answered", "No answer from N", "Message sent to N", "Not delivered: N"). The inbox screen and the send
screens belong to the Hub.

Debug link: the `dmr_data` and `dmr_ext` view sections and the DMR_DATA command (`docs/spec/debuglink.md` 3.7, 3.12).

## 10. Control station passthrough

With control station mode's passthrough on and the USB network link up (`usb-network.md` 5.2), the radio core calls
`dmrDataServiceSetPassthrough(true)` at every tick. Then, for packets to us or our groups that carry an IP header
(SAP 4, SAP 3):

| Service | Passthrough |
|---|---|
| ETSI response to confirmed data (3) | still sent by the radio |
| ETSI IP text (5.3), Motorola TMS (5.1) | not kept, no TMS ACK; the ACK of our own pending TMS is still noted (it sends nothing) |
| ARS answers and queries (6) | ignored; our own registration, refresh and power-off deregistration stop; the state shows `off` |
| LRRP requests (7) | not answered; queued requests and running triggers are dropped when passthrough starts |
| any other UDP, ICMP, SCTP | (the radio has no service for them) |
| defined short data and UDT text, CSBK signaling, emergency alarms | unchanged: no IP header, the PC can't answer them |

Every packet still reaches the PC through the packet hook (`dmrDataEnv_t.packet`). Counted in `data.pt_yielded`.

## 11. Tests

- `tests/host/dmrDataServiceTest.c`: the service on a fake host: replies and duplicates, confirmed data, queue
  overflow and aging, CSBK both ways, our SMS and its ACK, ARS, LRRP with and without a position, the block window,
  the mailbox; ETSI IP text (the builder's octets, the decoder with and without a byte order mark, ours by default,
  received, confirmed, group, duplicates, a stored TMS default), the passthrough table above. Built a second time
  with `CONFIG_DMR_TMS=0` (`dmrDataServiceTestNoTms`): a TMS heard is neither kept nor ACKed, confirmed still gets
  its response, TMS sends are refused.
- `tests/host/dmrDataChainTest.c`: end to end through the real slot engine (a model of the chip's receive reports
  and its page 2 / 0x40 / 0x50 writes), the real call stack and the service: call alert in and its ack out (CRC
  with CRC_MCU_Control), TMS in at rate 1/2, 3/4 and 1 and its ACK packet out, our radio check out and its answer
  in, an unanswered call alert (3 tries), a request sent 600 ms after a reception; the emergency alarm in and out;
  the ETSI text formats (ISO 8859-1, 7-bit, UTF-16 BE / LE / BOM, UTF-8, binary refused, our builder read back),
  an ETSI message in and its response out, ours out and done on the response; the radio check in the ETSI form
  both ways. Run by `make -C tests/host` (`run-rxchain`).
- `tests/hil/test_dmr_data.py`: the same on air against the SDR bench (opt-in RF).
