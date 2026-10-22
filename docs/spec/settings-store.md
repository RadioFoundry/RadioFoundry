<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The OpenDigitalRadio Authors -->

# Settings store and safe flash writes

How the radio keeps its own state across power cycles (`core/settings`) and how the codeplug's flash sectors are
rewritten without a power cut corrupting them (`core/storage`). Both live in the board's SPI NOR. Both are generic:
the board gives the addresses in its `hal_flash_map_t` (`board.flash->map`) and core code never names a radio.

## 1. Areas

The board's flash map (`hal/hal_flash.h`) names these areas. A board without a map runs everything from RAM: the
settings start from the defaults at every boot and nothing can be written.

| Field | Use |
|---|---|
| `settings_addr` / `settings_size` | the settings ring (2), at least 2 sectors |
| `journal_addr` | the sector journal (4), 2 sectors |
| `openfw_eeprom` | the open-firmware 64 KB "EEPROM" image (`open-firmware-cps.md` 7) |
| `openfw_flash` | the open-firmware 1 MB "flash" image |
| `openfw_end` | the end of the area an open-firmware CPS may write |
| `calibration_addr` / `_size` | the calibration working copy (the CPS window, `cps-protocol.md` 2.4); 0 when the board has none |
| `native_addr` / `native_size` | reserved for the native codeplug (`codeplug-native.md` 4) |
| `staging_addr` / `staging_size` | a new codeplug image waits here, checked, before it replaces the stored one (the TFTP write, `tftp.md` 4); never written by the open-firmware CPS (above `openfw_end`); 0 when the board has none |

The settings ring and journal go where no open-firmware tool reads or writes, and the CPS shim refuses to write there. The
boards' maps:

| Field | DM-1701, MD-380 (16 MB part) | DM-32UV | DB25-D PRO (NAND, 2.375 MB logical) |
|---|---|---|---|
| `settings_addr` / `settings_size` | 0x011000, 32 KB (8 sectors) | 0x711000, 32 KB (8 sectors) | 0x011000, 32 KB (8 sectors) |
| `journal_addr` | 0x019000 | 0x719000 | 0x019000 |
| `openfw_eeprom` | 0x000000 | 0x700000 | 0x000000 |
| `openfw_flash` | 0x020000 | 0x720000 | 0x020000 |
| `openfw_end` | 0xE00000 | 0xF00000 | 0x120000 |
| `calibration_addr` / `_size` | DM-1701 0x010000, 512 B; MD-380 none | none | none |
| `native_addr` / `native_size` | 0xE00000, 1 MB | 0xF00000, 1 MB | 0x120000, 1 MB |
| `staging_addr` / `staging_size` | 0xF00000, 256 KB | none | 0x220000, 256 KB |
| Details | `hardware-dm1701.md` 13.2, `hardware-md380.md` 12.2 | `hardware-dm32.md` 10.1, 17 | `hardware-db25d.md` 6.6 |

The GD-77 uses the DM-1701's layout moved up by 1 MB, above the radio's own data, with no calibration window and a
native and staging area only on 4 MB parts ([hardware-gd77.md](hardware-gd77.md) 6.2).

The core works in 4 KB erase units (`STORAGE_SECTOR`). A board whose flash erases in other units gets no writes. A
NAND board puts `drivers/flash/nand_sectors` in between, which gives NOR behavior in 4 KB sectors
([nand-sectors.md](nand-sectors.md)).

## 2. Settings records

The settings area is a ring of sectors. Each sector holds 16 slots of 256 bytes, and each slot holds at most one
record (little endian):

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `0x5445534F` ("OSET") |
| 4 | 4 | sequence number, +1 per record (compared modulo 2^32) |
| 8 | 2 | bits 0-11: layout (`SETTINGS_LAYOUT`, now 1); bits 12-15: the page (2.1) |
| 10 | 2 | payload length n (at most 236) |
| 12 | n | payload: the page's bytes of `settings_t` as it is in RAM (fixed-width fields, no padding) |
| 12 + n | 4 | CRC-32 (IEEE 802.3) of bytes 0 to 11 + n |
| 252 | 4 | commit word: programmed to 0 last |

**Load** (`settings_init`, once at boot, after `storage_init`): every slot is read. A slot is free when all its
bytes are 0xFF. It is valid when the magic, the length and the CRC match and the commit word is 0. Anything else
(a torn write) is counted in `settings.skipped` and ignored. The valid page 0 record with the highest sequence
number is in force (2.1 has the other pages). When its layout is not `SETTINGS_LAYOUT`, or no page 0 record is
valid, the radio starts from the defaults (`settings.load=defaults`). A shorter or longer payload of the same layout
fills the fields both firmwares know, and the rest keep their defaults (`upgraded`).

**Save**: the records go to the next free slots after the newest record. When a sector is full, the next sector of
the ring is erased first. Each record is programmed, then its commit word, then the slot is read back. A cut before
page 0's commit word leaves the previous set in force. A slot that doesn't read back is skipped and the next one is
tried, up to 4 slots per record. No save erases a sector that holds a page of the set in force, or a page this save
already wrote: the walk passes over such a sector to the next one. When every sector is protected that way the save
fails, and the set in force stays. Erases go round the ring: 8 sectors (every board's ring today) hold 128 records,
so with two pages a sector is erased once every 64 saves.

### 2.1 Pages

`settings_t` is longer than one slot's payload, so it is stored as pages: page k holds bytes `236 * k` on.
`SETTINGS_PAGES` is the number its size needs (2 now, 264 bytes); the page field allows 16 (3776 bytes). Each page is
a record with its own length, so `settings_t` grows by appending fields, and a short page fills the fields it has.

| Rule | Why |
|---|---|
| A save writes every page, the last first and page 0 last, each with the next sequence number | Page 0's commit makes the set the one in force. A cut before it leaves the previous set |
| Load: the newest page 0, then for each other page k the newest page k older than that page 0 | A page newer than page 0 belongs to a save that was cut |
| `loaded` only when every page is exact (page 0's sequence minus k) and full length, else `upgraded` | A firmware with one page (before pages) writes page 0 alone: the other pages come from the set before it, or their defaults |
| The next save numbers past the newest record of any page | Sequence numbers stay unique |
| No save erases a sector holding a page of the set in force (up to 32 sectors are tracked) | A page that an older firmware left behind stays readable until the next full save |

Firmware from before pages reads a page 1 record as an ordinary record of layout 0x1001, which it doesn't know, and
never sees it as newest: page 0 is always written last.

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
  check reply, call alert ACK, LRRP reply, the formats). `ds_stored` 0 means the service's defaults.
- The UI and the debug link's radio op 27: `usb_mode`, `net_flags`, `net_link_ip` (`usb-network.md` 5.4); the host
  link reads them at start and on `hostlink_settings_changed()`.
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
   a static assert checks. Page 1 starts at byte 236 (a field there must not straddle the page boundary; a static
   assert checks the first page 1 field's offset).
3. **Reserved bytes stay 0** in records we write. A later firmware may give them a meaning only if 0 means "as
   before".
4. **`SETTINGS_LAYOUT` is bumped only when a field moves or changes meaning.** Old records are then ignored and the
   radio starts from the defaults. Avoid this: append a new field instead.
5. 8-bit "not set" is `SETTINGS_UNSET8` (0xFF), meaning the radio's or the codeplug's default applies.

| Group | Fields |
|---|---|
| position | `vfo`, `squelch`, `zone_index`, `channel`, `zone_pos` (used when the channel number is gone after a codeplug write) |
| VFO | `vfo_rx_hz` (0 = never set), `vfo_tx_hz`, `vfo_step_hz` (0 = auto: the width's step), `vfo_mode` (bits 0-3: FM or DMR; a record from before the DMR VFO holds FM, and a build without the DMR VFO reads DMR as FM), `vfo_bw` (bits 0-3), `vfo_power`, `vfo_cc` (bits 0-3, the color code), `vfo_ts` (bits 0-1, the slot), tone types and values; the DMR VFO's talk target (a 24-bit ID and a private call flag) is packed into bytes that were reserved or hold a small number, so the record stays 236 bytes and an old record reads as no target (ID 0, talkgroup 9): ID bits 0-7 in `vfo_talk_lo` (the old `reserved1`'s second byte; `accent` has the first), bits 8-11 in `vfo_mode` bits 4-7, bits 12-15 in `vfo_bw` bits 4-7, bits 16-19 in `vfo_cc` bits 4-7, bits 20-23 in `vfo_ts` bits 2-5, private call in `vfo_ts` bit 7 (always through `settings_vfo_mode` / `_bw` / `_cc` / `_ts` / `_talk_id` / `_talk_private` and the setters in `settings.h`; appended for the DMR VFO); `net_flags` sits in the VFO block's spare byte (was `reserved0`; 0 = as before): bit 0 refuses TFTP codeplug writes (`tftp.md` 4, appended for mdnstftp), bit 1 control station passthrough, bit 2 the fixed link (`usb-network.md` 5.4, appended for dispatch) |
| preferences | `brightness`, `backlight_s`, `beeps` (0 or 1), `hotspot_allowed`, `master_power`, `usb_mode` (the USB personality at power-on: 0 serial, 1 network; `usb-network.md` 1), `accent` (in the old `reserved1`'s first byte: bits 0-2 the highlight color, 0 the theme's own, 1 red, 2 orange, 3 yellow, 4 teal, 5 light blue, 6 purple, 7 pink; bit 3 set: the status strip not filled; bits 4-5 the crash report mode; bits 6-7 the vocoder's mic options, bit 6 noise reduction off and bit 7 AGC on (`dmr-voice.md` 6), so a record from before them reads as noise reduction on and AGC off; 0 = the defaults, the theme's highlight and the strip filled with it; `ui.md` 8) |
| identity | `dmr_id` (0 = the codeplug's), `radio_name[16]` ("" = the codeplug's callsign) |
| privacy keys | `privacy_key[4]`, `privacy_type[4]`, `privacy_active` (appended for dmr-voice) |
| radio core settings | `rs_tot_s rs_tot_warn_s rs_bcl rs_flags` (roger beep, TX inhibit, scan priority, scan beep, key beep, no talk permit tone: 0 = on, as before) `rs_scan_resume rs_scan_hold_s rs_beep_level rs_scan_delay_ms rs_scan_dwell_ms rs_vox rs_vox_level rs_vox_tail_ms rs_rx_save rs_stored rs_rx_save_idle_s rs_battery_off_s rs_backlight_dim_s rs_backlight_off_s` (appended for radio-features) |
| DMR data settings | `ds_ars_server ds_flags` (radio check reply, call alert ACK, LRRP reply, SMS format ETSI short data, radio check ETSI form, UDP compression off, UDP compression all, SMS format ETSI IP text) `ds_stored` (appended for dmr-data; the ETSI bits for dmr-ext and dispatch, 0 = as before: a record with neither SMS format bit sends TMS, while the default without a record is ETSI IP text, `dmr-data.md` 8), `ds_ip_cnf` (in the old reserved4: confirmed IP tries in bits 0-3, 0 = the default 4; bits 4-5 0 rules, 1 off, 2 all) and `ds_ip_rule[2]` (appended at the end for sap3: 0 unused, a radio ID, or 0x80000000 \| a UDP port); the two compression bits 0 = auto ([dmr-data.md](dmr-data.md) 11, 12) |
| UI settings | `rs_reply_s` (the reply window in s, 0..30; `SETTINGS_UNSET8` = the codeplug's hang times; a radio core setting), `mic_gain` (0..10, default 5 = the board's levels; `radio_set_mic_gain`, radio-features.md 3.6), `auto_lock_s` (0 = off), `long_press_ms`, `repeat_ms` (0 = the input layer's defaults), `gps` (Settings > Location > GPS: `ui_svc_apply` hands it to `board_gps_enable` and `aprs_gps_enable`, at start-up and on each change, see aprs.md 4), `pos_format`, `pos_send_s` (stored for a GPS owner, act on nothing yet) (appended for glue) |
| DMR emergency | `em_dst em_mode em_flags` (target is a radio, ETSI alarm FID, no alert tone) `em_alarm_tries em_hot_mic_s em_stored` (appended for dmr-ext, `dmr-emergency.md` 5) |
| hotspot CW ID | `cw_wpm cw_tone_hz` (0 = 20 wpm / 800 Hz; appended for dmr-ext) |
| DMR roaming | `roam_rssi_dbm roam_hyst_db roam_probe roam_stored roam_check_s` (the radio core's roaming settings, taken when `roam_stored` is 1) and `roam_zones[10]` (bit z: zone z is a roaming zone, zones 0..79; written by the radio core on RREQ_ROAM) (appended for roaming, `dmr-roaming.md` 5.1) |
| more UI settings | `theme` (0 dark, 1 light, 2 high contrast), `ui_flags` (large text, hint row, message tone, talker alias shown, PTT when locked, hear all by default, 0x40 vibrate off, 0x80 `SETTINGS_UI_DUAL_TALK_BACK` dual watch talk back; default hint row, message tone, talker alias and PTT when locked on, and the vibrate-off and talk back bits clear, so vibrate is on and talk back is off), `updown_home` (0 channel, 1 zone, 2 talk target), `low_batt_pct` (5..30, default 15; 0 = the radio core's voltage levels only), `auto_off_min` (0 = never) and a reserved u16, now `aprsis_port` (appended for uifix; a record without them keeps these defaults) |
| roaming survey | `roam_beacon_s` (the repeaters' beacon interval, 0 = none, else 10..600, default 60), `roam_beacon_len_s` (1..10, default 4), `roam_survey` (0 off, 1 synced, 2 per site; default 1), `rpt_beacon` (our repeater roles' idle beacon, default 0), `aprs_power` (APRS's RF power, `aprs.md` 11; 0 = the radio's current level, which a record from before it reads as), `aprs_cp_path` (the codeplug APRS path last taken into `aprs_path`,
`aprs.md` 11; 0 = none yet), `volume` (the speaker volume of a radio without a knob; in the old `reserved6`, so a record from before it reads as the default. 0 = the default step; bit 7 set = `0x80 | (step + 1)` for step 0..16 (0 = mute); 1..11 = step + 1 of the first version's 0..10 range, read as step 0..10 scaled to 0..16 (round(step * 16 / 10): the default 5 is 8, 10 is 16) and rewritten in the new form at the next save; `radio-features.md` 4.2) |
| APRS | `aprs_flags aprs_flags2` (TX, SmartBeaconing, compressed, after PTT, boot, KISS, narrow, RX; iGate, iGate TX, messaging, UDT, to APRS-IS, DMR GPS Info) `aprs_region aprs_ssid aprs_freq_hz aprs_lat aprs_lon aprs_dmr_dst aprs_alt_m aprs_interval_s aprs_distance_m aprs_symbol[2] aprs_path aprs_pos_src aprs_level aprs_txdelay aprs_misc aprs_stored` and a reserved u16, now `snmp_rf_gap_s`; taken when `aprs_stored` is 1 (appended for aprs, `aprs.md` 11) |
| APRS-IS client | `aprsis_ip` (the server's IPv4 address, 0 = off; appended for tcp), `aprsis_port` (0 = 14580; in the old `reserved5`), `aprsis_range_km` (the filter `m/<km>`, 0 = none; in the old `reserved3`) (`tcp-services.md` 7) |
| control station | `net_link_ip` (the radio's fixed link address, 0 = 192.168.10.1; in force with `net_flags` bit 2), 0 = off, the link by DMR ID (appended for dispatch after `aprsis_ip`, `usb-network.md` 5.4) |
| SNMP agent | `snmp_read_hash snmp_write_hash` (FNV-1a hashes of the communities, 0 = "public" / none; appended for snmp after `net_link_ip`), `snmp_rf_manager[3]` (the RF manager's DMR ID, little endian, 0 = none; in the old `reserved2[3]`), `snmp_flags` (in the old `reserved4`), `snmp_rf_gap_s` (0 = 60; in the APRS block's old `reserved7`); all zero = the defaults (`snmp.md` 6) |
| dual watch | `ui_flags` bit 7 (0x80) is talk back (Settings > Talk > Dual watch talk back; off = the bit clear, as in a record from before). Page 1: `dual_b`, the last B: 0 none, 1..1024 a codeplug channel number, 1025 the first VFO, 1026 the second. The UI looks the channel number up again at power-on, in A's zone first, then in the first zone that holds it; if it is gone, the picker opens ([dual-watch.md](../ux/dual-watch.md) 5). `dual_state` bit 0 (`SETTINGS_DUAL_ON`) says the watch was on: the UI starts it again at power-on, with `dual_b` as the channel that wasn't selected and bit 1 (`SETTINGS_DUAL_B_SEL`) set when B was the selected one |
| second VFO | Page 1: `vfo_index` (the VFO in use at power off, 0 or 1) and `vfo2_rx_hz` (0 = never set), `vfo2_tx_hz`, `vfo2_talk` (the DMR talk target: ID in bits 0-23, bit 31 private call), tone values and types, `vfo2_mode`, `vfo2_bw`, `vfo2_power` (`SETTINGS_UNSET8` = default), `vfo2_cc`, `vfo2_ts`: the first VFO's fields, unpacked. Dual watch gives each channel its own VFO ([radio-features.md](radio-features.md) 2.1). A record without page 1 reads as the first VFO in use and no second VFO frequency |
| dual watch wait | Page 1: `rs_dual_delay`, `radio_settings_t.dual_delay_ms` as n - 1 steps of 100 ms (1 = no wait, 11 = 1 s); 0 (never set, or no page 1) = the default, 1 s. Settings > Talk > Dual watch delay |
| crash report | packed into spare bits, so an older record reads as off with no target: the mode (0 off, 1 ask, 2 send at once) in `accent` bits 4-5; the target's codeplug contact number (0 none, else 1..1024, 11 bits) bits 0-5 in `theme` bits 2-7 and bits 6-10 in `updown_home` bits 2-6. `theme` and `updown_home` keep their value in bits 0-1; read and write them through `settings_theme` / `settings_updown_home` and `settings_crash_*` (`settings.h`). Firmware from before reads a `theme` or `updown_home` with these bits as out of range: the dark theme and Up/Down = channel ([crash-log.md](crash-log.md) 5.2) |

All of these are still layout 1: they were appended before any firmware with the settings store shipped. Page 0
is full: its later fields went into reserved bytes where 0 already meant
"as before" (`net_flags`, `snmp_rf_manager[3]`, `aprsis_range_km`, `snmp_flags`, `aprsis_port`, `snmp_rf_gap_s`, `aprs_power`, `aprs_cp_path`, `volume`, `vfo_talk_lo`), or the new defaults (`accent`: the filled status strip, a new look on purpose). No reserved byte is left. One-byte
fields that hold small numbers (`hotspot_allowed`, `usb_mode`) still have free upper bits (`theme` and `updown_home` hold the crash report's target; `updown_home` bit 7 is free;
`accent` bits 6-7 are free); `vfo_power` does not (its "not set" is 0xFF). New settings are appended to page 1 as
plain fields; a page 2 follows when page 1 is full (2.1).

### 3.0 Who writes, and storage jobs

The settings are written through `settings_save` (debounced, written by `settings_poll` on the housekeeping task in
`app.c`). Other flash writes that a screen asks for (save to channel, the permanent talk target, factory reset) are
storage jobs: `storage_job_post(fn, ctx)` hands a function to the housekeeping task, which runs it from
`storage_job_poll` right after `settings_poll`. There is one slot: a second post while a job is pending is refused
(the UI says so), and `storage.job_busy` on the debug link shows a pending job. A job runs with no UI lock held, so
the UI keeps drawing during the flash write; the job reports back through `ui_notify`.

Factory reset (a job): `settings_defaults` into the live copy and save it at once, then `storage_native_erase`.
That erases the native area only when its first word is the native magic `STORAGE_NATIVE_MAGIC` (0x4E52444F, "ODRN")
or when it is blank already; anything else is reported as foreign and left alone. The open-firmware codeplug, the
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
CPS shim's area 1 reads, can see them. The settings area is outside what the open-firmware CPS reads by region, but area 1
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

`storage_write(addr, buf, len)` writes any range (the codeplug editor's records, names and bitmaps). Open-firmware
channel, zone and contact records don't divide 4 KB, so some cross a sector end; two separate sector rewrites would
leave such a record half old, half new after a cut between them (a channel with the new frequency and the old
timeslot). So a write of up to 4072 bytes, which touches one or two sectors, is one transaction:

| J0 offset | Content |
|---|---|
| 0 | magic `0x324E4A4F` ("OJN2"), then the address, the length and the CRC-32 of bytes 0 to 11 and the data |
| 16 | marks, 0xFF = not yet: first sector staged, first sector done, second sector staged, second sector done |
| 20 | all done |
| 24 | the data |

1. If the flash already holds exactly the data, nothing is done.
2. Erase J0, program the header and the data, read them back (the commit point).
3. For each sector: J1 erased and programmed with the sector merged with the data, "staged"; the sector erased and
   programmed from J1, read back, "done".
4. Program "all done".

Replay: a header with a good CRC and "all done" pending finishes the sectors not done. A sector not staged has not
been erased yet, so it is merged with the data from J0 again; a staged one is copied from J1. A longer write
(none of the core's callers makes one) goes sector by sector through `storage_sector_write`. The bytes are merged
in the shared 4 KB buffer, which is shared with the CPS shim's pending sector (`storage_buffer_acquire`), so an edit
while a CPS write is going on fails cleanly instead of waiting.

A flash that is copy on write underneath offers `hal_flash_t.replace` (the DB25-D's NAND, `nand-sectors.md` 5): the
sector rewrite then calls it and checks the result. There is no journal on such a flash (J0 and J1 stay blank), because
the replace is one atomic record, and the replay at boot finds nothing to finish. The patch transaction of
`storage_write` (up to 4 KB across one or two sectors) still uses the journal.

Wear: J0 and J1 are erased once per sector rewrite. A full qdmr write is about 46 sectors, so the journal sectors
take 46 erases per write. At 100,000 cycles, that is about 2,000 full codeplug writes. Spreading the journal over a
ring is possible later. The map has room after 0x01B000.

**Locks**: `storage_lock()` is a recursive FreeRTOS mutex around every flash sequence of the core: the codeplug
calls, a settings save, a CPS commit. `storage_quick_lock()` guards only RAM: the live settings copy. The radio task
takes only the quick lock on its own, so it never waits for an erase. A codeplug read from the radio task (a channel
change) does take `storage_lock` and may wait out a sector rewrite (about 100 ms) during a CPS write.

## 5. Foreign region guard

The flash of a radio that ran other firmware may hold that firmware's data where our settings ring and journal go
(on the DM-1701, 0x011000-0x01AFFF has never been checked on a used radio; on the DM-32UV the region is OpenDM32's
data area, `hardware-dm32.md` 10.1). Before anything is erased or programmed
there, `storage_init()` classifies the region:

| Part | Ours | Anything else |
|---|---|---|
| settings slot | all 0xFF; a valid record; our magic with a torn rest; a save cut inside the magic (the bytes so far on their way to "OSET", the rest 0xFF) | other bytes |
| J0 | all 0xFF; our header (magic "OJNL"), nothing after the done byte; our patch header ("OJN2"), nothing after its data | other bytes |
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

**Claim.** Only through the debug link, behind the chip UID token like REBOOT (`debuglink.md` 3.13). Two clients
send it:

- The web radio programming utility, right after a full backup (the backup holds the whole SPI flash, the region
  included), when `STORAGE INFO` reports the region foreign. The Codeplug Editor checks before every write and, on a
  foreign region, explains and claims only when the user confirms. Both poll INFO until the region is ours (20 s
  limit).
- `odr_debug.py storage claim`:


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
| `tests/host/settingsTest.c` | load, save, debounce, ring wrap, layout change, older and newer payload sizes; pages (a page 0 alone from an older firmware, a page newer than page 0, a save that passes over a sector holding an old page); a power cut at every flash operation of a save (the previous or the new set is in force, never neither, and never pages of two saves) |
| the journal part of `settingsTest.c` | a power cut at every one of the 701 flash operations of a sequence of sector rewrites, then a boot: every sector holds either its old or its new content, and the replayed ones the new |
| `tests/host/sim/sim_flash.c` | the simulated NOR: program only clears bits (violations counted), erase per 4 KB, a power cut after N operations (a torn program or erase leaves random bits) |
| `tests/host/radioCoreTest.c` `test_persist` | the radio core saves the zone, the channel and the VFO and comes back to them after a reboot |
| `tests/hil/test_cps.py` `test_settings_saved` | on the radio: a channel change is saved within 2 s (the debug link's `storage` view section, `debuglink.md` 3.7) |
| `settingsTest.c` `test_foreign` | another firmware's bytes in the ring / J0 / J1: foreign, no flash operation at all, writes refused, RAM defaults, the read window's limits; claim erases and saves; junk beside our record, our J0 header beside junk, a cut inside a journal rewrite after the mark, a cut inside the first mark: ours |
| `cpsTest.c` `test_foreign` | a foreign region: reads answered, `X 1` and codeplug edits refused, flash unchanged; after a claim, CPS writes work |
| `tests/hil/test_cps.py` `test_storage_region` | on the radio: the region state matches the view, reads inside the window work and outside it are refused (skips with instructions when foreign) |
| `tests/hil/test_cps.py` `test_settings_survive_restart` | opt-in `--allow-reboot`: the channel position survives a restart |
