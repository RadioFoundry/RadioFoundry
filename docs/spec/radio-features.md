<!-- SPDX-License-Identifier: MIT -->

# Everyday radio features

Scan, the FM transmit extras, beeps and power management in the radio core. Code: `core/radio/scan.c` (the scan
engine, pure), `core/radio/beep.c` (beep patterns and synthesis), `core/radio/battery.c` (battery levels, the
backlight helper), all glued into `core/radio/radio_sm.c`; board hooks in `hal/hal_radio.h` (`tx_tone`, `rx_sleep`)
and `hal/hal_power.h` (`mic_level`). Build switches: `docs/build-config.md`. Debug link: the `features` view section
and RADIO ops 20-24 (`docs/spec/debuglink.md` 3.7, 3.8). Host tests: `tests/host/radioFeaturesTest.c`; HIL tests:
`tests/hil/test_radio_features.py`.

## 1. Settings

The radio core keeps one `radio_settings_t` (`core/radio/radio.h`). It loads it from the settings store at
start-up (the `rs_*` fields, `settings-store.md` 3) and saves every change made with `radio_set_settings()`, so the
settings screen hands the whole struct over on a change and reads it back with `radio_get_settings()`. The debug
link's SETTING op goes through `radio_set_settings()` too, so its changes are saved.

| Setting | Default | Meaning |
|---|---|---|
| `tot_s` | 180 | the radio's TX time-out, for channels whose own (codeplug) value is 0; 0 = none |
| `tot_warn_s` | 10 | warning beep this long before the time-out; 0 = none |
| `bcl` | off | busy channel lockout on FM: off, carrier (anything heard), wrong tone (a carrier without our RX tone) |
| `roger_beep` | off | FM: a 1200 Hz + 900 Hz tone pair (100 ms each) at the end of each transmission |
| `tx_inhibit` | off | refuse every transmission (`tx_deny=inhibit`) |
| `scan_resume` | carrier | time (move on after `scan_hold_s` on a signal, or when it ends), carrier (move on `scan_delay_ms` after it ends), stop (the scan ends on the first signal) |
| `scan_hold_s` | 5 | time mode's hold |
| `scan_delay_ms` | 2000 | wait after a signal ends before moving on (a pause between overs) |
| `scan_dwell_ms` | 150 | listen time per channel with nothing heard (at least 50) |
| `scan_priority` | off | zone scan: the start channel again after every 2 others |
| `scan_beep` | off | a short beep when the scan stops on a signal |
| `beep_level` | 50 | 0 (silent) to 100, relative to the volume knob |
| `key_beep` | on | `radio_req_beep(BEEP_KEY)` sounds |
| `talk_permit` | on | DMR: the talk permit tone when the voice goes on air (stored as `SETTINGS_RS_NO_TALK_PERMIT` in `rs_flags`, 0 = on) |
| `vox` | off | off, on channels whose codeplug entry allows it, on every FM channel (CONFIG_VOX) |
| `vox_level` | 5 | sensitivity 1 (loud voice needed) to 10 |
| `vox_tail_ms` | 1500 | stays keyed this long after the voice stops |
| `rx_save` | 0 (off) | receiver battery saving: sleep 200, 400 or 800 ms per 100 ms awake (1-3) |
| `rx_save_idle_s` | 10 | battery saving starts after this long without activity |
| `battery_off_s` | 0 (never) | switch off after this long with the battery critical |
| `backlight_dim_s`, `backlight_off_s` | 30, 120 | for the UI's backlight policy (`radio_backlight_pct`, 5.3) |

TX time-out: one implementation for FM and DMR. A channel's codeplug `tot_s` wins; 0 there means the radio's
`tot_s`. The value in effect is `tot_s` in the core view, and the DMR call stack gets the same value. Open-firmware
codeplugs that wrote 0 for "no time-out" now get the radio's.

## 2. Scan (CONFIG_SCAN)

Requests: `radio_req_scan(cmd)` with stop, zone, VFO, dual, cancel, nuisance or toggle; `radio_req_scan_vfo(lo, hi)`;
`radio_req_scan_dual(pos)`. The view shows `scan` (off / zone / vfo / dual), `scan_phase` and `scan_skipped`; the
tuned channel or frequency is the core view's as usual.

- **Zone scan** visits the current zone's channels in order, from the one it started on, skipping empty positions and
  channels marked "zone skip" in the codeplug. FM and DMR channels mix freely: each stop tunes the channel as a
  channel change would.
- **VFO scan** steps the VFO frequency by its step from `lo` to `hi` and wraps. Without a range it sweeps the board
  band the VFO frequency is in. Both ends must be in one band, at least one step apart.
- **Dual watch** alternates between the current channel and a second one (`pos`), skip flags ignored.
- **Priority** (zone scan): the start channel again after every 2 others.

Each stop: 50 ms settle (readings from the previous frequency are ignored), then `scan_dwell_ms` listening. A signal
is "active" when FM squelch opens (with the RX tone if the channel needs one) or DMR plays a call (`dmr.state` rx).
A carrier that isn't active yet earns a probe: 350 ms on FM channels that need a tone (CTCSS detection takes 100 to
250 ms), 700 ms on DMR; other FM channels move on at once. On DMR a carrier seen in the last 150 ms still counts (direct
mode is on air 30 ms in every 60, and the noise reading drops between bursts), and once the modem delivers bursts with
the channel's color code since the stop's settle ("sync", from `dmr_call_last_activity`, a late entry still waiting for
its LC included) the probe lasts up to 1.5 s: a call already in progress is taken from its embedded LC, which needs the
next complete superframe at best and the one after it when the modem's sticky LC equals this call's (about 1.1 s from
the retune). A call known not to be for us (another talkgroup) ends the probe. Empty channels still move on after the
dwell. The view counts the stops probed (`scan_probes`) and those with sync (`scan_synced`) for each scan. Active: the scan holds
(`scan_phase=hold`), then follows `scan_resume`; after the signal ends it waits `scan_delay_ms` (`resume`) and picks
up again if the signal comes back. MONITOR held pauses the scan (`paused`).

Stopping: STOP stays where the scan is. CANCEL goes back to the starting channel or frequency. PTT on a held signal
stops there and talks back on it; PTT while still searching goes back to the start channel and transmits there. A
channel, zone, VFO or frequency change by the user ends the scan where the user put it. **Nuisance delete** skips
the current channel (VFO: frequency, up to 8) until the scan ends, and moves on. A scan starts only when there is
another channel to go to; it does not run in hotspot mode or while transmitting.

## 3. FM transmit extras

### 3.1 Busy channel lockout and TX inhibit

`tx_check` refuses PTT, before keying: TX inhibit (`inhibit`), a critical battery (`battery`, 5.2), busy channel
lockout (`busy`; carrier = squelch open or noise below the open threshold; wrong tone = that, while the channel's RX
tone is not decoded). The deny beep sounds; the state is `tx_denied` until PTT is released.

### 3.2 Time-out warning

`tot_warn_s` before the time-out the TOT warning beep sounds once in the speaker (FM and DMR; the view's
`tot_warning=1`). At the time-out the transmitter goes off, `tx_deny=timeout` until PTT is released.

### 3.3 Roger beep and 1750 Hz burst (CONFIG_FM_TX_TONES)

Both go out over the air through the board's `tx_tone(hz)` hook: an audio tone into the modulator in place of the
microphone. Roger beep: at PTT release the radio stays keyed for 1200 Hz then 900 Hz, 100 ms each (`roger=1`), then
unkeys; PTT again during the beep cancels it and the transmission goes on. Tone burst: `radio_req_tone_burst(true)`
keys up if needed and sends 1750 Hz until `false` (repeater access); `tone_burst=1` in the view. A board without
`tx_tone` gets neither.

The board restores the microphone path on `tx_tone(0)` and on every return to receive. How the DM-1701 makes the
tone: `hardware-dm1701.md` 5.8. Verified on air on the DM-1701 (HIL 2026-10-10): the burst, the roger beep and APRS
AFSK (`docs/testing/on-air-coverage.md`).

### 3.4 VOX (CONFIG_VOX; the `vox` setting is off by default)

On FM channels (all, or those with the codeplug VOX flag) the microphone level (`mic_level()`, 0-100 logarithmic) at
or above `100 - 8 x vox_level` for 2 polls (50 ms) keys up; below it for `vox_tail_ms` unkeys (with the roger beep
if on). Received audio, MONITOR, and 500 ms after a reception never key VOX (the speaker into the microphone). PTT
works as usual and wins. The level comes from the board's `board.power->mic_level` hook; a board without it has no
VOX (DM-1701: `hardware-dm1701.md` 11.3, not verified on the bench).
The Quick VOX tile (FM) turns the `vox` setting to all or off; `vox_on` in the view says whether VOX is armed on
the current channel. On a board without the hook the tile refuses to turn it on ("VOX: not on this radio yet")
rather than show "VOX on" for a VOX that never keys.

### 3.5 Squelch tail elimination

Not done. Without a reverse burst or a tone-loss signal from the other station there is nothing to act on; closing
early on CTCSS loss conflicts with the tone debounce (8 polls) that keeps fades from chopping audio. Left for later.

## 4. Beeps (CONFIG_BEEP)

| Beep | Sound | When |
|---|---|---|
| key | 1000 Hz 25 ms | `radio_req_beep(BEEP_KEY)` from the UI, if `key_beep` |
| error | 400 Hz 150 ms | UI errors |
| TX permit | 1600 Hz 50 ms twice | DMR: the channel is granted (unless the codeplug channel has "no beep", or `talk_permit` is off: Settings > Sound > Talk permit tone) |
| TX deny | 300 Hz 400 ms | any TX refusal |
| call end | 1200 Hz 50 ms, 900 Hz 70 ms | DMR: a played call ended (unless "no beep") |
| TOT warning | 1000 Hz 100 ms twice | 3.2 |
| battery low | 800 Hz, 600 Hz | entering low, then every 2 min |
| battery critical | 600 Hz three times | entering critical, then every 30 s |
| scan hold | 1400 Hz 40 ms | the scan stops on a signal, if `scan_beep` |
| emergency | 1500 / 900 Hz warble, 6 x 160 ms | someone's emergency heard (alarm or emergency call), if the emergency setting's tone is on (`dmr-emergency.md`) |
| message | 1000, 1300, 1700 Hz rising, 60-90 ms each | a text message arrived: the UI asks for it (`radio_req_beep(BEEP_MESSAGE)`) when Settings > Sound > Message tone is on |

Synthesis: a 64-entry sine table with linear interpolation, 2 ms fades (no clicks); level 100 is about half of full
scale so a beep mixed over voice doesn't clip. Volume is relative to the knob: the beep goes through the same
speaker path. When the DMR voice stream runs, the beep is mixed into it (`ambeVoiceSetMixer`); otherwise the beep
starts the board's PCM audio stream itself and stops it two silent blocks after the pattern. Counters in the view:
`beep.played` (started), `beep.no_stream` (no audio stream could be started: on a board that can't stream PCM in FM
mode this counts instead), `beep.blocks` (audio blocks with beep in them), `beep.busy`.

## 5. Power

### 5.1 Battery

The radio core reads `battery_mv` every second (not while transmitting nor for 3 s after: the sag under TX is not the
battery's state) and shows `battery_mv`, `battery_pct` (the board's) and `battery` (ok / low / critical). Thresholds
are per board (`board.battery_low_mv`, `battery_critical_mv`; 0 means no warnings; values in each radio's hardware
page, e.g. `hardware-dm1701.md` 11.1). Three readings in a row
below a threshold lower the level; it goes back up 200 mV above it.

### 5.2 Low and critical

Low: the battery low beep, again every 2 minutes; the backlight is capped at 30 % (5.3). Critical: the critical beep
every 30 s, every transmission refused (`tx_deny=battery`), and with `battery_off_s` set the radio core asks for
power off after that long (`power_off=1`, `radio_power_off_wanted()`); `core/app/app.c` then powers off as with the
switch.

### 5.3 Backlight

`radio_backlight_pct(brightness, idle_ms)`: the UI's brightness, dimmed to a fifth (at least 5 %) after
`backlight_dim_s` without input, off after `backlight_off_s`, capped at 30 % while the battery is low or critical.
The UI owns the backlight and calls it; the radio core doesn't drive the backlight itself.

### 5.4 Receiver battery save (CONFIG_BATTERY_SAVE, `rx_save` off by default)

On an idle FM channel after `rx_save_idle_s` without activity (keys, requests, receptions, PTT), the receiver sleeps
200 / 400 / 800 ms and listens 100 ms (`rx_sleeping` in the view). Any signal in the listen window keeps it awake;
anything else (a key, a request, MONITOR, scan, VOX, DMR channel, hotspot, a channel with the codeplug "no eco" flag,
APRS receive listening on the APRS frequency, `aprs.md` 5.3) stops it. The board's `rx_sleep` hook turns the receiver off and back on (DM-1701: `hardware-dm1701.md` 5.11). On air
on the DM-1701 since 2026-10-10 (`test_battery_save*`: cycles when idle, MONITOR and a call wake it); the setting stays off by default.

### 5.5 Power switch

The volume knob's switch on the DM-1701 and the MD-380 doesn't cut the supply: the MCU holds its own with the power
latch, and the switch only breaks the battery sense line, so the unfiltered battery reading drops to about 0 V the
moment the knob turns off (MD-380: raw 3345 at 8.3 V, 3 with the knob off). `core/radio/power_switch.c` turns that
into the board's `switch_on()`: three readings in a row below 5.5 V, at least 10 ms apart, mean off; one low reading
doesn't. The boards ignore it for the first 1.5 s after power-up while the readings settle. `core/app/app.c` polls
`switch_on()` and powers off (settings saved, latch released). 5.5 V sits well under a flat 2S pack (critical
6.0 V), so a weak battery never reads as the switch.

## 6. TX power, calibration, band limits

Checked, unchanged: the board clamps the power level, uses the calibration table's drive value for the band and level,
and refuses a TX frequency outside the board's bands (`tx_deny=band`). Without a calibration the board either drives
a low fixed value (the DM-1701, `hardware-dm1701.md` 7) or refuses to transmit (the MD-380, the DB25-D).

## 7. Verification status

Host-tested (`radioFeaturesTest`, 446 checks): scan engine and glue (zone, skip flags, nuisance, resume modes,
priority, dual watch, VFO band scan, mixed FM / DMR, a DMR call already in progress with a direct mode carrier and a
late entry at every superframe phase, PTT and retune behavior), battery levels and power off, beep
patterns and mixing, TOT warning, BCL, inhibit, roger beep, burst, VOX, battery save, DMR permit / call-end beeps.
On the radio (`docs/testing/on-air-coverage.md`, HIL 2026-10-10): zone scan stopping for DMR calls, VFO scan, dual
watch, the battery saver, the 1750 Hz burst and the roger beep on air; TX inhibit and the TOT warning on the radio's
state; VOX only in silence (the `mic_level` hook is unverified); whether PCM beeps play in FM mode is still open
(`beep.no_stream` / `beep.blocks` answer that).
