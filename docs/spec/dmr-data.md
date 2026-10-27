# DMR data services

Text messages, packet data answers, CSBK call alert and radio check, ARS registration and LRRP location answers on
the DMR call stack; IP packets with compressed headers and confirmed delivery (11, 12). Status: built and host-tested
(`dmrDataServiceTest`, `dmrDataChainTest`). On air against the SDR bench (measured): text messages,
call alert and radio check, both ways (`tests/hil/test_dmr_data.py`); ARS and LRRP not yet (`docs/testing/on-air-coverage.md`);
IP over DMR: 11.4.

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
| `CONFIG_DMR_UDP_COMPRESS` | compressed UDP/IPv4 headers (SAP 3) on transmit (11) |
| `CONFIG_DMR_CONFIRMED` | confirmed delivery of our IP packets, and the CRC-9 check, selective ACK, NACK and duplicate drop for confirmed data to us (12) |

All default to on with DMR (`CONFIG_DMR_TMS` follows `CONFIG_DMR_SMS`). A feature switched off is compiled out of the service (`if (!CONFIG_...)` guards,
the linker drops what is unused); its API calls return false. With all five off nothing of the data path is built,
and `core/dmr/dmr_data_api.h` turns into inline no-ops so UI code needs no `#if`.

## 2. Architecture

```
modem engine (hal_dmr_modem.h)               one burst per our slot from its lookahead queue; data bursts
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
- Confirmed data to us gets an ETSI response (ACK with its N(S)) before anything else (gotcha 5.6); bad or lost
  blocks a selective ACK, a CRC-32 failure with good blocks a NACK, a repeat of the packet we ACKed last an ACK only
  (12.3).
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
| ETSI IP text (5.3), the default | `ETSI_IP` (2) | UDP 5016, UTF-16BE, full IPv4 header, SAP 4 | individual: the ETSI response to the confirmed packet (12); group: nothing |
| ETSI defined short data (5.2) | `ETSI` (1) | DD_HEAD, SAP 10, ISO 8859-1 | the ETSI response (A bit) |
| Motorola TMS (5.1), `CONFIG_DMR_TMS` | `TMS` (0) | UDP 4007, UTF-16LE with the TMS header | the TMS ACK |

### 5.1 Motorola TMS (`CONFIG_DMR_TMS`)

- Format: IPv4 / UDP port 4007 to 4007, user data SAP 4, unconfirmed data. TMS header 0xE0 (ACK requested) or 0xA0,
  sequence number in 5 bits (with bit 7 set), encoding 4, the text as UTF-16LE (Latin-1 characters only on the
  radio side). Radio IDs map to 12.x.x.x, groups to 225.x.x.x (the CAI addresses; a packet from the host may carry
  others, 11.1).
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
  computed from those clauses apart from this code (`dmrDataChainTest`, "ETSI TXT 541"); a commercial radio is still
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

- Ours (`dmrDataBuildIPText`, `dmrDataIPTextUDP`): the text as UTF-16BE (Latin-1 on the radio side: one code unit
  per character, no byte order mark, no terminator), UDP from and to port 5016, the full IPv4 / UDP header with the
  12.x.y.z / 225.x.y.z addresses of 5.1, SAP 4, rate 1/2, at most 140 characters. Always the full header, not the
  compressed one (SAP 3), whatever the compression setting (11): every receiver of the IP bearer reads SAP 4, the
  compression is an option a receiver may lack.
- 5.7.1 defines no acknowledgment of its own, so an individual message goes as confirmed data (12: C_HEAD with N(S),
  the IP packet machinery's tries and response wait), and the ETSI response ACK from the destination completes it
  (`SMS_SENT`, the UI says "Delivered to", `sms_send_confirmed()` true); no response after the tries, or a NACK that
  ends the packet: `SMS_FAILED`. It shares the service's one IP packet slot with the gateway (one at a time; the
  gateway's packet waits, as for any busy slot). Unconfirmed, done once on air ("Sent to"): to a group, and with the
  confirmed IP setting off (`ip.confirmed` off, or a build without `CONFIG_DMR_CONFIRMED`); 2 preamble CSBKs then.
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
- Individual messages go as confirmed data, so the standard's ETSI response confirms delivery (5.3); group messages
  are sent once and nothing waits for an answer.

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
- Without a position source (`hasGps` false) every immediate request is answered with
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
| `hasGps` | false | the radio core sets it false on every radio today, GPS models included (`data_init` in `core/radio/radio_sm.c`) |
| `smsFormat` | ETSI IP text | the standard text application (5.4); ETSI short data and Motorola TMS (`CONFIG_DMR_TMS`) for radios that need them. A settings record from before this default (no format bit in `ds_flags`, `settings-store.md` 3) keeps TMS: only the default changed |
| `radioCheckEtsi` | off | the Motorola form is what most radios answer |
| `udpCompress` | auto | SAP 3 saves 19-23 octets per UDP packet; auto leaves the DMR application ports (MOTOTRBO servers) on the full header (11.2) |
| `ipConfirmed`, `ipConfirmedRule[2]` | rules, none | confirmed only where the user names a destination or UDP port: an unanswered confirmed packet costs every retry on air (12.4) |
| `ipConfirmedTries` | 4 | the first transmission and 3 retries (N_RtryLmt at most 8) |
| `ipGatewayId` | 0 (none) | the DMR ID of a gateway radio that gets the host's unicast IP for addresses outside the gateway's ranges ([usb-network.md](usb-network.md) 3.1); 0 drops those packets, as before the setting. Stored in `ds_ip_gateway` |

The settings are kept in the settings store (the `ds_*` fields, `settings-store.md` 3): the radio core loads them
at start and, from its data tick, saves any change made by `dmr_data_set_settings` or the debug link's DMR_DATA
SETTINGS. `hasGps` is the board's, not saved. The codeplug does not hold them. Only committed settings are applied, never an edit in
progress (gotcha 3.7).

Transmission rules: nothing is sent unless the channel is a DMR channel we may transmit on (not RX only, in band,
not in hotspot mode, an own ID set), the channel has been quiet (`dmr_call_channel_clear`: 120 ms in direct mode,
180 ms and no repeater hang time through a repeater, gotcha 3.6), and PTT is not held. A list still waiting after 10 s
(3 s for replies) is given up (`data.tx_gave_up`).

A DMR VFO counts as a DMR channel: it sends with its own color code, timeslot and talk target, and `tx_check` covers
its frequency. An FM VFO sends nothing. A VFO retune changes the ARS channel key, so registration waits until the
frequency has held still (5 s, `ARS_SETTLE_MS`) and turning the knob sends no burst of registrations.

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

## 11. Compressed UDP/IPv4 headers on transmit (SAP 3)

Source: ETSI TS 102 361-3 V1.3.1 5.6 and 7.2. Receive was already built (`dmrDataGetUDP`, the gateway delivers SAP 3 as
UDP); transmit is `CONFIG_DMR_UDP_COMPRESS`.

### 11.1 Format

The packet is an ordinary data packet with SAP 3 in its header; the first data block starts with the compressed header
(Table 7.14), the UDP payload follows:

| Octets | Field | Ours |
|---|---|---|
| 0-1 | IPv4 identification | the same counter as our SAP 4 packets; a confirmed retry keeps it |
| 2 | SAID (7-4), DAID (3-0) | SAID 0 (radio network: the source is the source LLID), DAID 0, or 2 (group network) for a talkgroup |
| 3 | opcode MSB (7), SPID (6-0) | opcode 00 (the only format defined); SPID 1 = 5016 (UTF-16BE text), 2 = 5017 (LIP), 98 = 4007 (TMS, the manufacturer specific ID MOTOTRBO uses: an over the air capture, `dmrDataTest`), 0 = in an extended header |
| 4 | opcode LSB (7), DPID (6-0) | the same for the destination port |
| 5-6 | extended header 1 | the source port when SPID is 0, else the destination port when DPID is 0 |
| 7-8 | extended header 2 | the destination port when both are 0 |

Not sent, rebuilt by the receiver: version, IHL 5, TOS 0, total length, flags 0, fragment offset 0, TTL (64, a
talkgroup 1), protocol 17,
both checksums, the UDP length (from the packet length), and both addresses: the source ID's 12.x.y.z and the
destination ID's 12.x.y.z or, for a talkgroup, 225.x.y.z (the ID's 24 bits under the CAI network octet).

IPv4 addresses are carried apart from the DMR IDs (`dmrDataEncodeIP`, `dmrDataServiceSendIP`, `ipGatewayIPToAir` take a
source and a destination address and a TTL). The radio's own packets (TMS, ARS, LRRP, text, SNMP answers) use the IDs'
addresses, so their bytes are as before. A packet from the host may not: the off-net route sends to a gateway radio's
ID with the real destination address ([usb-network.md](usb-network.md) 3.1). So a UDP packet goes compressed only when
both of its addresses are exactly the IDs' CAI addresses; any other goes with the full header, whatever `udpCompress`
says. `dmrDataGetIP` and `dmrDataGetUDP` return the addresses: from the header for SAP 4, rebuilt from the IDs for
SAP 3. On receive a packet is for an ID itself only when its IP destination is exactly the ID's CAI address: an off-net
host's address may have the same 24 bits under another first octet. A packet to our DMR ID for another address is
passed to the gateway only, never answered by our own services (TMS, ARS, LRRP, text, SNMP).

TTL: a radio's packets go with 64, the host's keep the host's TTL, less 1 on the off-net route. A talkgroup's always
go with 1, whatever the host asked: the air is the group's one hop, and a gateway that hears one must not route it on.
`dmrDataGetIP` and `dmrDataGetUDP` return the TTL: the header's for SAP 4, the sender's default for SAP 3.

The compressed header has no length field, so a datagram goes compressed only when its UDP length agrees
with what we have (octets past a shorter UDP length are padding and dropped; a longer one goes with the full header).
IP options never reach the air: the gateway strips them for SAP 4 too, so the rule that options need the full header
holds. Receive takes opcode 00 only (others are reserved) and SPID / DPID 1, 2, 98; an unknown ID is reported as
0x8000 | ID.

### 11.2 When (the setting `udpCompress`)

| Value | UDP packets sent compressed |
|---|---|
| off | none |
| auto (default) | those whose ports are both outside 4000-4099 |
| all | every one, TMS / ARS / LRRP included |

The mode decides by the ports only. A packet whose addresses are not its IDs' CAI addresses (11.1) goes with the full
header in every mode, `all` included.

It applies to every UDP packet the radio builds: the gateway's datagrams from the host (`dmrDataServiceSendIP`) and
the service's own (`dmrDataBuildUDP`: TMS, TMS ACK, ARS, LRRP, `dmrDataServiceSendUDP`, digital APRS). ICMP and SCTP
always go as SAP 4.

Why auto: the receive side of SAP 3 is in the standard every DMR packet data radio implements, and ours reads it, so
traffic between our radios and anything new the host sends gains the 19-23 octets. The DMR application ports
4000-4099 are where MOTOTRBO presence, text and location servers listen; whether a given control station or server
decompresses depends on its configuration ("compressed UDP data header" in MOTOTRBO CPS), and our TMS / ARS / LRRP
worked with the full header before, so auto leaves them as they were. `all` is for a fleet known to take SAP 3 (the
capture above shows MOTOTRBO sending TMS that way).

### 11.3 Air time

Unconfirmed rate 1/2 packets with 2 preamble CSBKs (as we send them): one burst per 60 ms frame. Bursts = 2 + 1 header +
blocks, blocks = (user data + 4 CRC octets) / 12 rounded up (confirmed: / 10). `testAirtime` in `dmrDataTest` prints
and checks these.

| Packet | UDP payload | SAP 4 bursts | SAP 3 bursts | Saved | Confirmed SAP 4 / SAP 3 |
|---|---|---|---|---|---|
| ARS registration (7 digit ID; 4005) | 14 | 7 (420 ms) | 6 (360 ms) | 1 burst | 8 / 6 |
| TMS, 16 characters (4007, IDs 98: 5 octet header) | 38 | 9 (540 ms) | 7 (420 ms) | 2 | 10 / 8 |
| TMS ACK | 5 | 7 | 5 | 2 | 7 / 5 |
| LRRP immediate answer with a position (4001) | 26 | 8 | 7 | 1 | 9 / 7 |
| SNMP GET (UDP 161 from an ephemeral port: 9 octet header) | 60 | 11 (660 ms) | 10 (600 ms) | 1 | 13 / 11 |
| SNMP response | 120 | 16 | 15 | 1 | 19 / 17 |
| ETSI LIP short report (5017, ID 2) | 9 | 7 | 5 | 2 | 8 / 5 |

The 4000-4099 rows save only with `all`. The saving is 19 octets with both ports in extended headers, 21 with one, 23
with none: one or two rate 1/2 blocks (60-120 ms) per packet, more for confirmed data (10 octets per block). The
debug link counts what was saved (`ip.octets_saved`, `ip.blocks_saved`).

### 11.4 Tests on air

`tests/hil/test_dmr_ip.py` (radio_tx + bench_service): the debug link's IP_SEND puts a UDP datagram into the gateway's
air path as the host would, the bench modem listens, and `tools/bench/dmr_packet.py` (the standard read apart from the
firmware) reassembles and decodes it: SAP 3 with the 9 octet header in auto, SAP 4 with compression off, SAP 4 in
`all` for an IP destination other than the ID's (IP_SEND's explicit address), and the confirmed retries (12). With `--allow-usb-switch` the last test sends a real datagram from the host over the USB
network.

## 12. Confirmed data

Source: ETSI TS 102 361-1 V2.7.1 8.2.1.2, 8.2.2.2-8.2.2.4, 9.2.1, 9.3.20-9.3.27, B.3.10, B.21; TS 102 361-3 V1.3.1 5.4
(stop and wait), Annex A. `CONFIG_DMR_CONFIRMED`.

### 12.1 What already existed

Receive: confirmed packets to us were reassembled (serial number and CRC-9 skipped, the CRC-32 deciding) and ACKed
with their N(S) (3). TMS, ARS and LRRP send unconfirmed with their own application ACKs and retries (5, 6, 7); nothing
sent confirmed data. The new code reuses the response builder (now `dmrDataBuildResponse`: ACK, NACK, selective ACK),
the reply queue and its 5 s age limit, and the service's TX list and listen before talk.

### 12.2 Ours (dmrDataServiceSendIP)

- Packet: C_HEAD (DPF 3, individual, A set, the pad octet count, SAP 3 or 4, F set on the first try, the blocks in this
  transmission, S clear, N(S), FSN 1000 single fragment), then the blocks: octet 0 the 7-bit serial number and the
  CRC-9's MSB, octet 1 its other 8 bits, 10 data octets (rate 1/2); the last block ends with the CRC-32 of all data
  octets (pad included). CRC-9 (B.3.10): the block's data octets then the 7 serial bits, x^9 M(x) mod
  x^9 + x^6 + x^4 + x^3 + 1, inverted, then masked with the rate's mask (0x0F0 rate 1/2, 0x1FF rate 3/4, 0x10F rate 1).
  At most 64 blocks (one selective ACK block) and the 64-burst list: every packet of the air MTU (596 octets, 60
  blocks at rate 1/2, 12.8) goes confirmed. A packet too long for it would go unconfirmed (`ip.cnf_too_long`); none is.
- In direct mode the transmission ends with the Data Terminator LC (FLCO 0x30; octet 8: G/I, A, F, S, N(S)) that
  opens the receiver's response window (TS 102 361-3 5.4.1); through a repeater nothing follows the last block.
- Then it waits `DMR_IPC_RESPONSE_MS` (1.5 s) after the list has gone for a response from the destination with the
  packet's SAP. The standard recommends 180 ms (2 s on simulcast); ours is longer because our own receiver answers
  after its listen before talk (120 ms) and a repeater wake-up.
- ACK (class 0, type 1) with our N(S): done; the next packet to that radio takes N(S) + 1 (mod 8).
- Selective ACK (class 2, type 0): the blocks whose flag is 0 again, with the same N(S) and POC and F clear; flags
  all 1: done.
- NACK class 1 type 5 (N(S) out of sequence, status = the receiver's VI): the packet again as N(S) = VI + 1, a first
  try. Type 4 (undeliverable) and 6 (not allowed): failed at once. Other NACKs (format, CRC, memory full): the whole
  packet again, F clear.
- No response in time: as a NACK (TS 102 361-3 5.4.3.1.3), the whole packet again, F clear.
- `ipConfirmedTries` transmissions in all (default 4: the first and 3 retries; N_RtryLmt recommends at most 8), then
  failed. A packet that finds no clear channel we may send on within 10 s fails too.
- N(S) per destination: the last 4 radios; a radio not in the table starts at a number from the clock, so a receiver
  still holding our number from before a restart doesn't take the first packet as a duplicate (a strict receiver's
  out of sequence NACK resynchronizes either way).
- The gateway's packets and zone sharing's go through one TX queue (12.6). A confirmed packet is always alone in its
  transmission and the next waits for its end; unconfirmed packets to the same destination share one (12.6.2).

### 12.3 Confirmed data to us

- Each block's CRC-9 is checked; a good one is placed by its serial number. A bad one is kept in arrival order on a
  first try (the CRC-32 may still pass: then it is counted in `ip.rx_crc9_mismatch`, a sign our CRC-9 disagrees with
  the sender's) and dropped in a selective retry (its serial number can't be trusted).
- The CRC-32 decides once every block has a place. A failure with blocks bad or missing: a selective ACK with their
  flags 0 (`ip.rx_sacks_sent`); the blocks stay held for the retry (F clear, same source, destination, SAP and N(S)).
  Every block good and the CRC-32 wrong: a NACK (class 1 type 1, `ip.rx_nacks_sent`). Blocks that stop coming before
  the header's count (lost bursts): the selective ACK 300 ms after the last one.
- A selective retry for a packet we don't hold (another packet came between): a NACK, so the sender sends it whole.
- The packet again with the N(S) we ACKed last from that radio, within 20 s: ACKed, not passed on (`ip.rx_duplicates`).
  Any other N(S) is taken as new (no out of sequence NACK: senders restart their numbers).
- Built without `CONFIG_DMR_CONFIRMED` receive behaves as before (ACK, the CRC-32 alone).

### 12.4 Which packets (the settings `ipConfirmed`, `ipConfirmedRule`, `ipConfirmedTries`)

| `ipConfirmed` | Confirmed |
|---|---|
| off | none |
| rules (default) | individual packets to a radio ID in a rule, or UDP with a rule's port at either end |
| all | every individual IP packet |

Two rules (`ipConfirmedRule[2]`): 0 unused, a radio ID, or 0x80000000 | a UDP port. Group packets are never
confirmed (nobody answers for a group). Only the gateway's packets (`dmrDataServiceSendIP`) are confirmed; the
service's own TMS, ARS and LRRP keep their application ACKs. Default rules: none, because an unanswered confirmed packet
costs every retry on air, and a radio that doesn't answer confirmed data (or isn't there) turns one packet into four
transmissions.

### 12.5 Debug link

The `dmr_ip` view section (`docs/spec/debuglink.md` 3.7, section 15): settings `ip.compress ip.confirmed ip.tries ip.rule0
ip.rule1 ip.gateway_id`, `ip.busy ip.result`, compression `ip.sap3 ip.sap4 ip.octets_saved ip.blocks_saved`, the gateway's packets
`ip.packets ip.sent ip.failed`, confirmed `ip.cnf ip.cnf_tx ip.cnf_retries ip.cnf_acks ip.cnf_sacks ip.cnf_nacks
ip.cnf_resyncs ip.cnf_timeouts ip.cnf_failed ip.cnf_too_long`, receive `ip.rx_sacks_sent ip.rx_nacks_sent
ip.rx_duplicates ip.rx_crc9_mismatch`. DMR_DATA ops 9 IP_SETTINGS (the gateway ID appended) and 10 IP_SEND (an
explicit IP destination optional) (`debuglink.md` 3.12; `rfoundry_debug.py data ip-settings ... gateway=ID`,
`data ip-send ID@A.B.C.D ...`).

### 12.6 Our transmissions: the TX queue, chaining and the guard

`core/dmr/dmrAirQueue.c` (`CONFIG_DMR_AIR_QUEUE`, built with the IP gateway or zone sharing). It replaced the
gateway's one-packet mailbox (#154): a host that sent a second datagram while the first was on air had it refused.

#### 12.6.1 The queue

| Property | Value |
|---|---|
| Producers | the IP gateway (the host link task), zone sharing (any task); `airqPut`, `airqPutBatch` (all or none) |
| Buffer | a static byte ring of `CONFIG_DMR_AIR_QUEUE_BYTES` octets (default 2048); no allocation |
| Record | a 36-octet header, then the packet, rounded up to 4 octets. Each packet takes its own length, not a fixed slot |
| Count | no limit but the bytes: a put is refused only when the packet doesn't fit |
| Layout | a record never wraps. When the end of the buffer has no room, the end is left unused and the record goes at the start, so the data service reads the oldest in place while others are added |
| Order | first in, first out; the radio task's `airqTick` hands the oldest to the data service |
| Wait | the gateway's packets are offered for 2 s while the service refuses them (busy, not a DMR channel we may send on), zone sharing's for 10 s; then dropped (`airq.gave_up`) |
| Age | a gateway packet still queued after 10 s is dropped unsent (`airq.aged`): what it was for has likely moved on |
| Visibility | depth in packets and octets, both high water marks, refusals and every way a packet ends: the `air` view section ([debuglink.md](debuglink.md) 3.7, section 24); drops are also logged on the debug link |

2048 octets hold two longest packets (612) with room to spare, a zone's four fragments (a build check), or about thirteen 120-octet
datagrams. The RAM is the buffer only: raw (SAP 10) packets are built in the data service's burst list (12.8). Measured on one
board's build: RAM use rose from 101,664 to 108,056 bytes of 131,068 with the queue, zone sharing and its screens,
which leaves about 22 KB. A packet may come in two pieces (`airqItem_t.tail`), copied into the queue one after the
other, so a zone share fragment needs no buffer of its own before the queue; boards with less headroom set a smaller `CONFIG_DMR_AIR_QUEUE_BYTES` (at least 2 x (36 + 612)).

#### 12.6.2 Chaining

ETSI DMR doesn't need a gap between data packets: each has its own data header, and a receiver syncs on it.

- When the oldest queued packet is an IP packet, the queue offers it together with the IP packets right after it to
  the same destination and call type, with the same IP addresses and TTL (`dmrDataServiceSendIPChain`; the
  gateway's off-net packets carry their own, 11.1). Anything else stops the chain, so the order stays.
- The service takes the first as usual. If it goes unconfirmed, the service adds the following ones that also go
  unconfirmed, up to 8 packets and the 64-burst list ([gotchas.md](gotchas.md) 5.5). What doesn't fit goes in the
  next transmission. The list is counted with each packet's own header: a full IPv4 header (SAP 4) takes more blocks
  than a compressed one.
- Confirmed packets are never chained: a confirmed first goes alone, and a confirmed packet after it ends the chain
  and waits its turn.
- On air: the preamble CSBKs once, then each packet's data header and blocks in order. Unconfirmed packets have no
  data terminator (TS 102 361-3 5.4 defines the Terminator Data LC only after confirmed data in direct mode, to open
  the response window), so a chain ends after its last block, like a single unconfirmed packet.
- The preamble count: TS 102 361-2 7.1.2.5 and 7.2.7 define the Pre_CSBK. Its CSBK Blocks to Follow is "the number
  of Preamble CSBK PDUs and either the following CSBK or data message that will be sent", not counting the CSBK
  itself. TS 102 361-1 and -3 don't define the preamble. Each data header's Blocks to Follow (TS 102 361-1 9.3.21)
  counts its own packet's blocks only. Ours: each preamble counts the preambles after it plus every burst of the
  chain, so a scanning or sleeping receiver that trusts the count stays for the whole transmission
  (`dmrDataBuildPreambles`).
- One transmission, one result: every packet in it ends sent or failed together.
- A transmission stays within the stack's TX deadline ([gotchas.md](gotchas.md) 3.9), which grows with the list.
- Receive needs nothing new: a data header resets the receiver, also right after the previous packet's last block
  (`dmrDataRxBurst`).

#### 12.6.3 The guard

Between separate transmissions the service waits `CONFIG_DMR_DATA_TX_GUARD_MS` (default 250 ms) after our own
transmission ended before it starts another that isn't a reply. Replies (ETSI responses, CSBK answers) are not held:
the sender waits for them. The end is stamped at the first tick after the stack reports it, never before the real end.
Lists held back are counted (`data.tx_guarded`).

The guard is a workaround. On the SDR bench, #154 found receivers lost the second of two packets sent as separate
transmissions with no gap, with `data.rx_blocks_stale` on the receiver: it never saw the second transmission's
preamble CSBKs or header. The cause is not known. Candidates: our own re-key (the transmitter's ramp), or the
receiving modem's end-of-call state (200-400 ms, [gotchas.md](gotchas.md) 3.6). 250 ms was enough in every trial.
Chaining avoids the re-key for packets that can share a transmission.

For finding the cause, the debug link's DMR_DATA op 11 TX_SPACING sets the guard and turns chaining off in RAM
(`rfoundry_debug.py data tx-spacing MS [chain|no-chain]`). `tests/hil/test_dmr_tx_spacing.py` (opt-in RF) measures
delivery chained, separate with no gap, and separate with 250 ms.

### 12.7 Open

- No commercial radio has answered our confirmed packets yet, nor sent us a selective retry: the CRC-9 and the
  response formats are from the standard and the vectors (`dmr_ip_vectors.py`); `ip.rx_crc9_mismatch` on air will
  show a disagreement.
- Rate 3/4 for confirmed IP (16 octets per block) and the sliding window (TS 102 361-3 5.4.4) are not used.
- Fragmentation (FSN chains) is not built: one packet per datagram, so a datagram longer than the air MTU (12.8)
  doesn't go. The gateway drops IP fragments from the host too ([usb-network.md](usb-network.md) 3.2).

### 12.8 The air MTU

The longest IP datagram we send in one transmission. It is not below the USB link's MTU (586,
[usb-network.md](usb-network.md) 3.2), so every packet a host sends to the air fits one transmission, confirmed or not.
`core/dmr/dmrData.h` sizes the burst list and the packet buffer for it:

| Constant | Value | From |
|---|---|---|
| `DMR_DATA_MAX_PACKET` | 612 | the packet buffer: 51 rate 1/2 blocks |
| `DMR_DATA_IP_MTU` | 596 | 612 - the padding to a whole block and the 4-octet CRC-32 (16) |
| `DMR_DATA_IP_PREAMBLES` | 2 | preamble CSBKs before an IP packet (also SMS) |
| `DMR_DATA_MAX_BURSTS` | 64 | the burst list of one transmission: a confirmed packet of 596 octets |

| A packet of 596 octets | Blocks | Bursts | Air time (60 ms a burst) |
|---|---|---|---|
| Unconfirmed, rate 1/2 (12 octets a block) | 50 | 2 + 1 + 50 = 53 | about 3.2 s |
| Confirmed, rate 1/2 (10 octets a block) | 60 | 2 + 1 + 60 + the terminator = 64 | about 3.8 s |

- The MTU counts the user data on air: the IP and UDP header as sent (SAP 4: 28 octets; SAP 3: 5, 7 or 9, 11.1)
  plus the payload. The largest UDP payload is 568 octets with the full header and up to 587 with the compressed one
  (9 octets: ports outside 4000-4099 in the extended headers). A raw port packet (no header) carries up to 608.
- Confirmed (12.2) takes every packet of the MTU: 60 blocks, under the 64 of one selective ACK block. Longer packets
  are refused (`DMR_REQ_FAILED`, `ip.failed`), nothing goes on air. Build-time asserts in `dmrData.c`,
  `dmrDataService.c` and `ipGateway.c` keep the four constants and the link MTU in step.
- The gateway still checks each host packet against the air MTU before the air queue, with the header it will get
  (`ipGatewayAirLength`). On the radio none is too long; the check and its ICMP fragmentation needed answer are
  covered by a host test built with a narrower air (`ipGatewayTestAir440`; [usb-network.md](usb-network.md) 3.2).
- Receiving: the receiver reassembles up to `DMR_DATA_MAX_PACKET` (51 blocks, 608 octets of user data unconfirmed)
  from other radios. A longer packet ends in a receive error (`data.rx_crc_errors`) and nothing is delivered. The
  gateway passes the host what fits its link (586 octets of IPv4); a longer packet, from a radio that sends more than
  we do, is dropped and counted (`net.up_too_big`).
- Keyed time: the stack's TX deadline grows with the list (10 s + 60 ms a burst, [gotchas.md](gotchas.md) 3.9), so a
  64-burst list has about 13.8 s. The guard (12.6.3) is per transmission and doesn't change.
- RAM: one list of 64 bursts (27 octets each, 1,728) in the data service. The air queue builds raw packets in it
  (`dmrDataServiceSendShortData`) instead of a list of its own; the hotspot keeps its own list (it runs in the host
  link's task), placed outside the CPU-only RAM where a board has one. The packet buffers (`txPacket`, `ipPacket`,
  `dmrDataRxPacket`) are 612 octets each.
- Not done, follow-up: fragmentation (12.7: a lost burst loses the whole datagram, and a receiver that misses one
  fragment drops the rest).

## 13. Tests

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
- IP packets (11, 12): `tests/host/dmrDataTest.c` (testSap3: the compressed header's 5 / 7 / 9 octet forms against
  vectors, auto / all / off, a UDP length that disagrees, a reserved opcode; testCRC9; testConfirmedTx: our confirmed
  packet and a selective ACK byte for byte against vectors, the selective retry round trip through the receiver with a
  bad block and with a lost block, the NACK cases; testAirtime: the table in 11.3), `tests/host/dmrDataServiceTest.c`
  (testIpTx: unconfirmed SAP 3, confirmed with ACK, selective ACK, N(S) resync, time-outs to failure, tries, repeater
  mode, group, too long, no channel; testIpAirMtu: the constants, SAP 4 and SAP 3 at the air MTU unconfirmed in 53 bursts and
  decoded back, confirmed in all 64, one octet more refused with nothing on air; testConfirmedRx: selective ACK and the retry passed on once, a duplicate, lost
  bursts, NACK), `tests/host/hostlinkNetTest.c` (the gateway's datagrams through the service path: SAP 3 for ports
  outside 4000-4099, SAP 4 for 4007). The vectors come from `tests/host/dmr_ip_vectors.py` (polynomial division from
  the standard's text, apart from the C code).
- `tests/hil/test_dmr_ip.py`: on air, the bench modem decoding with `tools/bench/dmr_packet.py` (opt-in RF; 11.4).
- `tests/hil/test_dmr_ip_burst.py`: datagrams back to back each way (radio, host and bench) and the air MTU on air
  (12.8): the largest datagram each way, the receiver's limit and one block more; one octet more to the air and the
  ICMP reply to the host on firmware with a narrower air MTU only. The
  bench sends UDP with `tools/bench/bench_tx.py data --udp-port` (its offline check: `bench_tx.py selftest`).
