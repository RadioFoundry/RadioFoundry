<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Scan lists

Named lists of channels from any zone, scanned without switching zones. This page is the stored list: its record, its
area in the flash and the calls that read and write it (sections 1 to 5), then the scan that walks a list (6) and the
screens that edit one (7). Code: `core/codeplug/scan_list.c`, the scan in `core/radio/radio_sm.c`, the screens in
`core/ui/ui_scanlist.c` and `core/ui/ui_svc_scanlist.c`; switch `CONFIG_SCAN_LIST` ([build-config.md](../build-config.md)).
Tests: `tests/host/scanListTest.c` (the store), `tests/host/radioCoreTest.c` (the scan), `tests/host/uiFlowTest.c` (the
screens).

| Section | Contents |
|---|---|
| 1 | What a scan list is |
| 2 | The area |
| 3 | The record |
| 4 | Calls |
| 5 | Safety |
| 6 | The scan |
| 7 | The screens |
| 8 | Not built yet |

## 1. What a scan list is

A zone scan visits the current zone's channels ([radio-features.md](radio-features.md) 2). A scan list is a second kind of
list for the same job: a name and up to 80 channel numbers (`CODEPLUG_ZONE_MAX`), each a channel of the codeplug in
any zone or in none. A list does not appear among the zones, so choosing one never changes the zone the radio is on.

- A list is a `zone_t` (`scan_list_t`), so the helpers in `zone_edit.h` that change one in RAM (find, insert, remove,
  move) serve a scan list too.
- A list is named by its slot, the way a zone is by its index. Deleting one frees its slot and the lists after it keep
  their numbers.
- A channel number the codeplug no longer holds stays in the list. The scan skips it.
- A channel's skip flag (`scan_skip`) does not apply to a list: a channel put in a list is scanned.

## 2. The area

`hal_flash_map_t.scan_addr` / `scan_size`: whole erase units outside every other area, never written by the
open-firmware CPS. A board with no area (`scan_size` 0) has no scan lists, and every call fails or reports none.
The core accepts an area only when it is aligned to erase units, inside the flash, clear of every other area
(settings, journal, open-firmware images, calibration, native codeplug, staging and the satellite area) and the
storage region is ours ([settings-store.md](settings-store.md) 1).

| Size | Lists |
|---|---|
| 1 unit (4 KB) | 16 |
| 2 units | 32 |

The lists are not part of the codeplug image: the CPS, the TFTP codeplug and a codeplug backup do not carry them.

## 3. The record

256 bytes, little endian, 16 to an erase unit. A record that fails any check reads as "not a list".

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `4F 53 43 4C` ("OSCL") |
| 4 | 1 | layout, 1 |
| 5 | 1 | flags, 0 (reserved) |
| 6 | 2 | channel count, at most 80 |
| 8 | 16 | name, NUL padded, bytes as in a codeplug name; no terminator when it uses all 16 |
| 24 | 160 | 80 channel numbers (u16, 1-based); the entries past the count are 0 |
| 184 | 68 | reserved, 0 |
| 252 | 4 | CRC-32 of bytes 0 to 251 |

A list is valid when it has a name, at most 80 channels, no channel 0 and no channel twice. A free slot is all 0xFF.
A slot that is neither a list nor free is someone else's data.

A later layout may use the reserved bytes and flags. A reader of this layout takes a record with another layout byte
as not a list.

## 4. Calls

| Call | Does |
|---|---|
| `scan_list_state` | `NONE` (no usable area), `OURS` (every slot free or a list) or `FOREIGN` (a slot holds something else) |
| `scan_list_slots`, `scan_list_count` | Slots in the area, lists in it |
| `scan_list_read` | Slot i's list; false for a free or foreign slot |
| `scan_list_write` | Store a list in a slot, replacing what is there |
| `scan_list_add` | Store a list in the first free slot: the slot, or -1 |
| `scan_list_delete` | Free a slot |
| `scan_list_clear` | Erase the area whatever it holds: the way out of `FOREIGN` once the flash is backed up |
| `scan_list_generation` | Counts every change, so a reader that kept a list knows to read again |

Every call takes the storage lock. A write rewrites a sector and can take some 100 ms: a task that must not wait
hands it to the housekeeping task.

## 5. Safety

- **Power cuts.** A change rewrites the record's sector through the sector journal ([settings-store.md](settings-store.md)
  4): after a cut the old sector or the new one is there, never a mix. `scanListTest` cuts power at every unit of a write.
- **Foreign data.** While a slot holds something else, the area is read only. The storage claim
  ([settings-store.md](settings-store.md) 5) clears other firmware's bytes from the area and keeps our lists, like the
  satellite area: a sector whose slots are all free or ours is not touched.
- **No RAM copy.** A list is read from flash when asked for.

## 6. The scan

`radio_req_scan_list(slot)` (`RSCAN_LIST`) starts the list in `slot`; the view's `scan` is 4. It uses the zone scan's
dwell, probe, hold and resume rules ([radio-features.md](radio-features.md) 2), `scan_stop` and CANCEL included.

- **Stops.** When the scan starts the radio finds each list channel's zone once: the zone in use first, then the others in
  order. A channel in no zone, or no longer in the codeplug, is left out of the scan (it stays in the list). The scan
  starts only with at least two stops, from a channel (not the VFO); otherwise the request does nothing.
- **Order.** The list's own order, from its first channel. There is no priority revisit: a list has no start channel.
- **The view.** Each stop tunes as a zone change plus a channel change does, so the view's zone, position and channel
  number are the stop's and agree with each other. Dual watch tunes across zones the same way.
- **Cancel** goes back to the channel the scan started from, in its zone.
- **Nuisance delete** leaves the stop out until the scan ends.
- **The codeplug changes** (any write, an import): the scan stops where it is, because its stops are zone positions found
  at the start.
- A stop that can't be read when its turn comes (the codeplug changed under it) is left out for the rest of the scan.

## 7. The screens

Tools > Scan opens "Scan" when the radio has a usable area; without one it starts the zone scan as it always did.
All of them are in `core/ui/ui_scanlist.c`; a write is a job for the housekeeping task (`ui_svc_scanlist_*`), one at a time,
so a second change while one is saving says "Busy saving: try again". [ui.md](ui.md) 2.7 has the screens.

## 8. Not built yet

- The Codeplug Editor and qdmr do not see the lists, and a codeplug backup does not carry them.
- Moving a channel up or down inside a list, and sharing a list.
- A scan list from the VFO.
