<!-- SPDX-License-Identifier: MIT -->

# DMR repeater roaming

A user radio on a wide-area system of conventional DMR repeaters (Tier II, repeater mode) keeps a list of repeater
sites that carry the same talkgroups. While idle it watches how well it hears the site it is on; when that site gets
weak it looks at the other sites and moves to the strongest one it can use. The user can lock the radio to the site
it is on, or ask for a search at once. Switch: `CONFIG_DMR_ROAMING`, a child of `CONFIG_DMR`, on by default
(`docs/build-config.md`). Roaming itself is off until the user turns it on for a zone.

## 1. Legal and standards position

ETSI TS 102 361-1..4 define no roaming procedure for Tier II conventional operation: a repeater neither announces its
neighbors nor tells a radio to move. Tier III (trunking, TS 102 361-4) has its own registration and hunting
procedures, which do not apply to conventional repeaters. Roaming between conventional repeaters is therefore a
radio-side feature, and this one uses only:

- the received signal strength the RF chip reports (`hal_radio_t.status`),
- the presence of the repeater's outbound: base station (BS) sourced sync with the expected color code, which the
  call stack already tracks for its own repeater handling (`dmr_call_view_t.repeater_up`),
- optionally the standard BS downlink activation CSBK (BS_Dwn_Act, TS 102 361-2), the same burst the radio sends
  before every transmission through a repeater that is not up, to wake a silent site for a measurement.

It does not implement, imitate or interoperate with any vendor's multi-site signaling: no Motorola IP Site Connect,
Capacity Plus or Linked Capacity Plus site beacons, rest channel or site announcements, no Hytera XPT, and no
about 2030-2032; nothing here decodes or sends what those systems send. Picking the strongest of several programmed
channels by signal strength, and waking a repeater with the standard activation burst, are generic radio behavior
that predates those systems (scan with priority, "strongest signal" selection in analog trunked and conventional
radios).

A "beacon check" (periodic look at the other sites, section 4.4) only listens for the standard BS outbound with the
right color code. A repeater that keys up on its own (an identification, an idle beacon, network traffic) is heard
like any other outbound; the radio decodes nothing proprietary from it.

## 2. The roam list (sites)

A **site** is a channel in the current zone that:

- is DMR, not RX-only, and a repeater channel (TX frequency set and different from RX, not "force DMO"),
- has its RX frequency inside one of the board's bands,
- talks to the same talk target as the channel the user picked (the resolved contact ID and call type; two channels
  with no contact both talk to the default talkgroup 9),
- and, when any channel of the zone has the legacy **Roaming** flag set (section 5.2), has that flag set too.

The sites are the first `ROAM_SITES_MAX` (16) such channels in zone order. Roaming is **active** when the zone is a
roaming zone (section 5.1), the radio is on one of its sites (not the VFO, not an FM channel, not a channel that fails
the rules above), and the zone has at least two sites.

The channel the user picked is the **home** site. Roaming moves the radio between sites without changing home; a
channel, zone or VFO change by the user (keys, Find, the debug link, a CPS write that moves the channel) makes the new
channel home and restarts the minimum dwell. The roaming moves themselves are remembered like any channel change, so
the radio powers up on the site it was on.

Why a zone and not a new list: the legacy codeplug layout that qdmr and the legacy CPS write
(`legacy-cps-interop.md` 7) has no roaming list or roaming zone record, and the native codeplug
(`codeplug-native.md`) is not built yet. A zone whose channels share their talkgroups is what a user of a multi-site
system already programs, so it is the natural roam list; the per-channel Roaming flag the legacy layout does have
narrows it when a zone mixes sites with other channels. When the native codeplug exists, the zone record gets a
"roaming zone" flag and the threshold values (section 9, decision 1), and the settings store bits become its default.

## 3. What the radio measures

- **Site heard**: `dmr_call_view()->repeater_up`: a burst with BS sync and the channel's color code within the last
  300 ms (`DMR_CALL_REPEATER_HOLD_MS`). Bursts with another color code are dropped by the call stack before they
  count, so a co-channel repeater with another color code is never taken for ours.
- **Signal**: `hal_radio_t.status().rssi_dbm`, read every poll (25 ms) while the site is heard. On the DM-1701 this is
  the AT1846S RSSI (`hardware-dm1701.md` 5.9); the HR_C6000's own RSSI registers are not used.
- **Current site average**: an exponential average of those readings (weight 1/8 per reading), restarted on every
  site change. Readings taken while the radio isn't hearing the site don't count, so a silent repeater keeps its last
  average until it is too old (`ROAM_FRESH_MS`, 60 s): then the site's signal is **unknown**.
- **Candidate measurement**: during a search, the strongest reading taken while the candidate is heard, after the
  settle time.

A conventional repeater only transmits while it carries traffic (plus its hang time), or when something wakes it.
Passive measurement therefore only sees sites that are busy; section 4.5 covers silent ones.

## 4. Behavior

### 4.1 State machine

| State | Meaning | Leaves to |
|---|---|---|
| `off` | roaming is not active (section 2) | `watch` when it becomes active |
| `watch` | on a site, measuring it; a search starts when a trigger is pending and nothing blocks it | `search`, `locked`, `off` |
| `search` | visiting the other sites one after another, then choosing | `watch` (moved or stayed), `watch` on abort (back to the origin) |
| `locked` | the user locked the radio to this site: nothing moves it | `watch` on unlock or "roam now", `off` |

### 4.2 Triggers

| Trigger (`roam.why`) | When | Waits for |
|---|---|---|
| `weak` | the current average is below the threshold, and has been for `ROAM_WEAK_MS` (5 s) with at least `ROAM_WEAK_SAMPLES` (20) readings in that time | the minimum dwell and the retry back-off |
| `tx_fail` | a PTT or data transmission failed because the repeater did not wake (`TXDENY_NO_REPEATER`) | nothing but the block list (it bypasses dwell and back-off) |
| `manual` | "Roam now" (UI, debug link) | the block list; clears a lock |
| `check` | the beacon check interval passed (`roam_check_s`, 0 = off) | the minimum dwell |
| `stale` | wake probing is on and the current site has not been heard for `ROAM_STALE_MS` (10 min) | the minimum dwell and the retry back-off |

A trigger is latched: a weak call that ends with the repeater's hang time still starts its search once the radio is
idle and nothing blocks. The latch clears when the average climbs back to the threshold (for `weak`) or when the
search runs.

### 4.3 Never roam while

The block list (`roam.blocked`), checked before a search starts:

| Reason | |
|---|---|
| `mode` | VFO, an FM channel, not on a site |
| `hotspot` | the host link owns the modem |
| `scan` | a scan runs (the user's scan wins) |
| `tx` | PTT held, transmitting, a refusal still shown, or the data service sending (`dmr_call_data_busy`) |
| `call` | a call heard on our slot (played or not), or the squelch state RX |
| `reply` | the reply window or a one-shot target is running |
| `quiet` | less than `ROAM_QUIET_MS` (3 s) since the last reception or transmission ended: the repeater's hang time and the other side's answer |
| `override` | a temporary change on the channel (power, CC, TS, mode...), a talk target chosen for now, hear all latched, monitor held |
| `emergency` | an emergency is in progress (`CONFIG_DMR_EMERGENCY`) |

A request from the user (any key action that reaches the radio core: PTT, channel or zone change, monitor, a
temporary change...) during a search ends it at once: the radio goes back to the site it started from (the origin)
before the request is handled, so PTT always transmits on the site the user saw before the search. A call that plays
on a candidate during the search ends the search early on that candidate if it qualifies (section 4.6), so the call
keeps playing; otherwise the search goes on.

### 4.4 The search

1. The visit list is every site except the origin, in zone order; the origin is visited too when its signal is
   unknown (section 3).
2. For each: tune (the radio core's normal channel change; the call stack is reconfigured for the site's color code),
   wait `ROAM_SETTLE_MS` (400 ms: longer than the 300 ms the call stack keeps the last site's outbound alive), then
   listen `ROAM_LISTEN_MS` (1.5 s), or `ROAM_PROBE_MS` (2.5 s) when a wake probe is sent (section 4.5). Readings are
   taken as in section 3.
3. After the last visit the radio chooses (section 4.6) and tunes the choice, or the origin.

A search over 4 other sites takes about 7.6 s passive, 11.6 s with probes. The status strip says `ROAM` during it
and the site name on Home follows the visits, as it does for a scan.

### 4.5 Silent repeaters: the wake probe (optional)

`roam_probe` = `wake` sends one BS_Dwn_Act CSBK on each visited site that is not already heard, through the call
stack's data path (`dmr_call_data_tx`, which wakes the repeater with its own activation bursts and retries, up to
`DMR_CALL_WAKEUP_ATTEMPTS`). A repeater in range answers by keying its outbound for its hang time, which the radio
measures. Probes are only sent when the radio could transmit on that channel anyway: a DMR ID is set, TX inhibit is
off, the battery is not critical, the TX frequency is in a TX band. They are counted (`roam.probes`).

Because the probe transmits and keys up every repeater in range, it is **off by default** (P7: no automatic
transmission until the user turns it on) and settable only through the settings (debug link setting 23) for now.
With probing off, roaming relies on traffic: a repeater nobody uses is not found until it carries a call, but a
failed transmission (`tx_fail`) still starts a search, and "Roam now" while the network is busy finds the others.

### 4.6 Choosing

Values: T = the threshold (`roam_rssi_dbm`, default -105 dBm), H = the hysteresis (`roam_hyst_db`, default 6 dB),
O = the origin's signal (its average, or its measurement from this search; unknown = -200; for `tx_fail` the origin
counts as unknown unless it was heard during the search).

- A candidate **qualifies** when it was heard and its measurement is at least T.
- B = the strongest qualifying candidate other than the origin.
- If home qualifies, is not the origin, is within H of B, and is at least O + H: go home (return to home wins ties).
- Else if B is at least O + H: go to B.
- Else stay on the origin.

Moving restarts the minimum dwell (`ROAM_DWELL_MS`, 30 s) and the average (seeded with the measurement). Staying
starts the retry back-off for `weak` and `stale`: 30 s, doubled after each fruitless search up to 240 s, back to 30 s
after a move or once the site is good again (average at least T + H).

### 4.7 Lock and roam now

- **Lock to this site**: state `locked` until unlocked, "roam now", or a channel change by the user. The site does not
  become home; the lock is RAM only.
- **Roam now**: a `manual` search at once (the block list still applies; blocked, it waits as a latched trigger and
  the UI says why).

## 5. Storage

### 5.1 Settings store (`settings-store.md` 3)

Appended to `settings_t`, layout 1:

| Field | Meaning | Default |
|---|---|---|
| `roam_rssi_dbm` (i8) | threshold T, -125 .. -60 | -105 |
| `roam_hyst_db` (u8) | hysteresis H, 1 .. 30 | 6 |
| `roam_probe` (u8) | 0 passive, 1 wake probe | 0 |
| `roam_stored` (u8) | 1 = the three above and `roam_check_s` are set | 0 |
| `roam_check_s` (u16) | beacon check interval, 0 = off, else 30 .. 3600 | 0 |
| `roam_zones[10]` | bit z % 8 of byte z / 8: zone z is a roaming zone (zones 0 .. 79) | all 0 |

The values reach the radio core as `radio_settings_t.roam_*`. The zone bits are written by the radio core when the
user turns roaming on or off for the current zone. They follow the zone index, so a CPS write that reorders or
deletes zones moves the flags with the indices (section 9, decision 2).

### 5.2 Legacy codeplug

The legacy channel record's LibreDMR flag 1 bit 0 "Roaming" (`legacy-cps-interop.md` 7, offset 0x26) is read into
`channel_t.roam`. It is never written by the radio (the encoder keeps the bit as read). When any channel of a roaming
zone has it, only those channels are sites. Whether qdmr exposes this bit for the legacy layout is not checked yet
(section 9, decision 3).

## 6. Interfaces

### 6.1 Radio core

- `radio_req_roam(cmd)` (`RREQ_ROAM`): `RROAM_ZONE_OFF` 0, `RROAM_ZONE_ON` 1, `RROAM_ZONE_TOGGLE` 2, `RROAM_NOW` 3,
  `RROAM_LOCK` 4, `RROAM_UNLOCK` 5, `RROAM_LOCK_TOGGLE` 6.
- `radio_view_t.roam` (`radio_roam_view_t`): zone flag, state, trigger, block reason, site count, current and home
  index, the current average, counters, and per site its channel number and last measurement.
- The engine is `core/radio/roam.c` (pure logic: sites, averages, triggers, the search sequence, the choice; no
  hardware and no codeplug, like `scan.c`); `core/radio/radio_sm.c` builds the site list, tunes, measures and applies
  the block list. The scan engine is not reused for the search: it stops on a signal, roaming has to measure every
  site and compare. What is shared is the radio core's tuning path (`set_channel` + `tune`), the scan's settle idea and
  the call stack's repeater tracking.

### 6.2 UI (`docs/ux`)

- **Home**: in DMR chips, `Roam` after the repeater chip when roaming is active (`Lock` when locked, amber `Roam?`
  when the current site is below the threshold). The status strip shows `ROAM` (amber) during a search, as it shows
  `SCAN`.
- **Tools > Roaming** (Hub 5, last row: 8, or 7 in a build without diagnostics, which keeps 7): 1 "Roam in this
  zone" On/Off (the zone flag), 2 "Roam now" (goes Home, where `ROAM` shows the search), 3 "Lock to this site"
  On/Off, and a body with the state: the site (its position in the list and the channel name), its signal, or why
  roaming is not available here ("Turn it on for this zone", "This channel is not a repeater site", "Needs two sites
  in the zone with the same talkgroup", not in the VFO). Rows 2 and 3 are dimmed while roaming is off here. Roaming
  is not in Quick settings: the nine tiles are taken (`information-architecture.md` 5) and roaming is set per zone,
  not changed per call.
- Toasts: "Roaming on for <zone>", "Roaming off", "Moved to <channel>" (after any move, the radio core's short scan
  hold beep when beeps are on), "Searching sites", "Locked to <channel>", "Roaming again" (unlocked), "Turn roaming on
  first" / "Can't roam on this channel" (rows 2 and 3 while roaming is off here).

### 6.3 Debug link

View section 12 `roam` (`docs/spec/debuglink.md` 3.7): `roam.built roam.zone_on roam.state roam.why roam.pending
roam.blocked roam.weak roam.sites roam.site roam.home roam.channel roam.rssi_dbm roam.searches roam.roams roam.stays roam.aborts roam.probes
roam.triggers set.roam_rssi_dbm set.roam_hyst_db set.roam_probe set.roam_check_s` and `site.<i>=<channel>:<rssi>` for
each site (rssi 0 = not measured). RADIO op 26 `cmd u8` = `radio_req_roam`; settings 21 `roam_rssi_dbm` (i16 as u16,
two's complement), 22 `roam_hyst_db`, 23 `roam_probe`, 24 `roam_check_s` through op 24.

## 7. Timings and constants

| Name | Value | Why |
|---|---|---|
| `ROAM_SITES_MAX` | 16 | RAM; a wide-area zone rarely has more |
| `ROAM_SETTLE_MS` | 400 | the call stack's 300 ms repeater hold plus the retune |
| `ROAM_LISTEN_MS` | 1500 | four superframes: a busy repeater is heard, an idle one is not |
| `ROAM_PROBE_MS` | 2500 | three wake-up attempts of 600 ms plus the measurement |
| `ROAM_DWELL_MS` | 30000 | minimum time on a site before an automatic search |
| `ROAM_WEAK_MS` / `ROAM_WEAK_SAMPLES` | 5000 / 20 | a weak site, not a fade |
| `ROAM_QUIET_MS` | 3000 | after the last reception or transmission (hang time, the answer) |
| `ROAM_FRESH_MS` | 60000 | an average older than this is unknown |
| `ROAM_RETRY_MS` .. `ROAM_RETRY_MAX_MS` | 30000 .. 240000 | back-off after searches that stayed |
| `ROAM_STALE_MS` | 600000 | wake probing: a silent home site is checked this often |

## 8. Tests

- Host (`tests/host/roamTest.c`): the engine alone (averages, weak latch, dwell, back-off, the choice with
  hysteresis, home preference, lock, abort) and through `radio_sm.c` against a fake board whose repeaters are
  frequencies with an RSSI and a color code, driven by the simulated modem (BS idle bursts while a site is "keyed",
  BS_Dwn_Act on air wakes it): no roam while a call plays, during PTT, in the reply window, with a temporary change, a
  manual target, hear all or monitor; weak site -> search -> the strongest qualifying site; hysteresis keeps the radio
  on a site that is not enough worse; return to home within the hysteresis; a candidate with the wrong color code is
  never chosen; PTT during a search returns to the origin first and transmits there; a failed wake-up on PTT starts a
  search; wake probing finds a silent site; lock and roam now; the zone flag is stored; `CONFIG_DMR_ROAMING=0` builds.
- UI flow (`tests/host/uiFlowTest.c`): Tools > Roaming toggles the zone flag in the press budget; "Roam now" asks the
  radio core.
- HIL (`tests/hil/test_roaming.py`, listed in `tests/hil/README.md`): radio only: the `roam` section, the settings
  round trip, the zone toggle and back, a passive roam now on the current zone (ends on one of its sites, nothing
  sent), lock and roam now ending it; with `--allow-rf --allow-service-stop --roam-rx --roam-tx`: the SDR bench runs
  `bench_repeater.py` as a second site and the radio, started on a dead site with the wake probe on, wakes it and
  moves there. A passive variant (the bench repeater carrying a bench call while the radio searches) needs the bench
  to transmit through its own repeater and is left for later.

## 9. Open decisions

1. The roaming zone flag and the thresholds as per-zone fields of the native zone record (when it exists), instead
   of radio-wide settings plus zone index bits.
2. Zone bits keyed by index break when a CPS reorders zones. A name hash would survive reordering but not renaming.
3. Whether qdmr exposes the legacy Roaming bit; if not, a qdmr extension or a radio-side channel editor field.
4. Wake probing in the UI (Settings > Talk & listen) and whether a probe should use the lowest power level.
5. Whether a move should be announced by a beep, a voice prompt or nothing (today a short beep with key beeps on).
6. Talk target matching: by resolved contact ID (today) or also by listen list, for systems where sites carry the
   same talkgroups on different timeslots.
