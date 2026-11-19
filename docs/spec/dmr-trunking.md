<!-- SPDX-License-Identifier: MIT -->

# DMR Tier III trunking (mobile station)

A radio on a DMR Tier III trunked system (ETSI TS 102 361-4) finds the system's trunked site control channel (TSCC),
stays on it, and follows the channel grants for the calls it wants to hear. Switch: `CONFIG_DMR_TRUNK`, a child of
`CONFIG_DMR` ([build-config.md](../build-config.md)); the access side (section 7) is `CONFIG_DMR_TRUNK_TX`, a child of
it. The radio core runs the engine on a system from the codeplug (section 12) or one set up over the debug link
(section 11).

| Section | Topic |
|---|---|
| 1 | Standards position |
| 2 | Modules |
| 3 | PDUs |
| 4 | System identity and authorization |
| 5 | Channel plans |
| 6 | The receive engine |
| 7 | Access (7.5 status, 7.6 short data) |
| 8 | Timers and constants |
| 9 | Tests |
| 10 | Not done yet |
| 11 | On the radio |
| 12 | In the codeplug |

## 1. Standards position

Everything here comes from ETSI TS 102 361-4 (the Tier III trunking protocol), TS 102 361-1 (the air interface) and
TS 102 361-2 (voice and generic services). Section and table numbers below are TS 102 361-4's unless marked -1 or -2.

- Only the standard's PDUs and procedures, with the standard feature set ID (FID 0). A CSBK with another FID is
  counted and ignored.
- Only the standard's trunking protocol, no proprietary one: FID 0 PDUs with the standard's CRC and framing. A system
  that sends its control channel with another CRC or framing fails the standard CRC checks and is never confirmed as
  a TSCC (section 9.2 checks this on recordings).
- On a trunked system the radio transmits only the access side (section 7) and the calls it is granted.
- Authentication only as section 7.10 gives it: with a key K, the radio answers a challenge and checks the TSCC's
  answer to its own; without one, a challenge is answered "not supported".

## 2. Modules

All in `core/dmr/trunk/`. None includes a board, platform or RTOS header; the host tests run them as they are.

| File | Content |
|---|---|
| `t3_pdu.{h,c}` | CSBK and MBC codec (7.1), CRC masks, C_SYS_Parms / P_SYS_Parms short LC (7.1.2) |
| `t3_sys.{h,c}` | C_SYScode partition, TSCC authorization, SYS_AREA, the Mask rule, access delay tables (6.2-6.4) |
| `t3_plan.{h,c}` | Logical channel number to frequencies: learned, table, fixed formula (Annex C) |
| `t3_ms.{h,c}` | The mobile station engine: hunt, confirm, camp, follow, return, access (6.2-6.6, Annex D) |

## 3. PDUs

A CSBK is 12 octets once the BPTC is decoded: octet 0 holds LB, PF and the 6-bit opcode, octet 1 the FID, octets 2-9
the fields, most significant bit first, octets 10-11 the CRC-CCITT.

| Block | CRC mask (-1 B.3.12) |
|---|---|
| CSBK | 0xA5A5 |
| MBC header | 0xAAAA |
| MBC continuation (last block) | none; the CRC covers the continuation only |

Decoded opcodes (`t3_csbk_parse`): C_ALOHA, C_BCAST, C_AHOY, C_ACKVIT, C_MOVE, C_RAND, C_ACKD, C_ACKU, P_ACKD, P_ACKU,
P_CLEAR, P_PROTECT, P_MAINT and the grants PV, TV, BTV, PD, TD (and the duplex and multi-item variants). `t3_csbk_parse`
returns false for another opcode but still fills the opcode, FID and the raw fields. `t3_csbk_build` packs the same set;
the engine's tests and the access side use it.

| PDU | Fields kept |
|---|---|
| C_ALOHA | TSCCAS, site timeslot sync, version, offset, active connection, Mask, service function, NRand_Wait, Reg, Backoff, C_SYScode, MS address |
| Grants | LPCN (12 bits), slot, late entry (TV/BTV), high rate (PD/TD), emergency, offset, destination, source |
| C_BCAST | announcement type, parameters 1 (14 bits), Reg, Backoff, C_SYScode, parameters 2 (24 bits) |
| C_MOVE | Mask, Reg, Backoff, LPCN, MS address |
| P_CLEAR | LPCN to return to (0: the last TSCC), group flag, destination, source |
| P_PROTECT, P_MAINT | kind, destination, source |

**Appended block (CdefParms type 0, 7.1.1.1.2).** A grant with LPCN 0xFFF (CG_AP) or a Chan_Freq broadcast (BC_AP)
carries the channel number and the MS's transmit and receive frequencies, each as MHz (10 bits) plus 125 Hz steps
(13 bits) (Annex C.1.1.4). CG_AP adds the payload channel's color code.

**Short LC.** C_SYS_Parms (SLCO 2) and P_SYS_Parms (SLCO 3) carry MODEL, the 12 NET+SITE bits, Reg (C) or the
composite flag (P), and the 9-bit common slot counter.

## 4. System identity and authorization

C_SYScode is 16 bits: MODEL (2), NET and SITE (14 together), PAR (2).

| MODEL | NET bits | SITE bits |
|---|---|---|
| 0 tiny | 9 | 3 |
| 1 small | 7 | 5 |
| 2 large | 4 | 8 |
| 3 huge | 2 | 10 |

PAR says which control categories may use the TSCC: 1 A, 2 B, 3 both.

`t3_authorize` checks a TSCC in the order of 6.3.2.2.2: model, network, the radio's site list (empty: any site), PAR
against the radio's category. SYS_AREA is the top DMRLA bits of SITE. Response_Check is bits 7-2 of the code (bit 0 the
least significant).

**Mask rule (6.2.1.1.1).** An Aloha's Mask (0-24) applies to a radio when the Mask low bits of the Aloha's address and
the radio's own match. Mask 0 means every radio.

**Access delays.** NRand_Wait 0-13 is that many TDMA frames, 14 is 15, 15 is 24 (table 6.2). Backoff 1-15 is
1, 2, 3, 4, 5, 8, 11, 15, 20, 26, 33, 41, 50, 70, 100 frames (table 6.3); 0 is reserved and reads 1.

## 5. Channel plans

A grant names a logical physical channel number (LPCN, 1-4094). `t3_plan_lookup` resolves it, first match wins:

1. **Learned**, when the system's record allows it: channels heard in Chan_Freq broadcasts and CG_AP blocks on the
   selected system. RAM only, up to 64 entries; when full the oldest is replaced. Dropped when the radio confirms a
   TSCC of another network (model or NET differs). A site change of the same network keeps them.
2. **Table**: the programmed per-channel list (Annex C.1.1.3).
3. **Fixed formula** (Annex C.1.1.2): MS transmit = base + (LPCN - first) x separation; MS receive = transmit minus
   the duplex split, or plus it when the plan says the MS transmits low. Separation codes 0-7: 5, 6.25, 10, 12.5, 15,
   20, 25, 30 kHz.

Then the programmed control channels (`chan_lookup`): a one-carrier site grants its own carrier's other slot.

Site tools and scanners often count each timeslot as a channel: (LPCN << 1 | slot bit) + 1, so LPCN 311 is their 623
(slot 1) and 624 (slot 2). A codeplug row typed with such a number would never match. When no source knows the LPCN,
the programmed rows (table, then control channels) are tried as 2 x LPCN + 1 and 2 x LPCN + 2, either slot since both
share the carrier. A match is used under the LPCN the grant named, so calls and TSCCs compare by it.

A grant whose channel nothing resolves is not followed. The engine counts it (`unmapped`) and keeps the last such
LPCN, so the user can see which channel is missing from the plan. When it is the radio's own call
(`T3_RES_UNMAPPED`), the request ends at once, not at TP_Timer, and PTT says which channel.

## 6. The receive engine

`t3_ms` follows the roaming engine's pattern: the radio core feeds it events and polls it for actions. It never
calls the HAL.

| Call | When |
|---|---|
| `t3_ms_start` | the user selected a trunked system |
| `t3_ms_rx` | every CSBK, MBC header and MBC continuation heard, with its slot and color code |
| `t3_ms_slc` | every short LC the CACH carried (CRC-8 good) |
| `t3_ms_activity` | any sync (voice or data) on the followed slot |
| `t3_ms_slot` | at each CACH heard: the inbound slot it announces and its AT bit (section 7) |
| `t3_ms_poll` | every 10-30 ms, until it returns false. `T3_ACT_TUNE`: tune to the channel and listen to the slot (0: both). `T3_ACT_SEND`: send the CSBK in the inbound slot just announced |
| `t3_ms_stop` | another zone or channel selected |

Actions wait in a queue of three, oldest first. A SEND goes out before a retune queued after it; a new retune drops a
retune not yet taken and what was queued after it.

### 6.1 States

| State | The radio |
|---|---|
| HUNT | tunes through the candidates looking for the TSCC |
| CAMPED | listens to the TSCC, both slots; a C_AHOY counts only on a slot an Aloha has shown to carry the TSCC (the carrier's other slot is a payload channel, whose P_AHOY has C_AHOY's opcode) |
| FOLLOW | listens to one slot of a payload channel |

**Hunt.** The candidates are the adjacent sites' control channels (7.18), then the programmed control channels in
order. Each gets a dwell (700 ms: at least 12 TSCC slots and two short
LCs). After a loss, the lost TSCC is tried first.

- An authorized C_ALOHA confirms the channel: the radio camps, and keeps the color code it was heard with.
- An Aloha of another system moves the hunt to the next candidate at once.
- Two C_SYS_Parms of another system in a row do the same. One alone is not trusted: the short LC CRC is 8 bits, and
  corrupt words pass now and then.
- Either keeps the other system's code for `T3_OTHER_MS` (10 s; `t3_ms_other`; PAR 0 from a short LC, which doesn't
  carry it), until the radio camps. A system set up with the wrong model or network is heard this way, and the screen
  names what it heard (12.4).

**Camped.** Every valid PDU from the TSCC (CRC good, our color code; Alohas and broadcasts also with our system code)
restarts T_Nosig. When it runs out, the engine hunts again. Short LCs don't restart it: every channel of the site
carries them. The registration state follows the latest Aloha or broadcast Reg flag:

| Reg | Listen-only | State |
|---|---|---|
| 0 | either | not needed |
| 1 | yes, or a build without access (`CONFIG_DMR_TRUNK_TX` off) | listen |
| 1 | no | needed; the access side registers (section 7.2) |

A registration record holds the area it was made in: MODEL, NET and SYS_AREA (the DMRLA top bits of SITE, 6.4.4).
DMRLA 0 is reserved (361-4 table 6.5), so a radio without one takes every site as an area of its own and registers
on each site it moves to.
A TSCC of another area needs a new registration. An area that denied registration stays denied until the engine
restarts.

**Composite TSCC.** A site with few channels may lend the TSCC's slot to a call (5.3.2). Its grant names the TSCC's
own channel and slot, and the radio follows it like any other; P_CLEAR to channel 0 brings it back to the TSCC. While
the slot is lent, its carrier's CACH carries P_SYS_Parms with Payload Channel Type 1 (table 7.34) and the TSCC sends
nothing. An idle radio that hears that short LC with its own system's MODEL, NET and SITE stays: each one restarts
T_Nosig, and no random access goes until the TSCC is back (an Aloha or C_SYS_Parms) or 360 ms pass without one. A
modem that doesn't report the short LC can't hear that: on a TSCC with no short LC from the system since the radio
came, a voice burst, voice header or terminator in the TSCC's color code on its slot counts the same
(`t3_ms_call_burst`), since a TSCC's slot carries a call only while lent. Once a short LC is heard, only short LCs
count.
P_SYS_Parms with type 0, or another system's, changes nothing. A radio waiting there that missed a call's grant (it
was in another call) follows a Late_Entry grant heard on the lent slot for one of its talkgroups (6.6.2.4.1), as on
the TSCC. A radio switched on meanwhile camps only at the Aloha: a grant doesn't carry the system's identity.

**Grants.** While camped, a grant is followed when:

| Grant | Followed when |
|---|---|
| TV, BTV | the radio is the source (its own call); otherwise, unless it is a late-entry grant for a talkgroup call the user left: the talkgroup is in the system's list with the listen flag, or it is an all-call (ALLMSID, ALLMSIDL, ALLMSIDZ) and all-calls are on, or the grant is an emergency and emergencies on any talkgroup are on, or monitor is on |
| PV | the radio is the source; or the destination is the radio's ID and private calls are on |
| PD, TD (and the multi-item 55, 56) | as data calls: PD with the radio as source or target; TD with the radio as source, or to a talkgroup it listens to (section 7.7) |

A gateway address (6.2, Annex A.4) is never followed as a talkgroup. While a request is in progress (section 7) only
the radio's own call or an emergency takes it off the TSCC. The TSCC repeats a grant while its call lasts,
for late entry. A Late_Entry grant for the call just left (same talkgroup, channel and slot) is not followed again for
2 s; a grant with Late_Entry 0 is a new call set-up and is always followed (6.6.2.4.1).

**Follow.** The engine asks for the payload channel and slot. The payload color code is CG_AP's when the grant had
one, else the TSCC's. On the payload channel:

| Heard on the call's slot | Result |
|---|---|
| any burst with voice or data sync, idle bursts in the hang included (6.6.2.3.2.9 a) | the call stays alive |
| P_CLEAR for the call, or to ALLMSI | return; to the LPCN it names, or to the frequencies of its appended block (LPCN 0xFFF, an MBC), else to the last TSCC |
| P_MAINT disconnect from the call's source (either party of a private call) | return |
| P_PROTECT DIS_PTT / EN_PTT to the radio, the talkgroup of its grant or ALLMSID (not to the other party of a private call) | PTT off / on |
| P_PROTECT EN_PTT_ONE_MS | PTT on for the radio it names; off for every other while they come (1 s after the last) |
| P_PROTECT ILLEGALLY_PARKED | return, unless it names the call |
| P_AHOY | section 7.4 |
| a grant for the same call to another channel or slot | move there |
| a grant for another call | ignored |

Bursts on the other slot are ignored: it carries another call, or the TSCC on a composite channel. Nothing on the
call's slot for TV_Inactive, or a call longer than the ceiling, also returns. After a return the TSCC has T_Nosig to
answer with valid PDUs.

### 6.2 Counters

`t3_ms_t.st`: CSBKs with a good CRC, CRC failures, grants heard while camped, calls followed, grants to unmapped
channels (and the last such LPCN), returns, hunts, foreign Alohas and broadcasts, foreign short LCs, learned channels,
CSBKs sent and random access tries.

## 7. Access

Built with `CONFIG_DMR_TRUNK_TX`. A system in listen-only mode never transmits, whatever the build. The engine builds
every burst; the radio core only sends it in the slot it is told.

| Call | When |
|---|---|
| `t3_ms_request` | PTT while camped: a voice call to a talkgroup or an individual. False (`ax.result` says why) when not allowed now |
| `t3_ms_cancel` | PTT released before the grant |
| `t3_ms_end_call` | the user ends the call being followed |
| `t3_ms_deregister` | the debug link's DEREG. Leaving a system or switching off sends nothing (section 12.4) |
| `t3_ms_can_talk` | may the user talk on the payload channel now |

`ax.result` holds the outcome: pending, granted, done, refused (`ax.reason` has the reason code), no answer, timeout,
not allowed, lost (the TSCC was lost meanwhile), canceled.

### 7.1 Random access (6.2)

One request at a time, on the TSCC's first slot (TS1 on a dedicated TSCC).

| Step | Rule |
|---|---|
| Before the first try | an Aloha heard since the radio came to this TSCC |
| Each inbound slot | the AT bit for the slot's inbound decoded and free (below); the Aloha's Mask matches the radio's ID; the Service Function allows the service (table below). An emergency skips Mask and Service Function unless the Mask is 24 |
| A slot not permitted | wait for the next permitted one |
| After a try | the answer may come in any of the NWait + 1 frames after the try's own (6.2.1.1.5, figure 6.7); no new try before the last of them has passed. The wait runs from the burst on air: the radio's stack reports it sent, and a burst it dropped goes to the backoff at once |
| No answer | try again 1 to Backoff frames later, at random (table 6.3); NRand_NR tries (NRand_NE for an emergency), then no answer |
| Overall | TRand_TC from the request, then timeout |
| C_WACKD, C_QACKD | stop retrying; TP_Timer for a call, TNP_Timer for the rest; a further one restarts it |

Which CACH's AT bit describes the TSCC's inbound depends on the Aloha's Offset bit. A CACH describes the inbound slot
its TC names (361-1 4.5). With aligned timing (Offset 0) logical channel 1 sends on inbound slot 2 (361-1 table 5.1), so
the TSCC slot's AT is in the CACH naming the other slot: the one right after the TSCC's own burst, where a PDU wanting
an answer is followed by AT busy (361-4 6.2.1.1.3). With offset timing it is the CACH naming the TSCC's slot
(`t3_ms_at_tc`).

| Service Function | Allows |
|---|---|
| 0 | everything |
| 1 | services with a payload channel (calls, packet data) and registration |
| 2 | services without one (status, short data, ...) and registration |
| 3 | registration only |

The C_BCAST Backoff replaces the Aloha's.

### 7.2 Registration (6.4.4.1)

The engine registers by itself when the state is "needed" and nothing else is in progress. Until registered it asks
for nothing else. The C_RAND (kind registration) targets the system code (Trg_Adr_Cnts 0), or with `T3_OPT_REG_TG`
subscribes to talkgroups. Sending it drops the old record. On a system whose C_ALOHA has Reg 0 the radio never
registers, and so subscribes to nothing: a subscription is part of a registration (6.4.4.1.13), and there the MS
creates no registration record (6.4.3 a).

| `T3_OPT_REG_TG`, talkgroups listened to | C_RAND |
|---|---|
| off | Trg_Adr_Cnts 00b, the system code |
| on, none | Trg_Adr_Cnts 01b, the talk group; the system code (00b) without one |
| on, one | Trg_Adr_Cnts 01b, the talk group, else the one listened to |
| on, two or more | Trg_Adr_Cnts 10b, TATTSI; the first seven as a list (6.4.4.1.13.2) |

A list goes in UDT blocks the C_RAND counts (Appended UDT Short Data: blocks less one; 1 block holds 3 addresses, 2
hold 7). The TSCC asks for them with a C_AHOY from TATTSI, Service_Kind registration, Service_Kind_Flag 1. The radio
sends C_UDTHU (UDT_Format 0001b address, to TATTSI, SF 1, A 0, Pad Nibble 0) and the blocks in the inbound slots
that follow: octet 0 is 0 (no one-key talkgroup), then the 24-bit addresses, the unused ones ADRNULL (B.3.2). The
final C_ACKD's Response_Info says which were accepted, bit 6 the first (table 6.10): `reg_tg_ok`. Only the bits of
the addresses sent are kept: all accepted is 1111111b however many were sent, so the rest mean nothing. A C_AHOY for the
list when none was sent, or for a list since changed, gets nothing.

| Answer to the radio's ID | Result |
|---|---|
| C_ACKD Reg_Accepted, Reg_Subscription | registered, for this area; a single talkgroup sent was taken with Reg_Accepted, refused with Reg_Subscription |
| another ACK-class reason | not a registration answer (table 7.41): ignored |
| C_NACKD Reg_Refused | hunt; registers again on returning, after 30 s at the earliest |
| C_NACKD Reg_Denied | the area is denied; hunt |
| none (NRand_NR tries) | the record stays as it was; tries again after 30 s |

C_NACKD MS_Not_Registered to any other request (6.4.4.1.12) means the TSCC has no record of the radio: it registers
again before asking for anything else.

De-registration is the same request with Reg_Dereg 0. After it the engine doesn't register again until restarted.

### 7.3 Calls (6.6.1, 6.6.2.2)

| Event | Result |
|---|---|
| request | C_RAND kind talkgroup or individual voice, the target, the radio's ID; EMERG option for an emergency |
| request to an all-call ID (ALLMSID, ALLMSIDL, ALLMSIDZ) or a talkgroup with `T3_TG_BROADCAST` | kind talkgroup voice with the broadcast option (6.6.1.7, table 7.52); the TSCC grants BTV_GRANT (or a TV_GRANT to the all-call ID, table 7.11); the voice LC then carries the broadcast service option (0x08, 361-2 5.3.2.1, 5.3.3.1) |
| C_WACKD, C_QACKD, C_ACKD MS_Alerting | waiting (TP_Timer) |
| a grant with the radio as source | follow; the call is the radio's own; result granted |
| C_NACKD | refused, with its reason |
| C_ACKD Call_Back | done |
| cancel, nothing sent yet | dropped |
| cancel, sent | a C_RAND kind cancel to the same target; the TSCC answers C_ACKD Message_Accepted |

PTT on the payload channel (`t3_ms_can_talk`): following, not disabled by P_PROTECT, and not a broadcast (BTV_GRANT) or
all-call someone else made. At the end of a call the radio made, or of any private call, `t3_ms_end_call` sends N_Maint
P_MAINT (DISCON, to TSI) in consecutive frames on the call's slot, then returns. A talkgroup call's recipient returns
without sending, and the call's late-entry grants don't bring it back.

### 7.4 Answers (6.4.8, 6.6.1.2.2, 6.6.2.3)

A C_AHOY or P_AHOY to the radio's ID (or to the followed talkgroup, on the payload channel; or a radio check of a
talkgroup the radio follows, on the TSCC) is answered with C_ACKU or P_ACKU on the inbound slot after the next CACH of
the slot it came on, without random access. The answer's target is the AHOY's source, its additional address the
radio's ID, also when a talkgroup was addressed (table 6.28).

| AHOY | Answer |
|---|---|
| radio check (kind registration, from TSI; 6.4.12) | MS_Accepted |
| voice radio check (P_AHOY kind individual or talkgroup voice, from TSI; tables 6.42, 6.43) | P_ACKU MS_Accepted (6.6.2.3.2.1) |
| availability check (kind individual voice): OACSU | MS_Accepted |
| availability check (kind individual packet data) | Message_Accepted (section 7.7); EquipBusy_Refused (0x13) while the data service sends; not supported in a build without it |
| availability check (kind duplex voice or duplex packet data) | MS_Duplex_Not_Supported (0x16): the radio is simplex (6.6.9.2.5, 6.6.10.2.5). It never follows a duplex grant (53, 54), whose bit 15 is Call Direction; their payload channel always uses offset timing (tables 7.10, 7.14) |
| packet data radio check (P_AHOY kind packet data, from TSI) | P_ACKU Message_Accepted (6.6.3.3.2.1) |
| call cancel (kind cancel) on the TSCC | MS_Accepted |
| selective clear (P_AHOY kind cancel) | MS_Accepted (6.6.2.3.2.7; tables 6.44 and 6.45 say Message_Accepted), then return |
| status (kind status) | MS_Accepted (section 7.5) |
| status poll (kind status, status 127) | Accepted for the Status Polling Service (0x47), Response_Info the radio's status |
| stun or revive (kind supplementary, from STUNI) | without `T3_OPT_STUN`: not supported; else without K: Message_Accepted; with K: C_ACKVIT, the radio's challenge (section 7.9) |
| kill (kind supplementary, from KILLI) | with K and `T3_OPT_KILL`: C_ACKVIT; else not supported (section 7.9) |
| authentication challenge (kind 1110b, source a challenge 0-0xFFFCDF), with K | Authentication_Response 0x48, the target the response (section 7.10) |
| authentication challenge without K, anything else to the radio | not supported (0x00); stunned, nothing |

### 7.5 Status (6.6.6)

A status is a 7-bit value: 0-126, 126 the emergency alarm and 124 its cancel; 127 is a poll. `t3_ms_status` sends one
to an MS or a talkgroup, or polls an MS, by random access (C_RAND, Service_Kind status, tables 6.57 and 6.58):

| Field | Holds |
|---|---|
| Service_Options | G/I (bit 6), SUPED_SV 0, the status's 5 most significant bits |
| the 4 service-dependent bits | SUPED_VAL 0, then the status's 2 least significant bits |

The TSCC's C_AHOY to the called party with our status as the source is progress (TNP_Timer); C_WACKD and C_QACKD are
too. C_ACKD ends it (`T3_RES_DONE`): MS_Accepted mirrored for an MS, Message_Accepted for a talkgroup, Accepted for
the Status Polling Service for a poll, with the polled status in Response_Info (`ax.info`). C_NACKD refuses it. A poll
to a talkgroup or a status past 127 isn't started (`T3_RES_NOT_ALLOWED`). The last status sent is the radio's own,
the answer to a poll.

**Radio check.** A radio check from the data service (the menus' Radio check, [dmr-data.md](dmr-data.md)) has no
CSBK of its own on a trunked system: the radio core sends it as a status poll to the MS. The poll's acknowledgement
answers the check (`dmrDataServiceSignalResult`); a refusal or no answer fails it at once, without the conventional
retries (the random access has its own).

A status heard in a C_AHOY on the TSCC (Service_Options_Mirror the 5 most significant bits, Appended_Blocks the 2
least) to the radio or to a talkgroup it listens to is kept in `status_rx`. To the radio it is answered (section 7.4);
to a talkgroup it isn't. Listen only, it is kept and not answered. The TSCC sends one to a talkgroup more than once,
and one to the radio again when the answer was lost: the same status from the same caller within `T3_REPEAT_MS`
(4 s) of the last is answered again but not counted as another.

### 7.6 UDT short data (6.5, 6.6.4)

A message is 1-46 octets in a UDT: a header (data type data header, CRC mask 0xCCCC) and 1-4 rate 1/2 blocks of 12
octets in consecutive frames of one slot. The blocks hold the data, pad octets, and a CRC-CCITT over all the octets
before it, unmasked (TS 102 361-1 8.2.2.5).

| Header field | Holds |
|---|---|
| G/I, A | a talkgroup; a response demanded |
| DPF, SAP | 0000b, 0000b |
| UDT_Format | table 7.88, as the caller gives it |
| Pad Nibble | BCD, ISO 7 bit, ISO 8 bit, UTF-16 and mixed: the 1111b pad nibbles, counted (B.3.3-B.3.5, B.3.8, B.3.9; a last BCD digit 1111b, DigitNULL, counts); ISO 7 bit counts the spare bits of its last octet too (table B.4); other formats: 0, pad octets 0 |
| UAB | blocks less one |
| SF, opcode | 0 (the user's data); C_UDTHD outbound, C_UDTHU inbound |

ISO 7-bit text is given one character an octet (1-52) and packed 7 bits each, first character first.

`t3_ms_sdata` sends one to an MS or a talkgroup:

1. C_RAND, Service_Kind 0100b (an MS) or 0101b (a talkgroup), Service_Options 0, the 4 service-dependent bits
   SUPED_VAL 0 and the blocks less one (table 6.55).
2. The TSCC's C_AHOY from SDMI to the radio, Service_Kind 0100b, asks for the upload: C_UDTHU and the blocks go in
   the inbound slots that follow, as one transmission. A second C_AHOY (a CRC failure at the TSCC) sends it again.
   With no message waiting, the radio sends nothing.
3. C_WACKD and C_QACKD are progress (TNP_Timer). C_ACKD ends it: MS_Accepted mirrored for an MS, Message_Accepted for
   a talkgroup. C_NACKD refuses it.

A C_UDTHD heard on the TSCC, its blocks whole and the CRC good, to the radio or to a talkgroup it listens to, is kept
in `sd_rx` (the data without its pad octets). To the radio with A set it is answered C_ACKU MS_Accepted; to a
talkgroup it isn't. A bad CRC or a missing block keeps nothing and answers nothing: the TSCC sends it again.
Supplementary data (SF set) is taken apart (below). A repeat (the same caller, destination, format and octets within
`T3_REPEAT_MS`) is answered again but not counted, as for a status.

Text kept in `sd_rx` (UDT_Format ISO 7 bit, ISO 8 bit or UTF-16BE) also goes into the message inbox as a message
from the caller, once per new one (`dmrDataServiceInboxAdd`), so the user reads it with the other messages. The
other formats stay for the debug link.

**Polling (6.6.5).** `t3_ms_sdata_poll` asks an MS for its short data: a C_RAND kind UDT poll (0110b) with the
format in POL_FMT (table 7.60) and the blocks less one in Appended_Short_Data (table 6.56). The size is prearranged
between the two MSs.

| Step | The radio |
|---|---|
| C_AHOY UDT poll from the radio to the polled MS | progress (TNP_Timer) |
| C_UDTHD from the polled MS to the radio | kept in `sd_rx`, answered C_ACKU MS_Accepted (6.6.5.5); done |
| C_NACKD | refused |

Polled (6.6.5.7): a C_AHOY UDT poll to the radio, from the calling MS or SDMI, gets the reply in `t3_cfg_t.poll_data`
as a C_UDTHU to that caller in the slots that follow, cut to the blocks asked for. With no reply set, or one in
another format than POL_FMT, the radio answers C_NACKU MSNot_Supported.

**Supplementary user data (6.4.13).** Data whose format and size are prearranged with the system (a position, for
example) can ride along with a call set-up. With `t3_cfg_t.supl_len` > 0 every voice, packet data, short data and
status request offers it: Service_Options SUPED_SV 1 and SUPED_VAL the blocks less one, above the service's two low
bits (tables 6.40, 6.50, 6.55, 6.57). A status's SUPED_SV is bit 5, above the status's five most significant bits.

| Step | The radio |
|---|---|
| C_AHOY from SUPLI to the radio, the request's Service_Kind | a C_UDTHU with SF 1 to SUPLI and the data's blocks in the slots that follow (table 6.30); the request goes on (TNP_Timer) |
| no C_AHOY from SUPLI | nothing: the TSCC may go on without it (6.4.13.0 a) |
| C_UDTHD from SUPLI, SF 1, to the radio | kept in `supl_rx`, answered C_ACKU Message_Accepted to SUPLI when A is set (6.4.13.2) |
| the same to a talkgroup the radio listens to | kept, not answered |
| the grant, user data or status that follows to the same destination, within `T3_SUPL_HOLD_MS` (10 s) | names the sender: `supl_rx.src` (0 until then) |

The download doesn't carry the sender (figures 6.38, 6.39), so the radio holds it until the call does. A repeat within
`T3_REPEAT_MS` is answered again and keeps a sender already named. The data comes from the system's codeplug record
(12.8), or the debug link stages it (`SUPL`, [debuglink.md](debuglink.md) 3.26); the view shows the last heard
(`supl_*`).

All of it is built with `CONFIG_DMR_TRUNK_SUPL` ([build-config.md](../build-config.md)). Without it the radio offers
none, and a download from SUPLI is taken off the air unanswered (it is never short data).

**Calls through a dialing gateway (6.6.2.2.3, 6.6.4.2).** A voice, short data or packet data request to PSTNI,
PABXI, LINEI or DISPATI (or their offset-timing addresses, A.4) carries no called number: the TSCC asks for it.
`t3_cfg_t.dial` holds the digits (`t3_dial_pack`: `0-9`, `*`, `#`, up to 44; table 7.46).

| Step | The radio |
|---|---|
| the request | individual (a talkgroup is refused), destination the gateway, Proxy Flag 0 for 1-20 digits, 1 for 21-44 (6.6.2.1.2); refused without digits |
| C_AHOY from that gateway to the radio | a C_UDTHU to the gateway, SF 1, BCD (B.3.3: the first digit in the high nibble, Pad Nibble per table B.3), in the slots that follow (tables 6.35, 6.41, 6.54); the request goes on |
| C_AHOY from SDMI or SUPLI | as without a gateway: the message, the supplementary data |
| C_UDTHD from a dialing gateway, SF 1, BCD, to the radio | the caller's number (CLI, table 6.33): kept in `cli`, answered C_ACKU Message_Accepted to the gateway when A is set |
| the same to a talkgroup the radio listens to | kept, not answered |

Built with `CONFIG_DMR_TRUNK_GW`. Without it a request to a dialing gateway is refused and a caller's number is
dropped unanswered. Two places set the digits of the system in use: the debug link (DIAL,
[debuglink.md](debuglink.md)) and Find (`radio_req_t3_dial`, up to 16 digits; a new start drops them).

| On screen | |
|---|---|
| Find, digits typed on a trunked system | a last row "Phone call 5551234": the digits go to the radio at once, PTT on the row calls PSTNI privately (one call), OK makes PSTNI the talk target for now. No actions, never the channel's own target |
| a call from a dialing gateway | the caller's number in place of the gateway's ID, on the call screen and in the private call notice; "Phone line" when the gateway gave none |

The number belongs to a call when the grant from that gateway to the same destination comes within `T3_CLI_CALL_MS`
(10 s) of it (`t3_call_t.cli`): the TSCC downloads it in the call set-up (figure 6.45).

### 7.7 Packet data (6.6.3)

Packet data uses the conventional data stack ([dmr-data.md](dmr-data.md)) on a payload channel the TSCC grants for
each transmission. Single slot, single item only.

`t3_ms_pdata` asks for the channel:

| Event | Result |
|---|---|
| request | C_RAND, Service_Kind 0010b (an MS) or 0011b (a talkgroup), Service_Options 0 (table 6.50) |
| C_WACKD, C_QACKD | waiting (TP_Timer) |
| PD_GRANT or TD_GRANT with the radio as source | follow it as a data call; result granted |
| C_NACKD | refused, with its reason |

Grants followed as data calls (6.6.3.2.7): PD_GRANT with the radio as source or target; TD_GRANT with the radio as
source, or to a talkgroup it listens to; the multi-item grants (55, 56) the same. On a data call PTT is off
(`t3_ms_can_talk`); `t3_ms_data_ready` is true once the radio has settled on the slot. P_CLEAR, P_MAINT from the other
party, inactivity and `t3_ms_end_call` end it as they end a voice call.

The radio's request is a single item (table 6.50), so it ends its own data call once the item is over (6.6.3.3.2.5),
with N_Maint P_MAINT (DISCON). This doesn't wait for a P_CLEAR, which the radio misses while it transmits, and on a
slot whose idles keep TD_Inactive from running out.

| The item | The call ends (`t3_ms_data_done`) |
|---|---|
| failed: no channel to send on, or every confirmed try unacknowledged (6.6.3.3.2.5 e) | at once |
| sent unconfirmed, or acknowledged (6.6.3.3.2.5 a, c) | `T3_DATA_LINGER_MS` (3 s) after the last data heard on the slot (`t3_ms_data_heard`): a reply in the TS's hang still arrives |

The availability check for data to the radio (C_AHOY, Service_Kind 0010b) is answered C_ACKU Message_Accepted
(6.6.3.2.6). From IPI the same C_AHOY asks for the IPv4 address while the radio's own call to IPI waits (7.7 above);
with no such call it is the check, for the network's data to the radio (a reply to its datagram, sent once it is back
on the TSCC).

**To an IP gateway (multi-part, 6.6.3.2.3).** `t3_ms_pdata_ip` asks for packet data to an IPv4 address outside the
radio network: the C_RAND targets IPI (0xFFFEC3) with the Proxy Flag 0 (IPv4).

| Event | Result |
|---|---|
| C_AHOY from IPI to the radio, Service_Kind 0010b (6.6.3.1.2 b) or 0100b (table 6.54) | the address upload: C_UDTHU, UDT_Format 0110b IP, one block, SF 1, A 0, target IPI (table 6.35's gateway rows); the block is B.3.7's IPv4 format, the address then 48 reserved bits. Asked again, sent again |
| C_WACKD, C_QACKD, C_NACKD, the grant | as for an MS. A grant without the C_AHOY is taken too |

On the payload channel the data goes to IPI (6.6.3.3.0) with the full IP header (the off-net route, dmr-data.md),
which carries the real destination. Data from the IP side comes from IPI and is delivered by its IP header.

The radio uses it on a system with option 0x1000 (12.1): while that system runs, the IP gateway's off-net route goes
to IPI instead of `ipGatewayId`, and the data service's packet to IPI asks with its own destination
(`dmrDataServiceTxIpDst`). The debug link's `t3.ip_gw` shows it.

On such a system the radio also gives the gateway its own address at registration (IP Connection Advice, 6.4.11),
so IP from outside reaches it by address. The address is the CAI address of the ID it registers with
(`dmrDataIdAddress`).

| Step | Detail |
|---|---|
| Request | The registration (to the system code) sets IP_Inform (Service Options 0x10) beside Reg_Dereg 1 and PowerSave_RQ. Not with a talkgroup or a list, whose answers are their own |
| C_AHOY from IPI, Service_Kind 0100b or 1110b | The radio is registered (6.4.11.1 e). It uploads its address as above, then waits TNP_Timer for the final answer |
| C_ACKD Reg_Accepted from IPI | The address is taken (6.4.11.1.4). Response_Info is the PowerSave_Offset, as in any Reg_Accepted |
| C_NACKD IP_Connection_failed (2Ch) from IPI, after the AHOY or at once | Registered without the address (6.4.11.1 f): no IP bearer service, or the address is another's. The radio stays on the site, reached by its ID, without power save |
| No final answer | Registered; the address is not known to be taken |

The debug link's `t3.ip_advised` says the system took the address. Deleting an address (Reg_Dereg 0 with IP_Inform 1)
isn't sent: the radio has one address and keeps it.

### 7.8 Emergency (6.6.6.3, 6.6.1)

The emergency key ([dmr-emergency.md](dmr-emergency.md)) works on a trunked system as follows:

| Part | On a trunked system |
|---|---|
| Alarm | Status 126 to the emergency target (section 7.5), instead of the alarm CSBK. Sent once: the TSCC acknowledges it, so the alarm tries don't repeat it. Refused or unanswered, the module goes on to the call as after its last try. |
| Call | PTT and the hot mic ask for a call to the emergency target with Service_Options EMERG set (N_Rand_NE tries). The hot mic lasts into the granted call. |
| Target 0 | The system's talk target, not the channel's. |
| Cancel | The emergency over, status 124 to the alarm's target, once the radio is camped. Only after an alarm the TSCC acknowledged. |
| Heard | Status 126 to the radio or a talkgroup it listens to raises the alarm alert. A call whose voice LC has the emergency option raises the call alert, as on a conventional channel; on a call followed from a grant with Emergency set, so does the grant. |

### 7.9 Stun, revive and kill (6.4.9, 6.4.10)

A C_AHOY to the radio from STUNI, Service_Kind supplementary (1101b), stuns it (Service_Kind_Flag 0) or revives it (1).
The radio takes it only with `T3_OPT_STUN`; without, it answers C_ACKU MSNot_Supported. With the option and without K
(section 7.10) the radio answers C_ACKU Message_Accepted and acts (6.4.9.1). An open system's radios leave the option
off: anyone could send an unauthenticated stun.

With K it authenticates the TSCC first (6.4.9.2.2, 6.4.10.2):

| Step | PDU |
|---|---|
| The radio's acknowledgement | C_ACKVIT (tables 6.21, 6.25): Service_Kind supplementary, Service_Kind_Flag as the AHOY's, target a random challenge 0-0xFFFCDF, source the radio |
| The TSCC's answer | C_ACKD Authentication_Response (0x64), target the radio, source the response (tables 6.20, 6.24) |
| Right answer (section 7.10's algorithm, the radio's K and PSN) | the radio acts, then C_ACKU MS_Accepted (0x44) to STUNI or KILLI (tables 6.22, 6.26) |
| Wrong answer | C_ACKU Recipient_Refused (0x14) to STUNI or KILLI; nothing done |

The challenge holds for TNP_Timer: a repeated C_AHOY gets the same one, a repeated answer the same acknowledgement
(acted on once). The figure text of 6.4.9.2.0 says Message_Accepted for the final acknowledgement; tables 6.22 and 6.26
give MS_Accepted, which the radio sends.

A kill (C_AHOY from KILLI) is accepted only with K and `T3_OPT_KILL`; otherwise the radio answers MSNot_Supported.
Killed, the radio loses all DMR functionality (6.4.10.0). The DMR stack stops at once and never starts again: no
trunked system, no conventional DMR channel (PTT there is denied, no DMR channel is heard), no hotspot. FM works. The
kill is kept in the settings (`t3_kill`, [settings-store.md](settings-store.md) 3), so it outlasts a restart and a
firmware update; only a factory reset clears it. No air message revives a killed radio. The debug link's `dmr.killed`
reads it.

Stunned, the radio:

- keeps hunting, registration, de-registration, radio checks and stun/revive;
- asks for no call, status, short data or packet data (refused as not allowed; PTT is denied);
- follows no grant, keeps no status or short data heard, and answers no other AHOY.

The stun outlasts leaving the system and starting it again, unless the new system is another network. On a codeplug
system (section 12) it also outlasts a restart of the radio: the settings keep it (`t3_stun`,
[settings-store.md](settings-store.md)) as the system's slot and a 4-bit hash of its network ID and model, set when the
stun is taken and cleared by the revive. Starting that slot's system with the same hash, the radio is stunned before
it hunts. A kept stun for another hash (the slot now holds another system) doesn't apply and stays as it is. A factory
reset clears it. The debug link's staged system keeps its stun in RAM only.

### 7.10 Authentication (6.4.8)

The system may hold a 128-bit key K and a 24-bit PSN (`t3_cfg_t.auth_key`, `psn`; 0: no PSN). A C_AHOY to the radio,
Service_Kind 1110b, whose source is below the gateway block (0-0xFFFCDF) is a challenge:

1. RC4 is keyed with the challenge (3 octets, most significant first) followed by K.
2. Of its keystream, octets 1-256 are dropped; octets 257-259 are the response, most significant first.
3. The response is XORed with the PSN.

The radio answers C_ACKU, Reason Authentication_Response (0x48, table 7.42), the target the response, the additional
address its own ID; on the payload channel, P_ACKU (6.6.2.3.2.2). A challenge during an access (a registration or a call
request) is progress: the access waits TNP_Timer for its final answer. Without K a challenge is answered not supported.
A radio check (from TSI, above the range) is unchanged. `t3_auth_response` passes the test vectors of tables 6.15 and
6.16.

### 7.11 Mass registration (6.4.5)

A C_BCAST MassReg (table 7.76) asks radios to register again. Broadcast_Parms1 holds Reg_Window and an Aloha-style
mask; it applies when the mask's low bits of its address equal the radio's ID (mask 24 names one radio, 0 all).

| Reg_Window | What the radio does |
|---|---|
| 1-15 | registers by random access at a random time inside the window (table 6.12: 0.5 s to 200 000 s); another MassReg before the window ends is ignored |
| 0 | cancels a pending one; a registration already sent completes |

Hunting for, or moving to, another TSCC cancels it too.

### 7.12 Move TSCC (C_MOVE, 7.1.1.1.3)

An applicable C_MOVE (the same mask rule as MassReg) sends the radio to another control channel: the logical channel
it names, or, in an MBC (LPCN 0xFFF), the frequencies and color code of the appended block. An access under way ends
(`lost`), a mass registration is dropped, and the radio sends nothing there until an Aloha confirms the channel;
T_Nosig without one starts the hunt (D.1.2). A move to the channel it is on does nothing.

### 7.13 Power save (6.4.7)

With `t3_cfg_t.ps_rq` 1-7 (1:2 to 1:128, table 6.13) the registration request carries PowerSave_RQ in its Service
Options (bits 3-1). A single-talkgroup registration doesn't ask (6.4.4.1.13.1 sets PS_RQ 000), nor does a list
(TATTSI): its answer's Response_Info is the list's. Only Reg_Accepted's Response_Info, masked to PowerSave_RQ bits, is
PowerSave_Offset, the wake frame (table 7.41); 0 means no power save.

| Rule | Detail |
|---|---|
| Wake frame | 480 ms. PS_Counter = Common_Slot_Counter / 4. The radio's frame: PS_Counter masked to PowerSave_RQ bits equals PowerSave_Offset |
| Counter | The CSC of the last C_SYS_Parms short LC heard, carried on one step per 120 ms. The value belongs to the four slots whose CACHs carried it, so it is taken as begun 120 ms before its reception was complete. The short LC comes from the modem (`DMR_EV_SHORT_LC`, dmr-modem.md 6.2): with a modem that doesn't report it there is no CSC and the radio never sleeps |
| Early | The radio wakes two CSC steps (240 ms) before its frame, so the receiver has synchronized |
| Asleep | Camped, confirmed, registered, no access, nothing queued, outside the wake frame, past T_Awake |
| T_Awake | 2 s (`awake_ms`): after registering, any access, traffic for the radio (AHOY, acknowledgement) and a call's end |
| T_Nosig | Counts only time awake. A PDU still heard in the first moments of a sleep (the receiver goes off after the engine sleeps) counts as heard now, never later |
| Level | Not taken asleep, nor for 60 ms after waking: with the receiver off or settling it reads no signal, which would roam the radio |
| MassReg | A due mass registration wakes the radio |

`t3_ms_asleep` is the engine's decision. While it holds, idle and not keyed, the radio core switches the receiver off
with the board's `rx_sleep` where the board sets `rx_sleep_dmr` (the modem keeps running). Waking (the engine's
decision, a tune, leaving the system) turns it on and restarts the modem's receive, which finds the TSCC's slot timing
again inside the two early CSC steps. The debug link's `t3.rx_off`, `rx_offs` and `rx_off_ms` show it. A board
without `rx_sleep_dmr` keeps the receiver on; the engine still keeps the wake frames.

### 7.14 Dynamic group assignment (DGNA, 6.6.8)

A UDT download with the C_DGNAHD header (opcode 36) to the radio's own ID sets its dynamic talkgroups. One to a
talkgroup is ignored. The radio answers MS_Accepted when the header asks (A flag).

| UDT_Format | Content | The radio |
|---|---|---|
| 0001b (DGNA_Address) | octet 0 bit 0 OK, then up to 15 24-bit addresses (B.3.2) | replaces the list; ADRNULL entries are skipped, all ADRNULL deletes it. OK: ADDRESS1 is the One-key talkgroup |
| 1010b (DGNA_Alias) | octet 0 bit 0 OK, a 24-bit address, up to 21 UTF-16BE characters (B.3.9) | names a talkgroup of the list, or sets the 16th; ADRNULL deletes the 16th. OK: the address is the One-key talkgroup |

DGNA talkgroups are heard like the configured ones and never attached (6.4.4.1.13.0). PTT talks to the One-key
talkgroup when the system has no talk ID of its own. The list lives in RAM: a restart or another system clears it.
The radio keeps one alias, the latest.

### 7.15 Call diversion (6.6.7)

| Request | C_RAND (Service_Kind 1000b) | Then |
|---|---|---|
| Set: the chosen services go to an MS or talkgroup | DIVONOFF 1, the services (DIV_VD, DIV_PD, DIV_SD, DIV_S), target MSI or GPI | a C_AHOY from DIVERTI: the radio uploads one UDT block, Address format, UDT_DIV set, to MSI or GPI, the address in ADDRESS1; C_ACK (0x60) or C_NACK (0x20 not supported, 0x23 not to there) ends it |
| Clear the radio's own | DIVONOFF 0, the services, target TSI | the acknowledgement |
| Clear every diversion to the radio | DIVONOFF 0, the services, target DIVERTI | the acknowledgement |

A call the radio asks for can be answered with a UDT download instead (6.6.1.3, table 6.65): supplementary, to the
radio, from MSI, GPI or DIVERTI, Address format. The request ends `diverted` and the view shows the new destination
(`divert_to`, `divert_group`). The radio doesn't call it by itself.

### 7.16 Ambient Listening (ALS, 6.6.2.6)

The radio answers ALS only with `T3_OPT_ALS`: it then transmits with no one at the PTT.

| Step | The radio |
|---|---|
| C_AHOY, individual voice, ALS_SERV 1, to the radio | C_ACKU Message_Accepted (0x60); without `T3_OPT_ALS`, C_NACKU MSNot_Supported (table 6.47). Starts T_ALS_REQUEST_LIFE_SPAN (5 s) |
| PV_GRANT from the same caller to the radio before it runs out | an ALS call: keys at once, for T_ALS (default 30 s), or T_ALS_E (60 s) when the grant has Emergency set |
| T_ALS over | unkeys (terminator with LC), then N_Maint P_MAINT (DISCON) and back to the TSCC |
| A grant after the life span, or from another caller | an ordinary private call |

Asking for one: a C_RAND for an individual voice call with ALS_SERV set (table 6.40). The radio listens on the grant.
The view shows `call_als` and `als_calls`.

### 7.17 Stopping the talker (6.4.14.3.3, 6.4.14.3.4)

`t3_ms_interrupt` asks the TS to stop the MS talking in the voice call the radio follows. The radio leaves the payload
channel and sends a status on the TSCC; the TS answers, then stops the talker with the reverse channel.

| Case | Status, to | After the answer |
|---|---|---|
| Payload interrupt, any voice call | 125, to the talkgroup or the other MS | back to the call, accepted or not (6.4.14.3.3.2); `ax.result` done = accepted |
| End an ALS call the radio asked for | 123, to the ALS MS | stays on the TSCC (6.4.14.3.4.3) |

- C_ACKD Message_Accepted: accepted (`done`). C_NACKD Not_Supported or Perm_User_Refused: `refused`. No answer ends
  as for any request (`no-answer`, `timeout`), and a payload interrupt goes back to the call all the same.
- Other calls' grants don't take the radio while it asks. An emergency grant does, and the radio stays in that call.
- 6.4.14.3.4.1 also says the ALS initiator goes back to the payload channel; the MS procedure (6.4.14.3.4.3) says it
  stays on the TSCC, and the radio follows that. The TS clears the call with P_CLEAR.
- The talker's side: the interrupted MS decodes the outbound reverse channel while it transmits, and on RC Command
  0100 (cease) or 0101 (cease request) ends after voice burst F with a terminator (6.4.15.2). The radio does this on
  boards that receive between their bursts on a split channel ([dmr-modem.md](dmr-modem.md), reverse channel).

### 7.18 Adjacent sites, Vote Now and signal quality (6.3.2.3, 6.3.3.1 a, 6.7.1.3, 6.7.1.7)

The standard leaves what a radio does with these to the manufacturer. The engine:

**Adjacent sites.** An Adjacent_Site or Vote_Now broadcast (tables 7.71, 7.79) names another TSCC: its logical
channel, or (channel 4 095) the frequencies of the appended block, and the 14 high bits of its system code. One that
belongs to the radio's network (authorized as an Aloha would be) is kept, up to 4, the oldest replaced. Hunting tries
them first. Another network's Aloha confirmed drops them, with the learned channels.

**Signal level.** The radio reports the tuned channel's level once a TDMA frame while it isn't following a call
(`t3_ms_level`); the engine averages it (3/4 old, 1/4 new) and starts again on each retune. The threshold is the
radio's roaming threshold (`roam_rssi_dbm`), unless the system's record sets one.

**Vote Now.** An idle radio, camped and confirmed, with nothing queued, samples the TSCC named for 360 ms: the frames
the TSCC keeps quiet for it (VOTE_BLK). An access started meanwhile brings it back at once. It counts the sample only
if it hears an Aloha or broadcast of that site's system code there, and then moves when the site is better:

| Priorities radiated (table 6.81) | Moves when |
|---|---|
| Adjacent higher than ours | its level is at or above the threshold |
| Adjacent lower than ours | ours is below the threshold, and it is 6 dB stronger and above the threshold |
| None, or equal | it is 6 dB stronger and at or above the threshold |

A move keeps the radio there unconfirmed until its Aloha (which registers again in a new SYS_AREA, 7.2); the TSCC
left becomes an adjacent site. Otherwise the radio returns, its level kept and the time away not counted in T_Nosig.
A site sampled isn't sampled again for 30 s unless ours is weak. `T3_OPT_NO_ROAM` turns sampling and the next rule off.

**Leaving a weak TSCC.** Camped, idle, below the threshold for 5 s, and with another candidate: the radio hunts from
the adjacent sites, and for one pass confirms only a TSCC at or above the threshold (the hunting threshold of
6.3.2.3). After a pass it takes any. With nowhere else to look it stays.

## 8. Timers and constants

| Name | Value | Standard range (Annex A) |
|---|---|---|
| T_Nosig | 5 s | 1-15 s |
| Hunt dwell | 700 ms | |
| Settle after a retune | 60 ms: what is heard sooner belongs to the old channel | |
| TV_Inactive | 2.5 s | 0-20 s |
| Call ceiling | 300 s | |
| Re-grant hold | 2 s | |
| MBC continuation gap | 130 ms: one TDMA frame, one lost | |
| TRand_TC | 10 s | 2-60 s |
| TP_Timer | 20 s | |
| TNP_Timer | 10 s | |
| NRand_NR, NRand_NE | 6, 10 | |
| N_Maint | 4 | |
| Registration retry | 30 s | |
| Vote Now sample | 360 ms | VOTE_BLK: 2-10 frames |
| Vote margin | 6 dB | |
| Sample again | 30 s | |
| Weak before leaving | 5 s | |

These values are fixed, but for four the debug link may set (section 11; 0 keeps the default): T_Awake (power
save), T_ALS, T_ALS_E and the Vote Now sample time.

## 9. Tests

### 9.1 PDUs (`tests/host/t3PduTest.c`)

`tests/host/data/t3_vectors.jsonl` holds PDUs packed by a separate encoder written from the field tables of 7.1, 7.2
and Annex C, with the field values beside each. The test parses each one, checks every field, rebuilds the exact
octets, and checks the CRC (including corrupted copies and the wrong mask). The same file is meant for every codec of
these PDUs. Then the rules: Mask, delay tables, acknowledgement classes, authorization order, SYS_AREA, plan order and
the learned ring.

### 9.2 Engine (`tests/host/t3MsTest.c`, `t3AccessTest.c`, `t3SiteTest.c`, `t3ReplayTest.c`)

`t3MsTest` drives the engine with built PDUs: hunting over silent and foreign candidates, the settle time, CRC and
color code filters, every follow rule, every return, T_Nosig, short LCs, learning from Chan_Freq and CG_AP, late MBC
continuations, monitor and emergency options, adjacent sites and the hunt order, Vote Now (stronger, within the
margin, silent, priorities, `T3_OPT_NO_ROAM`), leaving a weak TSCC and the hunting threshold, stop.

`t3AccessTest` runs the access side against a TSCC simulated frame by frame (CACH, AT bit, Aloha): registration and
its answers, the retry spacing and count, denied and refused areas, the AT bit, Mask, Service Function and first-Aloha
rules, group and private calls with their acknowledgements and timers, cancel, P_PROTECT, P_MAINT at the end, the
answers to C_AHOY and P_AHOY, de-registration, and the follow rules for the radio's own calls.

`t3SiteTest` runs the radio's own receive chain (CACH, short LC, slot type, BPTC) and the engine on raw control
channel bursts recorded off air on the SDR bench (`tests/host/data/t3_site_tscc.jsonl`, about 30 s of a test site's
TSCC, no voice): the engine camps, reads the system and learns the channel the site broadcasts.

`t3ReplayTest` feeds the engine off-air recordings decoded on the SDR bench, in time order; the engine hears a channel's
records only while it is tuned there. Its recordings are not in the repository: `RFOUNDRY_T3_RECORDINGS` names a
manifest (format in the test's header) with the files and what is known about the recorded system. Without it the test
skips, so CI doesn't run it. It checks that the engine camps within 1.5 s, reads the system code and Aloha parameters,
stays on the TSCC except across gaps in the recording, follows only planned channels and known talkgroups, keeps a call
alive on its voice, and never camps on another system or on a control channel that isn't Tier III.

### 9.3 On the radio's side (`tests/host/t3CodeplugTest.c`, `t3ViewTest.c`, `radioCoreTest.c`)

`t3CodeplugTest` reads systems and trunked channels from a simulated codeplug (section 12) and writes them over the
debug link (`DL_CMD_CPKV`). `t3ViewTest` checks that each Tier III debug link view section fits one reply with every
field at its widest. `radioCoreTest` runs the radio core with the engine: start, follow, PTT, fallback, the kill.

## 10. Not done yet

| Item | Needs |
|---|---|
| CACH short LC reception on modems that don't report it yet (`hrc_chip_t.slc_rx_addr` 0) | the location of the chip's short LC |
| A late veto of a random access whose slot turned busy | slot-timed transmit in the modem drivers |
| Duplex calls (voice, packet data) | not planned: the radios are simplex |
| Power save: the receiver off while asleep on boards without `rx_sleep_dmr` | each board's `rx_sleep` on a DMR channel |
| The interrupted talker's side on radios that can't receive between their bursts on a split channel (6.4.15.2) | the modem's `tx_listen_split` on those boards |
| A P_CLEAR sent while the radio is still talking on the payload channel is missed; on a slot whose carrier stays keyed (idle bursts count for TV_Inactive) the radio stays until the call ceiling | a rule for leaving a slot that carries only idle bursts after the radio's own over |

## 11. On the radio

`core/radio/radio_sm.c` runs one engine; `core/radio/radio_trunk.{h,c}` holds the system it runs.

| Step | What the radio core does |
|---|---|
| Set-up | A trunked channel selected (12.4) loads its system from the codeplug and starts it at the next tick. The debug link can instead stage a system in RAM (`radio_t3_staged`, `DL_CMD_T3`, [debuglink.md](debuglink.md) 3.26): its ID, authorization, color code, control channels, channel table, talkgroups; `RADIO_T3_START` copies it and starts the engine on whatever channel the radio is on. |
| Leave | `RADIO_T3_STOP`, a conventional channel or the VFO stops the engine and goes back to the channel or VFO. |
| Fallback | 12.5: `t3_tick` watches how long the engine has hunted. |
| Tune | A `T3_ACT_TUNE` sets the channel to DMR, repeater mode, the action's frequencies and color code. Slot 0 (the TSCC, hunting) listens to both slots; a payload channel listens to its slot only. A new carrier is a full tune; the same carrier is a reconfiguration of the stack. |
| Slots | Each slot tick from the modem goes to `t3_ms_slot` with its slot number and its CACH AT bit (`DMR_TICK_AT_KNOWN`, `DMR_TICK_AT_BUSY`). A modem that doesn't number slots gets them alternating; one that doesn't read AT gets every slot free. The HR_C6000 and HR_C7000 drivers number them and read AT once locked to the CACH's TC ([modem-hrc7000.md](modem-hrc7000.md) 2.5). |
| Receive | The stack's receive tap hands CSBKs and MBC blocks to `t3_ms_rx`. Any other burst on a known slot, idle ones excepted, is activity on that slot (`t3_ms_activity`). |
| Send | A `T3_ACT_SEND` keys one CSBK as a data list, on the action's slot only, and not while the radio is talking. A `T3_ACT_TUNE` that comes while the list is still with the stack waits until it has gone out (at most 500 ms): a new slot or channel would drop it, and an answer before leaving a payload channel (a selective clear's P_ACKU) would never be sent. |
| PTT | Camped, PTT asks for a call to the system's talk target (the first talkgroup with `T3_TG_TALK` when none is set) and waits with PTT held. The grant puts the radio on the payload slot and keys it. A refusal, no answer or timeout denies TX (`TXDENY_NO_DMR`). Released before the grant, the request is canceled. |
| Following | The stack takes the system's ID as its own, hears every call on the slot, and talks to the call's group or to the other party. The radio's own transmission counts as the call's activity: it never hears its own slot, so TV_Inactive would otherwise end the call while it talks. |
| Status | `RADIO_T3_STATUS` sends the staged status (section 7.5). The last status heard is in the view. |
| Short data poll | `RADIO_T3_SDPOLL` polls the staged MS (section 7.6). The reply to a poll is staged with the system (`DL_T3_POLL_REPLY`). |
| Interrupt | `RADIO_T3_INTERRUPT` and `RADIO_T3_ALS_END` call `t3_ms_interrupt` (section 7.17), not while the radio's PTT is held. |
| Short data | `RADIO_T3_SDATA` sends the staged message (section 7.6): the upload is a UDT header and its blocks, one data source on the TSCC's slot. Data headers and rate 1/2 blocks heard go to the engine. The last message heard is in the view. |
| Packet data | The data service's transmissions (text, IP packets, replies) start through `data_tx_start`. Camped, the radio keeps the burst list, drops its preambles, and asks for a data channel to the data header's destination (section 7.7). Once following the granted call and settled, it sends the list on the call's slot; a refusal, no answer, timeout or a call that ended first gives the list back failed. On a data call already, a list to the same party goes at once. While a data call is followed, its data headers and blocks go to the data service, not the engine. The data service can't send while hunting or in a voice call. Listen before talk is skipped: the TSCC gives the slot. |
| Emergency | `data_t3_start` sends the data service's alarm as status 126; `t3_emergency_tick` sends the cancel and passes a status 126 heard to the emergency module (section 7.8). |
| View | The `t3` section ([debuglink.md](debuglink.md) 3.26). |

Tests: `tests/host/radioCoreTest.c` (`test_t3_talk`) runs a simulated site through PTT, request, grant and a call with
nothing heard; `test_t3_pdata` through a text message out on a granted channel and one in; `test_t3_emergency` through
the alarm, the emergency call, the cancel and an alarm heard. `test_t3_channels` selects trunked channels from the
codeplug (same system, another system, unavailable, scan); `test_t3_never_keys` checks that a receive-only channel, a
listen-only system and a channel with no system send nothing; `test_t3_fallback` runs the fallback and its probes.
`tests/hil/test_t3_site.py` runs camp, registration, group and private calls against a site on the SDR bench.

## 12. In the codeplug

A trunked system is a record of its own; a **trunked channel** is an ordinary codeplug channel tagged with the system
it uses. Zones hold trunked channels like any other, mixed with conventional ones. The records are system type 1 and
channel kind 1 of the generic layer ([channel-modes.md](channel-modes.md)); their layouts are in
[dmr-trunking-codeplug.md](dmr-trunking-codeplug.md). This section is what the radio does with them.

| Keys | Records |
|---|---|
| 0x0100 + n | System n; the radio runs n = 0 to `RADIO_T3_SYSTEMS` - 1 (12.1) |
| 0x0200 + i | A system entry: a channel of a system, for the hunt list and the plan's table (12.2) |
| 0x0300 + n | System n's authentication key (12.3) |
| 0x0400 + c - 1 | The tag of channel c (12.4) |
| 0x0800 + n | System n's short data poll setting and reply (12.7) |
| 0x0900 + n | The supplementary user data the radio offers on system n (12.8) |

### 12.1 System

The record: [dmr-trunking-codeplug.md](dmr-trunking-codeplug.md) 1.

**Usable:** a system is usable when it has a control channel and, if its record says it needs its key, a key. A
channel tagged with a system that isn't usable is **unavailable** (12.4).

**Listen only.** A system record without its transmit flag is listen only: the radio follows the control channel and
the calls of its talkgroups, and never registers, affiliates or transmits there, whatever a channel's tag says. A
zeroed or short record is listen only.

**Authentication is the system's choice.** A system that doesn't authenticate needs no key: the radio registers and
answers a challenge "not supported". An open system (an amateur one, say) leaves the needs-key flag off and no radio
needs a key.

**Stun.** A stun is taken only with option 0x0400. With a key it is authenticated first (section 7.9); without one it
is taken as sent, so an open system's radios leave 0x0400 off.

### 12.2 System channels

The entries: [dmr-trunking-codeplug.md](dmr-trunking-codeplug.md) 2. Control channels are hunted in entry order. A
system takes its first `T3_CTRL_MAX` control channels and first `RADIO_T3_CHANS` channels; the rest are skipped.

### 12.3 Authentication key

The record: [dmr-trunking-codeplug.md](dmr-trunking-codeplug.md) 3. The key is in the codeplug so the Codeplug Editor
can set it; the editor's CSV and other-format exports leave it out.

### 12.4 Trunked channels

A trunked channel is a codeplug channel with a tag of kind 1 ([channel-modes.md](channel-modes.md) 2,
[dmr-trunking-codeplug.md](dmr-trunking-codeplug.md) 4): the system it is on, whether it may transmit, and a fallback
channel of its own.

**Name check.** A tag whose check doesn't match its channel's name is not live, and the channel is an ordinary one
([channel-modes.md](channel-modes.md) 2.2, 2.3). The same check guards a fallback channel, so a fallback never lands
on a renumbered conventional channel.

**The channel record** carries what the open-firmware format can say: DMR, receive only, RX and TX on the system's
first control channel, the contact (the talk target: a talkgroup, a private contact or the all-call contact) and the
RX group list (the talkgroups followed). A firmware or tool that doesn't know the tags sees a receive-only DMR channel
and never transmits on it. This firmware takes from the record the name, contact, RX group list, power, TX time-out
and talker alias; it ignores the frequencies, slot, color code, tones, direct mode and SFR.

**Transmit** is the tag's flag, not the record's: a tag without it (a zeroed one too) makes a receive-only trunked
channel. The radio follows the system on it as on a listen-only system (12.1) and refuses PTT.

| The user selects | The radio |
|---|---|
| a trunked channel, its system not running | starts the system (section 11), its talkgroups the channel's |
| a trunked channel of the system running | keeps the TSCC; takes the channel's talk target and talkgroups, and registers again only when its talkgroups changed and the system registers with them (0x0040) |
| a trunked channel of another system | leaves the running one and starts the new one |
| a conventional channel or the VFO | leaves the system |
| a receive-only trunked channel | runs the system listen only: no registration, nothing sent, PTT refused |
| an unavailable trunked channel (its tag names no system, or one that is absent, not a Tier III system, not usable or past the systems the radio runs) | leaves any system and works the record receive only, so PTT is refused; the screen says "System not set up" |

Leaving sends nothing: a site drops a radio that has gone quiet. Zone scan passes trunked channels by:
a frequency is not what the radio follows there. The talkgroups' re-registration waits until the user has rested on a
channel for `T3_REG_TG_DELAY_MS` (1 s), so turning the knob through channels registers once.

**On screen.** A zone or channel list names a trunked channel's system after its name, in place of the frequency.
An unavailable one is dimmed: OK on it says "System not set up" and stays, and channel stepping passes it. On a
trunked channel Home's chips row shows the system's name and its state in place of slot and color code: Ready,
Registering, Searching, Other system, Reg. refused, In call, Refused, Fallback (12.5) or System not set up, then the power.

**Other system.** Searching while the hunt hears only another system's control channel (`t3_ms_other`, 6.1). Home's
activity area names it in place of the last call: "Heard: Huge, net 0" and "site 83", with ", category A only" or
", category B only" when its PAR allows one category; the first line alone where the area has room for one. The user compares it with the system's model and network
(12.1). A wrong color code shows as Searching: the radio decodes nothing on the channel.

**Reg. refused.** The system answered the registration with Reg_Refused (6.4.4.1): it doesn't accept this radio, most
often because its ID isn't in the system's records. The radio hunts and asks again when it comes back to the same
TSCC (`t3_ms_reg_refused`). The state shows while hunting and while registering again there, and clears once a
registration is accepted. Refused is Reg_Denied: barred from the area, never asked again.

**PTT refused** on a trunked channel says why (`TXDENY_TRUNK`, the reasons P25 trunking shares):

| State | Shown |
|---|---|
| searching, another system heard | Another system heard / Check system settings |
| registration refused (Reg_Refused) | Registration refused / Ask to add your ID |
| searching | No control channel |
| not registered: registering, or the registration denied | Not registered yet |
| the system listen only here, or no talk target | This channel is RX only |
| the system refused the call, or the radio is stunned | System denied the call |
| no answer, a timeout | No answer from system |
| the control channel lost during the request | No control channel |
| the radio's call granted on a channel the codeplug doesn't list | Unknown channel N / Check system settings |

At power-on the radio restores the channel it was on, so a trunked channel starts its system. Keys written while a
system runs are taken at the next selection of a channel. A channel shown as not set up is selected again whenever a key is
written, so a system written after its channel (an editor writing channels first) is used at once.

### 12.5 Fallback

A system or a trunked channel may name a conventional **fallback channel**. When the radio has found no TSCC for
the hold time, it goes to the fallback channel and works there as on any conventional channel, RX and TX. The screen
says "Fallback".

| Rule | Value |
|---|---|
| Which | the trunked channel's own fallback when its tag has one (which may say none), else the system's; none when the channel's tag sets "no fallback" |
| Checked | the fallback must be a conventional channel (no live tag) whose name check matches; else there is none |
| When | the radio has been hunting, with no TSCC confirmed, for the hold time (0 = 30 s) |
| Looking back | every `RADIO_T3_PROBE_MS` (10 s), once the fallback channel is idle (nothing heard or sent for `RADIO_T3_PROBE_QUIET_MS`, 2 s, PTT up), the radio hunts the system's control channels for up to `RADIO_T3_PROBE_HUNT_MS` (2 s) |
| Returning | a TSCC of the system confirmed: the radio stays on it, takes the trunked channel back, registers, plays the system-back tone ([radio-features.md](radio-features.md) 4) and says "Back on <system>" |
| Not found | back to the fallback channel |
| PTT while looking | back to the fallback channel at once: PTT talks there |
| Leaving | the user selecting another channel ends the fallback as any change does |

### 12.6 Sharing

A zone share carries a zone's trunked channels in its modes part ([zone-share.md](zone-share.md) 4.2, 5.5), as the
system's sharing level allows:

| Sharing | The share carries |
|---|---|
| 0 off (the default) | the channels with no system: they come in unavailable |
| 1 identity only | the channels and the system's name, flags and identity (model, NET, category). Enough for a radio that already has the system |
| 2 the whole system | the channels, the system's block (color code, options, plan, sites) and its channels |

Never carried: the radio's ID on the system, a system's or a channel's fallback channel, kill or stun state,
registration, the short data poll setting (12.7), the supplementary data (12.8). The key goes only when the sender turns
keys on for that share, which a radio can't yet ([dmr-trunking-codeplug.md](dmr-trunking-codeplug.md) 6).

A receiver matches a shared system to one of its own by type and identity. A match is used as it is: the share
changes nothing in it. With no match, a share with the whole system adds it at the lowest free system number; one
with the identity only, or none, brings its channels in with no system, receive only and **unavailable**. The offer
says so before the import, and the banner after it: "3 trunked: system not set up". They work once the Codeplug Editor
sets the system up and names it in their tags.

A shared channel keeps transmit only when the receiver has its system; the system's transmit flag then rules, as the
sender set it. A system that needs its key comes in without one, so its channels are unavailable until the key is
set.

### 12.7 Short data poll

Key 0x0800 + n, system n's short data polling (6.6.5): the format and size the system's radios have agreed on, and
this radio's own reply.

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | UDT_Format (table 7.88): of the reply, and asked for when the radio polls |
| 1 | 1 | Blocks to ask for, 1-4 (0 or past 4: 4) |
| 2 | 0-46 | The reply's octets, in that format (ISO 8 bit: the text itself) |

With no record the radio asks for ISO 8 bit in 4 blocks and answers a poll "not supported". A reply is cut to the
blocks a poll asks for.

**From the menus.** On a trunked system the actions on a person have Poll short data (`RREQ_T3_POLL`): the radio polls
the MS in this format and size. The answer comes as short data and goes into the message inbox (7.6). A poll that
can't start, is refused, goes unanswered or is cut short by leaving the system counts in `t3.poll_fails`; the screen
says "Poll: no short data".

### 12.8 Supplementary data

Key 0x0900 + n, the supplementary user data (6.4.13, section 7.6) the radio offers with each call on system n: data in a
format and size prearranged with the system, a position or a unit number, say.

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | UDT_Format (table 7.88) |
| 1 | 1-46 | The octets, in that format (ISO 8 bit: the text itself) |

With no record, or one without octets, the radio offers none. The debug link's SUPL (11) replaces it for the staged
system until the next load. Built with `CONFIG_DMR_TRUNK_SUPL`; a build without it ignores the record.

### 12.9 Calls

| Call | |
|---|---|
| `codeplug_channel(c, &ch)` | The channel with its live tag applied: `ch.kind` 1, `ch.system`, `ch.rx_only` from the tag's transmit flag |
| `radio_t3_load(n, &setup)` | Fills a setup from system n's records; false when the system is absent or not usable |
| `radio_t3_talk(&ch, &setup)` | The channel's contact and RX group list as the setup's talk target and talkgroups |
| `radio_t3_fallback(c, &setup, &ch)` | Channel c's fallback channel: its number (0 none) and its record |
| `radio_t3_name(n, buf)` | The name only, for a list |
| `radio_t3_usable(n)` | Whether system n is usable |

The debug link's LOAD (section 11) stages system n with no channel's talkgroups, without starting it.
