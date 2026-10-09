<!-- SPDX-License-Identifier: MIT -->

# DMR repeater roaming

A user radio on a wide-area system of conventional DMR repeaters (Tier II, repeater mode) keeps a list of repeater
sites that carry the same talkgroups. While idle it watches how well it hears the site it is on and, when the
repeaters send idle beacons, surveys the other sites in the beacon window. When its site gets weak it moves to the
strongest one it can use, from the survey data when that is fresh, else after a search. The user can lock the radio
to the site it is on, or ask for a search at once. Switch: `CONFIG_DMR_ROAMING`, a child of `CONFIG_DMR`, on by
default (`docs/build-config.md`). Roaming itself is off until the user turns it on for a zone.

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

**Beacons.** Many conventional repeaters can key their standard outbound on their own for a few seconds every
interval while idle ("roaming beacon", "BS beacon"; networked repeaters are often set to do it at the same time).
The radio treats that as what it is on air: a standard ETSI BS-sourced outbound (BS sync, CACH, idle or any other
burst) with the site's color code. It measures it as it measures traffic (section 3) and learns when it comes
(section 4.8). It decodes nothing proprietary from it: no site or neighbor lists, no rest channel, no vendor
CSBK or data. Listening to a repeater at the time it is known to transmit is generic receiver behavior, as is a
periodic look at other programmed channels (scan with priority). The survey (section 4.9) and the periodic search
(section 4.2) only listen. Our own repeater roles can send such a beacon (section 7): idle bursts on the outbound,
nothing else.

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
"roaming zone" flag and the threshold values (section 10, decision 1), and the settings store bits become its default.

## 3. What the radio measures

- **Site heard**: `dmr_call_view()->repeater_up`: a burst with BS sync and the channel's color code within the last
  300 ms (`DMR_CALL_REPEATER_HOLD_MS`). Bursts with another color code are dropped by the call stack before they
  count, so a co-channel repeater with another color code is never taken for ours.
- **Signal**: `hal_radio_t.status().rssi_dbm`, read every poll (25 ms) while the site is heard. The board picks the
  source: the receiver's RSSI, not the DMR modem's (DM-1701: the AT1846S, `hardware-dm1701.md` 5.9; MD-380: the
  IF chain's RSSI on an ADC, `hardware-md380.md` 5.5).
- **Current site average**: an exponential average of those readings (weight 1/8 per reading), restarted on every
  site change. Readings taken while the radio isn't hearing the site don't count, so a silent repeater keeps its last
  average until it is too old (`ROAM_FRESH_MS`, 60 s): then the site's signal is **unknown**.
- **Candidate measurement**: during a search, the strongest reading taken while the candidate is heard, after the
  settle time.

- **Survey measurement**: during a survey visit (section 4.9), the mean of up to `ROAM_SURVEY_READINGS` (4)
  readings taken while the site is heard, after `ROAM_SURVEY_SETTLE_MS` (420 ms: the call stack's 300 ms hold of
  the last site's outbound, then about 120 ms for the PLL, the modem's sync and the receiver's RSSI filter). The
  site's survey average is the mean of its last two survey measurements; a site missed in two visits made while the
  beacons were on the air ("live": its own site or another one was heard in that window) counts as not heard.
- **Traffic**: a call or data on our slot (`RS_RX`, or the call stack not idle) while a site is heard. A keyup that
  carries traffic is never taken for a beacon.

Why 420 ms per visit and not less: all sites share the color code, and the call stack keeps a repeater "up" for 300
ms after its last burst, so the first 300 ms after a retune can't tell the new site from the old one. A visit could
be about 250 ms if the stack forgot the last site on a retune, but then a PTT that ends a visit would always go
through a repeater wake-up on the way back (section 4.9). On the DM-1701, the AT1846S RSSI and the HR_C6000's sync
acquisition need well under 100 ms on a continuous BS outbound (a burst every 30 ms); four 25 ms readings follow.

A conventional repeater only transmits while it carries traffic (plus its hang time), when it beacons, or when
something wakes it. Passive measurement therefore only sees sites that are busy or beaconing; section 4.5 covers
silent ones.

## 4. Behavior

### 4.1 State machine

| State | Meaning | Leaves to |
|---|---|---|
| `off` | roaming is not active (section 2) | `watch` when it becomes active |
| `watch` | on a site, measuring it; a search starts when a trigger is pending and nothing blocks it | `search`, `locked`, `off` |
| `search` | visiting the other sites one after another, then choosing | `watch` (moved or stayed), `watch` on abort (back to the origin) |
| `survey` | a survey round or a confirm visit (sections 4.9, 4.10): on another site for a few hundred ms | `watch` (back, or moved after a confirm), `watch` on any block or user action |
| `locked` | the user locked the radio to this site: nothing moves it (no survey either) | `watch` on unlock or "roam now", `off` |

### 4.2 Triggers

| Trigger (`roam.why`) | When | Waits for |
|---|---|---|
| `weak` | the current average is below the threshold, and has been for `ROAM_WEAK_MS` (5 s) with at least `ROAM_WEAK_SAMPLES` (20) readings in that time | the minimum dwell and the retry back-off |
| `tx_fail` | a PTT or data transmission failed because the repeater did not wake (`TXDENY_NO_REPEATER`) | nothing but the block list (it bypasses dwell and back-off) |
| `manual` | "Roam now" (UI, debug link) | the block list; clears a lock |
| `check` | the periodic search interval passed (`roam_check_s`, 0 = off; a full search, kept for systems without beacons) | the minimum dwell |
| `stale` | wake probing is on and the current site has not been heard for `ROAM_STALE_MS` (10 min) | the minimum dwell and the retry back-off |

A trigger is latched: a weak call that ends with the repeater's hang time still starts its search once the radio is
idle and nothing blocks. The latch clears when the average climbs back to the threshold (for `weak`) or when the
search runs. On an idle system the current site is heard only in its beacons; its average stays known for two
intervals plus the duration (at least `ROAM_FRESH_MS`), and the weak test is also made between readings, so a weak
idle site latches `weak` after its first weak beacon plus `ROAM_WEAK_MS`.

With the survey on and beacons set, `weak` and `manual` first look at the survey data (section 4.10); `tx_fail`,
`check` and `stale` always search.

### 4.3 Never roam while

The block list (`roam.blocked`), checked before a search or a survey round starts, and during a survey visit (any
reason ends the visit at once):

| Reason | |
|---|---|
| `mode` | VFO, an FM channel, not on a site |
| `hotspot` | the host link owns the modem |
| `scan` | a scan runs (the user's scan wins) |
| `tx` | PTT held, transmitting, a refusal still shown, or the data service sending (`dmr_call_data_busy`) |
| `call` | a call heard on our slot (played or not), or the squelch state RX; during a survey visit, a call heard on the visited site (back home at once: a networked call is likely on our own site too) |
| `reply` | the reply window or a one-shot target is running |
| `quiet` | less than `ROAM_QUIET_MS` (3 s) since the last reception or transmission ended: the repeater's hang time and the other side's answer |
| `override` | a temporary change on the channel (power, CC, TS, mode...), a talk target chosen for now, hear all latched, monitor held |
| `emergency` | an emergency is in progress (`CONFIG_DMR_EMERGENCY`) |

A request from the user (any key action that reaches the radio core: PTT, channel or zone change, monitor, a
temporary change...) during a search or a survey visit ends it at once: the radio goes back to the site it started from (the origin)
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
transmission until the user turns it on). Tools > Roaming row 7 and debug link setting 23 set it. With probing off,
roaming relies on beacons (sections 4.8 - 4.10) and traffic: on a system without beacons a repeater nobody uses is
not found until it carries a call, but a failed transmission (`tx_fail`) still starts a search, and "Roam now"
while the network is busy finds the others.

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

### 4.8 Beacons and the beacon schedule

Settings (radio-wide, section 5.1; they must match the repeaters, which the radio can't read):

| Setting | Range | Default | |
|---|---|---|---|
| `roam_beacon_s` | 0, 10 .. 600 s | 60 | the repeaters' beacon interval; 0 = they don't beacon |
| `roam_beacon_len_s` | 1 .. 10 s | 4 | how long each beacon lasts |
| `roam_survey` | 0 off, 1 synced, 2 per site | 1 | how the sites' beacons line up (or no survey) |

**Beacon detection.** The radio follows the keyups of the site it is tuned to (its own site while watching, the
visited one during a visit). A keyup is a beacon when it carried no traffic and lasted about the beacon duration:
from half of it to the duration + 1.5 s when its start was seen (at least `ROAM_CLEAN_MS`, 200 ms, of silence before
it), or at most the duration + 0.5 s when the radio tuned in during it (its start is then its end minus the
duration). The end is the last burst: "heard" falls `ROAM_HOLD_MS` (300 ms) after it.

**Synced (the main case).** A networked system keys all its repeaters' beacons in one shared window: the radio
learns the window from its own site's beacons (it is there most of the time) and from any site heard beaconing.
The first beacon gives a tentative window (`win_conf` 1, enough for a round); a beacon within `ROAM_WIN_TOL_MS`
(1 s) of the schedule confirms it (`win_conf` 2, 3) and pulls it a quarter of the way toward itself (jitter); a site
heard in the predicted window during a round confirms it too. A beacon off the schedule is held as an alternative,
and a second one in step with that moves the schedule (the network restarted). The schedule is forgotten after
`ROAM_WIN_STALE` (30) intervals without a matching beacon (crystals drift a few ms per minute). When its own site
gives no beacon for two intervals plus the duration while it is weak or not heard (out of range), the radio makes
one discovery visit: it listens to the site with the oldest data through a whole interval (at most interval +
duration + 1.4 s), and learns the window from that site's beacon.

**Per site (secondary).** Repeaters that beacon on their own clocks, each with its own phase: the radio keeps each
site's last beacon start (`phase_ms`) and visits one site just before its next beacon (section 4.9). A site with no
known phase gets a discovery visit (up to an interval + duration + 1.4 s). Phases also last `ROAM_WIN_STALE`
intervals and are refreshed by every visit that hears the site on time.

**No beacons** (`roam_beacon_s` 0) or survey off: no survey; roaming works as in sections 4.1 - 4.7 (traffic, the
optional wake probe, the optional periodic search).

### 4.9 The survey

While watching (not locked), with beacons set and the survey on, and nothing on the block list:

- **Synced**: in each window, `ROAM_WIN_LEAD_MS` (400 ms) after its start (the own site's beacon has begun, its
  start is seen and refines the schedule), a round visits as many other sites as fit before the window's end minus
  `ROAM_WIN_TAIL_MS` (300 ms), oldest data first (never measured first), so the list rotates across windows. Each
  visit lasts until four readings or `ROAM_SURVEY_VISIT_MS` (420 + 250 ms). With the defaults (4 s), six sites fit
  in a window, so a 16-site zone is covered in three windows. Outside the window the radio stays on its site:
  nothing else is on the air.
- **Per site**: one visit at a time, to the site with the oldest data, tuned `ROAM_SURVEY_SETTLE_MS` +
  `ROAM_CLEAN_MS` + half the tolerance before its expected beacon (so its start is seen and refines its phase),
  until four readings or the tolerance + 250 ms past the expected start.

Bounds:

- **Time away**: a round is at most the window minus 700 ms (3.3 s with the defaults). After a round or a visit the
  next one waits nine times as long as it was away (`ROAM_SURVEY_DUTY`, 10 %): with 60 s / 4 s that never skips a
  window; with a 10 s interval it skips two. A discovery visit (about an interval) is followed by about nine
  intervals without a survey.
- **Traffic on the own site**: no round within `ROAM_SURVEY_HOLD_MS` (10 s) after traffic there, doubled per busy
  period up to 160 s (`busy_score`, 0 - 4); each quiet round lowers the score by one. The radio can't hear its own
  site while it is away: a call that starts on it during a round is joined late, at most one round (3.3 s) late.
  In the synced case the own site is idle in the window (it is beaconing), so little is missed. A call heard on a
  visited site ends the round at once (on a networked system it is likely carried on the own site too).
- **PTT**: ends a visit at once (`roam_abort`, counted in `roam.cuts`); the radio retunes to its site and the
  transmission starts there. The call stack still holds the visited site's outbound as "up" for up to 300 ms, so
  with the own site beaconing in the window the voice starts without a wake-up: the extra delay is the retune
  (the host test measures PTT-to-voice within 100 ms of PTT on the own site without a survey). After a visit to a
  silent site longer than the hold, the call stack wakes the repeater first, as after any channel change.
- **Screens**: during a survey visit the screens and the debug link's core view keep showing the site the radio
  returns to (`radio_sm_shown_view`, applied when the view is published), so Home doesn't flicker once a minute.
  `roam.state=survey` and `roam.visit` say where the radio is.

Results: each visit updates the site's survey average and its time (`surv.<i>` on the debug link). Survey data is
**fresh** for `roam_fresh_ms()`: synced, (windows needed to cover the zone x windows between rounds + 2) intervals;
per site, (other sites + 2) intervals; never less than `ROAM_FRESH_MS`.

### 4.10 Moving with the survey data

A `weak` or `manual` trigger that is ready (dwell, back-off and block list as in 4.2 and 4.3), with beacons set and
the survey on:

1. Among the other sites with fresh data, B = the strongest that reaches the threshold T; home wins instead when
   it is fresh, reaches T, is within H of B and H better than the current site (as in 4.6).
2. If B is at least O + H (O = the current site's average, unknown = -200): a **confirm visit** to B when it is
   next on the air: now if it is in its beacon with time left, else just before its next beacon (the shared window,
   or its own phase), else now (traffic only). The visit listens up to `ROAM_SURVEY_SETTLE_MS` + `ROAM_CONFIRM_MS`
   (800 ms) or eight readings. B's mean reaching T and O + H: move (the radio is already there, `roam.roams` + 1,
   a confirm counted in `roam.confirms`). Else stay and back off (4.6); B's survey data takes the new measurement.
3. Else, if every other site's data is fresh: nothing is better; `weak` stays and backs off without a search,
   `manual` runs the full search (it also catches traffic).
4. Else, stale data with a known window: the stale sites are surveyed first, in every window regardless of the
   duty cycle, and step 1 runs again when they are fresh. If that takes longer than the freshness time, a full
   search.
5. No schedule: the full search (4.4).

A weak site on a synced system therefore moves within about one interval of the weak trigger (the next window),
without visiting sites that are known to be worse, and without the full search's 1.9 s per site, which on an idle
system would hear nothing.

### 4.11 Defaults: what turning roaming on for a zone does

Roaming for a zone (Tools > Roaming row 1) turns on: watching the site, the weak trigger, and the synced survey
with 60 s / 4 s beacons. The survey only listens, so P7 allows it on by default; 60 s / 4 s is what networked
systems commonly use. On a system without beacons it costs nothing: no beacon is learned, so no round runs (a
discovery visit happens only when the own site is weak or not heard). The wake probe stays off: it transmits and
keys every repeater in range. The periodic search (`roam_check_s`) stays off: the survey does its job without the
1.9 s per site. If the repeaters beacon on another interval, or not in step, the user sets rows 4 - 6.

## 5. Storage

### 5.1 Settings store (`settings-store.md` 3)

Appended to `settings_t`, layout 1:

| Field | Meaning | Default |
|---|---|---|
| `roam_rssi_dbm` (i8) | threshold T, -125 .. -60 | -105 |
| `roam_hyst_db` (u8) | hysteresis H, 1 .. 30 | 6 |
| `roam_probe` (u8) | 0 passive, 1 wake probe | 0 |
| `roam_stored` (u8) | 1 = the three above and `roam_check_s` are set | 0 |
| `roam_check_s` (u16) | periodic search interval, 0 = off, else 30 .. 3600 | 0 |
| `roam_zones[10]` | bit z % 8 of byte z / 8: zone z is a roaming zone (zones 0 .. 79) | all 0 |
| `roam_beacon_s` (u16) | the repeaters' beacon interval, 0 = none, else 10 .. 600 s | 60 |
| `roam_beacon_len_s` (u8) | beacon duration, 1 .. 10 s | 4 |
| `roam_survey` (u8) | 0 off, 1 synced, 2 per site | 1 |
| `rpt_beacon` (u8) | our repeater roles send an idle beacon (section 7) | 0 |

The last four are appended after `reserved5` (payload 168 -> 176 bytes, then `reserved6[3]`). A record written
before them is shorter: the loader keeps the defaults for the missing bytes (`settings-store.md` 3), so an
upgraded radio gets the survey on with 60 s / 4 s and the repeater beacon off.

The values reach the radio core as `radio_settings_t.roam_*`. The zone bits are written by the radio core when the
user turns roaming on or off for the current zone. They follow the zone index, so a CPS write that reorders or
deletes zones moves the flags with the indices (section 10, decision 2).

### 5.2 Legacy codeplug

The legacy channel record's LibreDMR flag 1 bit 0 "Roaming" (`legacy-cps-interop.md` 7, offset 0x26) is read into
`channel_t.roam`. It is never written by the radio (the encoder keeps the bit as read). When any channel of a roaming
zone has it, only those channels are sites. Whether qdmr exposes this bit for the legacy layout is not checked yet
(section 10, decision 3).

## 6. Interfaces

### 6.1 Radio core

- `radio_req_roam(cmd)` (`RREQ_ROAM`): `RROAM_ZONE_OFF` 0, `RROAM_ZONE_ON` 1, `RROAM_ZONE_TOGGLE` 2, `RROAM_NOW` 3,
  `RROAM_LOCK` 4, `RROAM_UNLOCK` 5, `RROAM_LOCK_TOGGLE` 6.
- `radio_view_t.roam` (`radio_roam_view_t`): zone flag, state, trigger, block reason, site count, current and home
  index, the current average, counters, and per site its channel number and last measurement; for the survey the
  visit kind, the schedule's confidence and next window, the pending confirm, the survey counters, and per site its
  survey average, its age, its beacon phase and whether it is fresh.
- `radio_sm_shown_view()`: the view the screens get during a survey visit (the site the radio returns to).
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
  in the zone with the same talkgroup", not in the VFO). Rows 2 and 3 are dimmed while roaming is off here.
  Radio-wide rows (all builds with roaming): 4 "Site survey" Synced / Per site / Off, 5 "Beacon interval" (60 s,
  2 min, 5 min, 10 min, None, 10 s, 15 s, 20 s, 30 s; Enter steps through them), 6 "Beacon length" (4, 5, 6, 8, 10,
  1, 2, 3 s), 7 "Wake probe" On/Off. The body adds "Beacons: surveying" (schedule known) or "Beacons: learning".
  The hint reads "1-7 pick Back". Roaming
  is not in Quick settings: the nine tiles are taken (`information-architecture.md` 5) and roaming is set per zone,
  not changed per call.
- Toasts: "Roaming on for <zone>", "Roaming off", "Moved to <channel>" (after any move, the radio core's short scan
  hold beep when beeps are on), "Searching sites", "Locked to <channel>", "Roaming again" (unlocked), "Turn roaming on
  first" / "Can't roam on this channel" (rows 2 and 3 while roaming is off here), "Survey: <mode>", "Beacons every
  <n>" / "No beacons", "Beacon length <n> s", "Wake probe on" / "Wake probe off".
- During a survey visit the screens keep showing the site the radio returns to (section 4.9); no chip changes.

### 6.3 Debug link

View section 12 `roam` (`docs/spec/debuglink.md` 3.7): `roam.built roam.zone_on roam.state roam.why roam.pending
roam.blocked roam.weak roam.sites roam.site roam.home roam.channel roam.rssi_dbm roam.searches roam.roams roam.stays roam.aborts roam.probes
roam.triggers roam.visit roam.win_conf roam.win_next_ms roam.confirm roam.surveys roam.visits roam.cuts roam.beacons
roam.confirms set.roam_rssi_dbm set.roam_hyst_db set.roam_probe set.roam_check_s set.roam_beacon_s
set.roam_beacon_len_s set.roam_survey set.rpt_beacon`, `site.<i>=<channel>:<rssi>` for each site (rssi 0 = not
measured) and `surv.<i>=<dbm>:<age_s>:<phase_ms>:<fresh>` (dbm 0 = never surveyed, age -1 = never, phase -1 =
unknown, fresh 0/1). `roam.visit` is the site being visited (-1 at home); `roam.win_next_ms` is the time to the next
window start (-1 unknown). RADIO op 26 `cmd u8` = `radio_req_roam`; settings 21 `roam_rssi_dbm` (i16 as u16, two's
complement), 22 `roam_hyst_db`, 23 `roam_probe`, 24 `roam_check_s`, 25 `roam_beacon_s`, 26 `roam_beacon_len_s`, 27
`roam_survey`, 28 `rpt_beacon` through op 24.

## 7. Our repeaters' beacons

The SFR runner (`docs/spec/sfr.md`: today MMDVM SFR, later Standalone SFR's repeater roles) can send
the beacon itself: with `rpt_beacon` on (debug link setting 28, off by default by P7), the runner keys its outbound with idle bursts for
`roam_beacon_len_s` every `roam_beacon_s` while it carries nothing (`sfrRunnerSetBeacon`, counted in
`sfr.tx_beacons` in the debug link's `hotspot` section). Traffic always has priority: a beacon due during a call is skipped, and a call that starts during a
beacon takes the outbound over. The beacon is standard idle bursts with the repeater's color code, nothing else.

The radio keeps no network time, so its beacon starts at its own phase (the first one when the role starts) and is
**not** in a network's shared window: a radio surveying such a system in the synced mode learns the window from
the other repeaters and finds ours only through traffic or a discovery visit; the per-site mode handles it. Aligning
it would need a time source: the hotspot host's clock (the MMDVM host knows the network time) could send the phase
later. MMDVM duplex hosts send their own beacons (the host's setting), not through this setting.

## 8. Timings and constants

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
| `ROAM_HOLD_MS` | 300 | the call stack's repeater hold (`DMR_CALL_REPEATER_HOLD_MS`): "heard" falls this long after the last burst |
| `ROAM_BEACON_DEFAULT_S` / `ROAM_BEACON_LEN_DEFAULT` | 60 / 4 | common networked settings (section 4.11) |
| `ROAM_WIN_TOL_MS` | 1000 | a beacon this close to the schedule matches it (network jitter, keyup time) |
| `ROAM_WIN_LEAD_MS` / `ROAM_WIN_TAIL_MS` | 400 / 300 | a round starts after the own site's beacon began and ends before the window does |
| `ROAM_WIN_STALE` | 30 intervals | the schedule (or a site's phase) is forgotten after this many without a match |
| `ROAM_CLEAN_MS` | 200 | silence before a keyup for its start to count |
| `ROAM_SURVEY_SETTLE_MS` | 420 | the 300 ms hold plus the retune and the RSSI filter (section 3) |
| `ROAM_SURVEY_LISTEN_MS` / `ROAM_SURVEY_READINGS` | 250 / 4 | a visit ends at four readings or after 420 + 250 ms |
| `ROAM_SURVEY_DUTY` | 10 % | time away at most a tenth of the time |
| `ROAM_SURVEY_HOLD_MS` / `ROAM_SURVEY_BUSY_MAX` | 10000 / 4 | no round within 10 s << score of own-site traffic |
| `ROAM_CONFIRM_MS` | 800 | a confirm visit listens this long after the settle |

## 9. Tests

- Host (`tests/host/roamTest.c`): the engine alone (averages, weak latch, dwell, back-off, the choice with
  hysteresis, home preference, lock, abort) and through `radio_sm.c` against a fake board whose repeaters are
  frequencies with an RSSI and a color code, driven by the simulated modem (BS idle bursts while a site is "keyed",
  BS_Dwn_Act on air wakes it): no roam while a call plays, during PTT, in the reply window, with a temporary change, a
  manual target, hear all or monitor; weak site -> search -> the strongest qualifying site; hysteresis keeps the radio
  on a site that is not enough worse; return to home within the hysteresis; a candidate with the wrong color code is
  never chosen; PTT during a search returns to the origin first and transmits there; a failed wake-up on PTT starts a
  search; wake probing finds a silent site; lock and roam now; the zone flag is stored; `CONFIG_DMR_ROAMING=0` builds.
  Survey (engine): window learning, jitter and moving the schedule, rotation oldest first, the 10 % duty, a weak
  site moving through a confirm visit within one window, stale data surveyed first then the full search, blocks and
  own-site traffic, a call on a visited site, the per-site mode and discovery, no survey without beacons. Survey
  (radio core, fake repeaters with a beacon period, length and phase): defaults and the stored fields, rounds in the
  window and the shown view, PTT during a visit transmits on the own site without a wake-up, own-site traffic holds
  the survey off. SFR (`tests/host/hotspotSfrTest.c`): the runner's idle beacon.
- UI flow (`tests/host/uiFlowTest.c`): Tools > Roaming toggles the zone flag in the press budget; "Roam now" asks the
  radio core.
- HIL (`tests/hil/test_roaming.py`, listed in `tests/hil/README.md`): radio only: the `roam` section, the settings
  round trip, the zone toggle and back, a passive roam now on the current zone (ends on one of its sites, nothing
  sent), lock and roam now ending it; with `--allow-rf --allow-service-stop --roam-rx --roam-tx`: the SDR bench runs
  `bench_repeater.py` as a second site and the radio, started on a dead site with the wake probe on, wakes it and
  moves there. A passive variant (the bench repeater carrying a bench call while the radio searches) needs the bench
  to transmit through its own repeater and is left for later. Beacons (`-k beacon`, `bench_repeater.py --beacon
  --beacon-len`, defaults from `--roam-beacon 20 --roam-beacon-len 3`): the survey settings round trip (radio
  only); the bench site as home beaconing, the radio learns the window (`roam.win_conf`, `roam.beacons`); the bench
  site as the other site, the radio surveys it in the window (`surv.<i>` fresh).

## 10. Open decisions

1. The roaming zone flag and the thresholds as per-zone fields of the native zone record (when it exists), instead
   of radio-wide settings plus zone index bits.
2. Zone bits keyed by index break when a CPS reorders zones. A name hash would survive reordering but not renaming.
3. Whether qdmr exposes the legacy Roaming bit; if not, a qdmr extension or a radio-side channel editor field.
4. Wake probing in the UI (Settings > Talk & listen) and whether a probe should use the lowest power level.
5. Whether a move should be announced by a beep, a voice prompt or nothing (today a short beep with key beeps on).
6. Talk target matching: by resolved contact ID (today) or also by listen list, for systems where sites carry the
   same talkgroups on different timeslots.
7. The survey costs about 5.8 KB of flash; a `CONFIG_DMR_ROAMING_SURVEY` sub-switch could drop it on tight radios.
8. Whether Home should show a survey visit at all (today it doesn't; the debug link does).
9. The "repeater up" from the CACH (slot toggle) is not color-code checked; on a system where a neighbor with
   another color code shares the frequency, a survey reading could count it.
10. Aligning our repeater's beacon with a network window (section 7) from the hotspot host's time.
11. A PTT after a visit to a silent site, once the 300 ms hold expired, goes through a wake-up on the own site.
