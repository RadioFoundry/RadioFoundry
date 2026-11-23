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

`CONFIG_RADIO_TUNE` builds the feature (on with the debug link). The radio's side is small: it checks, loads and
applies one record (`core/radio/radio_tune.c`). The host builds the record. The host side is the site's Radio Tuning
page at Tools > Radio tuning ([16-battery-and-power.md](../user/16-battery-and-power.md)).

## 1. What a board provides

| Trim | Needs | Without it |
|---|---|---|
| Drive | `hal_radio_t.tx_drive_level`, and the board's drive passed through `radio_tune_drive(hz, level, drive)` | no drive trims |
| Deviation | `board_t.tune` bit `1 << mode`, and the board scales that mode's deviation by `radio_tune_dev(mode, hz)` | no deviation trims for that mode |
| Frequency | nothing: the radio core adds `radio_tune_freq(mode, hz)` to the transmit frequency at key-up | |
| Names | nothing | |
| Battery | a battery (`board_has_battery()`) and `hal_power_t.battery_mv`: the radio core applies the trims ([radio-features.md](radio-features.md) 5.1) | no battery trims |
| Storage | `hal_flash_map_t.tune_addr`: two erase units (`HAL_FLASH_TUNE_UNITS`) | trims work live only, nothing is stored |

The tune area is outside the codeplug, the CPS windows, the settings ring and the calibration. The open-firmware CPS
shim, the fast CPS path and the staging area all refuse to write it. Codeplug reads, writes and shares never include
it. Which radios have which trims is in [feature-matrix.md](../feature-matrix.md).

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

Each copy holds one record at the start of its own erase unit, little endian:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `0x4E544652` ("RFTN") |
| 4 | 4 | sequence number, never 0, one more with each save |
| 8 | 1 | version (1) |
| 9 | 1 | entries, at most 32 |
| 10 | 2 | reserved, 0 |
| 12 | 4 | owner: CRC-32 of `board.name`, continued over the chip's unique ID (the name alone on a board without one) |
| 16 | 8 × entries | entries (below) |
| … | 4 | CRC-32 of everything above |
| … | 4 | commit word: 0 once the record is complete |

An entry's first byte is the kind in the high nibble and the level or mode in the low nibble:

| Kind | Low nibble | Bytes 1-7 |
|---|---|---|
| 0 drive | level | reserved u8, frequency kHz/25 u16, offset s16, factory drive u16 |
| 1 deviation | mode | reserved u8, frequency kHz/25 u16, permille s16, 0 u16 |
| 2 frequency | mode | reserved u8, frequency kHz/25 u16, Hz s16, 0 u16 |
| 3 name | level | `board.bands` index u8, name char[6] (NUL padded) |
| 4 battery | 0 gain, 1 empty, 2 full | reserved u8, 0 u16, value s16 (permille or mV), 0 u16 |

The CRC is the storage CRC-32 (`storage_crc32`, the reflected 0xEDB88320 polynomial, start and end inverted).

A copy is valid when the magic, version, entry count, sequence, owner, CRC and commit word check. A flash image copied
to another radio is ignored there. At start-up, after the radio's calibration is loaded, the newer valid copy (the
sequence compared as a signed difference) fills the RAM table. Entries the radio can't use are left out: a stale drive
trim, a value past ±4095, a level the board doesn't have, a frequency or band with no transmission, or a battery
trim on a board without a battery. With no valid copy the radio uses the factory values.

RAM: 20 words of 4 bytes for drive, deviation, frequency and battery trims (`RADIO_TUNE_WORDS`), 6 names of 8 bytes
(`RADIO_TUNE_NAMES`), 128 bytes of table. A load uses the shared storage buffer.

## 4. Saves

The host saves through the debug link's STORAGE command ([debuglink.md](debuglink.md) 3.13):

1. STORAGE INFO gives the tune area's address and erase unit. STORAGE READ reads both copies. The host picks the
   copy in force by the same rule as the radio.
2. The host writes the copy that is not in force with TUNE_WRITE: offset 0 erases the copy. The header, entries and
   CRC go in chunks of up to 60 bytes, then the commit word last.
3. The host reads the copy back with STORAGE READ and compares it.
4. The host writes and checks the other copy the same way.
5. TUNE_LOAD puts the new record in force. The reply gives the copies the radio found valid.

A power cut or a lost link during step 2 leaves the old record in force in the other copy. A cut during step 4 leaves
the new record in the first copy. TUNE_WRITE runs under the storage lock and is refused while the radio transmits.
TUNE_WRITE answers unavailable while the storage region still holds another firmware's data (the region not yet
claimed, [settings-store.md](settings-store.md)).

On some boards the tune area sits where another firmware may keep its own data, such as the end of a codeplug. So
before each erase (offset 0), TUNE_WRITE checks both units. Each unit must be blank or hold a tune copy: the record's
magic at its start, whole or cut short by a lost save, from this radio or another. Otherwise the radio refuses with
reason 2 and erases nothing. The page and `rfoundry_debug.py` show that the area holds other data and nothing was
saved. Only an offset-0 write checks and erases, so a write at a later offset goes only into a copy that an offset-0
write erased since the last TUNE_LOAD. Otherwise the radio refuses with reason 3 and writes nothing. A storage claim
does not erase the tune area. TUNE_LOAD answers busy while a save or a CPS write holds the shared
storage buffer, and the table stays as it was. The host tries again.

To clear every trim, the host saves a record with no entries. The page's Restore factory values also sets the
oscillator trim to 0.

## 5. Live tuning

| What | How | In effect |
|---|---|---|
| Any trim | RADIO TUNE_SET (op 44) replaces the RAM table's words | from the next use of the trim (key-up, a burst, a name drawn) |
| Drive during an FM transmission | RADIO TX_DRIVE (op 36) with the absolute drive | at once (`hal_radio_t.tx_drive_now`), and for that transmission |
| Oscillator | SETTING 35 (`ref_trim`) | at once, and stored as the menu stores it |
| Battery | RADIO TUNE_SET, word kind 3 | at the next battery reading: each second, outside transmissions and the 3 s after |

A RAM word is the frequency in 100 kHz units (bits 0-13), the kind (bits 14-15), the level or mode (bits 16-18) and
the value (bits 19-31, signed): `RADIO_TUNE_WORD(khz, kind, sel, value)`. A battery trim is word kind 3
(`RADIO_TUNE_W_BATT`, never a name in RAM) at frequency 0. Live words are not stored. TUNE_LOAD or a
restart brings back the stored record. Names are not set live: the host saves them.

A RAM word holds the frequency in 100 kHz units, the record in 25 kHz units. A load rounds each record frequency down
to 100 kHz, so two trims of one kind and level or mode less than 100 kHz apart become two words at the same frequency,
and the first one found wins. The page puts every trim on a 100 kHz grid (`gridKhz`), so only a hand-made record
has such a pair.

The view's `tune` key has `live`: 1 when the board has `tx_drive_now`. Then come the battery reading before the gain
(`battery_raw_mv`) and the board's empty and full voltages, all 0 without a battery. The page works out the gain
from that reading and a multimeter's, and reads the corrected voltage and charge from the `features` view. Without live drive, a drive change
reaches the PA at the next key-up, so the page ends the test transmission and keys again with the new words. Frequency,
deviation and oscillator changes also take effect at key-up, so the page re-keys for those on every board.

The table changes one aligned word at a time, and the words and their count are `volatile`, so each store happens
where the code puts it. TUNE_SET first shortens the table to the words before the first one it
replaces, so a reader on another task sees a shorter table, never a torn word. The table then ends after the last word
written: words past it from an earlier table never come back. A request that starts past the table's end is refused.
A TUNE_LOAD empties the table before it fills it again.

## 6. Debug link

| Use | Command |
|---|---|
| Tune area, read back | STORAGE INFO, STORAGE READ |
| Save, load | STORAGE TUNE_WRITE (op 3), TUNE_LOAD (op 4) |
| Live words | RADIO TUNE_SET (op 44) |
| Live drive | RADIO TX_DRIVE (op 36) |
| Oscillator | SETTING 35 |
| Factory drives, names, bands | core view `tx_drive`, `power_names`, `bands` |
| Table fill, deviation modes, battery | core view `tune`; `features` view `battery_mv`, `battery_pct` |

`tools/rfoundry_debug.py` has `tune_area`, `tune_write`, `tune_load` and `tune_set`.

## 7. Testing

`tests/host/radioTuneTest.c` covers interpolation and the clamps, the band edge, the tune area check before an erase
(blank, a record, a cut save, other data, a failed read), the copy choice (newest wins, a copy cut before the
commit word, a flipped bit, sequence wrap), a record from another radio, stale drive trims, names, live words and
battery trims. `tests/host/radioFeaturesTest.c` covers the charge curve, the moved empty and full and the gain.
`site/tests/tune.test.mjs` covers the page's record, CRC, payloads and charge against the same layouts.
`tests/hil/test_radio_tune.py` sets the battery trims live on a radio and checks the corrected reading and charge
against the raw reading, with no transmission.
