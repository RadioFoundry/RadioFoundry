<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->
<!-- Copyright (c) 2026 Alex Bloom -->

# Motorola voice-channel user alias reception

The MFID 90 receiver complements the A4 standard alias receiver in [P25](p25.md).
It accepts LDU1 and TDULC header 15 and data 17 (hex), with a bounded 16-block assembler.
Each block contributes 44 bits. A header supplies the talkgroup, block count,
Unicode format indicator and sequence number. A complete payload contains a
seven-byte system/unit identity, encoded UTF-16BE name, CRC-16/GSM and zero padding.

The receiver requires the header talkgroup and payload unit ID to match any
identity already known for the active voice call. On late entry, it collects
fragments even when the source is unknown. Only a complete validated payload may
supply that source ID and its header's group; partial or corrupt data never does.
An identified private call cannot be reinterpreted as a group call.
It requires every block, consistent sequence numbers, valid
CRC and printable ASCII text before publishing a name. Display is limited to
14 characters, but validation covers the whole name. Caller/call/NAC changes
clear pending data. Learning the first group-voice source after alias fragments
have started preserves those fragments if the group matches. The completed
payload must still match the newly known source and pass its checksum; learning
an identity never authorizes a partial name. Incomplete data expires after 80 received units without an
alias fragment. No transmit, affiliation, registration or query is added.

Motorola alias TDULCs keep the receive call alive without producing voice frames.
The clear MFID 90 opcode 0F Talk Complete TDULC can precede the alias header.
For a group call, its nonzero 24-bit address in bytes 6–8 must match any known
caller. It preserves receive context but never supplies a caller identity itself.
Valid group-voice and network LC can also occupy TDULCs during the channel hang.
The follower waits up to eight seconds from the first such TDULC, or 2.5 seconds
without a valid unit. The first alias fragment starts a separate absolute
two-second assembly window. A completed alias, a plain TDU, a null-group release,
or a different group's LC returns to control immediately. Damaged LC alone is
not treated as a release. New voice cancels the tail wait. This trades a bounded
wait after speech for reception of aliases sent late in the site's hang time.
A retained event lets the display and caller history consume the validated name
after returning. Late entry during a valid group TDULC can learn its caller;
zero-source hang words retain that caller. An alias header received without an
active voice call can start a metadata-only assembly; only its complete CRC-checked
payload supplies a missing identity, and metadata alone never unmutes voice.

The `p25_detail` debug section reports the most recent vendor assembly source,
sequence, expected block count, received-block bitmask, and result: 0 waiting,
1 complete, 2 invalid header, 3 missing header, 4 invalid block/sequence/group,
5 source mismatch, 6 checksum failure, 7 invalid text/padding. Results 2–7 also
increment `p25.alias.rejected`; incomplete valid sequences do not. The section
also repeats the voice playback counters so a full main P25 reply cannot hide
them. The fields describe the last observed assembly, not a persistent per-unit
database or proof that a silent unit transmitted an alias.

## Permissive reference and attribution

`core/p25/p25_alias90.c` adapts the cipher table and wire-format description from
[GopherTrunk](https://github.com/MattCheramie/GopherTrunk), revision
`8b9d9477e1365f801fca13ee7eb66c0107fc359e`, under Apache-2.0:

- `internal/radio/p25/motorola/alias.go`
- `internal/radio/p25/phase1/motorola_alias.go`
- `internal/radio/p25/motorola/alias_recovered_test.go` (independent published vector)

The reference's [provenance record](https://github.com/MattCheramie/GopherTrunk/blob/8b9d9477e1365f801fca13ee7eb66c0107fc359e/research/p25-talker-alias-cleanroom-provenance.md)
documents independent recovery from observed input/output behavior. No restricted
firmware, vendor SDK or GPL decoder source was used for the original cipher implementation.
The Apache license is retained in `LICENSES/Apache-2.0.txt`. The referenced repository
had no root NOTICE file at review. This C adaptation adds bounded storage, strict
checksum validation and caller binding; it does not copy its permissive reference's
best-effort display of invalid messages.

## Validation

Host tests cover the independent encoded name, complete fragment assembly, missing
blocks, sequence mismatch, CRC failure, corrected retransmission, caller mismatch
and call-boundary clearing. Tail tests cover TDULC assembly, ordinary termination,
the absolute deadline and return after completion without any registration or
transmission. Physical-radio reception decoded a complete console alias in a
passive trunked test. The complete-name counter incremented with ID 0 and no
transmit, affiliation or registration activity.
Display and history timing remain separate from decoder validation.

On November 22, a receive-only live trunked test also validated three distinct
field-unit names in Recent and caller details after fixing channel-hang handling
and metadata-only late entry. The same run received console aliases. The radio's
application was read back exactly; its original codeplug and codec were preserved,
and transmit, registration and affiliation counters remained zero. The local
validation artifacts retain caller screenshots and the diagnostic traces.

## November 22, 2026 requested interoperability audit

The owner explicitly requested comparison with SDRTrunk. Public master was reviewed
for protocol behavior; no GPL implementation or lookup table was imported.
The independent Apache-2.0 cipher remains unchanged. This audit is distinct from
the original clean-room implementation record above.

- [SDRTrunk's alias header documentation](https://github.com/DSheirer/sdrtrunk/blob/master/src/main/java/io/github/dsheirer/module/decode/p25/phase1/message/lc/motorola/LCMotorolaTalkerAliasHeader.java)
  records a Talk Complete message before the alias sequence. Previously both the
  call decoder and follower ended reception at that prefix. Regression tests now
  exercise the prefix, header and complete tail for clear and encrypted voice.
- [SDRTrunk's cipher decoder](https://github.com/DSheirer/sdrtrunk/blob/master/src/main/java/io/github/dsheirer/module/decode/p25/phase1/message/lc/motorola/MotorolaTalkerAliasComplete.java)
  is mathematically equivalent to our independently sourced cipher: all 256 table
  values differ by a constant 79 modulo 256, canceled by an accumulator offset
  of 0x4F00. The multiplier, low-byte inverse and per-input accumulator update
  agree. No cipher change was needed.
- A public air-message example in the header documentation supplies an
  independent seven-block test vector. Tests retain the observed wire bytes,
  not Java code. System identity and decoded names are omitted here.
- Both implementations validate the complete payload with CRC-16/GSM. Ours searches
  valid UTF-16 byte lengths with zero padding instead of guessing the end from the
  last nonzero bit. It additionally requires matching known caller/group and
  printable ASCII. Non-ASCII text is therefore a documented unsupported case.
- SDRTrunk can retain same-sequence data blocks before a later header. Our bounded
  assembler currently requires the header first and clears at a call boundary.
  This remains a recovery limitation, not evidence that a complete correctly
  ordered payload is decoded incorrectly.

The prefix fix uses the bounded hang policy above: up to eight seconds before
the alias, a two-second assembly window, and a 2.5-second inactivity timeout. No registration, affiliation, transmit permission, codeplug
layout, codec, or cipher checksum rule changes. A passing host test proves the
receive-state defect and correction; local on-air benefit requires a capture of
the prefix followed by alias data. The `p25_detail` field
`p25.alias.talk_complete` counts accepted prefixes independently of alias fragments.

Encrypted-call metadata publication is independent of voice playback. The engine
retains a selected caller's completed alias across an immediate return to control;
Home and Recent consume that event even when no voice was played. Destination
filtering still applies, and encrypted voice stays muted. Host tests cover live,
cached and terminator-tail aliases, Recent history, and another group's rejection.
A synthetic 39-character, 16-block Motorola payload also passes full CRC and text
validation before shortening to the display's 14 characters. Corruption after
that visible prefix is rejected. This boundary test is not a field-unit capture
and does not establish why any particular live field alias was missed.

`CONFIG_P25_ALIAS_TRACE` adds a bounded, receive-only RAM ring named
`p25_alias_trace`. It stores 64 entries (28 bytes each), preceded by a 32-bit
published count. Each entry contains sequence, frame timestamp, previous caller,
NAC, DUID, flags, nine decoded LC bytes and three padding bytes. Flag bits are
active (0), valid LC (1), failed LC FEC (2), and encrypted voice (3). DUID 255
marks loss of synchronization. Readers check entry sequence against the count;
the source field precedes processing of that entry and may still name the previous
caller. Unknown LC words and inactive TDULCs are included to diagnose early exits.
The trace does not store voice, affect transmit permissions, or bypass encryption.
The `p25_trace` debug view exports the newest 12 complete entries in one bounded
reply, newest first. `p25.trace.count` is the published count; `pt.N` contains
comma-separated decimal sequence, timestamp, previous caller, NAC, DUID, flags,
then 18 hex digits of LC. Sequence gaps indicate missed records. It is not a stable
external ABI. The normal build leaves it disabled.
