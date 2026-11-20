<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Tier III dedicated packet data

The dedicated packet-data profile is selected per [codeplug system](dmr-trunking-codeplug.md).
It adds the observed FID 0x08 and 0x68 procedures and the version 2 SMS envelope to the ETSI
Tier III and confirmed packet-data transport. The standard profile continues to use FID 0
procedures. The dedicated IP SMS path is for individual confirmed clear IP text. Existing SMS
format and acknowledgment settings still choose the service.

## 1. Packet sequence

The table gives the first ten decoded CSBK or data-header octets with CRCs omitted.
`S` and `D` are 24-bit sender and recipient IDs, in network byte order.
`U` is the uplink gateway `FF FD 02`. `I` is the downlink gateway `FF FD 01`.
Bracketed values depend on the packet. The same carrier can hold the control channel and payload slot.

| Slot | Direction | PDU and example octets | Expected result |
|---|---|---|---|
| Control | MS to TSCC | C_RAND, individual packet data, `9F 00 00 02 U S` | Request the uplink grant to U |
| Control | TSCC to MS | PD_GRANT, absolute LCN, `33 08 FF F8 [dst] [src]` | Follow the grant after both continuation blocks pass CRC. Uplink grants can put the requesting MS in the destination field and U in the source field |
| Payload | MS to site | Confirmed header, `43 94 U S 91 28` | Request a link ACK for a single fragment with 17 blocks to follow, including the proprietary header, for this pad count and example sequence |
| Payload | MS to site | Proprietary header, `9F 68 00 00 00 00 00 00 00 00`, then confirmed Rate 1/2 blocks | Submit the version 2 SMS envelope. The air blocks have a block serial, CRC-9 and ten user octets |
| Payload | Site to MS | Confirmed response, `01 90 S U 00 08` | The uplink gateway accepted the packet. The gateway can acknowledge with sequence zero after accepting a nonzero send sequence |
| Payload | MS to site | P_MAINT completion, `AA 00 30 10 U S` | Release the uplink slot. The radio returns to the control channel after the burst |
| Control | TSCC to MS | C_AHOY terminal check, `9C 68 0E 07 D I` | Check that the recipient can receive the dedicated download |
| Control | MS to TSCC | C_ACKU, `A1 00 02 88 I D` | MS Accepted, Response Info 1 and the reserved bit clear. A repeated check from the site receives another reply |
| Control | MS to TSCC | C_RAND, individual packet data, `9F 00 00 02 I D` | Request the download after allowing five TDMA frames for the most recent terminal reply |
| Control | TSCC to MS | System Busy, `A0 00 02 4E D I` | Retry after about two and then four seconds, within the original access deadline. The busy retry budget is separate from unanswered random accesses |
| Control | TSCC to MS | PD_GRANT from I to D | Follow the granted payload slot as the recipient |
| Payload | MS to site | P_MAINT readiness, `AA 68 30 1A I D` | Tell I that the recipient is ready. Repeat if I checks the recipient again on the payload slot |
| Payload | Site to MS | Confirmed header, for example `43 96 D I 8C 88`, then `9F 68` and confirmed Rate 1/2 blocks | Reassemble the complete version 2 envelope and verify packet CRC-32 |
| Payload | Site to MS | Trailing response request, for example `43 96 D I 00 88` | Supply the addresses, pad count and link sequence to a receiver that heard the body but missed the leading confirmed header |
| Payload | MS to site | Confirmed response, `01 90 I D 00 08` | ACK the complete packet to I. Store its original sender from the envelope |

For the FID 0x08 absolute grant, the example first continuation starts
`33 00 00 C0 49 10 00 12 32 C0` and defines the channel. The final continuation begins
`B3 00 20 00 7F F8 00 00 00 00`. The final CRC covers both continuation blocks, including all 12
octets of the first block and the first 10 of the last block. The first continuation alone cannot
authorize a retune or a transmission. The independent header CRC uses the MBC header mask.
See ETSI TS 102 361-1, sections 7.4 and 9.3, for MBC and burst framing.

## 2. Version 2 SMS envelope

All integers and tags are big endian. The envelope is carried as proprietary SAP 9 at the
confirmed data link. The inner sender and recipient are subscriber IDs even when the link
addresses one of the gateways.

| Offset | Bytes | Value |
|---|---:|---|
| 0 | 2 | Version and type `02 01` |
| 2 | 2 | Envelope length after this four-octet prefix |
| 4 | 2 | Operation `68 07` |
| 6 | 8 | Tag `00 01`, length `00 04`, four-octet message ID |
| 14 | 8 | Tag `00 02`, length `00 04`, four-octet sender ID |
| 22 | 8 | Tag `00 03`, length `00 04`, four-octet recipient ID |
| 30 | 6 | Tag `01 01`, length `00 02`, value `00 07` |
| 36 | variable | Tag `01 02`, two-octet byte length, UTF-16BE text |

When only the proprietary header is heard, recovery starts on a granted dedicated download.
The first block must have serial zero and a valid envelope prefix. The envelope length yields
the expected block count. The trailing request must match the pad count, and the full packet
CRC-32 must pass before the link ACK. A trailing request without the corresponding body is
ignored. A malformed envelope or one naming another recipient is not stored in the inbox.
Link ACK and end-to-end SMS delivery are separate events.

## 3. Verification

- The host fixtures replay both MBC continuations, corrupt their CRC, and exercise terminal check,
  readiness, busy responses, and uplink completion. The standard profile ignores the FID 0x68 check.
- The data fixtures exercise the dedicated confirmed header, normal and recovered downlink, original
  sender, same link sequence on successive messages, and a wrong embedded recipient.
- The opt-in two-radio Tier III bench test sends a unique 53-character message each way and checks
  the entire recipient inbox entry and its queued response. Capture the repeater return channel
  to verify that each response left the radio and reached the site. A sender display of `ok`
  alone verifies the uplink acceptance.
- On a single-payload-slot site, a fast recipient can request that slot while the upload still owns it.
  One measured edge was about 30 ms. The site subsequently delivered the packet on a later attempt,
  which took about 74–77 seconds in two captures. This behavior depends on the site's resource and retry
  scheduling. The profile does not change those timers.
