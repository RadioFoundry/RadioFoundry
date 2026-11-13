<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Satellite Tracking

Satellite tracking uses TLE records installed from a computer. The radio does not download orbital data. The optional
`CONFIG_SATELLITE_TLE` feature requires a board build with dedicated satellite storage and debug link support.

## Install orbital data

Connect the radio over its debug-link USB connection. On the computer, browse a supported catalog:

```sh
python tools/satellite_tle.py catalog --source amsat
```

Install only the catalog IDs you choose. Repeat `--id` or separate IDs with commas:

```sh
python tools/satellite_tle.py sync --source amsat --id 25544,43017
```

To prepare data offline, fetch selected IDs into a bundle, validate it, then install it when the radio is connected:

```sh
python tools/satellite_tle.py fetch --id 25544 --output satellites.json
python tools/satellite_tle.py validate satellites.json
python tools/satellite_tle.py install satellites.json
```

The utility checks both TLE lines, their identity and checksums, and element age before transferring the complete
selected set. After commit, it creates or updates the `Satellites` zone with a channel for each installed satellite.
Matching channels keep their current settings; new channels copy the current channel's frequencies, use FM, and start
with transmit enabled. New channels for satellites in the built-in AMSAT FM frequency table use the listed downlink,
uplink, and uplink tone. Other new channels use the current channel's frequencies. Existing channels are not changed.
Check the [AMSAT live FM satellite list](https://www.amsat.org/live-fm-satellites/) for current operating status before
transmitting. The built-in frequency snapshot reflects that page's October 31, 2026 update. SO-50 requires a 2-second
74.4 Hz CTCSS transmission to arm its timer; switch back to its normal 67.0 Hz uplink tone afterward. Channel names
use the first 16 characters of the TLE name because that is the codeplug limit. Edit frequencies and tones with the
normal channel editor. If channel synchronization fails, the TLEs are still installed; rerun the install to retry
synchronization. Use `read` to inspect installed IDs and epochs:

```sh
python tools/satellite_tle.py read
```

## Enter a TLE on the radio

Open **OK**, **6** (Tools), then **Satellite**. Choose a pass to open that satellite's details, then press **#**. Enter the complete
69-character first TLE line and press **OK**, then enter the complete second line and press **OK** again. The radio
keeps the existing catalog ID and name. It validates both lines and commits the replacement as one transaction; a
bad or incomplete pair does not replace the installed data. This edits both lines from scratch, so keep the original
TLE available while entering it.

## Track a satellite

Open **OK**, **6** (Tools), then choose **Satellite**. The radio uses the selected position source for coordinates
and altitude, and fresh GPS or computer NMEA for UTC, to predict passes for the installed TLEs. A saved fixed position
can therefore provide the observer location while the GPS is acquiring its first fix, once the receiver provides
current date and time. The list is ordered by time to AOS and shows each pass's maximum elevation. Up and Down
select a pass; **OK** opens its details and starts tracking. The selected satellite is remembered across power cycles.
The firmware includes initial ISS, SO-50, and AO-91 TLEs. Firmware builds refresh the shared TLE cache from CelesTrak
at most once per 24 hours; use `SATELLITE_TLE_OFFLINE=1` for an offline build. A failed refresh uses the last cache or
checked-in snapshot. On boot, missing built-in satellites are added to the installed list; existing records and their
TLEs are refreshed once when the firmware or built-in TLE set changes. Other satellite records are preserved. The
firmware fingerprint is retained across PC syncs and on-radio TLE edits, so your changes are not reset on each boot.
TLEs age, so refresh them from a PC regularly; daily is recommended.
Pass and Doppler values use an approximate Keplerian orbit model, not SGP4. They can differ from a precision ephemeris,
especially as the TLE ages, so keep the installed elements current.
Tracking gets its nominal RX/TX frequencies and optional transmit CTCSS tone from the built-in catalog-ID profile,
not from the selected VFO or codeplug channel. A satellite without a built-in profile cannot start tracking. While
tracking, the radio uses FM and 12.5 kHz bandwidth; stopping restores the prior radio settings. TLE providers supply
orbital elements, not communication frequencies. The optional `Satellites` zone is only an editable convenience:
channel synchronization seeds unsupported IDs from the current channel, but editing a channel does not change the
runtime profile used for tracking.

The screen reports visibility, receive and transmit Doppler offsets, the current Doppler-adjusted RX and TX frequencies,
and the next pass's AOS and LOS countdowns. Its
north-up circular sky plot shows the predicted path from horizon to zenith and marks the satellite's current position
while it is visible. During an active transmission, the TX row shows the frequency in use; tracking continues
calculating the latest target in the background and applies it as soon as transmission ends.
The radio retains one pass at a time: the upcoming pass becomes current at AOS, then tracking rolls forward to the next
pass after LOS. A missing position, altitude, valid UTC, unavailable record or stale TLE leaves the session unavailable
and does not apply a correction. The radio restores ordinary channel frequencies when tracking stops, the channel/VFO
context changes, scan or hotspot mode starts, or required inputs become invalid.

Press **OK** again while the active satellite is selected to stop tracking and restore the nominal frequencies.
Leaving the Satellite screen alone does not stop tracking. Starting a different satellite replaces the current
tracking session. Tracking never bypasses normal transmit band, inhibit or active-transmission rules.