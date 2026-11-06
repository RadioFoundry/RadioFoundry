<!-- SPDX-License-Identifier: MIT -->

# DMR Tier III trunking (mobile station)

A radio on a DMR Tier III trunked system (ETSI TS 102 361-4) finds the system's trunked site control channel (TSCC),
stays on it, and follows the channel grants for the calls it wants to hear. Switch: `CONFIG_DMR_TRUNK`, a child of
`CONFIG_DMR` (`docs/build-config.md`). Work in progress: the engine and its tests exist, nothing in the radio calls
them yet, so the image doesn't change.

| Section | Topic |
|---|---|
| 1 | Standards position |
| 2 | Modules |
| 3 | PDUs |
| 4 | System identity and authorization |
| 5 | Channel plans |
| 6 | The receive engine |
| 7 | Timers and constants |
| 8 | Tests |
| 9 | Not done yet |

## 1. Standards position

Everything here comes from ETSI TS 102 361-4 (the Tier III trunking protocol) and TS 102 361-1 (the air interface).
Section and table numbers below are TS 102 361-4's unless marked -1.

- Only the standard's PDUs and procedures, with the standard feature set ID (FID 0). A CSBK with another FID is
  counted and ignored.
- No vendor's own trunking protocol. A system that sends its control channel with another CRC or framing fails the
  standard CRC checks and is never confirmed as a TSCC (section 8.2 checks this on recordings).
- Receive only so far: the radio never transmits on a trunked system (section 9).

## 2. Modules

All in `core/dmr/trunk/`. None includes a board, platform or RTOS header; the host tests run them as they are.

| File | Content |
|---|---|
| `t3_pdu.{h,c}` | CSBK and MBC codec (7.1), CRC masks, C_SYS_Parms / P_SYS_Parms short LC (7.1.2) |
| `t3_sys.{h,c}` | C_SYScode partition, TSCC authorization, SYS_AREA, the Mask rule, access delay tables (6.2-6.4) |
| `t3_plan.{h,c}` | Logical channel number to frequencies: learned, table, fixed formula (Annex C) |
| `t3_ms.{h,c}` | The mobile station engine: hunt, confirm, camp, follow, return (6.3, 6.6, Annex D) |

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
| `t3_ms_poll` | every 10-30 ms; `T3_ACT_TUNE` means tune to the channel and listen to the slot (0: both) |
| `t3_ms_stop` | another zone or channel selected |

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
| 1 | no | needed (registration comes with the access side) |

**Grants.** While camped, a grant is followed when:

| Grant | Followed when |
|---|---|
| TV, BTV | the talkgroup is in the system's list with the listen flag; or it is an all-call (ALLMSID, ALLMSIDL, ALLMSIDZ) and all-calls are on; or the grant is an emergency and emergencies on any talkgroup are on; or monitor is on |
| PV | the destination is the radio's ID, and private calls are on |
| PD, TD | never yet (data comes later) |

A gateway address (6.2, Annex A.4) is never followed as a talkgroup. The TSCC repeats a grant while its call lasts,
for late entry; a grant for the call just left is not followed again for 2 s.

**Follow.** The engine asks for the payload channel and slot. The payload color code is CG_AP's when the grant had
one, else the TSCC's. On the payload channel:

| Heard on the call's slot | Result |
|---|---|
| voice or data sync, any CSBK with a good CRC | the call stays alive |
| P_CLEAR for the call, or to ALLMSI | return; to the LPCN it names, when it names one, else to the last TSCC |
| P_MAINT disconnect for the call | return |
| P_PROTECT for the call | disable or enable PTT (kept for the access side) |
| a grant for the same call to another channel or slot | move there |
| a grant for another call | ignored |

Bursts on the other slot are ignored: it carries another call, or the TSCC on a composite channel. Nothing on the
call's slot for TV_Inactive, or a call longer than the ceiling, also returns. After a return the TSCC has T_Nosig to
answer with valid PDUs.

### 6.2 Counters

`t3_ms_t.st`: CSBKs with a good CRC, CRC failures, grants heard while camped, calls followed, grants to unmapped
channels (and the last such LPCN), returns, hunts, foreign Alohas and broadcasts, foreign short LCs, learned channels.

## 7. Timers and constants

| Name | Value | Standard range (Annex A) |
|---|---|---|
| T_Nosig | 5 s | 1-15 s |
| Hunt dwell | 700 ms | |
| Settle after a retune | 60 ms: what is heard sooner belongs to the old channel | |
| TV_Inactive | 2.5 s | 0-20 s |
| Call ceiling | 300 s | |
| Re-grant hold | 2 s | |
| MBC continuation gap | 130 ms: one TDMA frame, one lost | |

The system's record may override T_Nosig, TV_Inactive and the dwell; 0 keeps the default.

## 8. Tests

### 8.1 PDUs (`tests/host/t3PduTest.c`)

`tests/host/data/t3_vectors.jsonl` holds PDUs packed by an independent implementation from the field tables of 7.1,
7.2 and Annex C, with the field values beside each. The test parses each one, checks every field, rebuilds the exact
octets, and checks the CRC (including corrupted copies and the wrong mask). The same file is meant for every codec of
these PDUs. Then the rules: Mask, delay tables, acknowledgement classes, authorization order, SYS_AREA, plan order and
the learned ring.

### 8.2 Engine (`tests/host/t3MsTest.c`, `tests/host/t3ReplayTest.c`)

`t3MsTest` drives the engine with built PDUs: hunting over silent and foreign candidates, the settle time, CRC and
color code filters, every follow rule, every return, T_Nosig, short LCs, learning from Chan_Freq and CG_AP, late MBC
continuations, monitor and emergency options, stop.

`t3ReplayTest` feeds the engine off-air recordings decoded on the SDR bench, in time order; the engine hears a
channel's records only while it is tuned there. The recordings are not in the repository: `RFOUNDRY_T3_RECORDINGS`
names a manifest (format in the test's header) with the files and what is known about the recorded system. Without
it the test skips. It checks that the engine camps within 1.5 s, reads the system code and Aloha parameters, stays on
the TSCC except across gaps in the recording, follows only planned channels and known talkgroups, keeps a call alive
on its voice, and never camps on another system or on a control channel that isn't Tier III.

## 9. Not done yet

| Item | Needs |
|---|---|
| Radio core: a trunked system as a place, retune on grants, both slots while camped | the codeplug keys for systems |
| CACH short LC reception | the modem drivers' CACH report |
| Access: registration, call requests, random access timing | slot-timed transmit with a late veto in the modem drivers |
| UI, debug link view | the radio core |
| Data grants, C_MOVE, adjacent sites, power save | later |
