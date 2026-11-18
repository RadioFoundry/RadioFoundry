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
AO-91 start selected when present. The page can push the selection directly over USB/serial or create a JSON bundle
for the PC utility. Direct push requires a supported browser and a secure context (HTTPS or localhost).

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
PC synchronization replaces the satellite TLE inventory and selected frequency profiles only. It does not create,
modify, or remove codeplug zones or channels. Check the [AMSAT live FM satellite list](https://www.amsat.org/live-fm-satellites/)
for current operating status before transmitting. The built-in frequency snapshot reflects that page's October 31,
2026 update. SO-50 requires a 2-second 74.4 Hz CTCSS transmission to arm its timer; switch back to its normal
67.0 Hz uplink tone afterward. Use `read` to inspect installed IDs and epochs:

```sh
python tools/satellite_tle.py read
```

Frequency profiles are separate from orbital elements. The daily TLE bulletin contains no frequencies; the page uses
the known entries in AMSAT's live FM table, which currently covers all listed FM satellites. Non-FM objects are omitted.
A profile list has a maximum of 10 entries. Sync replaces the radio's satellite TLE and frequency tables with the
selected satellites, so unselected satellites disappear from the tracking list. The codeplug is left unchanged. The
PC utility also reads or changes the built-in profile ID list directly:

```sh
python tools/satellite_tle.py profiles
python tools/satellite_tle.py profiles --id 25544,27607,43017
```

Protocol-v2 radios can still install schema-v1 orbital bundles additively. Selected-set replacement requires protocol
v4; schema-v3 frequency bundles require protocol v5. Protocol v5 supports up to four selected frequency profiles;
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
current date and time. The list is ordered by time to AOS and shows each pass's maximum elevation. Up and Down
select a pass; **OK** opens its details and starts tracking. The selected satellite is remembered across power cycles.
The firmware image contains no TLEs. Load selected records into the radio's flash store from the site's Satellite TLE
Update page or with `tools/satellite_tle.py sync`. With an empty store, Satellite shows **No TLEs: load from site**.
The pass list shows the common age of the installed TLE set when current UTC is available. At 14 days, it warns once to
refresh the set from the site and improve prediction accuracy.
Pass and Doppler values use an approximate Keplerian orbit model, not SGP4. They can differ from a precision ephemeris,
especially as the TLE ages, so keep the installed elements current.
Tracking gets its nominal RX/TX frequencies and optional transmit CTCSS tone from the persisted catalog-ID profile,
not from the selected VFO or codeplug channel. A satellite without a stored profile cannot start tracking. While
tracking, the radio uses FM and 12.5 kHz bandwidth; stopping restores the prior radio settings. TLE providers supply
orbital elements, not communication frequencies. PC synchronization does not touch the codeplug; editing an existing
channel does not change the runtime profile used for tracking.

The screen reports visibility, receive and transmit Doppler offsets, the current Doppler-adjusted RX and TX frequencies,
and the next pass's AOS and LOS countdowns. The north-up circular sky plot shows the predicted path from horizon to
zenith and marks the satellite's current position while it is visible. On 128x64 displays, the detail screen shows the
AOS and LOS countdowns instead of the plot. During an active transmission, the TX row shows the frequency in use;
tracking continues calculating the latest target in the background and applies it as soon as transmission ends.
While SO-50 is actively tracked, long-press **OK** on its detail screen to send the 74.4 Hz CTCSS arm signal for two
seconds. The radio applies its normal transmit checks and uses the active Doppler-adjusted uplink. This does not change
the normal 67.0 Hz profile tone, and the arm signal is never sent automatically when PTT is released.
The radio retains one pass at a time: the upcoming pass becomes current at AOS, then tracking rolls forward to the next
pass after LOS. A missing position, altitude, valid UTC, unavailable record or stale TLE leaves the session unavailable
and does not apply a correction. The radio restores ordinary channel frequencies when tracking stops, the channel/VFO
context changes, scan or hotspot mode starts, or required inputs become invalid.

Press **OK** again while the active satellite is selected to stop tracking and restore the nominal frequencies.
Leaving the Satellite screen alone does not stop tracking. Starting a different satellite replaces the current
tracking session. Tracking never bypasses normal transmit band, inhibit or active-transmission rules.