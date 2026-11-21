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

For a browser workflow, open **Tools > Satellite TLE Update** on the RadioFoundry site. Load the AMSAT daily bulletin
or browse for a TLE file, then choose up to 10 FM satellites by name. Non-FM objects are not shown. ISS, SO-50 and
AO-91 start selected when present. Search does not clear checked selections. The page refreshes the AMSAT bulletin
before each push or bundle download; a selected file is used as supplied. A push replaces checked satellites' existing
TLEs and verifies their epochs. The page can push directly over USB/serial or create a JSON bundle for the PC utility.
Direct push requires a supported browser and a secure context (HTTPS or localhost).
After an install or update, the web flasher offers this page when the installed build reports satellite support.

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

The utility checks both TLE lines, their identity and checksums, and element age before transferring the selected set.
The website replaces the satellite TLE inventory and selected frequency profiles. Command-line `sync` and `install`
also replace the entire TLE inventory with the selected records; an orbital-only bundle leaves frequency profiles
unchanged. Neither workflow creates, modifies, or removes codeplug zones or channels. Check the [AMSAT live FM satellite list](https://www.amsat.org/live-fm-satellites/)
for current operating status before transmitting. The built-in frequency snapshot reflects that page's October 31,
2026 update. SO-50 requires a 2-second 74.4 Hz CTCSS transmission to arm its timer; switch back to its normal
67.0 Hz uplink tone afterward. Use `read` to inspect installed IDs and epochs:

```sh
python tools/satellite_tle.py read
```

Frequency profiles are separate from orbital elements. The daily TLE bulletin contains no frequencies; the page uses
the known entries in AMSAT's live FM table, which currently covers all listed FM satellites. Non-FM objects are omitted.
A profile list has a maximum of 10 entries. A website push replaces the radio's satellite TLE and frequency tables with
the selected satellites, so unselected satellites disappear from the tracking list. The codeplug is left unchanged.
The PC utility also reads or changes the built-in profile ID list directly:

```sh
python tools/satellite_tle.py profiles
python tools/satellite_tle.py profiles --id 25544,27607,43017
```

The PC utility requires protocol v4 or later for replacement. Schema-v3 frequency bundles require protocol v5.
Protocol v5 supports up to four selected frequency profiles;
protocol v6 supports the full ten. Profile-only ID changes require protocol v3 or newer.

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
current date and time. Computer fixes and UTC do not survive a restart: send fresh NMEA again, including altitude and
date/time. The list is ordered by time to AOS and shows each pass's maximum elevation. When prediction finishes, the
first pass is highlighted. Up and Down select a pass; **OK** opens its details and starts tracking. The selected
satellite is remembered across power cycles.
The firmware image contains no TLEs. Load selected records into the radio's flash store from the site's Satellite TLE
Update page or with `tools/satellite_tle.py sync`. With an empty store, Satellite shows **No TLEs: load from site**.
The pass list shows the age of the oldest installed TLE when current UTC is available. Ages can differ even when
satellites are refreshed together. At 14 days, it warns once per inventory update to refresh the set and improve
prediction accuracy.
Pass and Doppler values use an approximate Keplerian orbit model, not SGP4. They can differ from a precision ephemeris,
especially as the TLE ages, so keep the installed elements current.
Tracking gets its nominal RX/TX frequencies and optional transmit CTCSS tone from the persisted catalog-ID profile,
not from the selected VFO or codeplug channel. A satellite without a stored profile cannot start tracking. While
tracking, the radio uses FM and 12.5 kHz bandwidth; stopping restores the prior radio settings. TLE providers supply
orbital elements, not communication frequencies. PC synchronization does not touch the codeplug; editing an existing
channel does not change the runtime profile used for tracking.

The screen reports receive and transmit Doppler offsets, the current Doppler-adjusted RX and TX frequencies,
and the next pass's AOS and LOS countdowns. The north-up circular sky plot shows the predicted path from horizon to
zenith and marks the satellite's current position while it is visible. On 128x64 displays, the detail screen shows the
AOS and LOS countdowns instead of the plot. Rise is left-aligned and set is right-aligned. While the satellite is up,
the rise countdown shows `Now`.
During an active transmission, the TX row shows the frequency in use;
tracking continues calculating the latest target in the background and applies it as soon as transmission ends.
While SO-50 is actively tracked, long-press **OK** on its detail screen to send the 74.4 Hz CTCSS arm signal for two
seconds. The radio applies its normal transmit checks and uses the active Doppler-adjusted uplink. This does not change
the normal 67.0 Hz profile tone, and the arm signal is never sent automatically when PTT is released.
The radio retains one pass at a time: the upcoming pass becomes current at AOS, then tracking rolls forward to the next
pass after LOS. A missing position, altitude, valid UTC, unavailable record or stale TLE leaves the session unavailable
and does not apply a correction. The radio restores ordinary channel frequencies when tracking stops, the channel/VFO
context changes, scan or hotspot mode starts, or required inputs become invalid.

Press **OK** again while the active satellite is selected, or leave its detail screen, to stop tracking and restore
the previous channel or VFO settings; leaving the screen shows a "Tracking stopped" notice. Opening the TLE editor or
Quick settings does not stop tracking. If transmitting, the radio restores the settings after transmission ends.Starting a different satellite replaces the current tracking session. Tracking never bypasses normal transmit band,inhibit or active-transmission rules.
