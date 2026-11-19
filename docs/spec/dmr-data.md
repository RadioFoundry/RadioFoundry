<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# DMR data services

This page describes the data services on the DMR call stack: text messages, packet data answers, CSBK call alert and
radio check, ARS registration and LRRP location answers, and IP packets with compressed headers (section 11) and
confirmed delivery (section 12).

Sources:

- ETSI TS 102 361-1: bursts, data headers, blocks, CRCs, CSBK.
- ETSI TS 102 361-2: call alert, radio check.
- ETSI TS 102 361-3 V1.3.1: packet data, responses, the IP text application of 5.7.1.
- The public descriptions of the TMS, ARS and LRRP formats.
- [gotchas.md](gotchas.md) 3.6-3.9, 5, 7, 14.1.

## 1. Switches

| Switch | What it adds |
|---|---|
| `CONFIG_DMR_SMS` | Text messages: ETSI IP text (5.3, the default) and ETSI defined short data (5.2). Receive (inbox, ETSI response, duplicate suppression) and send |
| `CONFIG_DMR_TMS` | Motorola TMS format support (5.1). Needs `CONFIG_DMR_SMS`. TMS receive (inbox, TMS ACK), send with ACK and retries |
| `CONFIG_DMR_ARS` | ARS registration with a presence server |
| `CONFIG_DMR_LRRP` | LRRP answers (immediate and triggered location requests) |
| `CONFIG_DMR_SIGNALING` | Call alert and radio check CSBKs: answered, and sent by the user |
| `CONFIG_DMR_EMERGENCY` | The emergency alarm CSBK, sent and heard ([dmr-emergency.md](dmr-emergency.md); the rest of the emergency is in the radio core) |
| `CONFIG_DMR_DATA_CORE` | Derived, on when any of the above is: the packet data core (`core/dmr/dmrData.c`), the service, confirmed data both ways (12) |
| `CONFIG_DMR_UDP_COMPRESS` | Compressed UDP/IPv4 headers (SAP 3) on transmit (11) |

All default to on with DMR (`CONFIG_DMR_TMS` follows `CONFIG_DMR_SMS`). A feature switched off is compiled out of the
service (`if (!CONFIG_...)` guards; the linker drops what is unused), and its API calls return false. With all five
off, nothing of the data path is built, and `core/dmr/dmr_data_api.h` turns into inline no-ops, so UI code needs no
`#if`.

## 2. Architecture

```
modem engine (hal_dmr_modem.h)               one burst per own slot from its lookahead queue; data bursts
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
  10 ms). It never touches the modem: it hands burst lists to its host (`dmrDataEnv_t.txStart`) and gets the result
  back (`dmrDataServiceTxDone`). An accepted start can complete synchronously, including a modem refusal, so the
  service marks the list running before it calls the host; that way the completion isn't lost. A start that returns
  false must not call `dmrDataServiceTxDone`.
- Any task can call `dmr_data_api.h`. Requests go into a mailbox that the radio task empties at its next tick. Reads
  are copies under the lock given to `dmrDataServiceSetLock` (in the firmware: scheduler suspend, a few
  microseconds).
- The call stack hands every burst heard on the radio's slot whose kind is CSBK, MBC, data header, rate 1/2 / 3/4 / 1
  data or USBD to the data hook. A voice call in progress is unaffected.
- TX (`dmr_call_data_tx`) is refused while the stack isn't running, while another transmission (voice or data) is on,
  while PTT is held, or while a list is already given.
  - Through a repeater, the stack wakes the repeater first (BS activation, as for voice), then sends aligned to its
    timing.
  - In direct mode, the stack aligns to the station just heard when it was heard less than 300 ms ago, else it uses
    its own timing. The modem keeps a reception's slot timing for 390 ms; within that window an answer stays in step
    with the sender.
  - Access is impolite: the service has done its own listen before talk.
  - The stack sends one burst per frame. A list ends after its last burst has gone (TX_DONE), or after 10 s plus 60 ms
    per burst (gotcha 3.9) with `DMR_TX_ERR_TIMEOUT`. A channel change or the stack stopping drops the list
    (`REJECTED`).
- PTT pressed during a data list: the list finishes first (it is at most a few hundred ms), then the voice call
  starts if PTT is still held.
- No reception while transmitting (gotcha 14.1): the modem is half duplex, and the service expects answers only after
  its list is over. The end of a transmission (TX_DONE, or REJECTED) wakes the radio task at once
  (`dmr_call_set_wake`) rather than at its next 10 ms tick. So the board has the receiver back before an answer that
  comes 30 ms after the radio's last burst (a confirmed data ACK).

## 3. Receive

- CSBKs and data headers: the service checks the CRC-CCITT with its mask (CSBK 0xA5A5, data header 0xCCCC) itself
  and counts disagreements with the modem's flag (`data.rx_crc_flag_mismatch`). Its own check decides.
- Blocks count only within 400 ms of the previous header, CSBK or block (gotcha 5.4); others are dropped
  (`data.rx_blocks_stale`). The service takes rate 1/2, 3/4 and 1 blocks, confirmed (serial number and CRC-9 skipped)
  and unconfirmed. The packet's CRC-32 decides.
- Addressed to the radio means: the radio's ID, or a group that is the channel's talkgroup or in its listen list.
- Confirmed data to the radio gets an ETSI response (ACK with its N(S)) before anything else (gotcha 5.6). Bad or lost
  blocks get a selective ACK, a CRC-32 failure with good blocks a NACK, and a repeat of the packet the radio ACKed
  last (same N(S) and data) an ACK only (12.3).
- Text to the radio (any format, 5) goes into the inbox: 4 messages, newest first, the oldest replaced. A TMS ACK
  request is answered with a TMS ACK. The same message again from the same radio within 60 s (sender, format, text;
  the sequence number can differ) is a retry: answered again, not kept twice (gotcha 5.8).
- An NMEA sentence from and to UDP port 10110 (a DMR APRS position) goes to the host's `position` hook, for the APRS
  station list ([aprs.md](aprs.md) 8.2). This holds whoever the packet is addressed to (usually the APRS gateway's
  ID), outside passthrough, and when the packet isn't the radio's own. A compressed header's DAID must be 0 (radio) or
  2 (group), not 1 (USB network, a host behind the radio). It is the one exception to "addressed to the radio":
  nothing else looks at a packet not for the radio, and it gets no response. The IP gateway still gets the packet, as
  every other.
- Control station passthrough ([usb-network.md](usb-network.md) 5.2): IP-based packets to the radio are left to the
  PC. Only the ETSI response above is still sent.
- Replies (responses, TMS ACKs, CSBK answers) wait in a 4-entry queue. A reply still queued after 5 s, or not started
  within 3 s for want of a clear channel, is dropped: the sender has given up (gotcha 5.7).

## 4. CSBK signaling

Formats (10 octets before the CRC; octets 4-6 and 7-9 are 24-bit radio IDs):

| CSBK | Octet 0 | FID | Octet 3 | 4-6 | 7-9 |
|---|---|---|---|---|---|
| Call alert | 0x80 \| 0x1F | 0x00 | 0 | The alerted radio | The caller |
| Call alert ack | 0x80 \| 0x20 | 0x00 | 0 | The caller | The alerted radio |
| Radio check request | 0x80 \| 0x24 | 0x10 (Motorola) | 0x80 | The checked radio | The checker |
| Radio check answer | 0x80 \| 0x24 | 0x10 | 0x00 | The checked radio | The checker |
| Radio check, ETSI form | 0x80 \| 0x24 | 0x00 | 0x80 request, 0x00 answer | As above | As above |
| Emergency alarm | 0x80 \| 0x27 | 0x10 or 0x00 | 0 | The group or radio | The radio in emergency ([dmr-emergency.md](dmr-emergency.md) 2) |

- A received call alert to the radio raises event `CALL_ALERT` (the UI shows "Call alert from N"), and the radio
  acknowledges it when the setting allows. A caller repeats until it hears the ack, so the same caller within 10 s
  rings once.
- A received radio check to the radio is counted, and answered (event `RADIO_CHECK_ANSWERED`) only when the setting
  allows.
- The radio answers both for FID 0x00 and 0x10, with the request's FID.
- The radio's own radio check goes with FID 0x10, or with 0x00 when the setting `radioCheckEtsi` is on. The "ETSI
  form" is the same opcode and octets with the ETSI FID; no ETSI clause names it. The radio takes an answer with the
  IDs in either order: octets 4-6 the checked radio, as the FID 0x10 answer has it, or swapped.
- The radio's own requests (`dmr_signal_call_alert`, `dmr_signal_radio_check`) get up to 3 tries. Each try waits 1.5 s
  after the CSBK has gone for the answer. Events: `CALL_ALERT_ACKED` / `RADIO_CHECK_OK`, or `..._FAILED`. One call
  alert or radio check runs at a time.

## 5. Text messages

The radio reads three formats, whatever the setting. A message the radio sends goes in one of them:

- Per message: `sms_send(..., DMR_SMS_FORMAT_ETSI_IP / _ETSI / _TMS)`, `dmrDataServiceSendSMSFormat`.
- The radio's default (`DMR_SMS_FORMAT_DEFAULT`): the setting `smsFormat`, which the UI uses (Settings > Connections >
  Message format).
- A reply uses the received message's format (`dmrDataMessage_t.format`).

The format isn't stored per contact: the codeplug's contact record has no field for it.

| Format | `dmr_sms_format_t` | Carried as | Delivery confirmed by |
|---|---|---|---|
| ETSI IP text (5.3), the default | `ETSI_IP` (2) | UDP 5016, UTF-16BE, full IPv4 header, SAP 4 | Individual: the ETSI response to the confirmed packet (12). Group: nothing |
| ETSI defined short data (5.2) | `ETSI` (1) | DD_HEAD, SAP 10, ISO 8859-1 | The ETSI response (A bit) |
| Motorola TMS (5.1), `CONFIG_DMR_TMS` | `TMS` (0) | UDP 4007, UTF-16LE with the TMS header | The TMS ACK |

### 5.1 Motorola TMS (`CONFIG_DMR_TMS`)

Format:

- IPv4 / UDP port 4007 to 4007, user data SAP 4. The radio sends it as confirmed data where 5.1.1 needs it (below),
  otherwise unconfirmed.
- TMS header 0xE0 (ACK requested) or 0xA0, the sequence number in 5 bits (with bit 7 set), encoding 4, then the text
  as UTF-16LE (Latin-1 characters only on the radio side).
- Header bits: 0x80 a sequence octet follows, 0x40 ACK requested, 0x20 set in every TMS packet, the low 5 bits the
  type (0 text, 0x1F ACK).
- The TMS ACK: header 0xBF, no address, then the sequence number. For s/n 10 it is `00 03 BF 00 0A`; past 31 a second
  octet carries bits 5-6, so s/n 126 is `00 04 BF 00 9E 60`. Commercial DMR radios and the network text service
  (5.1.1) send this form. The radio also takes an ACK with header 0x9F (0x20 clear), which other commercial equipment
  sends.
- Radio IDs map to 12.x.x.x and groups to 225.x.x.x (the CAI addresses). A packet from the host can carry other
  addresses (11.1).

The radio's text (`sms_send`):

- Rate 1/2 blocks (gotcha 5.3), 16 preamble CSBKs (12.6.4). One message at a time, at most 140 characters.
- Individual, with an ACK requested and the confirmed IP setting not off: confirmed data through the IP packet slot
  (12), as commercial DMR radios send it. The link ACK alone doesn't end it: the radio then waits 8 s for the TMS ACK
  with its sequence number, 2 tries, then `SMS_FAILED`. With no link ACK after the confirmed tries: `SMS_FAILED`. The
  TMS ACK without the link ACK (that one lost) ends it too, and its retries.
- With the confirmed IP setting off: the same 8 s and 2 tries, unconfirmed.
- To a group or without an ACK: unconfirmed, `SMS_SENT` once on air.

The radio's TMS ACK:

- Confirmed data when the confirmed IP setting isn't off and the IP packet slot is free, otherwise unconfirmed; 16
  preamble CSBKs either way. The network text service ignores an unconfirmed TMS ACK (5.1.1).
- After a link ACK from the radio (the message came confirmed), the radio waits `TMS_ACK_GAP_MS` (1.5 s) after that
  transmission ends.

UDP header compression: the radio compresses the UDP header of its confirmed TMS and TMS ACK (SAP 3, 11) only with UDP
compression set to all. Port 4007 is an application port, so auto keeps SAP 4. The network text service takes both
compressed (5.1.1), and so does the commercial portable of 12.9. Whether every commercial radio takes them depends on
its configuration (11.2), so auto stays the default.

Built without `CONFIG_DMR_TMS` (the TMS builder, decoder and ACK are left out):

- A TMS heard isn't decoded, so it is neither kept nor acknowledged. As confirmed data it still gets its ETSI response
  (3), and the IP gateway still forwards it to the PC like any datagram.
- `sms_send(..., TMS)` is refused. A stored default of TMS (a record written by a build with it) sends ETSI IP text
  instead.
- The UI's format list has no Motorola entry.
- Host test: `dmrDataServiceTestNoTms`.

#### 5.1.1 The network text service

BrandMeister's text service (ID 262993, reached through a hotspot) speaks TMS on UDP 4007. The service and the radio
behave as follows:

| | The network text service | The radio |
|---|---|---|
| Data | Confirmed (A=1), rate 3/4 | TMS and TMS ACK confirmed (A=1), rate 3/4 (12.9) |
| Preamble CSBKs | Sends 1. Answers only packets with 12 or more; to a packet with 1 or 2 it sends nothing back, not even a NACK, so a short preamble looks like a lost packet | 16 (12.6.4) |
| N(S) | 0 on every packet | Steps (12.2) |
| Link ACK | About 0.5 s after the last block, 1 preamble. It can reach the radio more than 1.5 s after the radio's transmission ends | Straight after the last block (3) |
| TMS ACK | About 5.7 s after the message, confirmed | Confirmed, 16 preambles, 1.5 s after the radio's link ACK |
| The radio's link ACK | Taken when nothing follows from the radio within 1 s; with the radio's TMS ACK 0.3 s behind it, not taken | |
| The radio's TMS ACK | A confirmed one is taken; an unconfirmed one is ignored | |
| No TMS ACK | Sends the text again after about 28 s, with a new sequence number | Link layer tries 3 s apart; after the link ACK, an 8 s wait and 2 tries |
| CRC-32 failure | NACK, class 1 type 1 | NACK (12.3) |
| Replies | Often sends a reply twice, the second with a new sequence number | Keeps it once (gotcha 5.8) |
| ETSI IP text on 5016 | Takes it from a radio, confirmed and unconfirmed. Answers in TMS on 4007, never on 5016. The text needs no leading CR LF | Sends no leading CR LF |
| Compressed UDP header (SAP 3) | Takes TMS and TMS ACK with a 5-octet compressed header and answers with a response in SAP 3; its own packets stay SAP 4 | SAP 3 only with compression set to all (11.2) |

Because N(S) can't tell the service's packets apart, a repeat is a repeat only when the data is the same too (12.3),
and a resent text with a new sequence number is kept once (gotcha 5.8).

No test in `tests/hil` or any automated or repeated test reaches the real network. Host tests use captured bytes of
the service's packets.

### 5.2 ETSI short data text

Format: defined short data, the DD_HEAD PDU of TS 102 361-1 V2.7.1 Figure 8.9 and 9.2.12.

| Where | Field |
|---|---|
| Octet 0 bits 3:0 | DPF 13 |
| Octet 1 bits 7:4 | SAP 10 |
| Octet 0 bits 5:4 and octet 1 bits 3:0 | The blocks to follow |
| Octet 0 bit 7 | Group |
| Octet 0 bit 6 | Response requested |
| Octet 8 bits 7:2 | The defined data format (bit 1 S, bit 0 F: one complete message) |
| Octet 9 | The bit padding |
| CRC | CRC-CCITT masked 0xCCCC |

Rate 1/2 blocks follow, the last ending with the CRC-32 (B.3.9, least significant octet first).

DD formats the radio reads (`dmrData.h`; TS 102 361-1 V2.7.1 Table 9.50, 9.3.38; the bit padding is 9.3.44):

| Code | Format |
|---|---|
| 0x02 | 7-bit (ISO/IEC 646), packed; the bit padding gives the length |
| 0x03-0x11 | ISO 8859-x, all read as 8859-1 |
| 0x12 | UTF-8 |
| 0x13 | UTF-16, with a byte order mark; big endian without one |
| 0x14 | UTF-16BE |
| 0x15 | UTF-16LE |

Characters outside Latin-1 become `?`. Binary and BCD aren't text: they're counted, not shown. A host test holds one
message computed from those clauses apart from this code (`dmrDataChainTest`, "ETSI TXT 541").

- The radio also reads UDT text (TS 102 361-4, UDT header formats 3 ISO 7-bit, 4 ISO 8-bit, 7 UTF-16BE; CRC-CCITT
  unmasked, the pad nibbles in octet 8).
- Received to the radio: into the inbox with format ETSI (`dmrDataMessage_t.format`). With the response bit set, the
  ETSI response (ACK: class 0, type 1, SAP 10) goes back. Duplicates: the same source, format and text within the TMS
  window.
- The radio's own: ISO 8859-1, response requested when individual (`ack`), 16 preamble CSBKs (12.6.4). Done
  (`SMS_SENT`) when the response ACK from the destination arrives. The radio waits 4 s per try, 2 tries, then
  `SMS_FAILED`. A NACK response isn't acted on: the message fails when the tries run out. To a group: `SMS_SENT` once
  on air.

### 5.3 ETSI IP text

TS 102 361-3 V1.3.1 5.7.1: text messaging over the IP bearer uses UTF-16BE (plane 0) on the default UDP port 5016.
The compressed UDP/IPv4 header (5.6, 7.2.4) has a port index for it: SPID / DPID 1 = 5016 (Table 7.17 / 7.18).

- The radio's own (`dmrDataBuildIPText`, `dmrDataIPTextUDP`): the text as UTF-16BE (Latin-1 on the radio side: one
  code unit per character, no byte order mark, no terminator), UDP from and to port 5016, the full IPv4 / UDP header
  with the 12.x.y.z / 225.x.y.z addresses of 5.1, SAP 4, rate 1/2, at most 140 characters. It always uses the full
  header, never the compressed one (SAP 3), whatever the compression setting (11): every receiver of the IP bearer
  reads SAP 4, and compression is an option a receiver can lack.
- 5.7.1 defines no acknowledgment of its own, so an individual message goes as confirmed data (12: C_HEAD with N(S),
  the IP packet machinery's tries and response wait). The ETSI response ACK from the destination completes it
  (`SMS_SENT`, the UI says "Delivered to", `sms_send_confirmed()` true). No response after the tries, or a NACK that
  ends the packet, gives `SMS_FAILED`. It shares the service's one IP packet slot with the gateway (one at a time; the
  gateway's packet waits, as for any busy slot).
- Unconfirmed, done once on air ("Sent to"): to a group, and with the confirmed IP setting off (`ip.confirmed` off).
- Received (SAP 4 or SAP 3, destination port 5016): the payload is decoded as UTF-16BE (`dmrDataDecodeIPText`; a byte
  order mark first is honored, either way) into the inbox with format `ETSI_IP`. It has no reply of its own;
  confirmed data gets its ETSI response (3). Duplicates as in 5.2.
- Through the IP gateway, the PC sends and receives the same datagrams on UDP 5016
  ([usb-network.md](usb-network.md) 5).

### 5.4 The default: ETSI IP text

The default for messages the radio sends is ETSI IP text, for these reasons:

- It is the text application TS 102 361-3 defines (5.7.1). Defined short data (5.2) is a bearer with a character set
  code; no ETSI clause defines a text messaging service around it, and receivers differ in which DD formats they
  show.
- It rides the IP bearer, which any radio that takes packet data decodes for its other services too (location,
  registration, manufacturer text formats): a full IPv4 / UDP header on SAP 4. A receiver needs no DPF 13 header
  support, only the UDP port and UTF-16 text. Radios and dispatch software that do ETSI text over IP read it.
- Individual messages go as confirmed data, so the standard's ETSI response confirms delivery (5.3). Group messages
  are sent once, and nothing waits for an answer.

Defined short data stays available (Settings > Connections > Message format: "ETSI short") for radios that show only
that form, and it confirms delivery. Motorola TMS ("Motorola") is the format for fleets of commercial DMR radios set
to TMS.

## 6. ARS

- ARS is off until a server ID is set (`dmrDataSettings_t.arsServer`). Then the radio registers 5-15 s after power-up
  (spread by its ID, so radios powered together don't collide). It registers again when the channel (frequency, slot,
  color code), its ID or the server has changed and then held for 5 s.
- Unacknowledged or refused: retried 5 times, 30 s apart and doubling. The state shows `no_answer` or `refused`
  meanwhile and after the last try, until something changes.
- Registered: the radio registers again after the refresh time the server's answer gives (never when it says 0), else
  every 30 min.
- Power-off deregistration: `dmrDataServicePowerOff()` sends it and returns true until it has gone (gotcha 7.4). The
  power-off path calls it through the radio core: `radio_req_power_off()` (`RREQ_POWER_OFF`), then `core/app/app.c`
  waits while `radio_power_off_busy()`, at most 3 s, before the settings flush and the latch.

## 7. LRRP

- The radio answers immediate location requests to it. A triggered request (periodic, distance) starts a trigger (2
  at most, reports at most every 5 s) with a start response, then reports.
- The position is the position service's ([position.md](position.md)), through the `ownPosition` hook the radio core
  sets (`position_report`: the board GPS, the host's NMEA or the fixed position, ambiguity applied). It needs no APRS
  and no GPS receiver: `CONFIG_DMR_LRRP` builds the service, and `hasGps` is set in every such build.
- While there is no position (no fix and no fixed position), the radio answers an immediate request with result 0x10
  (no fix). A triggered request starts its trigger; a report due without a position is skipped.
- Built without a position source (`hasGps` false: a platform that sets no hook), the radio answers every immediate
  request with result 0x10 (no fix) and refuses a triggered request with 0x10. No trigger starts, so nothing is sent
  later on its behalf.
- With `lrrpReply` off, requests are counted, not answered.
- Positions elsewhere: the ETSI GPS Info LC in voice calls is in [dmr-voice.md](dmr-voice.md) 3.1, and positions to an
  APRS gateway ID (LRRP triggered report or UDT location) in [aprs.md](aprs.md) 8. All of them read the same position.
- The service sends no UDT location reports and has no GPS power management. `dmrDataServiceLocationNeededInMs`
  exists for that.

## 8. Settings and defaults

| Setting | Default | Why |
|---|---|---|
| `arsServer` | 0 (off) | No transmission the user didn't ask for ([principles.md](../ux/principles.md)) |
| `radioCheckReply` | on | A radio check is how another user finds out whether this radio is on and in range; the answer is one short CSBK. A settings record saved before this default (no `SETTINGS_DS_CHECK_SET` in `ds_ip_cnf`, [settings-store.md](settings-store.md) 3) answers too |
| `callAlertAck` | on | The ack is what makes a call alert work for the caller |
| `lrrpReply` | on | Answers only a dispatcher's request; without a position (no fix, no fixed position) it says "no fix" |
| `hasGps` | true with `CONFIG_DMR_LRRP` | The radio core sets it when the position service answers (`data_init` in `core/radio/radio_sm.c`, `ownPosition`); a source can come after start-up (7) |
| `smsFormat` | ETSI IP text | The standard text application (5.4); ETSI short data and Motorola TMS (`CONFIG_DMR_TMS`) for radios that need them. A settings record from before this default (no format bit in `ds_flags`, [settings-store.md](settings-store.md) 3) keeps TMS: only the default changed |
| `radioCheckEtsi` | off | The FID 0x10 form is what most radios answer |
| `udpCompress` | auto | SAP 3 saves 19-23 octets per UDP packet; auto leaves the DMR application ports (commercial servers) on the full header (11.2) |
| `ipConfirmed`, `ipConfirmedRule[2]` | rules, none | Confirmed only where the user names a destination or UDP port: an unanswered confirmed packet costs every retry on air (12.4) |
| `ipConfirmedTries` | 4 | The first transmission and 3 retries (N_RtryLmt at most 8) |
| `ipGatewayId` | 0 (none) | The DMR ID of a gateway radio that gets the host's unicast IP for addresses outside the gateway's ranges ([usb-network.md](usb-network.md) 3.1); 0 drops those packets. Stored in `ds_ip_gateway` |

The settings store keeps the settings (the `ds_*` fields, [settings-store.md](settings-store.md) 3). The radio core
loads them at start and, from its data tick, saves any change made by `dmr_data_set_settings` or the debug link's
DMR_DATA SETTINGS. `hasGps` is the board's, not saved. The ARS server and the IP gateway are also in the codeplug's
settings area ([codeplug-settings.md](codeplug-settings.md) 5): a key there holds the setting, and the settings store
keeps the radio's own value for a codeplug without the key. Only committed settings apply, never an edit in progress
(gotcha 3.7).

Transmission rules: the radio sends nothing unless all of these hold:

- The channel is a DMR channel the radio can transmit on: not RX only, in band, not in hotspot mode, an own ID set.
- The channel has been quiet (`dmr_call_channel_clear`: 120 ms in direct mode; 180 ms and no repeater hang time
  through a repeater; gotcha 3.6).
- PTT isn't held.

A list still waiting after 10 s (3 s for replies) is given up (`data.tx_gave_up`).

A DMR VFO counts as a DMR channel: it sends with its own color code, timeslot and talk target, and `tx_check` covers
its frequency. An FM VFO sends nothing. A VFO retune changes the ARS channel key, so registration waits until the
frequency has held still (5 s, `ARS_SETTLE_MS`), and turning the knob sends no burst of registrations.

## 9. API (core/dmr/dmr_data_api.h)

| Call | What it does |
|---|---|
| `sms_inbox_count()`, `sms_inbox_unread()` | Inbox size and unread count |
| `sms_inbox_get(i, &msg)`, `sms_inbox_mark_read(i)` | Message i, 0 = newest: source, destination, group, time, unread, text |
| `sms_send(dst, group, text, ack, format)`, `sms_send_state()`, `sms_send_confirmed()` | Queue a message (format ETSI_IP, ETSI, TMS or DEFAULT = the setting); state idle / queued / sending / ok / failed; whether the last one done was acknowledged |
| `dmr_signal_call_alert(dst)`, `dmr_signal_radio_check(dst)`, `dmr_signal_state(&kind, &dst)` | Start one; the last one's state |
| `dmr_data_serial()`, `dmr_data_last_event()` | Bumped on every event and inbox change; the last event (kind, ID, time) |
| `dmr_data_ars_state()` | off / registering / registered / refused / no_answer |
| `dmr_data_get_settings()`, `dmr_data_set_settings()` | The settings above |

The UI's minimal hook (`core/ui/ui.c`) shows a toast on each event: "Message from N", "Call alert from N", "N
answered", "No answer from N", "Message sent to N", "Not delivered: N". The inbox screen and the send screens belong
to the Hub.

Debug link: the `dmr_data` and `dmr_ext` view sections and the DMR_DATA command ([debuglink.md](debuglink.md) 3.7,
3.12).

## 10. Control station passthrough

With control station mode's passthrough on and the USB network link up ([usb-network.md](usb-network.md) 5.2), the
radio core calls `dmrDataServiceSetPassthrough(true)` at every tick. Then, for packets to the radio or its groups that
carry an IP header (SAP 4, SAP 3):

| Service | Passthrough |
|---|---|
| ETSI response to confirmed data (3) | Still sent by the radio |
| ETSI IP text (5.3), Motorola TMS (5.1) | Not kept, no TMS ACK; the ACK of the radio's own pending TMS is still noted (it sends nothing) |
| ARS answers and queries (6) | Ignored; the radio's own registration, refresh and power-off deregistration stop; the state shows `off` |
| LRRP requests (7) | Not answered; queued requests and running triggers are dropped when passthrough starts |
| Any other UDP, ICMP, SCTP | (The radio has no service for them) |
| Defined short data and UDT text, CSBK signaling, emergency alarms | Unchanged: no IP header, the PC can't answer them |

Every packet still reaches the PC through the packet hook (`dmrDataEnv_t.packet`). Counted in `data.pt_yielded`.

## 11. Compressed UDP/IPv4 headers on transmit (SAP 3)

Source: ETSI TS 102 361-3 V1.3.1 5.6 and 7.2. The radio always reads SAP 3 (`dmrDataGetUDP`; the gateway delivers SAP
3 as UDP). `CONFIG_DMR_UDP_COMPRESS` adds transmit.

### 11.1 Format

The packet is an ordinary data packet with SAP 3 in its header. The first data block starts with the compressed
header (Table 7.14), and the UDP payload follows:

| Octets | Field | The radio's value |
|---|---|---|
| 0-1 | IPv4 identification | The same counter as its SAP 4 packets; a confirmed retry keeps it |
| 2 | SAID (7-4), DAID (3-0) | SAID 0 (radio network: the source is the source LLID), DAID 0, or 2 (group network) for a talkgroup |
| 3 | Opcode MSB (7), SPID (6-0) | Opcode 00 (the only format defined); SPID 1 = 5016 (UTF-16BE text), 2 = 5017 (LIP), 98 = 4007 (TMS, the manufacturer-specific ID commercial DMR radios use; `dmrDataTest` holds an example), 0 = in an extended header |
| 4 | Opcode LSB (7), DPID (6-0) | The same for the destination port |
| 5-6 | Extended header 1 | The source port when SPID is 0, else the destination port when DPID is 0 |
| 7-8 | Extended header 2 | The destination port when both are 0 |

The receiver rebuilds what isn't sent: version, IHL 5, TOS 0, total length, flags 0, fragment offset 0, TTL (64, a
talkgroup 1), protocol 17, both checksums, the UDP length (from the packet length), and both addresses. The addresses
are the source ID's 12.x.y.z and the destination ID's 12.x.y.z or, for a talkgroup, 225.x.y.z (the ID's 24 bits under
the CAI network octet).

IPv4 addresses are carried apart from the DMR IDs (`dmrDataEncodeIP`, `dmrDataServiceSendIP` and `ipGatewayIPToAir`
take a source and a destination address and a TTL):

- The radio's own packets (TMS, ARS, LRRP, text, SNMP answers) use the IDs' addresses.
- A packet from the host might not: the off-net route sends to a gateway radio's ID with the real destination address
  ([usb-network.md](usb-network.md) 3.1). So a UDP packet goes compressed only when both of its addresses are exactly
  the IDs' CAI addresses. Any other goes with the full header, whatever `udpCompress` says.
- `dmrDataGetIP` and `dmrDataGetUDP` return the addresses: from the header for SAP 4, rebuilt from the IDs for SAP 3.
- On receive, a packet is for an ID itself only when its IP destination is exactly the ID's CAI address: an off-net
  host's address can have the same 24 bits under another first octet. A packet to the radio's DMR ID for another
  address goes to the gateway only, and the radio's own services (TMS, ARS, LRRP, text, SNMP) never answer it.

TTL: the radio's packets go with 64, and the host's keep the host's TTL, less 1 on the off-net route. A talkgroup's
packets always go with 1, whatever the host asked: the air is the group's one hop, and a gateway that hears one must
not route it on. `dmrDataGetIP` and `dmrDataGetUDP` return the TTL: the header's for SAP 4, the sender's default for
SAP 3.

The compressed header has no length field, so a datagram goes compressed only when its UDP length agrees with the
data length. Octets past a shorter UDP length are padding and dropped; a longer one goes with the full header. IP
options never reach the air: the gateway strips them for SAP 4 too, so the rule that options need the full header
holds. Receive takes opcode 00 only (others are reserved) and SPID / DPID 1, 2, 98; an unknown ID is reported as
0x8000 | ID.

### 11.2 When (the setting `udpCompress`)

| Value | UDP packets sent compressed |
|---|---|
| off | None |
| auto (default) | Those whose ports are both outside 4000-4099 |
| all | Every one, TMS / ARS / LRRP included |

The mode decides by the ports only. A packet whose addresses aren't its IDs' CAI addresses (11.1) goes with the full
header in every mode, `all` included.

It applies to every UDP packet the radio builds: the gateway's datagrams from the host (`dmrDataServiceSendIP`) and
the service's own (`dmrDataBuildUDP`: TMS, TMS ACK, ARS, LRRP, `dmrDataServiceSendUDP`, digital APRS). ICMP and SCTP
always go as SAP 4.

Why auto: the receive side of SAP 3 is in the standard every DMR packet data radio implements, and RadioFoundry reads
it, so traffic between RadioFoundry radios, and anything new the host sends, gains the 19-23 octets. The DMR
application ports 4000-4099 are where commercial presence, text and location servers listen. Whether a given control
station or server decompresses depends on its configuration (a "compressed UDP data header" option in its CPS), and
TMS, ARS and LRRP work with the full header, so auto leaves them on it. `all` is for a fleet known to take SAP 3;
commercial DMR radios can send TMS that way.

### 11.3 Air time

Unconfirmed rate 1/2 packets with 2 preamble CSBKs send one burst per 60 ms frame:

- Bursts = 2 + 1 header + blocks. Up to 14 more preambles go where they fit (12.6.4): add 0.84 s.
- Blocks = (user data + 4 CRC octets) / 12, rounded up (confirmed: / 10).

`testAirtime` in `dmrDataTest` prints and checks these.

| Packet | UDP payload | SAP 4 bursts | SAP 3 bursts | Saved | Confirmed SAP 4 / SAP 3 |
|---|---|---|---|---|---|
| ARS registration (7 digit ID; 4005) | 14 | 7 (420 ms) | 6 (360 ms) | 1 burst | 8 / 6 |
| TMS, 16 characters (4007, IDs 98: 5 octet header) | 38 | 9 (540 ms) | 7 (420 ms) | 2 | 10 / 8 |
| TMS ACK | 5 | 7 | 5 | 2 | 7 / 5 |
| LRRP immediate answer with a position (4001) | 26 | 8 | 7 | 1 | 9 / 7 |
| SNMP GET (UDP 161 from an ephemeral port: 9 octet header) | 60 | 11 (660 ms) | 10 (600 ms) | 1 | 13 / 11 |
| SNMP response | 120 | 16 | 15 | 1 | 19 / 17 |
| ETSI LIP short report (5017, ID 2) | 9 | 7 | 5 | 2 | 8 / 5 |

The 4000-4099 rows save only with `all`. The saving is 19 octets with both ports in extended headers, 21 with one,
and 23 with none: one or two rate 1/2 blocks (60-120 ms) per packet, more for confirmed data (10 octets per block).
The debug link counts what was saved (`ip.octets_saved`, `ip.blocks_saved`).

### 11.4 Tests on air

`tests/hil/test_dmr_ip.py` (radio_tx + bench_service): the debug link's IP_SEND puts a UDP datagram into the gateway's
air path as the host would. The bench modem listens, and `tools/bench/dmr_packet.py` (the standard, read apart from
the firmware) reassembles and decodes it. The tests check:

- SAP 3 with the 9 octet header in auto.
- SAP 4 with compression off.
- SAP 4 in `all` for an IP destination other than the ID's (IP_SEND's explicit address).
- The confirmed retries (12).
- With `--allow-usb-switch`, a real datagram from the host over the USB network.

## 12. Confirmed data

Source: ETSI TS 102 361-1 V2.7.1 8.2.1.2, 8.2.2.2-8.2.2.4, 9.2.1, 9.3.20-9.3.27, B.3.10, B.21; TS 102 361-3 V1.3.1 5.4
(stop and wait), Annex A. Confirmed data is part of the data core, with no switch of its own, since text to the network
text service and to commercial DMR radios needs it (5.1.1).

### 12.1 Receive and application ACKs

- Receive: the radio reassembles confirmed packets to it and ACKs them with their N(S) (3, 12.3).
- TMS, ARS and LRRP send unconfirmed with their own application ACKs and retries (5, 6, 7), except where 5.1 sends
  TMS confirmed.
- Confirmed sending shares the response builder (`dmrDataBuildResponse`: ACK, NACK, selective ACK), the reply queue
  and its 5 s age limit, and the service's TX list and listen before talk.

### 12.2 Sending (dmrDataServiceSendIP)

- Packet: C_HEAD (DPF 3, individual, A set, the pad octet count, SAP 3 or 4, F set on the first try, the blocks in
  this transmission, S clear, N(S), FSN 1000 single fragment), then the blocks. Each block holds, in octet 0, the
  7-bit serial number and the CRC-9's MSB; in octet 1, the CRC-9's other 8 bits; then 16 data octets (rate 3/4, 12.9).
  The last block ends with the CRC-32 of all data octets (pad included).
- CRC-9 (B.3.10): the block's data octets, then the 7 serial bits; x^9 M(x) mod x^9 + x^6 + x^4 + x^3 + 1, inverted,
  then masked with the rate's mask (0x0F0 rate 1/2, 0x1FF rate 3/4, 0x10F rate 1).
- At most 64 blocks (one selective ACK block) and the 64-burst list: every packet of the air MTU (596 octets, 38
  blocks at rate 3/4, 12.8) goes confirmed. A packet too long for it would go unconfirmed (`ip.cnf_too_long`); no
  packet is that long.
- In direct mode the transmission ends with the Data Terminator LC (FLCO 0x30; octet 8: G/I, A, F, S, N(S)), which
  opens the receiver's response window (TS 102 361-3 5.4.1). Through a repeater nothing follows the last block.
- Then the radio waits `DMR_IPC_RESPONSE_MS` (3 s) after the list has gone for a response from the destination with
  the packet's SAP. The standard recommends 180 ms (2 s on simulcast). The radio waits longer because another
  RadioFoundry radio answers after its listen before talk (120 ms) and a repeater wake-up, and through a hotspot the
  answer comes from a network service: the network text service's link ACK can arrive more than 1.5 s after the
  transmission ends (5.1.1).
- The radio also takes an ACK with its N(S) before the stack has reported its list done (a fast answer, such as the
  network text service's through a hotspot, can come first: 5.1.1), and while the next try is queued (its retry is
  dropped). It doesn't take a selective ACK or NACK then: the try under way answers for it.

The radio handles the response as follows:

| Response | Action |
|---|---|
| ACK (class 0, type 1) with the radio's N(S) | Done. The next packet to that radio takes N(S) + 1 (mod 8) |
| Selective ACK (class 2, type 0) | The blocks whose flag is 0 again, with the same N(S) and POC and F clear. Flags all 1: done |
| NACK class 1 type 5 (N(S) out of sequence, status = the receiver's VI) | The packet again as N(S) = VI + 1, a first try |
| NACK type 4 (undeliverable) or 6 (not allowed) | Failed at once |
| Other NACKs (format, CRC, memory full) | The whole packet again, F clear |
| No response in time | As a NACK (TS 102 361-3 5.4.3.1.3): the whole packet again, F clear |

- The radio sends `ipConfirmedTries` transmissions in all (default 4: the first and 3 retries; N_RtryLmt recommends
  at most 8), then the packet fails. A packet that finds no clear channel the radio can send on within 10 s fails too.
- N(S) is kept per destination for the last 4 radios. A radio not in the table starts at a number from the clock, so a
  receiver still holding the radio's number from before a restart doesn't take the first packet as a duplicate. A
  strict receiver's out of sequence NACK resynchronizes either way.
- The gateway's packets and zone sharing's go through one TX queue (12.6). A confirmed packet is always alone in its
  transmission, and the next waits for its end. Unconfirmed packets to the same destination share one (12.6.2).

### 12.3 Confirmed data to the radio

- The radio checks each block's CRC-9 and places a good block by its serial number. On a first try it keeps a bad
  block in arrival order: the CRC-32 can still pass, and then it counts in `ip.rx_crc9_mismatch`, a sign that the
  radio's CRC-9 disagrees with the sender's. In a selective retry it drops a bad block (its serial number can't be
  trusted).
- The CRC-32 decides once every block has a place.
  - A failure with blocks bad or missing: a selective ACK with their flags 0 (`ip.rx_sacks_sent`). The blocks stay
    held for the retry (F clear, same source, destination, SAP and N(S)).
  - Every block good and the CRC-32 wrong: a NACK (class 1 type 1, `ip.rx_nacks_sent`).
  - Blocks that stop coming before the header's count (lost bursts): the selective ACK 300 ms after the last one.
- A selective retry for a packet the radio doesn't hold (another packet came between) gets a NACK, so the sender sends
  it whole.
- The packet again, with the N(S) and the data (a 16-bit fold of its CRC-32) that the radio ACKed last from that
  radio, within 20 s of the last copy: ACKed, not passed on (`ip.rx_duplicates`). The standard keys on N(S) alone, but
  the network text service sends N(S) 0 on every packet (5.1.1), so other data with the same N(S) is a new packet. Any
  other N(S) is taken as new, with no out of sequence NACK: senders restart their numbers.

### 12.4 Which packets (the settings `ipConfirmed`, `ipConfirmedRule`, `ipConfirmedTries`)

| `ipConfirmed` | Confirmed |
|---|---|
| off | None |
| rules (default) | Individual packets to a radio ID in a rule, or UDP with a rule's port at either end |
| all | Every individual IP packet |

There are two rules (`ipConfirmedRule[2]`); each is 0 (unused), a radio ID, or 0x80000000 | a UDP port. Group packets
are never confirmed: nobody answers for a group. Only the gateway's packets (`dmrDataServiceSendIP`) are confirmed by
these settings; the service's own TMS, ARS and LRRP keep their application ACKs. The default is no
rules, because an unanswered confirmed packet costs every retry on air: a radio that doesn't answer confirmed data
(or isn't there) turns one packet into four transmissions.

### 12.5 Debug link

The `dmr_ip` view section ([debuglink.md](debuglink.md) 3.7, section 15):

| Group | Keys |
|---|---|
| Settings | `ip.compress ip.confirmed ip.tries ip.rule0 ip.rule1 ip.gateway_id` |
| State | `ip.busy ip.result` |
| Compression | `ip.sap3 ip.sap4 ip.octets_saved ip.blocks_saved` |
| The gateway's packets | `ip.packets ip.sent ip.failed` |
| Confirmed | `ip.cnf ip.cnf_tx ip.cnf_retries ip.cnf_acks ip.cnf_sacks ip.cnf_nacks ip.cnf_resyncs ip.cnf_timeouts ip.cnf_failed ip.cnf_too_long` |
| Responses to the radio that didn't answer its packet | `ip.cnf_other` (count) and `ip.cnf_other_last` (the last, packed: the response octet in bits 7:0, its SAP 11:8, the radio's SAP 15:12, the radio's N(S) 18:16, its source the radio's destination 19, the radio's state 21:20 with 3 waiting, confirmed 22) |
| Receive | `ip.rx_sacks_sent ip.rx_nacks_sent ip.rx_duplicates ip.rx_crc9_mismatch` |

DMR_DATA ops 9 IP_SETTINGS (the gateway ID appended) and 10 IP_SEND (an explicit IP destination optional)
([debuglink.md](debuglink.md) 3.12; `rfoundry_debug.py data ip-settings ... gateway=ID`,
`data ip-send ID@A.B.C.D ...`).

### 12.6 The radio's transmissions: the TX queue, chaining and the guard

`core/dmr/dmrAirQueue.c` (`CONFIG_DMR_AIR_QUEUE`, built with the IP gateway or zone sharing) holds what the gateway
and zone sharing send, so a host can send a second datagram while the first is on air.

#### 12.6.1 The queue

| Property | Value |
|---|---|
| Producers | The IP gateway (the host link task), zone sharing (any task); `airqPut`, `airqPutBatch` (all or none) |
| Buffer | A static byte ring of `CONFIG_DMR_AIR_QUEUE_BYTES` octets (default 2048); no allocation |
| Record | A 36-octet header, then the packet, rounded up to 4 octets. Each packet takes its own length, not a fixed slot |
| Count | No limit but the bytes: a put is refused only when the packet doesn't fit |
| Layout | A record never wraps. When the end of the buffer has no room, the end is left unused and the record goes at the start, so the data service reads the oldest in place while others are added |
| Order | First in, first out; the radio task's `airqTick` hands the oldest to the data service |
| Wait | The gateway's packets are offered for 2 s while the service refuses them (busy, not a DMR channel the radio can send on), zone sharing's for 10 s; then dropped (`airq.gave_up`) |
| Age | A gateway packet still queued after 10 s is dropped unsent (`airq.aged`): what it was for has likely moved on |
| Visibility | Depth in packets and octets, both high water marks, refusals and every way a packet ends: the `air` view section ([debuglink.md](debuglink.md) 3.7, section 24); drops are also logged on the debug link |

2048 octets hold two longest packets (612) with room to spare, a zone's four fragments (a build check), or about
thirteen 120-octet datagrams. The RAM is the buffer only: raw (SAP 10) packets are built in the data service's burst
list (12.8). A packet can come in two pieces (`airqItem_t.tail`), copied into the queue one after the other, so a
zone share fragment needs no buffer of its own before the queue. Boards with less RAM headroom set a smaller
`CONFIG_DMR_AIR_QUEUE_BYTES` (at least 2 x (36 + 612)).

#### 12.6.2 Chaining

ETSI DMR doesn't need a gap between data packets: each has its own data header, and a receiver syncs on it.

- When the oldest queued packet is an IP packet, the queue offers it together with the IP packets right after it to
  the same destination and call type, with the same IP addresses and TTL (`dmrDataServiceSendIPChain`; the gateway's
  off-net packets carry their own, 11.1). Anything else stops the chain, so the order stays.
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
  counts its own packet's blocks only. In the radio's chains, each preamble counts the preambles after it plus every
  burst of the chain, so a scanning or sleeping receiver that trusts the count stays for the whole transmission
  (`dmrDataBuildPreambles`).
- One transmission has one result: every packet in it ends sent or failed together.
- A transmission stays within the stack's TX deadline ([gotchas.md](gotchas.md) 3.9), which grows with the list.
- Receive needs nothing extra: a data header resets the receiver, also right after the previous packet's last block
  (`dmrDataRxBurst`).

#### 12.6.3 The guard

Between separate transmissions, the service waits `CONFIG_DMR_DATA_TX_GUARD_MS` (default 250 ms) after the radio's own
transmission ended before it starts another that isn't a reply. Replies (ETSI responses, CSBK answers) aren't held:
the sender waits for them. The end is stamped at the first tick after the stack reports it, never before the real end.
Lists held back are counted (`data.tx_guarded`).

The guard works around a receive loss: receivers lose the second of two packets sent as separate transmissions with
no gap. The receiver then shows `data.rx_blocks_stale`: it never saw the second transmission's preamble CSBKs or
header. A 250 ms gap prevents the loss. Chaining avoids the re-key for packets that can share a transmission.

The debug link's DMR_DATA op 11 TX_SPACING sets the guard and turns chaining off in RAM
(`rfoundry_debug.py data tx-spacing MS [chain|no-chain]`). `tests/hil/test_dmr_tx_spacing.py` (opt-in RF) measures
delivery chained, separate with no gap, and separate with 250 ms.

#### 12.6.4 Preamble count

A text or IP packet from the radio goes with `DMR_DATA_TX_PREAMBLES` (16) preamble CSBKs where the burst list has
room for them, fewer for the longest packets, never fewer than `DMR_DATA_IP_PREAMBLES` (2, what the air MTU counts
on, 12.8). A confirmed packet of the air MTU gets 2. Each try sends them again. ACKs, ETSI responses and CSBK answers
keep their own counts. The service builds the packet with 2, then moves its bursts up and rebuilds the preambles
(`widenPreambles`), so no buffer grows.

Why 16: the network text service answers only packets with 12 or more preambles (5.1.1), and commercial DMR radios
send 15 or 16. The standard sets no count (TS 102 361-2 7.1.2.5). The cost is 0.84 s more on air per try. The waits
for the link ACK and the TMS ACK start when the transmission ends, so they don't shrink.

### 12.7 Limits

- The sliding window (TS 102 361-3 5.4.4) isn't used.
- There is no fragmentation (FSN chains): one packet per datagram, so a datagram longer than the air MTU (12.8)
  doesn't go. The gateway drops IP fragments from the host too ([usb-network.md](usb-network.md) 3.2).

### 12.8 The air MTU

The air MTU is the longest IP datagram the radio sends in one transmission. It isn't below the USB link's MTU (586,
[usb-network.md](usb-network.md) 3.2), so every packet a host sends to the air fits one transmission, confirmed or
not. `core/dmr/dmrData.h` sizes the burst list and the packet buffer for it:

| Constant | Value | From |
|---|---|---|
| `DMR_DATA_MAX_PACKET` | 612 | The packet buffer: 51 rate 1/2 blocks |
| `DMR_DATA_IP_MTU` | 596 | 60 rate 1/2 confirmed blocks of 10 octets less the 4-octet CRC-32 (612 - 16), the size of confirmed data at rate 1/2; unconfirmed, 51 blocks would carry 608 |
| `DMR_DATA_IP_PREAMBLES` | 2 | The fewest preamble CSBKs before an IP packet (also SMS); 16 where they fit (12.6.4) |
| `DMR_DATA_MAX_BURSTS` | 64 | The burst list of one transmission: 16 preambles where they fit (12.6.4) and a packet of 596 octets |

| A packet of 596 octets | Blocks | Bursts | Air time (60 ms a burst) |
|---|---|---|---|
| Unconfirmed, rate 1/2 (12 octets a block) | 50 | 2 + 1 + 50 = 53 | About 3.2 s |
| Confirmed, rate 3/4 (16 octets a block) | 38 | 2 + 1 + 38 + the terminator = 42 | About 2.5 s |

- The MTU counts the user data on air: the IP and UDP header as sent (SAP 4: 28 octets; SAP 3: 5, 7 or 9, 11.1) plus
  the payload. The largest UDP payload is 568 octets with the full header and up to 587 with the compressed one (9
  octets: ports outside 4000-4099 in the extended headers). A raw port packet (no header) carries up to 596 through
  the gateway, the air MTU (the encoder itself takes 608).
- Confirmed data (12.2) takes every packet of the MTU: 38 blocks, under the 64 of one selective ACK block. Longer
  packets are refused (`DMR_REQ_FAILED`, `ip.failed`), and nothing goes on air. Build-time asserts in `dmrData.c`,
  `dmrDataService.c` and `ipGateway.c` keep the four constants and the link MTU in step.
- The gateway still checks each host packet against the air MTU before the air queue, with the header it will get
  (`ipGatewayAirLength`). On the radio no packet is too long. A host test built with a narrower air
  (`ipGatewayTestAir440`; [usb-network.md](usb-network.md) 3.2) covers the check and its ICMP fragmentation needed
  answer.
- Receiving: the receiver reassembles up to `DMR_DATA_MAX_PACKET` (51 blocks, 608 octets of user data unconfirmed)
  from other radios. A longer packet ends in a receive error (`data.rx_crc_errors`), and nothing is delivered. The
  gateway passes the host what fits its link (586 octets of IPv4). A longer packet, from a radio that sends more than
  this radio does, is dropped and counted (`net.up_too_big`).
- Keyed time: the stack's TX deadline grows with the list (10 s + 60 ms a burst, [gotchas.md](gotchas.md) 3.9), so a
  64-burst list has about 13.8 s. The guard (12.6.3) is per transmission and doesn't change.
- RAM: one list of 64 bursts (27 octets each, 1,728) in the data service. The air queue builds raw packets in it
  (`dmrDataServiceSendShortData`) instead of a list of its own. The hotspot keeps its own list (it runs in the host
  link's task), placed outside the CPU-only RAM where a board has one. The packet buffers (`txPacket`, `ipPacket`,
  `dmrDataRxPacket`) are 612 octets each.

### 12.9 Rates with a commercial portable

A commercial portable on a simplex channel (direct mode) takes TMS from a RadioFoundry radio as follows, at each rate,
confirmed and unconfirmed. Delivered means the text shows on the portable (confirmed: also its TMS ACK comes back).

| Rate | Confirmed | Unconfirmed |
|---|---|---|
| 3/4 | Delivered | Not delivered |
| 1/2 | Not delivered | Delivered |
| 1 | Delivered | Delivered |

- SAP 3 and SAP 4 behave the same in every case.
- The portable sends confirmed data at rate 3/4 and unconfirmed at rate 1/2; the network text service sends confirmed
  at rate 3/4 (5.1.1). The radio sends the same: IP packets confirmed at rate 3/4, unconfirmed at rate 1/2
  (`IP_CONFIRMED_RATE`, `IP_UNCONFIRMED_RATE` in `dmrDataService.c`). Text and other packets outside the IP path stay
  at rate 1/2.
- A modem that can't send rate 3/4 blocks a receiver decodes (`hal_dmr_modem_t.tx_rate34_bad`) sends confirmed IP at
  rate 1/2 instead, which a receiver decodes but the portable refuses. The debug link can set either rate for the
  bench (`data.ip_cnf_rate`, [debuglink.md](debuglink.md) 3.12 op 12).
- Rate 1/2 confirmed: the portable answers the first try with a NACK (class 1, type 1, packet CRC failed), and each
  retry with a selective ACK that has only block 0. It takes block 0 even with a wrong CRC-9, so it doesn't check the
  blocks at this rate. The radio's blocks follow B.3.9 and B.3.10, and both CRCs, computed the same way, check on the
  portable's own rate 3/4 blocks. Rate 1/2 confirmed has been in the standard since V1.2.1, and the portable never
  sends it itself.
- Rate 3/4 unconfirmed: nothing comes back (none is due), and the portable shows nothing.
- The portable's link response comes in the next frame in the radio's timeslot: its first burst starts 60 ms after
  the start of the radio's data terminator, about 29 ms after the terminator's end. To catch it, the radio returns to
  receive at the slot engine's TX done, and the end of the transmission leaves the receiver as it is on the same
  channel: the modem glue keeps a back-to-receive flag ([dmr-modem.md](dmr-modem.md) 6.4).

## 13. Tests

- `tests/host/dmrDataServiceTest.c`: the service on a fake host. It covers replies and duplicates, confirmed data,
  queue overflow and aging, CSBK both ways, the radio's SMS and its ACK, ARS, LRRP with and without a position, the
  block window, the mailbox, ETSI IP text (the builder's octets, the decoder with and without a byte order mark, the
  default format, received, confirmed, group, duplicates, a stored TMS default), and the passthrough table above. It
  is built a second time with `CONFIG_DMR_TMS=0` (`dmrDataServiceTestNoTms`): a TMS heard is neither kept nor ACKed,
  confirmed data still gets its response, and TMS sends are refused.
- `tests/host/dmrDataChainTest.c`: end to end through the real slot engine (a model of the chip's receive reports and
  its page 2 / 0x40 / 0x50 writes), the real call stack and the service. It covers call alert in and its ack out (CRC
  with CRC_MCU_Control), TMS in at rate 1/2, 3/4 and 1 and its ACK packet out, the radio's radio check out and its
  answer in, an unanswered call alert (3 tries), a request sent 600 ms after a reception, the emergency alarm in and
  out, the ETSI text formats (ISO 8859-1, 7-bit, UTF-16 BE / LE / BOM, UTF-8, binary refused, the radio's builder read
  back), an ETSI message in and its response out, the radio's own out and done on the response, and the radio check in
  the ETSI form both ways. `make -C tests/host` runs it (`run-rxchain`).
- `tests/hil/test_dmr_data.py`: the same on air against the SDR bench (opt-in RF).
- IP packets (11, 12):
  - `tests/host/dmrDataTest.c`: testSap3 (the compressed header's 5 / 7 / 9 octet forms against vectors, auto / all /
    off, a UDP length that disagrees, a reserved opcode), testCRC9, testConfirmedTx (the radio's confirmed packet and a
    selective ACK byte for byte against vectors, the selective retry round trip through the receiver with a bad block
    and with a lost block, the NACK cases), testAirtime (the table in 11.3).
  - `tests/host/dmrDataServiceTest.c`: testIpTx (unconfirmed SAP 3, confirmed with ACK, selective ACK, N(S) resync,
    time-outs to failure, tries, repeater mode, group, too long, no channel), testIpAirMtu (the constants, SAP 4 and
    SAP 3 at the air MTU unconfirmed in 53 bursts and decoded back, confirmed at rate 3/4 in 38 blocks, one octet more
    refused with nothing on air), testConfirmedRx (selective ACK and the retry passed on once, a duplicate, lost
    bursts, NACK).
  - `tests/host/hostlinkNetTest.c`: the gateway's datagrams through the service path: SAP 3 for ports outside
    4000-4099, SAP 4 for 4007.
  - The vectors come from `tests/host/dmr_ip_vectors.py` (polynomial division from the standard's text, apart from
    the C code).
- `tests/hil/test_dmr_ip.py`: on air, the bench modem decoding with `tools/bench/dmr_packet.py` (opt-in RF; 11.4).
- `tests/hil/test_dmr_ip_burst.py`: datagrams back to back each way (radio, host and bench) and the air MTU on air
  (12.8): the largest datagram each way, the receiver's limit and one block more; one octet more to the air and the
  ICMP reply to the host on firmware with a narrower air MTU only. The bench board sends UDP with a `bench_tx` `data
  --udp-port` job.
