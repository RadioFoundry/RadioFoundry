<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Native codeplug

The radio stores its codeplug in the open-firmware layout ([open-firmware-cps.md](open-firmware-cps.md) 7), so qdmr
and the open-firmware CPS work unchanged. This page describes the editing model the core uses on top of that layout,
and the flash area reserved for a native format.

| Part | Where |
|---|---|
| Editing model | `core/codeplug/codeplug.h`, `core/codeplug/codeplug.c` |
| Native area and its factory reset | `core/storage/storage.h` (`STORAGE_NATIVE_MAGIC`, `storage_native_erase`) |
| Area in the flash map | `hal_flash_map_t.native_addr`, `native_size` (`hal/hal_flash.h`) |
| Tests | `tests/host/codeplugEditTest.c`, `tests/host/uiFlowTest.c`, `tests/hil/test_cps.py` |

Section numbers are identifiers: code and other pages cite them. Removed sections leave gaps.

## 1. Design

- One editing model serves the radio's own menus and the open-firmware CPS shim.
- Edits on the radio are safe against power cuts ([settings-store.md](settings-store.md) 4) and cost one sector
  rewrite each, not a whole-image rewrite.
- Radio-wide state, keys included, lives in the settings store, not in the codeplug. You can share a codeplug file
  without leaking keys or the owner's last channel.
- Radio-wide settings that the open-firmware layout has no field for are keys in the codeplug settings area
  ([codeplug-settings.md](codeplug-settings.md)).

## 2. The editing model

`core/codeplug/codeplug.h` is the model. Callers see records and never touch addresses:

| Record | Read | Edit |
|---|---|---|
| Channel 1..1024 (`channel_t`) | `codeplug_channel` | `codeplug_set_channel`, `codeplug_delete_channel`, `codeplug_free_channel` |
| Zone 0..67 (`zone_t`, up to 80 channels) | `codeplug_zone`, `codeplug_zone_count`, `codeplug_zones_stored` | `codeplug_set_zone`, `codeplug_delete_zone` |
| Contact 1..1024 (`contact_t`) | `codeplug_contact`, `codeplug_contact_find` | `codeplug_set_contact`, `codeplug_delete_contact` |
| RX group list 1..76 (`listen_list_t`) | `codeplug_listen_list` | `codeplug_set_listen_list`, `codeplug_delete_listen_list` |
| Radio name, own DMR ID, hang times | `codeplug_radio_name`, `codeplug_dmr_id`, `codeplug_dmr_settings` | `codeplug_set_radio_name`, `codeplug_set_dmr_id` |

The model follows these rules:

- **Every call takes `storage_lock`**, so any task can call. An edit costs one journaled sector rewrite (about
  100 ms on the W25Q128). A new record also rewrites its bitmap's sector: the record goes first, then its in-use bit.
- **Fields the model doesn't own are kept.** An edit reads the stored record, changes the bytes the model knows and
  writes it back. Bytes and flags that no field covers survive a radio-side edit.
- **`codeplug_serial()`** goes up on every change. That covers edits and `codeplug_reload()` after the CPS shim
  writes the image underneath. The radio core polls it and re-reads its zone and channel (`RREQ_RELOAD`). It falls
  back to the saved zone position when the saved channel number is gone. It also forgets the contact names it looked
  up for IDs (a "no name" answer is cached too), so a contact added after its ID was heard or called shows by name.
- **The built-in set.** With no codeplug stored (blank flash, `codeplug=builtin`), the radio has a small built-in
  list of channels. The first edit *materializes* it: the built-in channels are written as stored records, the
  bitmaps and list counts are cleared, and the edit applies on top. After that the source is `open-firmware`.
- **A failed read is not a free slot.** The index (start-up and `codeplug_reload()`) reads each bank's in-use bitmap,
  then every record it marks. A marked record whose frequencies aren't valid BCD is dropped (an erased bank marks
  every slot); the next save in its bank clears its bit in flash. A read that fails, of a bitmap or a record, hides
  what it covers and leaves the bank unknown until the next index. In an unknown bank `codeplug_free_channel` offers
  no slot, `codeplug_set_channel` rewrites only a channel shown, `codeplug_delete_channel` fails, and the built-in
  set isn't materialized. A zone save fails if it would store the virtual zone, or if the stored zone lists a channel
  of such a bank: both were read without the hidden channels. So no save writes a bitmap or a zone that drops
  channels it couldn't see. Start new (the answer to another firmware's codeplug, below) writes every bitmap anyway.
- **Another firmware's codeplug.** A radio that ran another firmware may hold that firmware's own codeplug where the
  open-firmware one goes. Its bytes can pass the per-record checks by chance (an in-use bit set, frequencies that are
  valid BCD), so the radio decides for the whole codeplug at every index (start-up and `codeplug_reload()` after a
  CPS write). It counts good and bad records by the web editor's open-firmware evidence rule
  ([codeplug-port.md](codeplug-port.md) 5, step 1; the same thresholds, with `site/static/js/codeplug/detect.js` as
  the reference implementation), on the channels, zones, and RX group lists only. It leaves out the contacts and the
  general settings: another firmware's contacts left beside a codeplug edited on the radio must not hide it, and the
  checks cost flash. With bad records and fewer than four good ones for each bad one,
  `codeplug_foreign()` is true. The radio then shows no codeplug: the built-in set, `codeplug=builtin`, no zones,
  contacts, RX group lists, DMR ID, radio name or boot text. It never erases it on its own. A CPS write that leaves a
  valid codeplug flips it back on the reload. Editing refuses to write over it (`materialize()` fails); the UI asks
  first. A save on the radio (save to channel, channel or contact edit, zone edit, talk target, DMR ID, radio name,
  quick keys) opens the "Old codeplug" screen (`codeplug-foreign`) and holds the write until it is answered. A
  setting kept as a key of the settings area isn't such a save: it is saved as the radio's own
  ([codeplug-settings.md](codeplug-settings.md) 5.2):

  | Choice | What happens |
  |---|---|
  | Cancel (the default; also Back or leaving the screen) | Nothing is written. Toast "Not saved" |
  | Start new | `codeplug_start_new()`: clears the general settings up to the hang times (radio name, DMR ID and hang times, 0x00E0-0x00F9: 0xFF), the boot text (0xFF), the quick-key slots (0), the 1024 contact records (0xFF, seven sector rewrites) and the codeplug settings area (0xFF, [codeplug-settings.md](codeplug-settings.md) 5.2), then materializes as for a blank flash. The held write then goes on. The codeplug made is a clean one, which the next start-up takes as the radio's own |
  | Use anyway | `codeplug_accept(true)` and a reload: nothing is erased and the verdict is off, so the radio shows that codeplug's records. The held write is dropped (it was made against the built-in set); toast "Old codeplug kept: edit again" |

  The acceptance belongs to the codeplug stored when it was given. It is kept in the settings (`cp_flags` bit 0,
  [settings-store.md](settings-store.md)), loaded at start-up before the index. `codeplug_written()`, which the CPS
  shims and the TFTP server call when a write commits, drops it, and so does a commit finished at start-up and a
  factory reset. A zone import from a zone share isn't asked about: it fails without writing. Use anyway on a codeplug
  with no channel that passes the checks shows the built-in set, and the first edit then materializes it, clearing the
  bitmaps, zones and lists. Debug link (`storage` view): `codeplug.foreign`, `codeplug.accepted` and
  `codeplug.evidence=good,bad` (the counts of the last index, with bad set to 0 by Start new).
- **A free channel slot.** `codeplug_set_channel` on a channel number that isn't in use starts from a blank record,
  not from the bytes in the slot. Only an existing channel keeps the bytes the model doesn't own.
- **Zones.** With no zone stored there is one virtual zone, "All channels". The first `codeplug_set_zone(0, ...)`
  replaces it with a stored zone table, using the 80-channel zone layout.
- **Validation.** A channel number out of range or a malformed frequency (0, 1 GHz or more), a zone naming a
  channel that isn't stored, or a list naming a contact index out of range is refused. The caller gets `false` and
  nothing is written. Band limits are checked when the radio tunes, not at storage.

`codeplug_openfw_decode()` and `codeplug_openfw_encode()` expose the 56-byte channel record conversion for the CPS
shim's VFO handling (`cps-protocol.md` 2.3).

## 4. The native area

The board's flash map can reserve an area for a native codeplug format: `hal_flash_map_t.native_addr` and
`native_size`, above anything an open-firmware tool writes (`openfw_end`). The open-firmware CPS shim never writes it,
and nothing in the firmware writes a native codeplug there. A board without the area sets the size to 0.

- **Magic.** A native area's header sector starts with `STORAGE_NATIVE_MAGIC`, `0x4E52444F` (bytes `4F 44 52 4E` in
  memory order). The magic is fixed because the factory reset keys on it.
- **Factory reset.** `storage_native_erase()` erases the native area only when it starts with the magic. It erases
  the non-blank sectors from the end and the header sector last, so a power cut leaves the magic in place and the next
  reset finishes the job. It reports the area blank when it's all `0xFF`, and otherwise leaves it alone as foreign
  (another firmware's data). It never touches the open-firmware image, the calibration, or a foreign-guarded region.
