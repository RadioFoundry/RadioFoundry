<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# MFID 0x90 voice-channel user alias reception

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

After a transmission-end terminator, the trunk follower waits up to 150 ms for a
valid alias header, then at most 500 ms total from that terminator for completion.
Standard call termination (LCO 15) releases immediately. New voice cancels the wait.
These are receiver policy choices; [trunking termination](p25-trunking.md#41-transmission-end-and-call-termination)
describes the distinction and bounds.
Completed names are retained as events for caller history; metadata never unmutes voice.

## Attribution

The cipher table and transform retain their Apache-2.0 attribution in
`core/p25/p25_alias90.c`; the license is in `LICENSES/Apache-2.0.txt`.
The independent reference revision is `8b9d9477e1365f801fca13ee7eb66c0107fc359e`.

## Validation

Host tests cover the permissively sourced vector, bounded assembly, full-name
validation before truncation, corruption, sequence and caller mismatches, ordinary
termination, the absolute alias deadline, and return after completion.
Hardware validation is recorded in the PR.
