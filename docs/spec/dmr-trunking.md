<!-- SPDX-License-Identifier: MIT -->

# DMR Tier III trunking (mobile station)

A radio on a DMR Tier III trunked system (ETSI TS 102 361-4) finds the system's trunked site control channel (TSCC),
stays on it, and follows the channel grants for the calls it wants to hear. Switch: `CONFIG_DMR_TRUNK`, a child of
`CONFIG_DMR` (`docs/build-config.md`); the access side (section 7) is `CONFIG_DMR_TRUNK_TX`, a child of it. Work in
progress: the engine and its tests exist, nothing in the radio calls
them yet, so the image doesn't change.

| Section | Topic |
|---|---|
| 1 | Standards position |
| 2 | Modules |
| 3 | PDUs |
| 4 | System identity and authorization |
| 5 | Channel plans |
| 6 | The receive engine |
| 7 | Access |
| 8 | Timers and constants |
| 9 | Tests |
| 10 | Not done yet |

## 1. Standards position

Everything here comes from ETSI TS 102 361-4 (the Tier III trunking protocol) and TS 102 361-1 (the air interface).
Section and table numbers below are TS 102 361-4's unless marked -1.

- Only the standard's PDUs and procedures, with the standard feature set ID (FID 0). A CSBK with another FID is
  counted and ignored.
- No vendor's own trunking protocol. A system that sends its control channel with another CRC or framing fails the
  standard CRC checks and is never confirmed as a TSCC (section 9.2 checks this on recordings).
- The radio core doesn't transmit on a trunked system yet: the access side (section 7) exists in the engine only
  (section 10).
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
for late entry; a grant for the call just left is not followed again for 2 s.

**Follow.** The engine asks for the payload channel and slot. The payload color code is CG_AP's when the grant had
one, else the TSCC's. On the payload channel:

| Heard on the call's slot | Result |
|---|---|
| voice or data sync, any CSBK with a good CRC | the call stays alive |
| P_CLEAR for the call, or to ALLMSI | return; to the LPCN it names, or to the frequencies of its appended block (LPCN 0xFFF, an MBC), else to the last TSCC |
| P_MAINT disconnect from the call's source (either party of a private call) | return |
| P_PROTECT DIS_PTT / EN_PTT to the radio, the call or ALLMSID | PTT off / on |
| P_PROTECT EN_PTT_ONE_MS | PTT on for the radio it names, off for every other |
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
| Each inbound slot | the AT bit decoded and free; the Aloha's Mask matches the radio's ID; the Service Function allows the service (table below). An emergency skips Mask and Service Function unless the Mask is 24 |
| A slot not permitted | wait for the next permitted one |
| After a try | wait NRand_Wait + 1 frames for an answer |
| No answer | try again 1 to Backoff frames later, at random (table 6.3); NRand_NR tries (NRand_NE for an emergency), then no answer |
| Overall | TRand_TC from the request, then timeout |
| C_WACKD, C_QACKD | stop retrying; TP_Timer for a call, TNP_Timer for the rest; a further one restarts it |

| Service Function | Allows |
|---|---|
| 0 | everything |
| 1 | calls and registration |
| 2 | everything but calls, and registration |
| 3 | registration only |

The C_BCAST Backoff replaces the Aloha's.

### 7.2 Registration (6.4.4.1)

The engine registers by itself when the state is "needed" and nothing else is in progress. Until registered it asks
for nothing else. The C_RAND (kind registration) targets the system code (Trg_Adr_Cnts 0), or the talk group with
`T3_OPT_REG_TG` (subscription, Trg_Adr_Cnts 1). Sending it drops the old record.

| Answer to the radio's ID | Result |
|---|---|
| C_ACKD Reg_Accepted, Reg_Subscription | registered, for this area |
| C_NACKD Reg_Refused | hunt; registers again on returning, after 30 s at the earliest |
| C_NACKD Reg_Denied | the area is denied; hunt |
| none (NRand_NR tries) | the record stays as it was; tries again after 30 s |

De-registration is the same request with Reg_Dereg 0. After it the engine doesn't register again until restarted.

### 7.3 Calls (6.6.1, 6.6.2.2)

| Event | Result |
|---|---|
| request | C_RAND kind talkgroup or individual voice, the target, the radio's ID; EMERG option for an emergency |
| C_WACKD, C_QACKD, C_ACKD MS_Alerting | waiting (TP_Timer) |
| a grant with the radio as source | follow; the call is the radio's own; result granted |
| C_NACKD | refused, with its reason |
| C_ACKD Call_Back | done |
| cancel, nothing sent yet | dropped |
| cancel, sent | a C_RAND kind cancel to the same target; the TSCC answers C_ACKD Message_Accepted |

PTT on the payload channel (`t3_ms_can_talk`): following, not disabled by P_PROTECT, and not an all-call someone
else made. At the end of a call the radio made, or of any private call, `t3_ms_end_call` sends N_Maint P_MAINT
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
| call cancel (kind cancel) on the TSCC | MS_Accepted |
| selective clear (P_AHOY kind cancel) | Message_Accepted, then return |
| authentication challenge, anything else to the radio | not supported (0x00) |

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
| Radio core: a trunked system as a place, retune on grants, both slots while camped | the codeplug keys for systems |
| CACH short LC reception | the modem drivers' CACH report |
| Access on the radio: sending the engine's CSBKs in the announced slot, voice on the payload channel | slot-timed transmit with a late veto in the modem drivers, the CACH AT bit |
| Talkgroup lists of more than one group, data and status services | later |
| UI, debug link view | the radio core |
| Data grants, C_MOVE, adjacent sites, power save | later |
