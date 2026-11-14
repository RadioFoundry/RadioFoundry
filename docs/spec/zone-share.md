<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Zone sharing over DMR data

One radio sends a zone, or one channel, to another radio or to a talkgroup over DMR data. The receiver shows it as an
offer and imports it only when the user says so. Status: built and host-tested (`zoneShareTest`, `zoneShareSvcTest`,
`zoneImportTest`, the UI flow tests). On a radio, sending one channel has been checked against the SDR bench; sending
a zone, offers and the import are untested there. The opt-in HIL test is `tests/hil/test_zone_share.py` (feature
`zone_share`).

Sources: ETSI TS 102 361-3 (packet data, the UDP/IP bearer), RFC 768 (UDP), [dmr-data.md](dmr-data.md) 11 and 12 (IP
over DMR, the TX queue). The record format is ours.

## 1. Switches and files

| Item | Where |
|---|---|
| Switch | `CONFIG_ZONE_SHARE`, on by default where DMR is built ([build-config.md](../build-config.md)). It also builds the TX queue (`CONFIG_DMR_AIR_QUEUE`) |
| Record and messages | `core/dmr/zoneShare.c`: encode, strict decode, reassembly. No state but the buffer the caller owns |
| Service | `core/dmr/zoneShareSvc.c` (`zone_share_api.h`): sending through the TX queue, offers, the rate limit, directory requests |
| Import and export | `core/codeplug/zone_import.c`: the only code that writes the codeplug for zone sharing (6) |
| UI | `core/ui/ui_svc_zone.c` (the glue, the import as a storage job), screens in `core/ui/ui_menus.c` |
| Debug link | the `air` view section: `zs.*` keys ([debuglink.md](debuglink.md) 3.7) |

## 2. Transport

- UDP, source and destination port **4100**, over the data service's IP bearer ([dmr-data.md](dmr-data.md) 11). Port
  4100 is outside 4000-4099, so the UDP header is compressed on auto (SAP 3).
- To a radio: confirmed packets. To a talkgroup: unconfirmed packets. Each message is one IP packet.
- Every message goes through the TX queue ([dmr-data.md](dmr-data.md) 12.6). A send puts all its fragments in at once
  or none of them. Unconfirmed fragments to one talkgroup go out chained in one transmission (12.6.2).
- A fragment the data service doesn't take within 10 s fails the send. The other fragments of that send are canceled.

## 3. Messages

Every message starts with the version (1) and the type. Multi-octet fields are big-endian.

| Type | Octets | Layout |
|---|---|---|
| 1 Fragment | 7 to 328 sent, up to 568 taken | version, type, transfer number, index (bits 7-4) and count (bits 3-0), offset (2), chunk |
| 2 Request | 5 | version, type, request number, what (1: the zone of the site the request came in through), options (0) |
| 3 Refusal | 4 | version, type, request number, code |

### 3.1 Fragments

- A record of up to 1024 octets goes in 1 to 4 fragments of 322-octet chunks, the last one shorter (`ZS_SEND_CHUNK`).
- The chunk size is the sender's. A receiver takes any from 128 octets up to the longest message the air MTU allows
  with the full IPv4 and UDP headers (`ZS_MSG_MAX` = `DMR_DATA_IP_MTU` - 28: 568 octets, a 562-octet chunk,
  [dmr-data.md](dmr-data.md) 12.8). That limit grows with the air MTU. A record in 128-octet chunks takes 8 fragments,
  the most a transfer may have.
- We still send 322: the first builds take only that chunk size and messages of at most 328 octets, and would drop
  larger fragments. Every build since takes chunks up to 562, so the sender can move up to 562 (2 fragments for a
  full record) once the first builds are no longer in use. A build whose air MTU is larger takes larger chunks, but
  must not send them while builds that take only 562 are in use. `zoneShareTest` checks our fragments against the
  first builds' rules.
- Every fragment but the last is full, and its offset is index x its length. The last fragment's offset is a whole
  number of chunks of a size the receiver takes, and it is no longer than one. Every fragment of a transfer must give the
  same chunk size. A fragment that breaks a rule is dropped (`zs.rx_bad`).
- The transfer number is the sender's, one per send.

### 3.2 Reassembly

| Rule | Behavior |
|---|---|
| One at a time | one transfer from one sender is assembled. A fragment from anyone else meanwhile is dropped (`zs.rx_busy`) |
| Time-out | a transfer whose fragments stop for 30 s is dropped when the next fragment arrives, from anyone |
| Started over | the same sender with another transfer number or fragment count replaces its transfer |
| Duplicates | a fragment already in is ignored |
| Done | when every fragment is in, the record is decoded (4) as a whole; one that fails is dropped |

### 3.3 Refusal codes

1 site unknown, 2 site not seen for a long time, 3 no frequency on record, 4 busy, 5 not supported, 255 other.

## 4. The record

| Part | Octets | Fields |
|---|---|---|
| Header | 10 + name | version (1), length (2, the whole record), zone ID (4, not 0), flags, channel count (1-80), contact count (0-32), name |
| Contact | 4 + name | type (bits 1-0: 0 group, 1 private, 2 all call) and slot (bits 3-2: 0 the channel's, 1, 2), ID (3, not 0), name |
| Channel | 6 to 16 + name | flags, RX Hz (4), then as the flags say: TX offset (4, signed, not 0), DMR (2 or 3), RX tone (2), TX tone (2); name |
| Systems | 1 + each | count (1-4), then each trunked system (4.2). Only with record flag bit 1 |

- A name is a length (0 to 16) and that many printable ASCII octets. The sender writes `?` for anything else.
- Record flags: bit 0, one channel (the count must be 1); bit 1, the record carries trunked systems (4.2). The other
  bits are reserved and must be 0. A decoder without trunking refuses a record with bit 1.
- Channel flags: bit 0 DMR, 1 25 kHz, 2 TX offset present, 3 RX tone present, 4 TX tone present, 5 receive only on
  the sender, bits 7-6 power (0 the default, 1 low, 2 mid, 3 high).
- DMR octets: the color code (bits 3-0), slot 2 (bit 4), direct mode (bit 5), trunked (bit 6), bit 7 reserved; then
  the contact, a 1-based index into the record's contacts or 0; then, for a trunked channel only, its system, a
  1-based index into the record's systems. A DMR channel carries no tones.
- A tone: the type in bits 15-14 (1 CTCSS, 2 DCS, 3 DCS inverted), the value in bits 13-0. CTCSS is Hz x 10 (600 to
  2600). DCS is the code as an octal number (023 is 19), at most 0777.
- Contacts come first, then systems, then channels. The record must end exactly where its length says. Bit 1 is set
  exactly when a systems part is there.
- The decoder checks every length, count, reference and value before use. A record that fails one check is refused
  whole.
- Zone ID: the sender's choice, the same each time it sends the same zone. Ours is an FNV-1a hash of the zone name and
  the flags.

### 4.1 What a send holds

The sender takes the zone's channels in order, each with its contact and, for a trunked channel, its system, while the
record fits 1024 octets and 80 channels. The rest are left out, and the UI says how many go ("Sending 40 of 60
channels"). A trunked channel whose system's sharing is off is left out too ([dmr-trunking.md
12.6](dmr-trunking.md#126-sharing)). Nothing else of the codeplug is sent: no scan lists, no RX group lists, no
privacy keys.

### 4.2 Trunked systems

A system as its sharing setting allows: 1 the identity, 2 the whole system. Multi-octet fields are big-endian.

| Field | Octets | Value |
|---|---|---|
| Level | 1 | bits 1-0: 1 identity, 2 whole; bit 2 key present, bit 3 PSN present (whole only); the rest 0 |
| Name | 1 + name | as any name |
| Model | 1 | 0 tiny, 1 small, 2 large, 3 huge |
| NET | 2 | in the model's range |
| Category | 1 | 1 A, 2 B |
| Color code | 1 | 0-15 |
| Options | 2 | the system record's options (dmr-trunking.md 12.1), only the bits in mask `0x0FE7` |
| *Whole only:* | | |
| Fallback hold | 1 | seconds |
| Fallback | 1 | a 1-based index into the record's channels, or 0. It must name a conventional channel of the record |
| Plan | 12 | the system record's fixed plan: base Hz (4), first LCN (2), separation code (1), flags (1, bits 1-0 only), split Hz (4) |
| Leave level | 1 | dBm, signed |
| Random access | 1 | 0-7 |
| Sites | 1 + 2 each | count (0-8), then each site's number |
| Channels | 1 + 12 each | count (1-8), then each: flags (bit 0 control), logical channel (2), RX Hz (4), TX Hz (4), color code (1, 0-15, or 0xFF the system's) |
| Key | 16 | when bit 2 |
| PSN | 3 | when bit 3 |

Never sent: the radio's own ID and its ID on the system, kill or stun state, registration. A whole system has at least
one control channel. A system no channel of the record names is refused.

## 5. Receiving

### 5.1 The setting

**Settings > Connections > Accept zone shares**: On (the default) or Off. With On, a share is offered to the user,
who imports it or not (5.2); nothing is imported without that. With Off, fragments are dropped (`zs.rx_off`) and
nothing reaches the user. The setting is `zs_offers` in the settings record: 0 (never set) and 1 are On, 2 is Off
([settings-store.md](settings-store.md)). In the service and on the debug link, 1 (`ZS_OFFERS_ASK`) is On and 0 Off.

### 5.2 Offers

| Rule | Value |
|---|---|
| Offers at a time | one. While it waits, other records are dropped (`zs.rx_busy`) |
| Rate limit | at most 6 offers an hour; more are dropped (`zs.rate_limited`) |
| Lifetime | an offer not opened within 10 minutes is dropped (`zs.expired`). Once opened, it waits for the user |
| Group offers | a record sent to a talkgroup the radio listens to is offered like one sent to it |

The offer shows as a banner, "Zone from <name or ID>" with "Zone <name>, <n> channels". **#** opens it. The banner
has no timer: it stays until **#** opens it, **Back** hides it (on any screen), or the offer ends (it expires, or is answered from
Tools, which also hides it). Another banner can take its place. While it waits it is also under **Tools > Zones/Chs Received**. The offer screen (Zone Share Received, or Channel Share Received) reads "Zone <name>, <n> channels, from <ID>" and
how many are not in this radio's bands, and how many are trunked on a system this radio can't use: "2 trunked channels
unavailable: system not set up" (dmr-trunking.md 12.6). Three choices:

- **Import** imports it (5.3).
- **Preview** lists the channels: name, frequency, mode, "Not in band" for those that can't be received and "System
  not set up" for trunked ones that would come in unavailable.
- **Reject** drops it.

A one-channel offer asks which zone to add the channel to.

### 5.3 Import

| Rule | Behavior |
|---|---|
| Band limits | only channels whose RX frequency is in this radio's bands are imported (`board_in_band`). The rest are counted and left out. None in band: nothing is imported |
| Receive only | every imported conventional channel is receive only, whatever the sender had. The user turns transmit on per channel. A trunked channel transmits when its system is usable: the site grants every transmission |
| Trunked systems | matched by model, NET and category against the radio's systems; a match is used as it is. With none, a shared system is added in the lowest free system slot, with sharing off and no ID of its own. One that came by identity, or that authenticates and came without its key, leaves its channels unavailable: locked out, "System not set up" (dmr-trunking.md 12.4). With no free slot its channels are left out and counted, and so are their contacts. Each trunked channel gets its tag |
| Power | the power class maps to this radio's levels: low the lowest, mid the middle, high the highest, default the codeplug's default |
| Contacts | matched by ID and type against the user's contacts; a match keeps the user's name. Only contacts that imported channels use are added, in the lowest free slots. The same contact twice in a record becomes one |
| New zone | added through the zone editor's write layer, as a new zone there is ([ui.md 2.2](ui.md#22-zone-editor)): it takes the lowest free slot, which can be ahead of other zones, and whoever holds a zone index follows the move. An update rewrites its zone in place. A name already in use gets " 2" to " 99", shortened to fit 16 characters |
| No stored zones | the radio's "All channels" zone is stored as zone 0 first, so the user keeps it |
| One channel | added to the end of the zone the user picked (`zone_edit_insert`) |
| Fallback | a system the import added falls back to the channel its share named, as written here |
| Failure | anything written is undone, the systems it added too: no full zone list, no free channel or contact, a flash write error. Only trunked channels in band, and no room for their system: nothing is imported |

The import runs as a storage job, not on the UI task. The banner after it reads "Imported <zone>" or "Updated
<zone>" with "<n> channels, receive only", or "<n> trunked: system not set up" when trunked channels came in
unavailable or were left out.

### 5.4 Updates

A zone sent again by the same sender with the same zone ID updates the earlier import instead of adding a zone.

- The radio remembers its last four imports in the settings record (`zs_import`): a key (an FNV-1a hash of the
  sender's ID and the zone ID), the name the zone got, how many channels the import wrote and their signature (the
  XOR of an FNV-1a hash of each channel's number and every field). The oldest is forgotten first.
- The update finds the zone by that name. If the user renamed or deleted it, the zone comes in as a new one.
- An update replaces only the earlier import's channels that the user left unchanged. Anything the user added to
  the zone or changed (a field, transmit turned on) stays in the zone, after the new channels, in its order.
- Which channels are the import's:

  | Case | Replaced |
  |---|---|
  | The zone still starts with the channels the import wrote, unchanged (the first `count` match the signature) | those; everything after them is the user's |
  | Otherwise (the user changed, moved in or removed a channel among them) | only channels identical to one the new copy brings. An earlier channel the sender has since changed or dropped stays; the user deletes it |

- Replaced channels that no zone lists any more are deleted, with their trunked tag. Contacts and systems are never
  deleted.

## 6. Directory requests

A radio may ask a directory service for the zone of the repeater or hotspot its request went through
(`zone_share_request`, type 2). The answer is a record, taken as an offer even with offers Off (the user asked), or
a refusal (3.3). An answer later than 60 s is not taken. No screen sends requests yet.

## 7. Sending

In a zone's channel list, **#** opens "Share zone to..." and "Share channel to..." (the focused channel). With the
zone editor (`CONFIG_ZONE_EDIT`), **#** on a channel opens its Channel actions, where "Share channel to..." is item 5,
and "Share zone to..." is item 3 of Zone options ([ui.md](ui.md) 2.2). The channel editor has "Share channel to..."
after "Tune to this ch" ([ui.md](ui.md) 2.1); it sends the stored channel, so unsaved changes must be saved first. The rows
carry a share icon, and the offer's title and banner a download icon. Then the contact list (Share To): a private contact gets confirmed packets, a talkgroup unconfirmed ones. Digits that make an ID add two rows after the contacts,
"Private ID 9991234" and "TG 9991234", so a station that isn't a contact can be sent to. The send works only on a DMR
channel: on another, OK says "Go to a DMR channel to send" and sends nothing. The send's toast names the frequency:
"Sending to Q1AAA Joe via 442.375" ("Sending via 442.375" where that doesn't fit). It is the channel's receive frequency, which on a repeater is its output.
The banner says "Sent to <name>" or "Not sent to <name>". One send at a time.

## 8. Limits

| Limit | Value |
|---|---|
| Record | 1024 octets, 80 channels, 32 contacts, 4 trunked systems of up to 8 channels and 8 sites each |
| Names | 16 printable ASCII characters |
| Fragments | sent: up to 4 of 322 octets; taken: up to 8, chunks of 128 to 562 octets |
| RAM | about 4 KB on one board's build; the record being sent comes from the RTOS heap while it is made ([feature-matrix.md](../feature-matrix.md)) |
