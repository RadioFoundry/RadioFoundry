<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Satellite Tracking

Satellite tracking predicts a selected satellite's position and visible passes, and can continuously compensate its
configured uplink and downlink frequencies for Doppler shift. It is an optional radio feature. It does not start a
transmission, change the radio's supported bands, or bypass a channel's transmit restrictions.

## 1. Inputs and ownership

Tracking needs three independent inputs:

| Input | Requirements |
|---|---|
| Orbital elements | A valid, paired two-line element set (TLE) and its epoch. Reject malformed or incomplete sets. The element epoch and current UTC determine whether the prediction is usable. |
| Observer location | Latitude, longitude and altitude from the selected position source. A missing or stale position stops pass and Doppler updates; fixed position is usable when selected. |
| Current time | Valid UTC. A local monotonic tick may schedule work but is not a substitute for UTC in orbit propagation. |

Satellite calculations are pure, bounded work: they take a TLE, observer state and UTC instant, then return a result.
They do not read board registers, access settings storage, wait, or tune hardware. Position remains owned by
`core/position`; the radio state machine remains the only owner of an applied channel tune. Satellite tracking supplies
frequency targets to that state machine. `core/satellite/service.c` coordinates tracking requests, pass scans and their
cache without RTOS or radio-state-machine dependencies. The radio task supplies current radio context and applies the
service's updates. `core/satellite/store.c` owns persistent TLE storage; debug-link commands adapt its transfer API.

Use the lightweight Keplerian model implemented by `core/satellite/orbit.c`: interpret the TLE mean elements as an
elliptic orbit, integrate the line-1 mean-motion derivative, solve Kepler's equation with a bounded Newton iteration,
and apply secular J2 rates to the ascending node and argument of perigee. Rotate the state to Earth-fixed coordinates
using Greenwich sidereal time, then calculate observer-relative azimuth, elevation, range and range rate. This is an
approximation, not SGP4/SDP4: it omits periodic perturbations and detailed atmospheric drag, and its pass and Doppler
estimates can drift from a standards-based ephemeris, particularly as the TLE ages. Tests verify the model's orbital
invariants and cross-check positions against published SGP4 vectors at 1, 3 and 7 days. Propagation retains the
fractional second of the TLE epoch even though the radio clock supplies whole UTC seconds. Do not treat TLE elements as
long-term orbital constants: expose their epoch and reject or mark stale elements outside the supported age. Builds and
the radio do not fetch or embed TLE data. Load selected records into the radio's flash store with
`tools/satellite_tle.py sync` or the site's Satellite TLE Update page.

Firmware propagation uses integer fixed-point arithmetic, CORDIC trigonometry and integer roots. Keep double-precision
math, libm calls and 64-bit division out of the firmware path.

## 2. Satellite and pass results

For a selected satellite and UTC instant, the calculation reports its topocentric azimuth, elevation, range and range
rate. Elevation at or above the local geometric horizon is visible. A pass has an acquisition of signal (AOS), loss of
signal (LOS), maximum elevation and the time of maximum elevation. Prediction is bounded to a 24-hour horizon and
retains only the earliest current or upcoming pass. It stops sampling at that pass's LOS; after LOS, tracking starts a
new search for the next pass. The retained pass includes at most 16 packed azimuth/elevation samples for its sky path.

Pass prediction may be spread across service iterations so a long forecast cannot block radio, input or UI work. The
pass-list scan performs one orbit initialization or propagation sample per radio tick, with the interval measured from
completion so a slow propagation step cannot trigger a catch-up burst. A partial prediction is publishable
as soon as the first pass is found, and its path grows as samples arrive. If inputs
change, the result must be invalidated or restarted; small GPS variations do not count as an observer change, while a
movement of about 500 m horizontally or 100 m vertically restarts the forecast. Results from different TLE epochs or
observer locations must not be mixed. A pass already in progress is represented as visible now; propagation looks back up to 30 minutes to recover
its AOS when available. The UI reports countdowns to AOS and LOS and plots the retained pass on a north-up polar sky
plot, with elevation mapped from the horizon at the rim to the zenith at the center. On 128x64 displays, the plot is
replaced by a compact row with the AOS and LOS countdowns. AOS and LOS endpoints are interpolated to the horizon, and
adjacent projected samples are smoothly connected across azimuth wrap. The live position is marked separately.

The radio task allocates its satellite service workspace from the RTOS heap at startup and retains it while running.
Allocation failure triggers the normal RTOS assertion. The task-stack hardware test checks the heap's minimum free
space; this must be measured with the feature enabled as well as disabled.

Once the pass path is predicted, retain that arc for the pass. While the satellite is visible, update only its live
position marker; do not restart the path for observer-position drift. Resume prediction after the pass ends so the next
upcoming arc can be shown.

The completed pass list is cached for five minutes. Returning from a satellite details screen reuses the same results
while that cache is fresh; an expired or absent cache starts a new scan.

## 3. Doppler compensation

Let range rate `v` be positive when the satellite is moving away from the observer, in meters per second, and let `c`
be the speed of light. For nominal downlink `f_rx` and uplink `f_tx`, the radio targets are:

$$
f_{rx,target} = f_{rx}(1 - v/c), \qquad
f_{tx,target} = f_{tx}(1 + v/c).
$$

Approaching motion therefore raises the received downlink target and lowers the transmitted uplink target; receding
motion does the reverse. Calculate both from the configured nominal frequencies on every update. Never apply a new
correction to an already corrected frequency. Round only at the radio's supported tuning resolution, and retain enough
precision before rounding to avoid a direction-dependent bias.

The receive and transmit nominal frequencies are independent. Apply the correction separately to each, including when
they are in different bands. A missing or zero frequency disables correction for that side. The displayed nominal
frequency and applied frequency should remain distinguishable, with the current signed offset and the current Doppler-
adjusted RX/TX frequencies available to the user.

## 4. Tracking behavior and radio safety

Tracking is an explicit user-selected mode for one satellite. Its nominal RX/TX frequencies and optional TX CTCSS come
from the persisted catalog-ID frequency profile, independently of the selected VFO or codeplug channel. IDs without a
stored profile cannot start tracking; do not substitute the selected channel's frequencies. Tracking temporarily
selects FM mode and 12.5 kHz bandwidth, then restores the prior radio settings when stopped. Optional channel
synchronization only creates editable codeplug entries; those entries are not used by runtime tracking. While enabled,
update the orbital state and frequency targets at a bounded cadence (nominally once per second). Do not retune the receiver
for changes smaller than a deadband chosen to avoid audible interruptions and repeated synthesizer programming; the
deadband must not be smaller than the board's effective frequency step. Continue calculating and publishing Doppler
targets while PTT is held or a transmission is active, but keep the applied transmit frequency fixed for that
transmission. Apply the newest valid target as soon as the transmitter is off. The radio HAL has no operation for
changing TX frequency while keyed, so do not call the initial TX setup again mid-call.

Every target still passes through the normal radio state machine and board band/range validation. A target rejected by
those checks is not applied. Tracking must not undo a transmit inhibit or other channel safety rule. When tracking is
stopped, the selected channel changes, or required inputs become invalid, remove the tracking override and restore the
channel's ordinary frequencies. Do not retain an old Doppler correction as a new nominal frequency.

No valid TLE, expired TLE, invalid UTC, stale observer position, non-finite calculation or out-of-range target means no
automatic retune. The user-visible state should distinguish unavailable tracking from a valid pass below the horizon.
Reacquiring valid inputs may resume tracking only while the same satellite and frequency pair remain selected.

## 5. User-facing behavior

Tools > Satellite first predicts passes for every locally installed TLE using one current GPS position and UTC
snapshot. The list is ordered by AOS, shows time to AOS in hours and minutes (`H:MM`) and maximum elevation, and
reports prediction progress. Missing position, altitude or valid UTC leaves the list unavailable instead of using stale
inputs. Its AOS countdown updates as time passes from the scan's UTC snapshot. The pass list shows the age of the oldest
stored TLE when UTC is available, updates it as time passes, and warns once per inventory generation when it reaches
14 days. TLEs fetched together can have different epochs. When prediction completes with passes, the first pass is
focused; returning from details preserves the selected row while the results remain available. Selecting a pass persists its record,
starts tracking, and opens that satellite's detail screen with Doppler offsets, pass countdowns (`↑ in` to
AOS and `↓ in` to LOS) and the sky path. Countdown minutes round up while time remains. The detail screen's pass data
continues updating in bounded service steps. The rise countdown is left-aligned and the set countdown is right-aligned.
When the satellite is visible, the rise countdown shows `Now` in the normal text style.
The tracking status row is reserved for unavailable or inactive tracking. The nominal RX/TX pair and optional TX tone come from the selected
satellite's persisted catalog profile. An active session keeps those nominal
values even as Doppler targets
change. While PTT is held, the detail screen shows the applied TX frequency during on-air transmission and otherwise
continues to show the live Doppler target; the newest target is applied when transmission ends.
While tracking SO-50, a long press of OK on its detail screen explicitly sends the 74.4 Hz CTCSS timer-arm signal for
two seconds. The radio uses its normal transmit checks and the active Doppler-adjusted uplink. The action does not
change the profile's normal 67.0 Hz tone, and PTT release never starts an arm transmission.
The store derives the arm capability from the catalog ID when it validates a TLE; the UI uses that capability rather
than identifying the satellite by name or number. This does not change the stored TLE format.

Manual channel and VFO operation remain unchanged when tracking is off. Leaving the Satellite detail screen stops
tracking and restores the prior channel or VFO frequencies, mode, bandwidth and tones. A temporary screen above it,
such as the TLE editor or Quick settings, does not stop tracking. OK on the active satellite also stops tracking.
Restoration waits until an active transmission ends. Starting another satellite replaces the previous tracking session.
Changing channel/VFO context, entering scan or hotspot mode, or losing required inputs stops the session.

Press **#** on a selected satellite's detail screen to replace its orbital elements on the radio. Enter line 1 and
line 2 as separate exact 69-character fields with the keypad editor. The catalog ID and name stay unchanged. The radio
checks line format, catalog identity, epoch and checksums, then writes a new complete inventory generation; an
incomplete or invalid pair leaves the installed record unchanged. Manual entry does not require a computer or debug
link. Editing is a full-line replacement, not an in-place cursor edit.

## 6. PC TLE workflow

Orbital data is gathered and curated on a PC, not downloaded by the radio. The host utility is `tools/satellite_tle.py`.
It accepts an explicit selection of satellites by stable catalog ID, with optional provider groups used only to help
find candidates. A group download must not silently install every returned satellite.

The catalog view includes the AMSAT daily amateur-satellite bulletin at
`https://www.amsat.org/tle/daily-bulletin.txt` and the ARISS ISS feed at `https://live.ariss.org/iss.txt`. The PC tool
also supports CelesTrak's public GP endpoint, `https://celestrak.org/NORAD/elements/gp.php`, queried with
`CATNR=<comma-separated catalog IDs>&FORMAT=TLE`. Provider URLs are configurable but must use HTTPS, including after
redirects. Catalog browsing is separate from installation: the user selects catalog IDs, and a response or group is
never installed wholesale. For ID-based provider requests, the returned ID set must exactly match the selection.

The site Tools page links to a standalone Satellite TLE page. It lists IDs in the curated FM frequency catalog and
allows up to 10 profiles. Non-FM TLE objects are omitted. The AMSAT
button loads the fixed daily bulletin through a same-origin site endpoint; Browse remains available for another local
source. When AMSAT is selected, the page fetches the bulletin again without caching before each radio push or bundle
download. A failed refresh stops the operation; a local file is used as supplied. Checked selections persist across
searches. The daily TLE bulletin itself contains no frequency data. The page validates selected IDs, checksums, orbital
fields and age, then creates a schema-v3 bundle or
pushes TLE and frequency data over Web Serial/WebUSB. It verifies the installed IDs and epochs against the selected
TLEs before reporting success, including when a selected ID was already installed. A direct browser connection requires a supported browser and a
secure context (HTTPS or localhost). TLE IDs and profile IDs must match exactly. `tools/satellite_tle.py install`
supports the same schema-v3 bundle. Unselected installed TLEs disappear from the radio's satellite-tracking inventory.
After a verified install or update, the web flasher offers the page when the reported or installed image's feature
list includes `SATELLITE_TLE`; it does not send a separate satellite capability command.

The commands `catalog --source amsat|ariss` browse a provider, `sync --source amsat|ariss --id <catalog ID>` fetches
and installs only the selected IDs, and `read` queries the radio's installed inventory. When no IDs are supplied to
`sync` in an interactive terminal, the tool displays the catalog and prompts for an explicit list. Non-interactive
sync requires `--id` values.

An offline bundle is UTF-8 JSON with a provider URL, an ISO-8601 UTC `fetched_at`, and a `records` array. Schema v1
contains records only and remains installable. Schema v2 also contains `profile_ids`, an ordered set of 1-10 IDs from
the firmware's built-in frequency-profile catalog. Schema v3 contains `profile_ids` and a matching `profiles` array
with `catalog_id`, `rx_hz`, `tx_hz`, and `tx_tone_tenths`. Each record has `catalog_id` (integer), `name`, `line1`, and
`line2`. The bundle contains no other radio mode settings. The default accepted TLE age is 30 days at the time of validation; the utility allows a
caller-selected limit. Older elements are shown and require explicit acceptance (or the command is cancelled); a
future epoch more than one day ahead of the PC's UTC clock is invalid.

TLE text is exactly the two 69-character lines, without line endings. Validation checks the standard line number,
catalog-ID columns, checksum column, epoch syntax and calendar day, and finite/ranged orbital fields used by propagation.
Every entry is validated before any install begins. Duplicate IDs, malformed entries, or a provider response that
omits or adds an ID reject the complete batch.

Install uses debug-link command `0x26` (`SATELLITE`) and is available only when capability operation `INFO` succeeds.
Protocol-v7 adds `SYNC_CHANNELS_STATUS=13` to the operations: `INFO=0`, `LIST=1`, `BEGIN=2`, `DATA=3`,
`COMMIT=4`, `ABORT=5`, `SYNC_CHANNELS=6`, `PROFILE_GET=7`, `PROFILE_SET=8`, `REPLACE_BEGIN=9`,
`PROFILE_STAGE=10`, `PROFILE_VALUES_GET=11`, `PROFILE_STAGE_CHUNK=12`, and `SYNC_CHANNELS_STATUS=13`. `INFO` reports
protocol version, record capacity (`10`) and installed count as `u8, u16, u16`; it is unavailable when the feature is
off or the board has no valid dedicated satellite-data flash area. `LIST` takes a start index `u16` and returns
a count `u8`, then that many `(catalog ID u32, epoch Unix seconds u32)` pairs. The radio advertises a capacity of
10 records. `BEGIN` takes record count `u16`,
serialized byte count `u16`, and CRC-32 `u32`; it returns a transaction ID `u16`. Records are serialized in ascending
catalog-ID order as catalog ID `u32`, UTF-8 name length `u8`, name bytes (1-48 bytes), and the two exact 69-byte TLE
lines. The maximum serialized set is 1,910 bytes. `REPLACE_BEGIN` has the same payload and reply as `BEGIN`; its
committed records become the complete TLE inventory instead of being merged with the old inventory. `DATA` takes transaction ID `u16`, byte offset `u16`, then up to 59 data bytes; the reply is the next expected
offset `u16`. `COMMIT` takes transaction ID `u16` and expected CRC-32 `u32`. These requests fit the debug link's
64-byte request payload limit. `PROFILE_STAGE=10` takes a transaction ID `u16`, profile count `u8`, then up to four
14-byte records `(catalog ID u32, RX Hz u32, TX Hz u32, tone tenths u16)` and is valid only during replacement. This
protocol-v5 operation remains supported. `PROFILE_VALUES_GET=11` returns the complete table in the same record format.
Protocol v6 adds `PROFILE_STAGE_CHUNK=12`: transaction ID `u16`, total profile count `u8`, starting index `u8`, chunk
count `u8`, then up to four profile records. Chunks must be consecutive and together contain exactly the replacement
inventory's profiles. The profile IDs must exactly match the staged TLE IDs; commit refuses an incomplete set.
The radio stores the inventory and profiles in three 4 KB erase sectors (12 KB total): one active
generation, one incoming transaction and one commit target. The satellite area is carved from the existing update
scratch allocation only when `CONFIG_SATELLITE_TLE` is enabled; it reduces TFTP/CPS-fast staging by 12 KB. `BEGIN`
merges selected records with the current set; `REPLACE_BEGIN` atomically removes every omitted record. A disconnect or
failed operation is followed by `ABORT` when possible. After commit, the host re-queries capability, enumerates every
inventory page and compares the complete ID/epoch map with the expected result.
The `read` operation treats the radio inventory as authoritative and remembers names/epochs locally by chip UID (or the selected
port if no UID is available). It can show the last synced list on a later read without contacting a provider. A cached
name is shown only when its catalog ID and epoch still match the radio; changed or unknown entries are not assigned
guessed names.
`ABORT` takes a transaction ID `u16` and discards an uncommitted batch. Firmware continues to support protocol-v2
additive TLE transfers for older clients. Schema-v2 replacement bundles require protocol v4. Schema-v3 bundles require protocol v5 and
stage TLEs and frequency values before one commit; protocol v5 supports up to four selected profiles, while protocol
v6 and later support the full ten. The host reads both back before reporting success. The standalone
`profiles` command requires protocol v3 or newer. `PROFILE_GET` returns up to 10 catalog IDs and `PROFILE_SET` accepts
IDs from the built-in catalog for the legacy profile-only workflow. The PC utility uses replacement for every install
and sync, including schema-v1 bundles, and requires protocol v4 or later. PC TLE synchronization does not invoke
`SYNC_CHANNELS` and does not create, modify, or remove codeplug zones or channels. The separate `SYNC_CHANNELS`
operation remains explicit and is not part of PC install. Removing a TLE from the satellite inventory removes it from
the tracking list without changing user channel data. The persistent satellite-bank format remains independently
versioned.

The separate `SYNC_CHANNELS` operation, when explicitly requested, queues work on the housekeeping task and returns
`BUSY` if another storage job is active. Poll `SYNC_CHANNELS_STATUS` for `IDLE=0`, `RUNNING=1`, `COMPLETE=2`, or
`FAILED=3`. It ensures a `Satellites` codeplug zone contains one channel for each installed TLE, reusing an existing
same-name channel when possible. A missing channel is created from the current channel profile with its name set to the
TLE name (truncated to the codeplug's 16-character limit), FM mode, and transmit enabled. For FM satellites in the
radio's built-in AMSAT frequency table, the new channel uses the listed downlink for receive, uplink for transmit, and
listed uplink CTCSS tone. Other new channels copy the current channel's frequencies. Existing channel frequencies,
tones and other settings are not overwritten. PC installs do not request this operation. The frequency table is a
snapshot of AMSAT's page updated October 31, 2026; operating status can change, so check the live page before
transmitting. SO-50 requires a 2-second 74.4 Hz CTCSS transmission to arm its timer before use; its normal uplink tone
is 67.0 Hz.

The radio's dedicated `hal_flash_map_t.satellite_*` area is separate from the codeplug and reserved from update
staging. It uses three
4 KiB sectors: the active snapshot, the incoming transfer and the next committed snapshot. Each committed snapshot
contains a versioned header, generation, record count, serialized length and CRC-32. The maximum inventory is 10
records (1,910 serialized bytes at the 48-byte name limit). Firmware validates serialized lengths, catalog identity,
line checksums, epochs, uniqueness and ordering, then validates the merged inventory before writing the next
generation; its commit marker is programmed last. The PC validates the additional propagator input ranges before
transfer. At boot the radio selects the newest valid
generation; an erased area means an empty inventory, while invalid non-erased data is ignored unless no valid
generation remains. A power cut during transfer or commit leaves the previous complete snapshot available.

User-configured nominal frequency pairs and radio mode are separate from TLE data: refreshing orbital elements must
not change those settings. Tracking uses the selected satellite's persisted frequency profile, not the current channel
or VFO's frequency pair, and does not infer frequencies or tones from a TLE.

Before install, the PC tool previews the selected entries and validates each pair: both lines are present, checksums
and fixed-format fields are valid, the catalog ID matches on both lines, IDs are unique, the epoch is parseable, and
the element age is within the configured acceptance limit. Reject invalid entries rather than silently dropping
them. Report stale elements and let the user cancel or explicitly accept them. Online fetch is an explicit user action;
offline bundle validation and install do not need a network connection.

Install only to a radio that advertises satellite-data support. Use a dedicated, versioned satellite-data transaction
over the existing debug-link transport, not arbitrary CPS image addresses. The transaction supports capability and
capacity discovery, begin, chunked record transfer, commit and abort. Records carry catalog ID, orbital elements and
their epoch; configured frequency pairs remain independent. The host checks the completion status and reads back the
installed IDs and element epochs before reporting success.

The radio stages a complete batch and makes it visible only after validation and commit. A failed transfer, checksum,
capacity check, disconnect or power loss leaves the previously installed dataset usable. Additive installs replace
matching records and preserve unselected satellites. Replacement installs make the selected records the entire
inventory. A batch with duplicate IDs or any invalid record is rejected as a whole. Firmware without satellite support reports that
capability as unavailable, and the PC tool must not attempt a raw flash fallback.

## 7. Configuration and feature switch

Satellite support is behind `CONFIG_SATELLITE_TLE` in `config/features.mk` and `core/config.h`. It is enabled by default
in full and amateur builds on boards with enough program flash and disabled in commercial builds. A board may default
it off when the feature does not fit. Persistent inventory transfer also needs a dedicated satellite-data flash range
in the board map. With the switch off, no satellite parser, propagator, pass service or tracking UI is included, and
the normal build remains clean. The append-only settings record retains a single unused selection byte so its layout
stays consistent across feature configurations.

Persist the user's selected TLE index using RadioFoundry's settings interface. The nominal frequency pair comes from
the persisted satellite frequency profile, not the current channel or VFO configuration. Treat TLE data as replaceable
orbital input, not as user frequency configuration. A radio without a GPS
may use another valid configured position source; a build without any usable observer source may still inspect stored
satellite data but cannot calculate local passes or Doppler compensation.

## 8. Verification

Host tests cover:

| Test | Checks |
|---|---|
| TLE input | Valid pairs, malformed lines, mismatched satellite identity, invalid checksums/fields and stale epochs |
| Propagation | Keplerian radius/energy bounds, secular-rate velocity consistency, longitude wrapping, horizon crossing and finite outputs |
| Passes | AOS, LOS, maximum elevation, current-pass handling, forecast bounds and invalidation after input changes |
| Doppler | Zero range rate, approach and recession sign, separate uplink/downlink values, rounding and no cumulative drift |
| Tracking lifecycle | Stale or missing inputs, rejected band targets, stop/restore, channel changes and updates during active TX |
| On-radio TLE editing | Selected-record identity/name preservation, invalid checksum rejection, and interrupted replacement commits |
| Satellite UI | TLE editor open/cancel flows, polar path layout and live-position rendering across display geometries |
| Feature switch | A build with satellite support disabled and its dependent code omitted cleanly |
| PC data workflow | Provider responses through fixtures, multi-ID selection, malformed/stale TLE rejection, offline bundles, partial-transfer failure, atomic commit and read-back verification |

Hardware-in-the-loop tests may verify that accepted targets reach the radio state machine and that active TX is not
retuned. They must not transmit unless explicitly opted in by the HIL safety procedure.

## 9. References

- NORAD Two-Line Element set format and mean-element conventions.
- RadioFoundry [position service](position.md) for observer-location sources and freshness.
- RadioFoundry [feature matrix](../feature-matrix.md) for per-radio support.