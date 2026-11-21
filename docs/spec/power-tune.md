<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Power tuning

The owner's own adjustments to each power level's PA drive, measured with a wattmeter on a dummy load, and the names
they give the levels. Power tuning sits on top of the factory calibration and never changes the calibration.
`CONFIG_POWER_TUNE` builds the feature (on with the debug link). The code is in `core/radio/power_tune.c` (the
applied table), `core/radio/power_tune_store.c` (the record) and `core/debuglink/debuglink_tune.c` (the host
command). The host side is the web tool at Tools > Power tuning ([16-battery-and-power.md](../user/16-battery-and-power.md)).

## 1. What a board provides

| Need | Field | Without it |
|---|---|---|
| The drive of a level at a frequency | `hal_radio_t.tx_drive_level` | no tuning: INFO reports no store |
| A tune area, two 4 KB erase units | `hal_flash_map_t.tune_addr` | no tuning: INFO reports no store |
| The lowest drive the PA still controls | `hal_radio_t.tx_drive_floor` (optional) | the lowest level can only go up |
| The tuning applied at key-up | `power_tune_drive(hz, level, drive)` on the board's drive | the stored points have no effect |

The tune area is outside the codeplug, the CPS windows, the settings ring and the calibration. The open-firmware CPS
shim, the fast CPS path and the staging area all refuse to write it. Codeplug reads, writes and shares never include
it. Which radios have power tuning is in [feature-matrix.md](../feature-matrix.md).

## 2. Points and levels

A tuning point is a frequency, a level and a drive offset (signed, in the board's drive units). The offset at a
transmit frequency is interpolated between the points of that level in the same band, and held flat past the outer
points. A band without a point for a level leaves that level at the factory drive.

| Limit | Value |
|---|---|
| Points | 20 (`POWER_TUNE_POINTS`) |
| Level names | 8 (`POWER_TUNE_LABELS`), one per band and level |
| Name length | 6 characters, the status line's power field |
| Offset | ±4095 drive units |
| Levels a point can name | 8 |

The tuned drive is clamped at key-up:

- Never above the top level's factory drive at that frequency. The top level can only go down.
- Never below the board's floor (`tx_drive_floor`). Without a floor hook, never below the lowest level's factory
  drive, so the lowest level can only go up.

Each point keeps the factory drive it was tuned against. A point applies only while `tx_drive_level` still returns
that drive. New drive tables, a new calibration, or levels added or removed make the point stale, and a stale point
is never applied. Names follow the same rule. A user's names replace the board's names in the status line, the menus,
the debug link view and the web and SNMP services (`power_tune_name`).

## 3. The record

Each copy holds one record at the start of its own erase unit, little endian:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `0x54504652` ("RFPT") |
| 4 | 4 | sequence number, one more with each save |
| 8 | 1 | version (1) |
| 9 | 1 | points |
| 10 | 1 | names |
| 11 | 1 | the board's level count when saved |
| 12 | 4 | CRC-32 of `board.name` |
| 16 | 4 | CRC-32 of the chip's unique ID |
| 20 | 16 × points | khz u32, level u8, state u8, factory drive u16, offset s16, reserved u16, reading mW u32 |
| … | 20 × names | khz u32, level u8, state u8, factory drive u16, target mW u32, name char[8] (NUL padded) |
| … | 4 | CRC-32 of everything above |
| … | 4 | commit word: 0 once the record is complete |

The state byte is 0 in flash. A READ reply fills it in: 0 applied, 1 stale, 2 gone (no such level, or the frequency
is in no transmit band now).

A copy is valid when the magic, version, counts, CRC and commit word check, and the board and chip IDs match this
radio. A flash image copied to another radio is ignored there. At start-up, after the radio's calibration is loaded,
the newer valid copy (sequence compared modulo 2^32) fills the RAM table. With no valid copy the radio uses the factory
drive.

## 4. Saves

Every edit builds the new record in the shared storage buffer and saves it to both copies:

1. The copy that is not in force first: erase, record, CRC, then the commit word, then read back.
2. Then the other copy, the same way.

A power cut during step 1 leaves the old record in force in the other copy. A cut during step 2 leaves the new record
in the first copy. A save that fails its read-back reports FAILED, and the record that was in force stays in force.
Saves run under the storage lock and never while the radio transmits.

RAM: the applied table is 4 bytes per point and 8 per name, about 150 bytes in all. Loads and saves use the shared
storage buffer.

## 5. Preview

A preview is a try-out offset for one level, RAM only, used in place of the stored offsets. Setting another level's
preview ends the first. A preview lapses 2 minutes after it was set (`POWER_TUNE_PREVIEW_MS`), and level 0xFF ends it.
The key-up clamp applies to previews too, so a host cannot drive past the limits. The web tool keys the radio on a
preview, then saves the offset as a point once the reading is right.

## 6. Debug link

`DL_CMD_TUNE` (`0x27`), [debuglink.md](debuglink.md) 3.31. `tools/rfoundry_debug.py` has `tune_info`, `tune_limits`,
`tune_read`, `tune_preview`, `tune_put_point`, `tune_del_point`, `tune_put_label` and `tune_clear`.

## 7. Testing

`tests/host/powerTuneTest.c` covers interpolation, the clamp and the floor, preview lapse, corrupt copies, a power cut
at every flash step of a save, stale points after a drive change, a record from another chip, names, a full table and
the debug link payloads.
