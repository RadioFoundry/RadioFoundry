<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Scan lists

Named lists of channels from any zone, scanned without switching zones. This page is the stored list: its record, its
area in the flash and the calls that read and write it. The scan that walks a list, and the menus that edit one, are
not built yet (section 6). Code: `core/codeplug/scan_list.c`, switch `CONFIG_SCAN_LIST`
([build-config.md](../build-config.md)). Test: `tests/host/scanListTest.c`.

| Section | Contents |
|---|---|
| 1 | What a scan list is |
| 2 | The area |
| 3 | The record |
| 4 | Calls |
| 5 | Safety |
| 6 | Not built yet |

## 1. What a scan list is

A zone scan visits the current zone's channels ([radio-features.md](radio-features.md) 2). A scan list is a second kind of
list for the same job: a name and up to 80 channel numbers (`CODEPLUG_ZONE_MAX`), each a channel of the codeplug in
any zone or in none. A list does not appear among the zones, so choosing one never changes the zone the radio is on.

- A list is a `zone_t` (`scan_list_t`), so the helpers in `zone_edit.h` that change one in RAM (find, insert, remove,
  move) serve a scan list too.
- A list is named by its slot, the way a zone is by its index. Deleting one frees its slot and the lists after it keep
  their numbers.
- A channel number the codeplug no longer holds stays in the list. The scan skips it.
- A channel's skip flag (`scan_skip`) applies to every list, as it does to the zone scan.

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

## 6. Not built yet

- The scan: starting one on a list, and the view following each channel's zone as dual watch does.
- The menus: create, name, add channel, remove, rename and delete a list, and starting its scan.
- The Codeplug Editor and qdmr do not see the lists.
