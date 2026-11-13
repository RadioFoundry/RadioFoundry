<!-- SPDX-License-Identifier: MIT -->

# DMR Tier III trunking (mobile station)

A radio on a DMR Tier III trunked system (ETSI TS 102 361-4) finds the system's trunked site control channel (TSCC),
stays on it, and follows the channel grants for the calls it wants to hear. Switch: `CONFIG_DMR_TRUNK`, a child of
`CONFIG_DMR` (`docs/build-config.md`); the access side (section 7) is `CONFIG_DMR_TRUNK_TX`, a child of it. The radio core runs the engine on
a system set up over the debug link (section 11); the codeplug has no keys for a system yet, and there is no UI.

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

## 1. Standards position

Everything here comes from ETSI TS 102 361-4 (the Tier III trunking protocol) and TS 102 361-1 (the air interface).
Section and table numbers below are TS 102 361-4's unless marked -1.

- Only the standard's PDUs and procedures, with the standard feature set ID (FID 0). A CSBK with another FID is
  counted and ignored.
- No vendor's own trunking protocol. A system that sends its control channel with another CRC or framing fails the
  standard CRC checks and is never confirmed as a TSCC (section 9.2 checks this on recordings).
- On a trunked system the radio transmits only the access side (section 7) and the calls it is granted.
- No authentication: a challenge is answered "not supported".

## 2. Modules

All in `core/dmr/trunk/`. None includes a board, platform or RTOS header; the host tests run them as they are.

| File | Content |
|---|---|
| `t3_pdu.{h,c}` | CSBK and MBC codec (7.1), CRC masks, C_SYS_Parms / P_SYS_Parms short LC (7.1.2) |
| `t3_sys.{h,c}` | C_SYScode partition, TSCC authorization, SYS_AREA, the Mask rule, access delay tables (6.2-6.4) |
| `t3_plan.{h,c}` | Logical channel number to frequencies: learned, table, fixed formula (Annex C) |
| `t3_ms.{h,c}` | The mobile station engine: hunt, confirm, camp, follow, return, access (6.2-6.6, Annex D) |

## 3. PDUs

A CSBK is 12 octets after the BPTC (-1 9.3.6): octet 0 holds LB, PF and the 6-bit opcode, octet 1 the FID, octets 2-9
the fields, most significant bit first, octets 10-11 the CRC-CCITT.

| Block | CRC mask (-1 B.3.12) |
|---|---|
| CSBK | 0xA5A5 |
| MBC header | 0xAAAA |
| MBC continuation (last block) | none; the CRC covers the continuation only |

Decoded opcodes (`t3_csbk_parse`): C_ALOHA, C_BCAST, C_AHOY, C_RAND, C_ACKD, C_ACKU, P_ACKD, P_ACKU, P_CLEAR,
P_PROTECT, P_MAINT and the grants PV, TV, BTV, PD, TD (and the duplex and multi-item variants). `t3_csbk_parse`
returns false for another opcode but still fills the opcode, FID and the raw fields. `t3_csbk_build` packs the same
set; the engine's tests and the access side use it.

| PDU | Fields kept |
|---|---|
| C_ALOHA | TSCCAS, site timeslot sync, version, offset, active connection, Mask, service function, NRand_Wait, Reg, Backoff, C_SYScode, MS address |
| Grants | LPCN (12 bits), slot, late entry (TV/BTV), high rate (PD/TD), emergency, offset, destination, source |
| C_BCAST | announcement type, parameters 1 (14 bits), Reg, Backoff, C_SYScode, parameters 2 (24 bits) |
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
against the radio's category. SYS_AREA is the top DMRLA bits of SITE. Response_Check is bits 7-2 of the code.

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

A grant whose channel nothing resolves is not followed. The engine counts it (`unmapped`) and keeps the last such
LPCN, so the user can see which channel is missing from the plan.

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
| CAMPED | listens to the TSCC, both slots |
| FOLLOW | listens to one slot of a payload channel |

**Hunt.** The candidates are the programmed control channels in order, then the extra hunt LPCNs through the plan.
Each gets a dwell (700 ms: at least 12 TSCC slots and two short LCs). After a loss, the lost TSCC is tried first.

- An authorized C_ALOHA confirms the channel: the radio camps, and keeps the color code it was heard with.
- An Aloha of another system moves the hunt to the next candidate at once.
- Two C_SYS_Parms of another system in a row do the same. One alone is not trusted: the short LC CRC is 8 bits, and
  corrupt words pass now and then.

**Camped.** Every valid PDU from the TSCC (CRC good, our color code; Alohas and broadcasts also with our system code)
restarts T_Nosig. When it runs out, the engine hunts again. Short LCs don't restart it: every channel of the site
carries them. The registration state follows the latest Aloha or broadcast Reg flag:

| Reg | Listen-only | State |
|---|---|---|
| 0 | either | not needed |
| 1 | yes | listen |
| 1 | no | needed; the access side registers (section 7.2) |

A registration record holds the area it was made in: MODEL, NET and the DMRLA top bits of SITE (SYS_AREA, 6.4.4).
A TSCC of another area needs a new registration. An area that denied registration stays denied until the engine
restarts.

**Grants.** While camped, a grant is followed when:

| Grant | Followed when |
|---|---|
| TV, BTV | the radio is the source (its own call); or, unless it is a late-entry grant for a talkgroup call the user left, the talkgroup is in the system's list with the listen flag; or it is an all-call (ALLMSID, ALLMSIDL, ALLMSIDZ) and all-calls are on; or the grant is an emergency and emergencies on any talkgroup are on; or monitor is on |
| PV | the radio is the source; or the destination is the radio's ID and private calls are on |
| PD, TD | never yet (data comes later) |

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
| `t3_ms_deregister` | switching off or leaving the system |
| `t3_ms_can_talk` | may the user talk on the payload channel now |

`ax.result` holds the outcome: pending, granted, done, refused (`ax.reason` has the reason code), no answer, timeout,
not allowed, lost (the TSCC was lost meanwhile), cancelled.

### 7.1 Random access (6.2)

One request at a time, on the TSCC's first slot (TS1 on a dedicated TSCC).

| Step | Rule |
|---|---|
| Before the first try | an Aloha heard since the radio came to this TSCC |
| Each inbound slot | the AT bit for the slot's inbound decoded and free (below); the Aloha's Mask matches the radio's ID; the Service Function allows the service (table below). An emergency skips Mask and Service Function unless the Mask is 24 |
| A slot not permitted | wait for the next permitted one |
| After a try | wait NRand_Wait + 1 frames for an answer |
| No answer | try again 1 to Backoff frames later, at random (table 6.3); NRand_NR tries (NRand_NE for an emergency), then no answer |
| Overall | TRand_TC from the request, then timeout |
| C_WACKD, C_QACKD | stop retrying; TP_Timer for a call, TNP_Timer for the rest; a further one restarts it |

Which CACH's AT bit describes the TSCC's inbound depends on the Aloha's Offset bit. A CACH describes the inbound slot its
TC names (361-1 4.5). With aligned timing (Offset 0) logical channel 1 sends on inbound slot 2 (361-1 table 5.1), so the
TSCC slot's AT is in the CACH naming the other slot: the one right after the TSCC's own burst, where a PDU wanting an
answer is followed by AT busy (361-4 6.2.1.1.3). With offset timing it is the CACH naming the TSCC's slot
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
subscribes to talkgroups. Sending it drops the old record.

| `T3_OPT_REG_TG`, talkgroups listened to | C_RAND |
|---|---|
| off | Trg_Adr_Cnts 00b, the system code |
| on, one | Trg_Adr_Cnts 01b, the talk group |
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

PTT on the payload channel (`t3_ms_can_talk`): following, not disabled by P_PROTECT, and not a broadcast (BTV_GRANT)
or all-call someone else made. At the end of a call the radio made, or of any private call, `t3_ms_end_call` sends N_Maint P_MAINT
(DISCON, to TSI) in consecutive frames on the call's slot, then returns. A talkgroup call's recipient returns
without sending, and the call's late-entry grants don't bring it back.

### 7.4 Answers (6.4.8, 6.6.1.2.2, 6.6.2.3)

A C_AHOY or P_AHOY to the radio's ID (or to the followed talkgroup, on the payload channel) is answered with C_ACKU or
P_ACKU on the inbound slot after the next CACH of the slot it came on, without random access. The answer's target is
the AHOY's source, its additional address the radio's ID.

| AHOY | Answer |
|---|---|
| radio check (kind registration, from TSI) | MS_Accepted |
| availability check (kind individual voice): OACSU | MS_Accepted |
| availability check (kind individual packet data) | Message_Accepted (section 7.7); EquipBusy_Refused (0x13) while the data service sends; not supported in a build without it |
| availability check (kind duplex voice or duplex packet data) | MS_Duplex_Not_Supported (0x16): the radio is simplex (6.6.9.2.5, 6.6.10.2.5). It never follows a duplex grant (53, 54), whose bit 15 is Call Direction; their payload channel always uses offset timing (tables 7.10, 7.14) |
| packet data radio check (P_AHOY kind packet data, from TSI) | P_ACKU Message_Accepted (6.6.3.3.2.1) |
| call cancel (kind cancel) on the TSCC | MS_Accepted |
| selective clear (P_AHOY kind cancel) | Message_Accepted, then return |
| status (kind status) | MS_Accepted (section 7.5) |
| status poll (kind status, status 127) | Accepted for the Status Polling Service (0x47), Response_Info the radio's status |
| stun or revive (kind supplementary, from STUNI) | without K: Message_Accepted; with K: C_ACKVIT, the radio's challenge (section 7.9) |
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

ISO 7-bit text is given one character an octet (1-52) and packed 7 bits each, first character first.
| UAB | blocks less one |
| SF, opcode | 0 (the user's data); C_UDTHD outbound, C_UDTHU inbound |

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
Supplementary data (SF set) isn't taken. A repeat (the same caller, destination, format and octets within
`T3_REPEAT_MS`) is answered again but not counted, as for a status.

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

### 7.7 Packet data (6.6.3)

Packet data uses the conventional data stack ([dmr-data.md](dmr-data.md)) on a payload channel the TSCC grants for
each transmission. Single slot, single item only; no gateway addressing.

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

The availability check for data to the radio (C_AHOY, Service_Kind 0010b) is answered C_ACKU Message_Accepted
(6.6.3.2.6).

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
Without K (section 7.10) the radio answers C_ACKU Message_Accepted and acts (6.4.9.1).

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
Killed, the radio hears and sends nothing on any trunked system until it restarts: the kill is kept in RAM only, so a
test can't disable a radio for good.

Stunned, the radio:

- keeps hunting, registration, de-registration, radio checks and stun/revive;
- asks for no call, status, short data or packet data (refused as not allowed; PTT is denied);
- follows no grant, keeps no status or short data heard, and answers no other AHOY.

The stun outlasts leaving the system and starting it again, unless the new system is another network. It is kept in
RAM, with the staged system: a restart of the radio clears it.

### 7.10 Authentication (6.4.8)

The system may hold a 128-bit key K and a 24-bit PSN (`t3_cfg_t.auth_key`, `psn`; 0: no PSN). A C_AHOY to the radio,
Service_Kind 1110b, whose source is below the gateway block (0-0xFFFCDF) is a challenge:

1. RC4 is keyed with the challenge (3 octets, most significant first) followed by K.
2. Of its keystream, octets 1-256 are dropped; octets 257-259 are the response, most significant first.
3. The response is XORed with the PSN.

The radio answers C_ACKU, Reason Authentication_Response (0x48, table 7.42), the target the response, the additional
address its own ID. A challenge during an access (a registration or a call request) is progress: the access waits
TNP_Timer for its final answer. Without K a challenge is answered not supported. A radio check (from TSI, above the
range) is unchanged. `t3_auth_response` passes the test vectors of tables 6.15 and 6.16.

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
| Counter | The CSC of the last C_SYS_Parms heard, carried on one step per 120 ms |
| Early | The radio wakes two CSC steps (240 ms) before its frame, so the receiver has synchronized |
| Asleep | Camped, confirmed, registered, no access, nothing queued, outside the wake frame, past T_Awake |
| T_Awake | 2 s (`awake_ms`): after registering, any access, traffic for the radio (AHOY, acknowledgement) and a call's end |
| T_Nosig | Counts only time awake |
| MassReg | A due mass registration wakes the radio |

`t3_ms_asleep` is the engine's decision. Switching the receiver off with it needs each board's `rx_sleep` to work on a
DMR channel; today it works on FM only (section 10).

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
- Not done: the talker's side. The interrupted MS decodes the outbound reverse channel while it transmits, and on RC
  Command 0100 (cease) or 0101 (cease request) ends after voice burst F with a terminator (6.4.15.2). The modem drivers
  drop reverse channel bursts.

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

The system's record may override T_Nosig, TV_Inactive, the dwell, TRand_TC, TP_Timer and TNP_Timer; 0 keeps the
default.

## 9. Tests

### 9.1 PDUs (`tests/host/t3PduTest.c`)

`tests/host/data/t3_vectors.jsonl` holds PDUs packed by an independent implementation from the field tables of 7.1,
7.2 and Annex C, with the field values beside each. The test parses each one, checks every field, rebuilds the exact
octets, and checks the CRC (including corrupted copies and the wrong mask). The same file is meant for every codec of
these PDUs. Then the rules: Mask, delay tables, acknowledgement classes, authorization order, SYS_AREA, plan order and
the learned ring.

### 9.2 Engine (`tests/host/t3MsTest.c`, `tests/host/t3ReplayTest.c`)

`t3MsTest` drives the engine with built PDUs: hunting over silent and foreign candidates, the settle time, CRC and
color code filters, every follow rule, every return, T_Nosig, short LCs, learning from Chan_Freq and CG_AP, late MBC
continuations, monitor and emergency options, stop.

`t3AccessTest` runs the access side against a TSCC simulated frame by frame (CACH, AT bit, Aloha): registration and
its answers, the retry spacing and count, denied and refused areas, the AT bit, Mask, Service Function and first-Aloha
rules, group and private calls with their acknowledgements and timers, cancel, P_PROTECT, P_MAINT at the end, the
answers to C_AHOY and P_AHOY, de-registration, and the follow rules for the radio's own calls.

`t3ReplayTest` feeds the engine off-air recordings decoded on the SDR bench, in time order; the engine hears a
channel's records only while it is tuned there. The recordings are not in the repository: `RFOUNDRY_T3_RECORDINGS`
names a manifest (format in the test's header) with the files and what is known about the recorded system. Without
it the test skips. It checks that the engine camps within 1.5 s, reads the system code and Aloha parameters, stays on
the TSCC except across gaps in the recording, follows only planned channels and known talkgroups, keeps a call alive
on its voice, and never camps on another system or on a control channel that isn't Tier III.

## 10. Not done yet

| Item | Needs |
|---|---|
| A trunked system in the codeplug, and its screens | codeplug keys for systems |
| CACH short LC reception | the modem drivers' CACH report |
| A late veto of a random access whose slot turned busy | slot-timed transmit in the modem drivers |
| Supplementary data, a list subscription without registering | later |
| Short data polling from the UI, and the reply in the codeplug | trunked systems in the codeplug |
| A kill kept across a restart, and conventional DMR off when killed | later |
| A stun kept across a restart of the radio | trunked systems in the codeplug |
| Duplex calls (voice, packet data) | not planned: the radios are simplex |
| Data to a gateway, adjacent sites | later |
| Power save: the receiver off while asleep (boards' `rx_sleep` on DMR channels) | later |
| The interrupted talker's side: the reverse channel's cease command while transmitting (6.4.15.2) | reverse channel reception in the modem drivers |

## 11. On the radio

`core/radio/radio_sm.c` runs one engine; `core/radio/radio_trunk.{h,c}` holds the system it runs.

| Step | What the radio core does |
|---|---|
| Set-up | The debug link stages a system in RAM (`radio_t3_staged`, `DL_CMD_T3`, [debuglink.md](debuglink.md) 3.26): its ID, authorization, color code, control channels, channel table, talkgroups. `RADIO_T3_START` copies it and starts the engine. |
| Leave | `RADIO_T3_STOP`, or any channel or VFO change, stops the engine and goes back to the channel or VFO. |
| Tune | A `T3_ACT_TUNE` sets the channel to DMR, repeater mode, the action's frequencies and color code. Slot 0 (the TSCC, hunting) listens to both slots; a payload channel listens to its slot only. A new carrier is a full tune; the same carrier is a reconfiguration of the stack. |
| Slots | Each slot tick from the modem goes to `t3_ms_slot` with its slot number and its CACH AT bit (`DMR_TICK_AT_KNOWN`, `DMR_TICK_AT_BUSY`). A modem that doesn't number slots gets them alternating; one that doesn't read AT gets every slot free. |
| Receive | The stack's receive tap hands CSBKs and MBC blocks to `t3_ms_rx`. Any other burst on a known slot, idle ones excepted, is activity on that slot (`t3_ms_activity`). |
| Send | A `T3_ACT_SEND` keys one CSBK as a data list, on the action's slot only, and not while the radio is talking. |
| PTT | Camped, PTT asks for a call to the system's talk target (the first talkgroup with `T3_TG_TALK` when none is set) and waits with PTT held. The grant puts the radio on the payload slot and keys it. A refusal, no answer or timeout denies TX (`TXDENY_NO_DMR`). Released before the grant, the request is cancelled. |
| Following | The stack takes the system's ID as its own, hears every call on the slot, and talks to the call's group or to the other party. The radio's own transmission counts as the call's activity: it never hears its own slot, so TV_Inactive would otherwise end the call while it talks. |
| Status | `RADIO_T3_STATUS` sends the staged status (section 7.5). The last status heard is in the view. |
| Short data poll | `RADIO_T3_SDPOLL` polls the staged MS (section 7.6). The reply to a poll is staged with the system (`DL_T3_POLL_REPLY`). |
| Interrupt | `RADIO_T3_INTERRUPT` and `RADIO_T3_ALS_END` call `t3_ms_interrupt` (section 7.17), not while the radio's PTT is held. |
| Short data | `RADIO_T3_SDATA` sends the staged message (section 7.6): the upload is a UDT header and its blocks, one data source on the TSCC's slot. Data headers and rate 1/2 blocks heard go to the engine. The last message heard is in the view. |
| Packet data | The data service's transmissions (text, IP packets, replies) start through `data_tx_start`. Camped, the radio keeps the burst list, drops its preambles, and asks for a data channel to the data header's destination (section 7.7). Once following the granted call and settled, it sends the list on the call's slot; a refusal, no answer, timeout or a call that ended first gives the list back failed. On a data call already, a list to the same party goes at once. While a data call is followed, its data headers and blocks go to the data service, not the engine. The data service can't send while hunting or in a voice call. Listen before talk is skipped: the TSCC gives the slot. |
| Emergency | `data_t3_start` sends the data service's alarm as status 126; `t3_emergency_tick` sends the cancel and passes a status 126 heard to the emergency module (section 7.8). |
| View | The `t3` section ([debuglink.md](debuglink.md) 3.26). |

Tests: `tests/host/radioCoreTest.c` (`test_t3_talk`) runs a simulated site through PTT, request, grant and a call
with nothing heard; `test_t3_pdata` through a text message out on a granted channel and one in; `test_t3_emergency` through the alarm, the emergency call, the cancel and an alarm heard. `tests/hil/test_t3_site.py` runs camp, registration, group and private calls against a site on
the SDR bench.
