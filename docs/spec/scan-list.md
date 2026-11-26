<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Scan lists

Named lists of channels from any zone, scanned without switching zones. This page is the stored list: its record, its
place in the codeplug and the calls that read and write it (sections 1 to 5), then the scan that walks a list (6) and the
screens that edit one (7). Code: `core/codeplug/scan_list.c`, the scan in `core/radio/radio_sm.c`, the screens in
`core/ui/ui_scanlist.c` and `core/ui/ui_svc_scanlist.c`; switch `CONFIG_SCAN_LIST` ([build-config.md](../build-config.md)).
Tests: `tests/host/scanListTest.c` (the store), `tests/host/radioCoreTest.c` (the scan), `tests/host/uiFlowTest.c` (the
screens).

| Section | Contents |
|---|---|
| 1 | What a scan list is |
| 2 | Where lists live |
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

## 2. Where lists live

Each list is one record of the codeplug settings area ([codeplug-settings.md](codeplug-settings.md)), key `0x1E00` plus
its slot (the family is reserved in [channel-modes.md](channel-modes.md) 3). The area is in every board's open-firmware
image, so every radio with the switch on has scan lists, and the area's rules apply: a write is one journaled sector
rewrite, other firmware and tools that don't know the key keep it, and a later firmware's area is never written.

| | |
|---|---|
| Slots | 16 (`SCAN_LIST_SLOTS`) |
| Size | 18 bytes and 2 a channel: 20 to 178 bytes |
| Room | The area's 4 KB is shared with the other keyed settings. A list that doesn't fit is refused, and what is stored stays |

The lists are in the codeplug image's settings area, so a codeplug backup carries them, and a CPS that keeps unknown
bytes (the Codeplug Editor) round trips them. The area's continuation ([codeplug-settings.md](codeplug-settings.md) 2.4)
will add room without changing the record.

## 3. The record

Little endian. Value of key `0x1E00 + slot`. A record that fails a check reads as "not a list": the slot is free to
`scan_list_add`.

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | flags, 0 (reserved) |
| 1 | 1 | channel count, at most 80 |
| 2 | 16 | name, NUL padded, bytes as in a codeplug name; no terminator when it uses all 16 |
| 18 | 2 a channel | channel numbers (u16, 1-based) |

A list is valid when it has a name, at most 80 channels, no channel 0 and no channel twice. A reader ignores bytes after
the channels, so a later version appends fields or adds keys of its own.

## 4. Calls

| Call | Does |
|---|---|
| `scan_list_usable`, `scan_list_slots` | Whether lists can be stored (the area exists and isn't a later firmware's); the slots: 16, or 0 |
| `scan_list_count` | Lists stored |
| `scan_list_read` | Slot i's list; false for a free slot or a record that isn't a list |
| `scan_list_write` | Store a list in a slot, replacing what is there |
| `scan_list_add` | Store a list in the first free slot: the slot, or -1 |
| `scan_list_delete` | Free a slot |
| `scan_list_generation` | Moves on every change here and every change of the settings area, so a reader that kept a list knows to read again |

A write rewrites a sector and can take some 100 ms: a task that must not wait hands it to the housekeeping task.

## 5. Safety

- **Power cuts.** A change is the area's journaled sector rewrite ([settings-store.md](settings-store.md) 4): after a cut
  the old sector or the new one is there, never a mix. `scanListTest` cuts power at every unit of a write.
- **Shared area.** A list is a key like any other: writing one keeps the other keys, and the other settings keep the
  lists. Raw writes of the area follow the area's rule ([codeplug-settings.md](codeplug-settings.md) 6).
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

Tools > Scan opens "Scan" when the settings area can take lists; without it it starts the zone scan as it always did.
All of them are in `core/ui/ui_scanlist.c`; a write is a job for the housekeeping task (`ui_svc_scanlist_*`), one at a time,
so a second change while one is saving says "Busy saving: try again". [ui.md](ui.md) 2.7 has the screens.

## 8. Not built yet

- The Codeplug Editor and qdmr do not edit the lists: the Editor keeps the bytes, qdmr drops its own `scanLists`.
- Moving a channel up or down inside a list, and sharing a list.
- A scan list from the VFO.
