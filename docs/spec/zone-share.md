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
| 1 Fragment | 7 to 328 | version, type, transfer number, index (bits 7-4) and count (bits 3-0), offset (2), chunk |
| 2 Request | 5 | version, type, request number, what (1: the zone of the site the request came in through), options (0) |
| 3 Refusal | 4 | version, type, request number, code |

### 3.1 Fragments

- A record of up to 1024 octets goes in 1 to 4 fragments. Each chunk is 322 octets, the last one shorter. The largest
  message, 328 octets, fits one confirmed packet with the full IPv4 and UDP headers.
- The offset must be index x 322. Every fragment but the last must be full. A fragment that breaks a rule is dropped
  (`zs.rx_bad`).
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
| Channel | 6 to 15 + name | flags, RX Hz (4), then as the flags say: TX offset (4, signed, not 0), DMR (2), RX tone (2), TX tone (2); name |

- A name is a length (0 to 16) and that many printable ASCII octets. The sender writes `?` for anything else.
- Record flags: bit 0, one channel (the count must be 1). The other bits are reserved and must be 0.
- Channel flags: bit 0 DMR, 1 25 kHz, 2 TX offset present, 3 RX tone present, 4 TX tone present, 5 receive only on
  the sender, bits 7-6 power (0 the default, 1 low, 2 mid, 3 high).
- DMR octets: the color code (bits 3-0), slot 2 (bit 4), direct mode (bit 5), bits 7-6 reserved; then the contact, a
  1-based index into the record's contacts or 0. A DMR channel carries no tones.
- A tone: the type in bits 15-14 (1 CTCSS, 2 DCS, 3 DCS inverted), the value in bits 13-0. CTCSS is Hz x 10 (600 to
  2600). DCS is the code as an octal number (023 is 19), at most 0777.
- Contacts come first, then channels. The record must end exactly where its length says.
- The decoder checks every length, count, reference and value before use. A record that fails one check is refused
  whole.
- Zone ID: the sender's choice, the same each time it sends the same zone. Ours is an FNV-1a hash of the zone name and
  the flags.

### 4.1 What a send holds

The sender takes the zone's channels in order, each with its contact, while the record fits 1024 octets and 80
channels. The rest are left out, and the UI says how many go ("Sending 40 of 60 channels"). Nothing else of the
codeplug is sent: no scan lists, no RX group lists, no privacy keys.

## 5. Receiving

### 5.1 The setting

**Settings > Connections > Zone offers**: Off (the default) or Ask. With Off, fragments are dropped (`zs.rx_off`) and
nothing reaches the user. The setting is `zs_offers` in the settings record ([settings-store.md](settings-store.md)).

### 5.2 Offers

| Rule | Value |
|---|---|
| Offers at a time | one. While it waits, other records are dropped (`zs.rx_busy`) |
| Rate limit | at most 6 offers an hour; more are dropped (`zs.rate_limited`) |
| Lifetime | an offer not opened within 10 minutes is dropped (`zs.expired`). Once opened, it waits for the user |
| Group offers | a record sent to a talkgroup the radio listens to is offered like one sent to it |

The offer shows as a banner, "Zone from <name or ID>" with "Zone <name>, <n> channels". **#** opens it. While it
waits it is also under **Tools > Zones/Chs Received**. The offer screen (Zone Share Received, or Channel Share Received) reads "Zone <name>, <n> channels, from <ID>" and
how many are not in this radio's bands, with three choices:

- **Import** imports it (5.3).
- **Preview** lists the channels: name, frequency, mode, and "Not in band" for those that can't be received.
- **Reject** drops it.

A one-channel offer asks which zone to add the channel to.

### 5.3 Import

| Rule | Behavior |
|---|---|
| Band limits | only channels whose RX frequency is in this radio's bands are imported (`board_in_band`). The rest are counted and left out. None in band: nothing is imported |
| Receive only | every imported channel is receive only, whatever the sender had. The user turns transmit on per channel |
| Power | the power class maps to this radio's levels: low the lowest, mid the middle, high the highest, default the codeplug's default |
| Contacts | matched by ID and type against the user's contacts; a match keeps the user's name. Only contacts that imported channels use are added, in the lowest free slots. The same contact twice in a record becomes one |
| New zone | added through the zone editor's write layer, as a new zone there is ([ui.md 2.2](ui.md#22-zone-editor)): it takes the lowest free slot, which can be ahead of other zones, and whoever holds a zone index follows the move. An update rewrites its zone in place. A name already in use gets " 2" to " 99", shortened to fit 16 characters |
| No stored zones | the radio's "All channels" zone is stored as zone 0 first, so the user keeps it |
| One channel | added to the end of the zone the user picked (`zone_edit_insert`) |
| Failure | anything written is undone: no full zone list, no free channel or contact, a flash write error |

The import runs as a storage job, not on the UI task. The banner after it reads "Imported <zone>" or "Updated
<zone>" with "<n> channels, receive only".

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

- Replaced channels that no zone lists any more are deleted. Contacts are never deleted.

## 6. Directory requests

A radio may ask a directory service for the zone of the repeater or hotspot its request went through
(`zone_share_request`, type 2). The answer is a record, taken as an offer even with offers Off (the user asked), or
a refusal (3.3). An answer later than 60 s is not taken. No screen sends requests yet.

## 7. Sending

In a zone's channel list, **#** opens "Share zone to..." and "Share channel to..." (the focused channel). With the
zone editor (`CONFIG_ZONE_EDIT`), **#** on a channel opens its Channel actions, where "Share channel to..." is item 5,
and "Share zone to..." is item 3 of Zone options ([ui.md](ui.md) 2.2). The channel editor has "Share channel to..."
after "Go to it" ([ui.md](ui.md) 2.1); it sends the stored channel, so unsaved changes must be saved first. The rows
carry a share icon, and the offer's title and banner a download icon. Then the contact list (Share To): a private contact gets confirmed packets, a talkgroup unconfirmed ones. The send works only on a DMR
channel; the banner says "Sent to <name>" or "Not sent to <name>". One send at a time.

## 8. Limits

| Limit | Value |
|---|---|
| Record | 1024 octets, 80 channels, 32 contacts |
| Names | 16 printable ASCII characters |
| Fragments | 4 of up to 322 octets |
| RAM | about 4 KB on one board's build; the record being sent comes from the RTOS heap while it is made ([feature-matrix.md](../feature-matrix.md)) |
