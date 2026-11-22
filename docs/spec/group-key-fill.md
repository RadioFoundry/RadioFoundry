<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Group key fill

One radio broadcasts a stored AES-256 key to every radio listening on the same DMR AES channel. Each receiver shows
the key's sender and check value, and its user saves it under a key ID of their choice. It suits a net rekey at an
agreed time: the net control sends once, every radio in the field that is listening receives it.

Point-to-point key fill ([key-fill.md](key-fill.md)) stays the stronger way to load one radio: there, an X25519
exchange and a six-digit code tie the key to the radio across the table. Group key fill has no exchange. Its secrecy is
the channel key's: whoever holds the channel key can read a group key sent on that channel (section 6).

## 1. Switches and files

| Item | Value |
|---|---|
| Switch | `CONFIG_GROUP_KEY_FILL`, on where `CONFIG_KEY_FILL` and `CONFIG_DMR_AES` are |
| Message | `core/keyfill/groupkey.{h,c}`: building, checking and opening a message. Pure: no RTOS, no allocation |
| Transport | `core/keyfill/groupkey_svc.c`: UDP port 4102 through the air queue, the received message held for the UI |
| UI | `core/ui/ui_svc_groupkey.c` and the screens in `core/ui/ui_menus.c` |
| Tests | `tests/host/groupKeyTest.c`, `uiFlowTest` flows `G ...` |

## 2. Roles and the flow

| Step | Sender (net control) | Receivers |
|---|---|---|
| 1 | | At the agreed time: Tools > Key fill > **Receive group key**. The screen shows the channel and a countdown from 30:00 |
| 2 | Tools > Key fill > Send a key (the stored keys), OK on an AES-256 key, then **Send to group** | |
| 3 | The message goes to the channel's talkgroup 4 times, 3 s apart. The screen says each copy and then "Sent" with the key's check value | |
| 4 | | The first good copy shows "Key from 4670502", the sent key ID, the check value and "Save as key: 5" (the sent ID, editable) |
| 5 | | OK saves it into the channel's AES system under that key ID (replacing a key of that ID), then shows "Saved as key 5" and the check value. Back drops it and listens on |
| 6 | Net control reads the check value on voice; each user confirms theirs | |

The sender never receives a key and never changes its own keys. Receivers send nothing.

Both sides need a DMR AES-256 channel with its key, and a talkgroup as the channel's target. Anywhere else the screen
says so and nothing is sent or received: a clear channel would leave the key with only its inner wrapping (section 4),
and the inner wrapping alone is not the protection the channel key's AES gives the whole packet.

## 3. Transport

| Item | Value |
|---|---|
| Packet | One UDP datagram, source and destination port 4102, unconfirmed, to the channel's talkgroup (`radio_view_t.dmr.target_id`, a group) |
| Privacy | The channel's AES data privacy ([dmr-data.md](dmr-data.md) 13.4): every copy is encrypted under the channel key with its own MI |
| Copies | 4, the first at once and then every 3 s (an unconfirmed group packet has no acknowledgement). Each copy is the same message |
| Air queue | Owner `AIRQ_OWNER_GROUP_KEY`; a copy the data service doesn't take within 10 s fails and the screen says so |
| Receiving | A datagram to port 4102 for a group is held for the UI while Receive group key is open, one at a time; closed, every one is dropped |

## 4. Message

All of it is inside the AES-encrypted UDP payload. Numbers are big-endian.

| Octets | Field |
|---|---|
| 0 | 0x47 ('G') |
| 1 | Version 1 |
| 2-9 | Session: 8 random octets from the sender's randomness pool, the same in every copy |
| 10-13 | The sender's DMR ID |
| 14 | Algorithm 0x25 (AES-256) |
| 15 | Key ID, as stored on the sender |
| 16 | Key length: 32 |
| 17-48 | The key, encrypted |
| 49-64 | Tag |

The keys come from the channel's AES key `C` with HKDF-SHA-256 ([key-fill.md](key-fill.md) 7 uses the same
primitives):

```
PRK   = HKDF-Extract(salt = session, IKM = C)
K_enc = HKDF-Expand(PRK, "RFGKF1 key", 32)
K_mac = HKDF-Expand(PRK, "RFGKF1 mac", 32)
encrypted key = key XOR K_enc
tag   = HMAC-SHA-256(K_mac, octets 0-48), first 16 octets
```

A receiver takes a message only when all of these hold. Otherwise it is dropped, with a counter and nothing shown:

- the length is 65, octets 0, 1, 14 and 16 are as above;
- the sender's DMR ID in the message is the packet's source ID;
- the tag matches (compared in constant time), under the receiver's own channel key;
- the session isn't one this window already showed.

## 5. States and times

| Item | Value |
|---|---|
| Receive window | 30 minutes from opening the screen, then it closes by itself ("No key came"). Back closes it at any time |
| Sender | Done after the 4th copy goes, or failed when one doesn't go within 10 s |
| Randomness | The session's 8 octets need the randomness pool full, as key fill does ([key-fill.md](key-fill.md) 6): the sender is asked for key presses first when it isn't |
| Leaving the screen | Ends it: the sender's remaining copies are cancelled, a key not yet saved is wiped |

## 6. Security

- **Secrecy.** The key is encrypted twice: under the inner wrapping and, with the whole packet, under the channel
  key's AES. Both come from the channel key, so anyone with the channel key can read every group key sent on that
  channel. That is the point of a group rekey; choose the channel accordingly.
- **Authenticity.** The tag stops anyone *without* the channel key from changing a key in flight or making one up; the
  data layer's AES alone (OFB) would not. Anyone *with* the channel key can send a group key of their own. The receiver
  shows the sender's DMR ID and asks before saving, and the net confirms the check value on voice. Point-to-point key
  fill doesn't have this limit.
- **Replay.** A recorded copy played back later has a valid tag. A radio only takes one while its user has the
  receive screen open, shows its sender and check value, and asks first; a session already shown in that window is
  ignored. A replayed old key would show its old check value, which the voice check catches.
- **What a listener without the key sees:** that the sender sent a data packet to the talkgroup four times. Not the
  port, not the key, not its ID.
- **Wiping.** The received key, the derived keys and the channel key copy are wiped when the screen closes, saved or
  not; the emergency wipe ([zeroize.md](zeroize.md)) wipes a running session too.

## 7. Radio UI

| Screen | Shows | Keys |
|---|---|---|
| Tools > Key fill > Receive group key | "Listening on <channel>", "29:41 left" | Back ends |
| Key received | "Key from 4670502", "Key ID 5", "Check 9507A7", "Save as key: 5" | Digits edit the key ID (0-255), OK saves, Back drops it and listens on |
| Saved | "Saved as key 5", "Check 9507A7" | Back |
| Stored keys > OK on an AES-256 key > Send to group | "Sending to group 2/4", then "Sent to group", "Check 9507A7" | Back stops |
| Refused | "Group key fill needs an AES channel with its key" or "... a talkgroup as the target" | Back |

## 8. Limits

- AES-256 traffic keys only. A privacy slot or another system's key isn't offered for Send to group.
- One key per message.
- No acknowledgement: the sender can't know who got the key. The voice check is the acknowledgement.
- Not checked against other makes: other makes ignore port 4102.

## 9. Tests

`tests/host/groupKeyTest.c`: a known message from fixed inputs; round trip; every field and every octet of the tag
altered; another channel key; the source ID not the message's; a short and a long message; the same session twice. The
UI flows: the refusals, a received key saved under another ID, Back, the window running out, a send's four copies.
