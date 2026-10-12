<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The OpenDigitalRadio Authors -->

# Native codeplug (draft)

Status: **draft**. Today the radio stores the open-firmware layout (`open-firmware-cps.md` 7), so qdmr and the open-firmware CPS
work unchanged. This page sets out the model the core already uses, which stays when the storage changes, and the
plan for a native format. Nothing in section 4 is built yet.

## 1. Goals

- One editing model for the radio's own menus, the open-firmware CPS shim and a future native CPS protocol.
- Edits on the radio are safe against power cuts (`settings-store.md` 4) and cost one sector rewrite each, not a
  whole-image rewrite.
- The open-firmware layout keeps working for as long as qdmr and the open-firmware CPS are the tools people have.
- The native format carries what the open-firmware one can't: per-channel privacy key slot, more zones, longer names, and
  per-channel features without overloading vendor flag bits.
- Radio-wide state, keys included, lives in the settings store, not in the codeplug. A codeplug file can be shared
  without leaking keys or the owner's last channel.

## 2. The editing model (built)

`core/codeplug/codeplug.h` is the model. Callers see records and never touch addresses:

| Record | Read | Edit |
|---|---|---|
| channel 1..1024 (`channel_t`) | `codeplug_channel` | `codeplug_set_channel`, `codeplug_delete_channel`, `codeplug_free_channel` |
| zone 0..67 (`zone_t`, up to 80 channels) | `codeplug_zone`, `codeplug_zone_count`, `codeplug_zones_stored` | `codeplug_set_zone`, `codeplug_delete_zone` |
| contact 1..1024 (`contact_t`) | `codeplug_contact`, `codeplug_contact_find` | `codeplug_set_contact`, `codeplug_delete_contact` |
| RX group list 1..76 (`listen_list_t`) | `codeplug_listen_list` | `codeplug_set_listen_list`, `codeplug_delete_listen_list` |
| radio name, own DMR ID, hang times | `codeplug_radio_name`, `codeplug_dmr_id`, `codeplug_dmr_settings` | `codeplug_set_radio_name`, `codeplug_set_dmr_id` |

Rules:

- **Every call takes `storage_lock`**, so any task may call. An edit costs one journaled sector rewrite (about
  100 ms on the W25Q128). A new record also rewrites its bitmap's sector: the record goes first, then its in-use bit.
- **Fields the model doesn't own are kept.** An edit reads the stored record, changes the bytes the model knows and
  writes it back. Vendor bytes and flags that no field covers survive a radio-side edit.
- **`codeplug_serial()`** goes up on every change. That covers edits and `codeplug_reload()` after the CPS shim
  wrote the image underneath. The radio core polls it and re-reads its zone and channel (`RREQ_RELOAD`). It falls
  back to the saved zone position when the saved channel number is gone. It also forgets the contact names it looked
  up for IDs (a "no name" answer is cached too), so a contact added after its ID was heard or called shows by name.
- **The built-in set.** With no codeplug stored (blank flash, `codeplug=builtin`), the radio has a small built-in
  list of channels. The first edit *materializes* it: the built-in channels are written as stored records, the
  bitmaps and list counts are cleared, and the edit applies on top. After that the source is `open-firmware`.
- **Zones.** With no zone stored there is one virtual zone, "All channels". The first `codeplug_set_zone(0, ...)`
  replaces it with a stored zone table, using the 80-channel zone layout.
- **Validation.** A channel number out of range or a malformed frequency (0, 1 GHz or more), a zone naming a
  channel that isn't stored, or a list naming a contact index out of range is refused. The caller gets `false` and
  nothing is written. Band limits are checked when the radio tunes, not at storage.

`codeplug_openfw_decode()` and `codeplug_openfw_encode()` expose the 56-byte channel record conversion for the CPS
shim's VFO handling (`cps-protocol.md` 2.3).

## 3. The native channel record (plan)

The fields `channel_t` holds today, plus what the open-firmware record can't hold:

| Field | Size | Notes |
|---|---|---|
| name | 16 | UTF-8, NUL padded (open-firmware: 16 ASCII, 0xFF padded) |
| rx_hz, tx_hz | 4 + 4 | Hz, binary (open-firmware: BCD of 10 Hz) |
| mode, bw, power, flags | 4 | flags: rx_only, dmo, ... |
| rx_tone, tx_tone | 2 + 2 | type in the top 2 bits, value |
| color_code, timeslot | 2 | |
| contact, listen_list | 2 + 2 | indices |
| dmr_id | 4 | 0 = the radio's |
| tot_s | 2 | 0 = the radio's |
| alias_tx | 1 | `CODEPLUG_ALIAS_*` bits |
| **privacy_slot** | 1 | 0 = the radio-wide choice (`settings.privacy_active`), 1..4 = that key slot of the settings store (`settings-store.md` 3.1), 0xFF = privacy off on this channel |
| reserved | to 64 bytes | 0, append only |

The privacy key itself is never in the codeplug. A channel names a slot. The open-firmware layout has no field for it, so
until the native format exists every channel uses the radio-wide slot.

## 4. Storage plan (not built)

- **Area.** `hal_flash_map_t.native_addr` / `native_size`, set by the board: 1 MB above anything an open-firmware tool
  writes (`openfw_end`). The CPS shim refuses writes there. Per radio: `hardware-dm1701.md` 13.2,
  `hardware-md380.md` 12.2, `hardware-dm32.md` 17.
- **Layout.** A header sector (magic, format version, table offsets, CRC) then fixed-size tables (channels, zones,
  contacts, lists) with a used bitmap each, sector-aligned so an edit rewrites one sector through the journal.
  The magic is the header's first word, `STORAGE_NATIVE_MAGIC` 0x4E52444F ("ODRN" in memory order), already fixed
  because factory reset keys on it.
- **Factory reset (built).** `storage_native_erase()` erases the native area only when it starts with the magic
  (non-blank sectors from the end, the header sector last, so a power cut leaves the magic in place and the next
  reset finishes the job), reports it blank when it is all 0xFF, and otherwise leaves it alone as
  foreign (another firmware's data). It never touches the open-firmware image, the calibration or a foreign-guarded region.
- **Switching.** `codeplug_init()` prefers a valid native header and otherwise uses the open-firmware image. A one-way
  "convert" (UI or CPS) writes the native area from the open-firmware image. The open-firmware image is left as it was, so a
  firmware downgrade still finds it.
- **Open-firmware tools after conversion.** Two options, open: (a) the open-firmware CPS shim keeps reading and writing the open-firmware
  image and a commit triggers a re-import into the native area, or (b) the shim serves an open-firmware view generated from
  the native records. (a) is simpler; (b) never lets the two drift apart.
- **Native protocol.** `cps-protocol.md` 3 drafts the record-level protocol a native CPS (and qdmr, if it adds a
  driver) would use: read and write records by type and index, no addresses.

## 5. Open decisions

1. Where the native area goes on radios whose board has no flash map yet, and whether 1 MB is the right size.
2. Re-import (4a) or generated open-firmware view (4b).
3. Whether the radio-side editor should be allowed while a CPS session is open. Today edits fail cleanly while the
   CPS holds a pending sector, and otherwise interleave with CPS commits at sector granularity.
4. UTF-8 names: the font has to cover them first.
