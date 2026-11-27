<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Radio tuning

The owner's own trims on top of the factory calibration, which they never change:

- each power level's PA drive, measured with a wattmeter on a dummy load;
- each mode's transmit deviation;
- each mode's carrier frequency (a DC offset in one mode's modulator shows as a carrier offset in that mode only);
- the power levels' names;
- the battery reading's gain, and the voltages the charge percentage runs between.

The reference oscillator trim is the existing Freq trim setting (`settings_t.ref_trim_ppb`,
[radio-features.md](radio-features.md) 6.1). Radio tuning reuses that setting and adds nothing for the oscillator.

`CONFIG_RADIO_TUNE` builds the feature (on with the debug link). The radio keeps the trims in one RAM table and saves
that table as one record in the settings ring (`core/radio/radio_tune.c`, [settings-store.md](settings-store.md) 2.2).
The host side is the site's Radio Tuning page at Tools > Radio tuning
([16-battery-and-power.md](../user/16-battery-and-power.md)). Settings > Advanced > Clear tuning on the radio removes
the trims.

## 1. What a board provides

| Trim | Needs | Without it |
|---|---|---|
| Drive | `hal_radio_t.tx_drive_level`, and the board's drive passed through `radio_tune_drive(hz, level, drive)` | no drive trims |
| Deviation | `board_t.tune` bit `1 << mode`, and the board scales that mode's deviation by `radio_tune_dev(mode, hz)` | no deviation trims for that mode |
| Frequency | nothing: the radio core adds `radio_tune_freq(mode, hz)` to the transmit frequency at key-up | |
| Names | nothing | |
| Battery | a battery (`board_has_battery()`) and `hal_power_t.battery_mv`: the radio core applies the trims ([radio-features.md](radio-features.md) 5.1) | no battery trims |
| Storage | a settings ring (`hal_flash_map_t.settings_addr`, [settings-store.md](settings-store.md) 1) | trims work live only, nothing is stored |

The record is outside the codeplug and the calibration. Codeplug reads, writes and shares never include the record,
and a factory reset keeps the record. Which radios have which trims is in [feature-matrix.md](../feature-matrix.md).

## 2. Trims

A trim is a value at a frequency for one power level or one mode (`radio_mode_t`). The value at a transmit frequency
is interpolated between the trims of that level or mode in the same band, and held flat past the outer trims. A band
without a trim for a level or mode leaves it at the factory value.

| Kind | Value | Clamp where used |
|---|---|---|
| Drive | offset in the board's drive units | between level 0's factory drive and the top level's at that frequency |
| Deviation | permille of the board's deviation | ±250 (`RADIO_TUNE_DEV_MAX`) |
| Frequency | Hz added to the transmit frequency | ±2000 Hz (`RADIO_TUNE_FREQ_MAX`), and never past the edge of the channel's transmit band |
| Battery gain | permille added to the battery reading | ±150 (`RADIO_TUNE_GAIN_MAX`) |
| Battery empty, full | mV added to the board's empty or full voltage | ±1000 mV (`RADIO_TUNE_BATT_MAX`) |

The drive clamp keeps the top level at or under the radio's rating and the lowest level where the PA keeps control.
Every value is clamped where it is used, so neither a stored record nor a live word can take the radio past these
limits.

A drive trim keeps the factory drive it was measured against. The trim loads only while `tx_drive_level` still
returns that drive. New drive tables, a new calibration, or levels added or removed make the trim stale, and a stale
trim is not loaded. A level name replaces the board's name for one level in one band in the status line, the menus,
the debug link's `power_name` and the web and SNMP services (`radio_tune_name`). The view's `power_names` stays the
board's names.

Battery trims are one value each, at no frequency. The gain corrects the reading itself: `battery_mv`, the low and
critical thresholds, the transmit refusal and the power-off all use the corrected value. A gain set from a wrong meter
reading moves these protections too, so the clamp is ±15 %. Empty and full move only the charge percentage
(and the Low battery warning, which follows the percentage). When the moved voltages are less than 400 mV apart
(`BATT_SPAN_MV`), the radio keeps the board's own.

A frequency trim stops at the edge of the transmit band. A channel 1 kHz inside the top edge with a +1500 Hz trim
transmits on the edge, and a channel on the edge gets no upward trim.

The frequency trim applies to every transmission the radio core keys, including DMR bursts keyed later in a call. The
trim is the one of the mode the board transmits in: P25 and NXDN use the DMR trim, since the board keys them in its
DMR mode. The
deviation trim applies at key-up in the modes the board marks in `board_t.tune`.

## 3. The record

The record is one slot of the settings ring: page field 15, layout 1 ([settings-store.md](settings-store.md) 2.2). Its
payload, little endian:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | owner: CRC-32 of `board.name`, continued over the chip's unique ID (the name alone on a board without one) |
| 4 | 8 × n | entries (below), at most 26 (`RADIO_TUNE_ENTRIES`, the RAM table's room); a slot holds 29 |

An entry's first byte is the kind in the high nibble and the level or mode in the low nibble:

| Kind | Low nibble | Bytes 1-7 |
|---|---|---|
| 0 drive | level | reserved u8, frequency kHz/25 u16, offset s16, factory drive u16 |
| 1 deviation | mode | reserved u8, frequency kHz/25 u16, permille s16, 0 u16 |
| 2 frequency | mode | reserved u8, frequency kHz/25 u16, Hz s16, 0 u16 |
| 3 name | level | `board.bands` index u8, name char[6] (NUL padded) |
| 4 battery | 0 gain, 1 empty, 2 full | reserved u8, 0 u16, value s16 (permille or mV), 0 u16 |

The CRC is the storage CRC-32 (`storage_crc32`, the reflected 0xEDB88320 polynomial, start and end inverted).

The RAM table is the only copy in RAM. A save writes the record from the table (a drive entry gets the factory drive at
the entry's frequency at that moment), and a load fills the table from the record. A record whose owner is another radio's loads no trim:
a settings ring copied to another radio with a flash image brings its settings, never one radio's trims to another.
At start-up, after the radio's calibration is loaded, the record in force fills the table. Entries the radio can't use
are left out: a stale drive trim, a value past ±4095, a level the board doesn't have, a frequency or band with no
transmission, a battery trim on a board without a battery, or an entry past the table's room. With no record the radio
uses the factory values.

RAM: 20 words of 4 bytes for drive, deviation, frequency and battery trims (`RADIO_TUNE_WORDS`), 6 names of 8 bytes
(`RADIO_TUNE_NAMES`), 128 bytes of table. A load and a save use the settings ring's own slot buffers.

## 4. Saves

The host saves through the debug link's STORAGE command ([debuglink.md](debuglink.md) 3.13):

1. TUNE_PUT puts the entries in the RAM table, up to 7 a request; the first request empties the table. The entries are
   live at once.
2. TUNE_SAVE writes the table as the new record. The settings ring writes, commits and reads back the record as for
   a settings page: a power cut or a failed write leaves the record before in force.
3. TUNE_GET reads the table back, 7 entries a request, and the host compares the table with the entries put.

TUNE_LOAD fills the table from the record in force again, which undoes live tries. TUNE_CLEAR empties the table and
saves an empty record: no trims, now and after a restart. Each answers the entries now in the table, or unavailable on
a radio without a settings ring of its own (none, or one still holding another firmware's data,
[settings-store.md](settings-store.md) 5).

On the radio, Settings > Advanced > Clear tuning does what TUNE_CLEAR does, on a hold of OK. A factory reset keeps the
trims. The page's Restore factory values also sets the oscillator trim to 0.

## 5. Live tuning

| What | How | In effect |
|---|---|---|
| Any trim | RADIO TUNE_SET (op 44) replaces the RAM table's words | from the next use of the trim (key-up, a burst, a name drawn) |
| Drive during an FM transmission | RADIO TX_DRIVE (op 36) with the absolute drive | at once (`hal_radio_t.tx_drive_now`), and for that transmission |
| Oscillator | SETTING 35 (`ref_trim`) | at once, and stored as the menu stores it |
| Battery | RADIO TUNE_SET, word kind 3 | at the next battery reading: each second, outside transmissions and the 3 s after |

A RAM word is the frequency in 100 kHz units (bits 0-13), the kind (bits 14-15), the level or mode (bits 16-18) and
the value (bits 19-31, signed): `RADIO_TUNE_WORD(khz, kind, sel, value)`. A battery trim is word kind 3
(`RADIO_TUNE_W_BATT`, never a name in RAM) at frequency 0. Live words are saved only by a TUNE_SAVE. TUNE_LOAD or
a restart brings back the stored record. Names are set with TUNE_PUT.

A RAM word holds the frequency in 100 kHz units, an entry in 25 kHz units. A put rounds each entry's frequency down
to 100 kHz, so two trims of one kind and level or mode less than 100 kHz apart become two words at the same frequency,
and the first one found wins. The page puts every trim on a 100 kHz grid (`gridKhz`), so the table reads back as
put.

The view's `tune` key has `live`: 1 when the board has `tx_drive_now`. Then come the battery reading before the gain
(`battery_raw_mv`) and the board's empty and full voltages, all 0 without a battery. The page works out the gain
from that reading and a multimeter's, and reads the corrected voltage and charge from the `features` view. Without live drive, a drive change
reaches the PA at the next key-up, so the page ends the test transmission and keys again with the new words. Frequency,
deviation and oscillator changes also take effect at key-up, so the page re-keys for those on every board.

The table changes one aligned word at a time, and the words and their count are `volatile`, so each store happens
where the code puts it. TUNE_SET first shortens the table to the words before the first one it
replaces, so a reader on another task sees a shorter table, never a torn word. The table then ends after the last word
written: words past it from an earlier table never come back. A request that starts past the table's end is refused.
TUNE_PUT with its start flag and TUNE_LOAD empty the table, then add one entry at a time: a reader sees the table grow.

## 6. Debug link

| Use | Command |
|---|---|
| Table in, read back | STORAGE TUNE_PUT (op 3), TUNE_GET (op 7) |
| Save, load, clear | STORAGE TUNE_SAVE (op 5), TUNE_LOAD (op 4), TUNE_CLEAR (op 6) |
| Live words | RADIO TUNE_SET (op 44) |
| Live drive | RADIO TX_DRIVE (op 36) |
| Oscillator | SETTING 35 |
| Factory drives, names, bands | core view `tx_drive`, `power_names`, `bands` |
| Table fill, deviation modes, battery | core view `tune`; `features` view `battery_mv`, `battery_pct` |

`tools/rfoundry_debug.py` has `tune_put`, `tune_get`, `tune_save`, `tune_load`, `tune_clear` and `tune_set`.

## 7. Testing

`tests/host/radioTuneTest.c` covers interpolation and the clamps, the band edge, saves and restarts (the newest record,
the table read back as put, its room, live words not saved until a save, clearing, chunked puts), a record from another
radio, stale drive trims, names, live words and battery trims. `tests/host/radioFeaturesTest.c` covers the charge curve,
the moved empty and full and the gain; its `radioFeaturesTestTune` build keys with a carrier trim and reads the battery
with the trims through the radio core. `tests/host/debuglinkCmdTest.c` covers the STORAGE operations: put in two
requests, read back, save, restart, load, clear, and no ring. `tests/host/settingsTest.c` covers the record in the ring:
a ring from a firmware without the record, 1000 settings saves round the ring (the record carried, wear even), a factory
reset that keeps the record, an empty record, and a power cut at every unit of a tune save and of a settings save that
carries the record. `tests/host/uiFlowTest.c` covers Clear tuning. `site/tests/tune.test.mjs` covers the page's entries,
payloads, save and charge against the same layouts. `tests/hil/test_radio_tune.py` sets the battery trims live on a
radio and checks the corrected reading and charge against the raw reading, with no transmission; with `--allow-reboot`
the test saves a trim, restarts the radio, makes a settings save and restarts again, then clears the trims and restarts
once more. `tests/hil/test_ui.py` `test_factory_reset` checks that a factory reset keeps a saved trim.
