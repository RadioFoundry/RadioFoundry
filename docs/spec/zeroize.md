<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Emergency wipe (zeroize)

The emergency wipe erases the radio's keys, and by default its codeplug, when the owner types a 4-digit wipe code and
`#` on Home or while the keys are locked. It is for a radio about to be lost or taken: the keys stored in it are
erased from the flash, not marked deleted, and the copies in RAM are zeroed. Then the radio restarts on the built-in
channels. Status: built and host-tested (`zeroizeTest`, `nandSectorsTest`, `keyFillTest`, the UI flow tests);
untested on a radio. Switch: `CONFIG_ZEROIZE`.

A wipe only reaches the radio. Codeplug files, backups, the Codeplug Editor's autosave in a computer's browser and
any other radio still hold the keys. A group whose radio was lost must still change its keys (6).

## 1. Files

| Item | Where |
|---|---|
| The code, the key sequence, the wipe | `core/zeroize/zeroize.c` (`zeroize.h`) |
| Flash: erase a range, scrub the settings region, purge | `core/storage/storage.c` (`storage_erase`, `storage_scrub`, `storage_purge`) |
| The settings ring emptied and saved again | `core/settings/settings.c` (`settings_scrub`) |
| Secrets out of the codeplug settings area | `core/codeplug/codeplug_kv.c` (`codeplug_kv_remove_range`) |
| The codeplug's regions | `core/codeplug/codeplug.c` (`codeplug_wipe`) |
| NAND: superseded copies erased | `drivers/flash/nand_sectors` (`nand_sectors_purge`), `hal_flash_t.purge` |
| RAM in the radio core | `core/radio/radio_sm.c` (`RREQ_ZEROIZE`), `core/radio/radio_nxdn.c` |
| A key fill session (`CONFIG_KEY_FILL`) | `core/ui/ui_svc_keyfill.c` (`zeroize_ram_hook`), `core/support/entropy.c` (`entropy_wipe`) |
| Radio UI | Settings > Privacy rows, the code screen and the wipe screen (`core/ui/ui_menus.c`), the key sequence (`core/ui/ui_app.c`), `core/ui/ui_svc_zeroize.c` |

## 2. The wipe code

Four digits, set in Settings > Privacy > Wipe code: typed twice, the same both times. The radio keeps a salted hash,
never the code:

```
hash = first 16 octets of SHA-256( "RFZZ1" | UID | salt | code )
```

| Field | Octets | Value |
|---|---|---|
| Label | 5 | ASCII `RFZZ1` |
| UID | 12 | The MCU's 96-bit unique ID (`hal_system_t.chip`, `uid[0]` first, each word little endian); 12 zero octets on a board without one |
| Salt | 4 | Little endian, chosen when the code is set (below) |
| Code | 4 | The four digits in ASCII |

The salt is the uptime in ms mixed with the previous salt when the code is set. It is not random: the HAL has no
random source. The UID and the salt make the hash differ from radio to radio and from one setting to the next, so a
table of hashes made in advance doesn't help.

**What the hash protects, and what not.** There are 10,000 codes. Anyone who reads the flash (a flash reader, the
debug link's read window, the CPS shim's area 1 reads) and knows the format above tries them all in a moment: the hash
keeps the code from being read at a glance, nothing more. The keys the wipe protects sit in the same flash in plain
text (settings-store.md 3.1), so the code is no weaker than what it guards.

**Where.** In the settings store (settings-store.md 3, `zz_*` fields), not the codeplug. A codeplug is a file people
copy, share and load into other radios: a wipe code in it would travel with every copy (and its hash with it), and a
radio loaded with a group's codeplug would take a code its owner never chose. The settings store stays with the radio.
For the same reason the Codeplug Editor has no wipe code field: the editor writes codeplugs.

Settings > Privacy > Wipe code clears the code when it is set (the code screen's first choice). A factory reset clears
it, and so does a wipe of keys and codeplug.

## 3. The key sequence

Typed on Home, or on any screen while the keys are locked:

```
d d d d #
```

Four digit presses (short presses), then `#`, the context key (`ACT_CONTEXT`; `#` on every keypad map). Exactly four
digits: a fifth, any other key, a long press or leaving Home starts the count again. On Home in channel mode the first
digit opens Find as usual; the sequence goes on there.

| What | Rule | Why |
|---|---|---|
| Digits | Never taken: each does what it does without the code (Find, the VFO's frequency entry, the lock's toast) | No digit sequence behaves differently, so channel and frequency entry are unchanged and an observer sees nothing until `#` |
| `#` after the code | Taken: Find closes, the frequency typed is dropped, and the wipe (or its confirmation) starts | The code is never also a channel search or a frequency |
| `#` after other digits | Not taken: it does what it does (tunes the VFO entry, Find's action, the lock's toast) | |
| PTT held on FM | Digits are DTMF and go to the radio core; the UI never sees them, so no wipe | A code typed while transmitting would also go on air: it is never sent as DTMF by the wipe, and never matched from DTMF |
| No code set | `#` is never taken | |

While the keys are locked the digits show the lock's toast as before; `#` after the code passes the lock.

### 3.1 Options

| Setting | Values | Default | Stored |
|---|---|---|---|
| Wipe code | Set, Off | Off | `zz_flags` bit 0, `zz_salt`, `zz_hash` |
| Wipe what | Keys + codeplug, Keys only | Keys + codeplug | `zz_flags` bit 1 set: keys only |
| Wipe confirm | On, Off | On | `zz_flags` bit 2 set: off |
| Wipe after | Off, 3, 5, 10 wrong codes | Off | `zz_flags` bits 4-7 |

**Confirm On.** After the code, the wipe screen asks once: **OK** wipes, any other key cancels and goes back (still
locked if it was). One extra press. **Off**: the wipe starts at `#`.

**Wipe after n wrong codes.** Counted only while the keys are locked: four digits and `#` that aren't the code. On the
nth the wipe starts at once, with no confirmation. Unlocking the keys, or a restart, starts the count again. Not
counted on Home with the keys unlocked, where four digits and `#` are an ordinary frequency entry (`1462#` tunes
146.2 MHz) and counting them would wipe a radio in normal use. It guards a locked radio against someone trying codes;
someone who unlocks it first is not counted.

### 3.2 Clear all keys

Settings > Privacy > **Clear all keys** erases every key on the radio and keeps the channels, zones and contacts: the
privacy slots, the traffic keys (AES and the other modes' keys), their copies in RAM and a running key fill session.
It runs the same steps as a wipe with Wipe what at Keys only (4.1, 4.2, 4.5), whatever Wipe what is set to, and the
radio restarts. It needs no wipe code and always asks first, whatever Wipe confirm says: **OK** clears, any other key
goes back. A failure shows as for the wipe (4.5), and **OK** tries the keys again.

It is for handing a radio on, or loading a new key set over a test one: the keys go with real sector erases, not by
overwriting a field. As with the wipe (6), copies of the keys in backups and CPS files on a PC are not touched.

## 4. The wipe

A storage job on the housekeeping task (settings-store.md 3.0), like other codeplug writes. The UI shows the wipe
screen while it runs. Steps, in this order:

### 4.1 Keys in the flash

Erased with sector erases, read back blank.

| Keys | Where | Keys only | Keys + codeplug |
|---|---|---|---|
| Traffic keys (0x1900-0x1CFF) and system secrets (0x0300-0x03FF) | The codeplug settings area, EEPROM image 0xE000 ([codeplug-settings.md](codeplug-settings.md), [channel-modes.md](channel-modes.md) 5.4, 7.4) | The two families removed, the rest kept: one journaled rewrite of the area's sector. An area that can't be read as ours (damaged, a later version, the sector buffer taken by a CPS session) or rewritten may hold keys all the same: its sector is erased | The area's sector and its reserved continuation (0xF000) erased |
| Privacy key slots | The settings ring ([settings-store.md](settings-store.md) 3.1) | The slots zeroed in the live copy, then the scrub below | The settings back to their defaults, then the scrub |

**The scrub** (`settings_scrub`, `storage_scrub`). Every old settings record holds the keys of its day, so zeroing the
live copy and saving isn't enough. Under the storage lock: every sector of the settings ring and both journal sectors
are erased and read back blank (J1 holds a copy of the last sector the journal rewrote, which may be the settings
area; J0 a patch transaction's data), the staging area's non-blank sectors are erased (a TFTP or fast CPS image waiting
there holds a whole codeplug, settings area included; a pending commit there is dropped), the ownership mark is
written (settings-store.md 5), and the live settings are saved as the first record of the empty ring.

### 4.2 Keys in RAM

| Copy | Cleared by |
|---|---|
| The radio core's privacy key and type, the call stack's and the data layer's key contexts | `RREQ_ZEROIZE`: privacy off, the copies zeroed, DMR reconfigured |
| The NXDN engine's scramble keys | The same request (`radio_nxdn_wipe`) |
| The settings store's copies (`live`, `saved`, `before`), its slot buffers | `settings_scrub` |
| The storage's shared sector buffer and slot buffers | `storage_scrub` |
| A key fill session | `zeroize_ram_hook()` (below) |

`zeroize_ram_hook()` is a weak function with an empty default (`zeroize.h`). A feature that holds keys in RAM of its
own defines it and zeroes them there. It runs on the housekeeping task, after the flash steps and after the radio
core's request was posted, with no lock held. It must not block for long or take the storage lock.

Key fill (`CONFIG_KEY_FILL`) defines it in `core/ui/ui_svc_keyfill.c`, built only with both switches on: an emergency
wipe clears a running session ([key-fill.md](key-fill.md) 5). The session and the randomness pool belong to the UI
task, so the hook wipes them in place with the scheduler suspended: the session's secrets, its messages and the store
job's key, then the pool (`entropy_wipe`). The session is left marked ending, with nothing to send; the UI task frees
the wiped block on its next tick (the pointer is cleared before the block is freed, so the hook never writes to a freed
block), and a store job not yet run stores nothing. With the scheduler running again the hook closes the transport
(`key_fill_open(false)`: the held message cleared, its queued message canceled) and gives back the low power override
(`radio_req_tx_low(false)`). A computation the UI task was in the middle of when the wipe came (an X25519 step) may
still write its result into the ending session; the UI task wipes it with the block on its next tick, and the restart
follows a wipe that went.

The radio core's request goes twice. First at the start of the wipe, without waiting: the radio core saves the key in
use with the channel (settings-store.md 2), so it must drop its key before the settings are scrubbed, or it could
write it back. The housekeeping task waits for it (up to 1 s, `radio_zeroize_busy`) just before the settings step.
Then again with the other RAM steps, waited for the same way before the restart. A radio core that doesn't answer in
time is reported (`radio RAM`).

### 4.3 The codeplug (Keys + codeplug)

| Region | Erased |
|---|---|
| The open-firmware EEPROM image, 64 KB (channels 1-128, zones, general settings, boot text, DTMF and APRS records, the settings area) | Every sector |
| The flash image's codeplug, 0x7B000-0x8EFFF (channels 129-1024, contacts, RX group lists) | Every sector |
| The native area | `storage_native_erase`: only when it holds our data, as a factory reset |
| The radio's settings | Back to the defaults (4.1) |

Each sector that isn't blank is erased and read back blank (`storage_erase`). `storage_erase` refuses any range outside
the open-firmware images, the native area and the staging area, and any range that touches the calibration window, the
settings ring or the journal.

Kept: the firmware (MCU flash, never written), the calibration (factory and working copy), the DMR ID database, the
voice prompts and the custom data area (boot image, melody: flash image 0x00000), which hold no keys. A radio with
another firmware's codeplug in the open-firmware area has it erased too.

### 4.4 NAND

A NAND flash behind the sector layer is copy on write ([nand-sectors.md](nand-sectors.md)): a sector erase or rewrite
writes a new record and leaves the old one in its block until garbage collection. The last flash step is
`storage_purge`, which calls `hal_flash_t.purge` when the board offers it (`nand_sectors_purge`): every block that may
hold a superseded record has its current records moved out and is erased. A block the layer retired (a failed program
or erase) can't be erased and may keep what it held; one with foreign data is never touched. A SPI NOR flash erases in
place and has no purge.

### 4.5 Then

All steps done: the radio restarts (`hal_system_t.reboot`) and comes up on the built-in channels (Keys + codeplug) or
its codeplug without keys (Keys only).

A step that fails doesn't stop the others: every step runs, RAM is cleared, and the wipe screen lists what failed
("Wipe incomplete: keys area, settings, codeplug, purge") and stays, with no restart, so the radio is never left half
wiped without saying so. OK on that screen runs the wipe again. A radio whose storage isn't usable (no flash map, a
foreign region: settings-store.md 5) has stored no key, so only RAM is cleared.

## 5. Settings store fields

Page 1, appended after `reserved11` (an older record reads no code and the defaults):

| Field | Size | Meaning |
|---|---|---|
| `zz_flags` | 1 | Bit 0 a code is set; bit 1 keys only; bit 2 no confirmation; bit 3 reserved (0); bits 4-7 wrong codes before a wipe, 0 off |
| `zz_reserved` | 3 | 0 |
| `zz_salt` | 4 | The salt (2) |
| `zz_hash` | 16 | The hash (2) |

## 6. What a wipe does not do

- Copies elsewhere: a codeplug file, a backup of the flash, the Codeplug Editor's autosave in a browser, another
  radio. The keys are still there.
- A key already heard: traffic recorded off air can be decrypted with a copy of the key. Change the keys.
- A flash read before the wipe.
- Erase a retired NAND block (4.4).
- Wipe a radio whose keys aren't locked against PTT: the code typed with PTT held on FM is DTMF on air (3).

## 7. Tests

- `tests/host/zeroizeTest.c`: the hash (salt and UID change it, the code isn't stored), the key sequence (the right
  code, a wrong one, five digits, a key between, no code set), wrong codes counted only when locked; a whole wipe on a
  simulated flash in both scopes: the regions erased are exactly the expected ones (sector by sector), the
  calibration window, the ID database, the voice prompts and the custom data survive, no old settings record or
  journal copy keeps a key, the staging area is empty, the settings that remain are kept (keys only), the codeplug
  reads as the built-in set (keys + codeplug); RAM copies cleared, the hook called, a failed erase reported.
- `tests/host/keyFillTest.c`: key fill's hook with no session, a listening receiver and a receiver whose store job
  hasn't run: the session zeroed in place, nothing sent after, the port closed, the low power override given back, the
  block freed by the UI task's next tick, nothing stored; the randomness pool as at power on.
- `tests/host/nandSectorsTest.c`: a sector's old content is in the NAND after a rewrite and gone after the purge; the
  current content and the mount survive.
- `tests/host/uiFlowTest.c`: the settings rows, setting and clearing the code, the code on Home and while locked
  with the confirmation on and off, a wrong code, wrong codes while locked, Find and the VFO entry with other digits
  and with the code, the code with PTT held.
