<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The OpenDigitalRadio Authors -->

# Settings store and safe flash writes

How the radio keeps its own state across power cycles (`core/settings`) and how the codeplug's flash sectors are
rewritten without a power cut corrupting them (`core/storage`). Both live in the board's SPI NOR. Both are generic:
the board gives the addresses in its `hal_flash_map_t` (`board.flash->map`) and core code never names a radio.

## 1. Areas

The board's flash map (`hal/hal_flash.h`) names these areas. A board without a map runs everything from RAM: the
settings start from the defaults at every boot and nothing can be written.

| Field | DM-1701 | Use |
|---|---|---|
| `settings_addr` / `settings_size` | 0x011000, 32 KB (8 sectors) | the settings ring (2), at least 2 sectors |
| `journal_addr` | 0x019000, 8 KB (2 sectors) | the sector journal (4) |
| `legacy_eeprom` | 0x000000 | the legacy 64 KB "EEPROM" image (`legacy-cps-interop.md` 7) |
| `legacy_flash` | 0x020000 | the legacy 1 MB "flash" image |
| `legacy_end` | 0xE00000 | the end of the area a legacy CPS may write |
| `calibration_addr` / `_size` | 0x010000, 512 B | the calibration working copy (the CPS window, `cps-protocol.md` 2.4) |
| `native_addr` / `native_size` | 0xE00000, 1 MB | reserved for the native codeplug (`codeplug-native.md` 4) |
| `staging_addr` / `staging_size` | 0xF00000, 256 KB | a new codeplug image waits here, checked, before it replaces the stored one (the TFTP write, `tftp.md` 4); never written by the legacy CPS (above `legacy_end`) |

The DM-1701's settings ring and journal sit in the gap between the calibration sector and the legacy flash image
(`hardware-dm1701.md` 13.2). No legacy tool reads or writes there, and the CPS shim refuses to write there.

The core works in 4 KB erase units (`STORAGE_SECTOR`). A board whose flash erases in other units gets no writes.

## 2. Settings records

The settings area is a ring of sectors. Each sector holds 16 slots of 256 bytes, and each slot holds at most one
record (little endian):

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `0x5445534F` ("OSET") |
| 4 | 4 | sequence number, +1 per record (compared modulo 2^32) |
| 8 | 2 | layout (`SETTINGS_LAYOUT`, now 1) |
| 10 | 2 | payload length n (`sizeof(settings_t)`, at most 236) |
| 12 | n | payload: `settings_t` as it is in RAM (fixed-width fields, no padding) |
| 12 + n | 4 | CRC-32 (IEEE 802.3) of bytes 0 to 11 + n |
| 252 | 4 | commit word: programmed to 0 last |

**Load** (`settings_init`, once at boot, after `storage_init`): every slot is read. A slot is free when all its
bytes are 0xFF. It is valid when the magic, the length and the CRC match and the commit word is 0. Anything else
(a torn write) is counted in `settings.skipped` and ignored. The valid record with the highest sequence number is
the one in force. When its layout is not `SETTINGS_LAYOUT`, or no record is valid, the radio starts from the
defaults (`settings.load=defaults`). A shorter or longer payload of the same layout fills the fields both firmwares
know, and the rest keep their defaults (`upgraded`).

**Save**: the record goes to the next free slot after the newest one. When that sector is full, the next sector of
the ring is erased first. It holds only older records, because the newest is in the sector before it. The record
is programmed, then the commit word, then the slot is read back. A cut before the commit word leaves an unfinished
slot that the next load skips, so the previous record stays in force. A slot that doesn't read back is skipped and
the next one is tried, up to 4 slots. Erases go round the ring: the DM-1701's 8 sectors take 128 saves per pass,
so a sector is erased once every 128 saves.

**When**: `settings_begin()` returns the live copy under a short RAM-only lock (`storage_quick_lock`) and
`settings_end(now)` releases it. If anything changed, it notes the time. No flash operation happens inside, so the
radio task may change settings. The housekeeping task calls `settings_poll()` every 100 ms. It saves once the
changes have rested `SETTINGS_SAVE_DELAY_MS` (2 s), so turning the knob through 20 channels writes one record.
`settings_flush()` saves at once: at power-off and on the CPS's "save" (`C 6 0` and `C 6 2`).

**Who writes what**:

- The radio core (`radio_sm.c` `persist()`): VFO or channel mode, the zone, the channel number and its position,
  the VFO's frequency and settings, and the privacy key in use.
- The radio core, for `radio_set_settings()`: the `rs_*` fields (TOT, busy channel lockout, scan, beeps, VOX,
  battery saving, backlight times; `radio-features.md` 1). `rs_stored` 0 means the radio core's defaults.
- The radio core's data tick, for the DMR data service (`dmr-data.md`): the `ds_*` fields (ARS server, radio
  check reply, call alert ACK, LRRP reply). `ds_stored` 0 means the service's defaults.
- The UI: brightness.
- The CPS shim: the VFO when a CPS writes a changed VFO A record (`cps-protocol.md` 2.3).

At boot, `apply_prefs` and `restore_vfo` read the squelch, the master power, the VFO step, the own DMR ID override,
the radio name override and the privacy key. The radio name override is the talker alias text; empty means the
codeplug's callsign.

## 3. Fields and the append-only rule

`settings_t` (`core/settings/settings.h`) is the payload. The rules that keep old records readable:

1. **Fields are only appended.** A new field goes at the end, before nothing. Its default comes from
   `settings_defaults()`, which a record written by an older firmware doesn't overwrite.
2. **No padding.** Members are fixed width and ordered so the compiler adds none. `sizeof` is a multiple of 4, which
   a static assert checks, and must stay at most 236 bytes (one slot).
3. **Reserved bytes stay 0** in records we write. A later firmware may give them a meaning only if 0 means "as
   before".
4. **`SETTINGS_LAYOUT` is bumped only when a field moves or changes meaning.** Old records are then ignored and the
   radio starts from the defaults. Avoid this: append a new field instead.
5. 8-bit "not set" is `SETTINGS_UNSET8` (0xFF), meaning the radio's or the codeplug's default applies.

| Group | Fields |
|---|---|
| position | `vfo`, `squelch`, `zone_index`, `channel`, `zone_pos` (used when the channel number is gone after a codeplug write) |
| VFO | `vfo_rx_hz` (0 = never set), `vfo_tx_hz`, `vfo_step_hz`, `vfo_mode`, `vfo_bw`, `vfo_power`, `vfo_cc`, `vfo_ts`, tone types and values; `net_flags` sits in the VFO block's spare byte (was `reserved0`): bit 0 refuses TFTP codeplug writes (`tftp.md` 4), 0 = allowed as before |
| preferences | `brightness`, `backlight_s`, `beeps`, `hotspot_allowed`, `master_power`, `usb_mode` |
| identity | `dmr_id` (0 = the codeplug's), `radio_name[16]` ("" = the codeplug's callsign) |
| privacy keys | `privacy_key[4]`, `privacy_type[4]`, `privacy_active` (appended for dmr-voice) |
| radio core settings | `rs_tot_s rs_tot_warn_s rs_bcl rs_flags` (roger beep, TX inhibit, scan priority, scan beep, key beep, no talk permit tone: 0 = on, as before) `rs_scan_resume rs_scan_hold_s rs_beep_level rs_scan_delay_ms rs_scan_dwell_ms rs_vox rs_vox_level rs_vox_tail_ms rs_rx_save rs_stored rs_rx_save_idle_s rs_battery_off_s rs_backlight_dim_s rs_backlight_off_s` (appended for radio-features) |
| DMR data settings | `ds_ars_server ds_flags` (radio check reply, call alert ACK, LRRP reply, SMS format ETSI, radio check ETSI form, UDP compression off, UDP compression all) `ds_stored` (appended for dmr-data; the two ETSI bits for dmr-ext, 0 = as before), `ds_ip_cnf` (in the old reserved4: confirmed IP tries in bits 0-3, 0 = the default 4; bits 4-5 0 rules, 1 off, 2 all) and `ds_ip_rule[2]` (appended at the end for sap3: 0 unused, a radio ID, or 0x80000000 \| a UDP port); the two compression bits 0 = auto ([dmr-data.md](dmr-data.md) 11, 12) |
| UI settings | `rs_reply_s` (the reply window in s, 0..30; `SETTINGS_UNSET8` = the codeplug's hang times; a radio core setting), `mic_gain` (0..10, default 5 = unity; applied to DMR audio), `auto_lock_s` (0 = off), `long_press_ms`, `repeat_ms` (0 = the input layer's defaults), `gps`, `pos_format`, `pos_send_s` (stored for a GPS owner, act on nothing yet) (appended for glue) |
| DMR emergency | `em_dst em_mode em_flags` (target is a radio, ETSI alarm FID, no alert tone) `em_alarm_tries em_hot_mic_s em_stored` (appended for dmr-ext, `dmr-emergency.md` 5) |
| hotspot CW ID | `cw_wpm cw_tone_hz` (0 = 20 wpm / 800 Hz; appended for dmr-ext) |
| DMR roaming | `roam_rssi_dbm roam_hyst_db roam_probe roam_stored roam_check_s` (the radio core's roaming settings, taken when `roam_stored` is 1) and `roam_zones[10]` (bit z: zone z is a roaming zone, zones 0..79; written by the radio core on RREQ_ROAM) (appended for roaming, `dmr-roaming.md` 5.1) |
| more UI settings | `theme` (0 dark, 1 light, 2 high contrast), `ui_flags` (large text, hint row, message tone, talker alias shown, PTT when locked, hear all by default; default hint row, message tone, talker alias and PTT when locked on), `updown_home` (0 channel, 1 zone, 2 talk target), `low_batt_pct` (5..30, default 15; 0 = the radio core's voltage levels only), `auto_off_min` (0 = never), `reserved5` (appended for uifix; a record without them keeps these defaults) |
| roaming survey | `roam_beacon_s` (the repeaters' beacon interval, 0 = none, else 10..600, default 60), `roam_beacon_len_s` (1..10, default 4), `roam_survey` (0 off, 1 synced, 2 per site; default 1), `rpt_beacon` (our repeater roles' idle beacon, default 0), `reserved6[3]` (appended for roam2, `dmr-roaming.md` 5.1; a record without them keeps these defaults) |
| APRS | `aprs_flags aprs_flags2` (TX, SmartBeaconing, compressed, after PTT, boot, KISS, narrow, RX; iGate, iGate TX, messaging, UDT, to APRS-IS, DMR GPS Info) `aprs_region aprs_ssid aprs_freq_hz aprs_lat aprs_lon aprs_dmr_dst aprs_alt_m aprs_interval_s aprs_distance_m aprs_symbol[2] aprs_path aprs_pos_src aprs_level aprs_txdelay aprs_misc aprs_stored` and a reserved byte; taken when `aprs_stored` is 1 (appended for aprs, `aprs.md` 11) |
| APRS-IS client | `aprsis_ip` (the server's IPv4 address, 0 = off) `aprsis_port` (0 = 14580) `aprsis_range_km` (the filter `m/<km>`, 0 = none) (appended for tcp, `tcp-services.md` 7) |

All of these are still layout 1: they were appended before any firmware with the settings store shipped. The
payload is 228 bytes now (at most 236).

### 3.0 Who writes, and storage jobs

The settings are written through `settings_save` (debounced, written by `settings_poll` on the housekeeping task in
`app.c`). Other flash writes that a screen asks for (save to channel, the permanent talk target, factory reset) are
storage jobs: `storage_job_post(fn, ctx)` hands a function to the housekeeping task, which runs it from
`storage_job_poll` right after `settings_poll`. There is one slot: a second post while a job is pending is refused
(the UI says so), and `storage.job_busy` on the debug link shows a pending job. A job runs with no UI lock held, so
the UI keeps drawing during the flash write; the job reports back through `ui_notify`.

Factory reset (a job): `settings_defaults` into the live copy and save it at once, then `storage_native_erase`.
That erases the native area only when its first word is the native magic `STORAGE_NATIVE_MAGIC` (0x4E52444F, "ODRN")
or when it is blank already; anything else is reported as foreign and left alone. The legacy codeplug, the
calibration and the foreign-guarded regions (section 5) are never touched; a reset of those would need its own
second confirmation and is not offered.

### 3.1 Privacy key slots (the hook for DMR basic privacy)

The key store for `core/dmr/dmrPrivacy.c` (`dmr-voice.md` 5). Four slots, each a key value (`privacy_key[i]`,
u32: 16 bits are used today by Motorola and Anytone basic privacy, and the slot is wide enough for a 32-bit key
later) and a type (`privacy_type[i]`, `dmrPrivacyType_t`). `privacy_active` picks the slot in use radio-wide: 1 to
4, or 0 for privacy off. Turning privacy off keeps the key in its slot.

Today the radio core writes slot 1 when the key is set (the debug link's `RADIO` privacy op) and loads the
active slot at boot, checking it with `dmrPrivacyKeyIsValid`. A per-channel choice of key will be a slot number in
the native channel record (`codeplug-native.md` 3: 0 = the radio-wide choice, 1 to 4 = that slot). The keys
themselves stay in the settings and never go into a codeplug file, so a codeplug can be shared without its keys.

**Open: the keys are stored in plain text in the SPI NOR.** Anyone with the radio and a flash reader, or with the
CPS shim's area 1 reads, can see them. The settings area is outside what the legacy CPS reads by region, but area 1
reads any address. Options: refuse area 1 reads of the settings area, or obfuscate with a key from the MCU's UID.
Neither stops a determined reader, and basic privacy is weak anyway.

## 4. The sector journal

A codeplug edit, or a CPS commit, rewrites a whole 4 KB sector: erase, then program. A cut between the two would
leave the sector erased: channels, zones or contacts gone. The journal (`core/storage/storage.c`) makes the rewrite
repeatable. It uses two sectors at `journal_addr`:

| Sector | Content |
|---|---|
| J0 | header, 16 bytes: magic `0x4C4E4A4F` ("OJNL"), target address, CRC-32 of the copy, CRC-32 of the first 12 bytes; then a "done" byte at offset 16 (0xFF = pending, 0x00 = done) |
| J1 | the new content of the target sector |

`storage_sector_write(addr, data)`:

1. If the target already holds exactly `data`, nothing is done (`storage.sector_skips`).
2. Erase J1, program the copy, read it back and check its CRC.
3. Erase J0, program the header.
4. Erase the target, program `data`, read it back.
5. Program the done byte to 0.

`storage_init()` at boot, before the settings and the codeplug are read: a J0 with a valid header and a pending
done byte means a cut after step 3. If J1's CRC matches, steps 4 and 5 run again (`storage.replays`). A cut before
the header is complete leaves the target untouched, because the header is the commit point. A cut during the replay
is replayed again at the next boot.

`storage_write(addr, buf, len)` writes any range. For each sector it touches, it reads the sector into the shared
4 KB buffer, merges the bytes in and rewrites the sector as above. The buffer is shared with the CPS shim's pending
sector (`storage_buffer_acquire`), so an edit while a CPS write is going on fails cleanly instead of waiting.

Wear: J0 and J1 are erased once per sector rewrite. A full qdmr write is about 46 sectors, so the journal sectors
take 46 erases per write. At 100,000 cycles, that is about 2,000 full codeplug writes. Spreading the journal over a
ring is possible later. The map has room after 0x01B000.

**Locks**: `storage_lock()` is a recursive FreeRTOS mutex around every flash sequence of the core: the codeplug
calls, a settings save, a CPS commit. `storage_quick_lock()` guards only RAM: the live settings copy. The radio task
takes only the quick lock on its own, so it never waits for an erase. A codeplug read from the radio task (a channel
change) does take `storage_lock` and may wait out a sector rewrite (about 100 ms) during a CPS write.

## 5. Foreign region guard

The flash of a radio that ran other firmware may hold that firmware's data where our settings ring and journal go
(on the DM-1701, 0x011000-0x01AFFF has never been checked on a used radio). Before anything is erased or programmed
there, `storage_init()` classifies the region:

| Part | Ours | Anything else |
|---|---|---|
| settings slot | all 0xFF; a valid record; our magic with a torn rest; a save cut inside the magic (the bytes so far on their way to "OSET", the rest 0xFF) | other bytes |
| J0 | all 0xFF; our header (magic "OJNL"), nothing after the done byte | other bytes |
| J1 | all 0xFF, or anything once J0 holds our header | non-blank while J0 is blank |

**Foreign** = some part holds other bytes **and** no valid record of ours (a settings record or a J0 header with
a good CRC) exists anywhere in the region. Then:

- `storage_init()` returns false, `storage_ready()` is false, nothing is replayed.
- Every erase and program in the region is refused. `storage_sector_write` and `storage_write` fail
  (`storage.foreign_refusals`), so codeplug edits and CPS commits fail too (the journal is the region). The CPS
  shim refuses `X 1` (`cps-protocol.md` 2.3).
- `settings_init()` returns `SETTINGS_FOREIGN` and the radio runs on the defaults in RAM; changes are kept in RAM
  and not saved.
- The debug link's view shows `settings.load=foreign`, `storage.region=foreign`, `storage.journal=foreign` and
  `storage.foreign_slots` (slots and journal sectors with other bytes).

Junk beside a valid record of ours (a torn erase of our own ring) is not foreign: the slots are skipped and
reused once their sector comes round.

**Ownership mark.** When the region is ours but holds no valid record (a blank flash), `storage_init()` programs
one valid settings slot of layout 0 and sequence 0 (no payload) in the first free slot. The settings load their
defaults past it (no settings layout is 0) and the first real save gets sequence 1. From then on a valid record
always exists, because saves never erase the sector with the newest record, so a power cut inside a journal
rewrite (J0 erased, J1 full) can't make the region look foreign at the next boot.

**Claim.** Only through the debug link, behind the chip UID token like REBOOT (`debuglink.md` 3.13):

1. `odr_debug.py storage claim` reads the settings ring, the journal and the calibration copy through the debug
   link's read-only SPI flash window (twice, compared) into `the external reference folder` (refused inside any git
   work tree), then sends the claim with the token.
2. The firmware notes the request (`storage.claim_pending`). The housekeeping task (`settings_poll`) erases every
   settings sector and both journal sectors, checks they read blank, marks the region ours and writes the
   ownership mark, then saves the settings the radio is running with.
3. The tool polls until `storage.region=ours`.

`odr_debug.py storage dump` saves the same files without claiming; `storage info` shows the state and addresses.

## 6. Testing

| Test | What |
|---|---|
| `tests/host/settingsTest.c` | load, save, debounce, ring wrap, layout change, older and newer payload sizes; a power cut at every flash operation of a save (the previous or the new record is in force, never neither) |
| the journal part of `settingsTest.c` | a power cut at every one of the 701 flash operations of a sequence of sector rewrites, then a boot: every sector holds either its old or its new content, and the replayed ones the new |
| `tests/host/sim/sim_flash.c` | the simulated NOR: program only clears bits (violations counted), erase per 4 KB, a power cut after N operations (a torn program or erase leaves random bits) |
| `tests/host/radioCoreTest.c` `test_persist` | the radio core saves the zone, the channel and the VFO and comes back to them after a reboot |
| `tests/hil/test_cps.py` `test_settings_saved` | on the radio: a channel change is saved within 2 s (the debug link's `storage` view section, `debuglink.md` 3.7) |
| `settingsTest.c` `test_foreign` | another firmware's bytes in the ring / J0 / J1: foreign, no flash operation at all, writes refused, RAM defaults, the read window's limits; claim erases and saves; junk beside our record, our J0 header beside junk, a cut inside a journal rewrite after the mark, a cut inside the first mark: ours |
| `cpsTest.c` `test_foreign` | a foreign region: reads answered, `X 1` and codeplug edits refused, flash unchanged; after a claim, CPS writes work |
| `tests/hil/test_cps.py` `test_storage_region` | on the radio: the region state matches the view, reads inside the window work and outside it are refused (skips with instructions when foreign) |
| `tests/hil/test_cps.py` `test_settings_survive_restart` | opt-in `--allow-reboot`: the channel position survives a restart |
