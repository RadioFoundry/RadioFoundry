<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Position

Where the radio is: one service, `core/position`, for every protocol that reports a position and for the Position
screen. None of its users needs another one built in: DMR GPS Info and LRRP work in a build without APRS, and APRS
works in a build without DMR.

Code: `core/position/position.c` (the service), `nmea.c` (RMC and GGA), `nmea_sky.c` (satellites, `CONFIG_GPS_SKY`),
the UI in `core/ui/ui_position.c` and Settings > Location, the debug link in `core/debuglink/debuglink_position.c`.

## 1. Users and switches

| User | Switch | What it takes |
|---|---|---|
| DMR GPS Info in voice calls ([dmr-voice.md](dmr-voice.md) 3.1) | `CONFIG_DMR_GPS` | `position_now`: the position, its error, the "Send in DMR calls" setting. A channel's codeplug setting turns it on for that channel too |
| LRRP answers and triggered reports ([dmr-data.md](dmr-data.md) 6) | `CONFIG_DMR_LRRP` | `position_report` through the data service's `ownPosition` hook |
| APRS beacons ([aprs.md](aprs.md) 7) | `CONFIG_APRS` | `position_fix` (exact; APRS formats its own ambiguity) |
| Tools > Position ([ui.md](ui.md) 2.3) | `CONFIG_GPS` | `position_gps`, `position_gps_sat` |

| Switch | Set by | Builds |
|---|---|---|
| `CONFIG_POSITION` | derived: `GPS`, `APRS`, `DMR_LRRP` or `DMR_GPS` | the service, its settings, the Settings > Location rows, the debug link keys |
| `CONFIG_GPS` | the build | the board GPS half: the board fix, `position_gps_enable`, the Position screen's data |
| `CONFIG_POSITION_HOST` | derived: `APRS` | the host half: a fix from NMEA sent by a computer. Its feeders are APRS's host port and the debug link's NMEA command |
| `CONFIG_GPS_SKY` | the build (needs `GPS`) | the satellite table (section 4) |

A radio without a GPS receiver, or a build with `CONFIG_GPS=n`, still has a position when one of the reporting
switches is on: the fixed position.

## 2. Sources

Position source (setting: Auto, GPS, Computer, Fixed):

| Source | From | Carries |
|---|---|---|
| GPS | the board's GPS driver: `position_nmea(..., gps = true)` (`$xxRMC`, `$xxGGA` with valid checksums) | speed, course, altitude, UTC time and date |
| Computer | host NMEA: `position_nmea(..., gps = false)` | the same |
| Fixed | the settings: latitude, longitude, altitude | position only |

- Auto takes a GPS fix younger than 60 s (`POSITION_STALE_MS`), else a host fix younger than 60 s, else the fixed
  position.
- GPS, Computer and Fixed take that source only. No position when it has none.
- The fixed position counts as unset while latitude and longitude are both 0.
- Settings > Location > GPS off (`position_gps_enable(false)`) drops the board fix at once and ignores later
  sentences, so the position falls back.
- In a build without the host half, Computer is not offered and a stored Computer counts as Auto.

## 3. What a reader gets

| Call | Result |
|---|---|
| `position_fix` | the position in use, exact, with its source (`POSITION_SRC_NONE`: none) |
| `position_report` | the same with the ambiguity setting applied, and the error estimate in meters |
| `position_now` | latitude and longitude of `position_report`, the error, and the "Send in DMR calls" setting |

Ambiguity (0-4, as APRS101 chapter 8) blurs a DMR report to the center of its grid cell:

| Ambiguity | Grid | Error reported |
|---|---|---|
| 0 | exact | 20 m (GPS, Computer), 200 m (Fixed) |
| 1 | 0.1 minute | 185 m |
| 2 | 1 minute | 1852 m |
| 3 | 10 minutes | 18.5 km |
| 4 | 1 degree | 111 km |

Every call takes the lock set with `position_set_lock` (the scheduler suspended for short copies): NMEA arrives in
the GPS driver's task, readers are the radio, UI and debug link tasks. Each read takes the lock once.

## 4. The board GPS's state and satellites

`position_gps()` gives the UI the board GPS as it is now: false while Settings > Location > GPS is off or no valid
sentence came for 60 s, else the last fix (60 s old at most), satellites used and HDOP (GGA), speed, course and time
(RMC).

With `CONFIG_GPS_SKY`, `nmea_sky.c` also reads the board GPS's satellite sentences:

| Sentence | What it gives |
|---|---|
| GSV (talker GP, GL, GA, BD / GB, GQ, GN) | each satellite in view: number, elevation, azimuth, SNR. A round (messages 1 to N of one talker) replaces that system's satellites; one the round no longer lists is dropped. A receiver that sends a round per signal (the NMEA 4.1 signal ID, e.g. L1 and L5) has only the first signal heard kept per system. A GN talker places a satellite by its number (GLONASS 65-96, QZSS 193-200, BeiDou 201-263 and 401-463, Galileo 301-336); SBAS and unknown numbers are skipped |
| GSA | the fix mode (1 none, 2 2D, 3 3D) and the satellites the fix uses. The GSAs in a row (one per system, or several GN) make one round; the first clears the used set. A GN GSA takes its system from the NMEA 4.1 system ID when present, else from the numbers |

The table holds 28 satellites of 4 bytes each (number, elevation, system, SNR, azimuth / 2); the used sets are a
64-bit mask per system. In all 168 B, kept in the service's state under its lock; `position_gps_sat(i)` copies one
satellite out, so a reader needs no table of its own. Turning the GPS setting off empties it.

## 5. Settings

The service owns its settings (`position_cfg_t`): `position_load` at start-up and when the settings are replaced,
`position_set_cfg` to change and save them. They are in `settings_t` ([settings-store.md](settings-store.md)):

| Setting | Field | Default |
|---|---|---|
| Position source | `aprs_pos_src` | Auto |
| Fixed position | `aprs_lat`, `aprs_lon` (1e-7 degree), `aprs_alt_m` | none |
| Ambiguity | `aprs_misc` bits 4-6 | 0 |
| Send in DMR calls | `aprs_flags2` `SETTINGS_APRS2_DMR_GPS` | off |
| Stored marker | `aprs_misc` bit 7 (`SETTINGS_APRS_MISC_POSITION`) | |

- The fields keep the names and places they had when APRS owned them, so a stored record reads the same.
- They are taken when the marker is set, or when `aprs_stored` is 1 (a record APRS saved before the marker
  existed). Otherwise the defaults.
- `position_set_cfg` sets the marker and never `aprs_stored`: saving a position setting doesn't make APRS's
  defaults count as stored.
- APRS saves the same record: it calls `position_cfg_store` first and leaves these fields alone.
- Out of range values are refused: a source past the last, latitude past 90 degrees, longitude past 180, ambiguity
  past 4.

## 6. UI

Settings > Location, with `CONFIG_POSITION`:

| Row | Does |
|---|---|
| Position source | Auto, GPS, Computer, Fixed (Computer only with the host half) |
| Fixed position | opens the page below |
| Send in DMR calls | on: our position goes into every DMR voice call. Off: only on channels whose codeplug setting asks for it ([dmr-voice.md](dmr-voice.md) 3.1). "DMR position: not in this build" without `CONFIG_DMR_GPS` |

Fixed position (`fixed-pos`, `ui_position.c`). Rows: Latitude and Longitude ("Not set" while both are 0). OK opens an
editor in decimal degrees to 4 places (about 11 m): the digits fill `DD.DDDD` or `DDD.DDDD` from the left (a
longitude's first digit of 2-9 gets its leading zero by itself), a digit that would pass 90 or 180 degrees is refused
with a toast, Up / Down or the knob switch N / S or E / W, BACK takes the last digit back (with none typed it closes
the editor unchanged), OK saves with the places not typed as zeros. The editor starts with the stored value shown;
the first digit starts over.

The altitude and the ambiguity are set over the debug link.

## 7. Debug link

The APRS view ([debuglink.md](debuglink.md) 3) carries the position keys in every build with `CONFIG_POSITION`,
APRS or not:

| Key | Value |
|---|---|
| `pos.built`, `pos.host_built` | 1, and whether the host half is built |
| `pos.src` | none, gps, host, fixed |
| `pos.lat`, `pos.lon`, `pos.err_m`, `pos.age_s` | the position a DMR report would carry now |
| `pos.gps_heard`, `pos.gps_fix`, `pos.gps_sats` | the board GPS (`CONFIG_GPS`) |
| `set.aprs_pos_src`, `set.aprs_dmr_gps`, `set.pos_ambiguity`, `set.pos_lat`, `set.pos_lon` | the settings (the settings part of the view) |

The APRS command's SET fields for the fixed latitude and longitude, the source, "Send in DMR calls" and the
ambiguity, and its NMEA operation (host half), are answered by the position service, so they work without APRS.

## 8. Tests

| Test | Checks |
|---|---|
| `tests/host/radioCoreTest.c` `test_position` (a build without APRS) | GPS fix to LRRP and the Position screen; "Send in DMR calls" saved with its own marker and back after a restart; GPS Info sent with it on, not with it off; the fixed position on a radio without a GPS answers LRRP and goes out in calls after a restart; source GPS excludes the fixed position; a record APRS stored before; defaults |
| `tests/host/aprsRadioTest.c` | APRS saves its settings without changing the position's; the host NMEA path |
| `tests/host/uiFlowTest.c` | the Settings > Location rows, the Fixed position page and its editor |
| `tests/hil/test_gps.py` (opt-in) | the position keys and settings on the radio, with and without APRS |
