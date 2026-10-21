<!-- SPDX-License-Identifier: MIT -->

# Everyday radio features

Scan, the FM transmit extras, beeps and power management in the radio core. Code: `core/radio/scan.c` (the scan
engine, pure), `core/radio/beep.c` (beep patterns and synthesis), `core/radio/vibe.c` (vibration patterns),
`core/radio/battery.c` (battery levels, the backlight helper), all glued into `core/radio/radio_sm.c`; board hooks in
`hal/hal_radio.h` (`tx_tone`, `rx_sleep`, `mic_gain`) and `hal/hal_power.h` (`mic_level`, `vibrate`). Build switches: `docs/build-config.md`. Debug link: the `features` view section
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
`radio_req_scan_dual(pos)`; `radio_req_dual(ref, sel)` (2.1). The view shows `scan` (off / zone / vfo / dual), `scan_phase` and `scan_skipped`; the
tuned channel or frequency is the core view's as usual.

- **Zone scan** visits the current zone's channels in order, from the one it started on, skipping empty positions and
  channels marked "zone skip" in the codeplug. FM and DMR channels mix freely: each stop tunes the channel as a
  channel change would.
- **VFO scan** steps the VFO frequency by its step from `lo` to `hi` and wraps. Without a range it sweeps the board
  band the VFO frequency is in. Both ends must be in one band, at least one step apart.
- **Dual watch** alternates between two channels, skip flags ignored (2.1). `radio_req_scan_dual(pos)` (the debug
  link's) watches the current channel and position `pos` of the current zone.
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
the current channel (VFO: frequency, up to 8) until the scan ends, and moves on (not in dual watch). A scan starts
only when there is another channel to go to; it does not run in hotspot mode or while transmitting.

### 2.1 Dual watch

The radio has one receiver, so dual watch alternates it between two channels with the scan's timing (a pseudo dual
watch; a receive set with a receiver per channel is #96). `radio_req_dual(ref, sel)` names the two, A and B: each a
channel (zone index and position, any zone) or the VFO with its own settings. `ref[sel]` is the selected channel.
`NULL` stops the watch on the selected channel. The view shows `scan=dual`, and the tuned channel as usual: its
`zone_index`, `zone_pos` and zone name, or `vfo=1`, so the screens can tell which of the two the receiver is on.

| Event | What happens |
|---|---|
| Start | Refused when both name the same channel, or one can't be read (an empty position, no such zone, a build without a VFO). The receiver listens where it is when that is one of the two, else it tunes the selected one |
| The same pair again | Only the selection changes: no retune, the stop goes on (cheap; the UI asks again on every change) |
| A new pair | The receiver stays where it is when that channel is still one of the two (a call it holds too), else it tunes the selected one |
| A signal | Holds the receiver as in a scan, then the resume delay. Resume mode stop counts as carrier: a signal never ends the watch |
| DMR reply window | Holds the selected channel while it runs (PTT talks there). On the other channel it doesn't |
| PTT | Tunes the selected channel first if the receiver is on the other, and transmits there. The watch goes on: after the transmission (DMR: after its tail) the selected channel waits the resume delay, and a reply holds it |
| A channel, zone, VFO, tone, bandwidth, mode, timeslot or color code change | Ends the watch on the selected channel, then applies the change there |
| Hotspot mode, `radio_req_scan` stop | Ends it where the receiver is. Cancel ends it on the selected channel |

`radio_get_vfo()` returns the VFO's settings while a channel is tuned (a VFO pane). Built without `CONFIG_DUAL_WATCH`
only the debug link's dual watch is left: two channels of the current zone, and PTT ends it as it ends any scan.

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
VOX. Boards put their reading on the hal's scale with `miclevel_log()` in `drivers/audio/miclevel/miclevel.h` (100 / 12
steps per bit, about 0.72 dB per step), so a threshold `100 - 8 x vox_level` sits `(8 x vox_level) x 0.72` dB below
the top of the scale: about 6 dB at sensitivity 1, 29 dB at 5, 58 dB at 10.

| Radio | Where the level comes from | Checked |
|---|---|---|
| DM-1701, MD-380 | an analog level tap on PA3 (ADC1 IN3), 12 bits, passed straight in (`hardware-dm1701.md` 11.3, `hardware-md380.md`) | DM-1701: in silence only (`test_vox_quiet`) |
| HR_C7000 radios (DM-1801 Pro, DM-32UV) | the codec ADC's mic PCM, 80 samples every 10 ms (`platform/hrc7000/audio_pcm.c`, `drivers/audio/miclevel`): the peak of each half from its mean, 100 = a full-scale 16-bit peak; the level follows a louder half at once and falls 4 steps per half. These radios have no analog level tap. The first `mic_level()` call starts the meter (AUDIO_CONTROL b4 and the PCM interrupt, shared with the PCM stream) and returns 0; 1 s without a call stops it. The meter's interrupt clears b4 itself, so every AUDIO_CONTROL update in the driver (FM receive, transmit and idle, DMR entry) runs with interrupts off (`hrc7000_set_crit`, set by `hrc_audio_pcm_attach`). Needs `CONFIG_DMR` (the PCM stream); without it there is no hook | not on the bench |

The debug link's `vox.level` (features section) calls the hook on FM channels outside hotspot mode, so the level can
be read with VOX off.

The Quick VOX tile (FM) turns the `vox` setting to all or off; `vox_on` in the view says whether VOX is armed on
the current channel. On a board without the hook the tile refuses to turn it on ("VOX: not on this radio yet")
rather than show "VOX on" for a VOX that never keys.

### 3.5 Squelch tail elimination

Not done. Without a reverse burst or a tone-loss signal from the other station there is nothing to act on; closing
early on CTCSS loss conflicts with the tone debounce (8 polls) that keeps fades from chopping audio. Left for later.

### 3.6 Mic gain

Settings > Talk & listen > Mic gain: a step 0-10, 2 dB a step, 5 = the board's own levels (`core/radio/mic_gain.c`).
The UI stores it (`settings_t.mic_gain`) and applies it at start-up and on each change with `radio_set_mic_gain()`;
the debug link's SETTING op 29 sets it in RAM only.

| Board | FM | DMR |
|---|---|---|
| with `hal_radio_t.mic_gain` (HR_C7000 radios: DM-1801 Pro) | the board's FM level for the channel bandwidth plus (step - 5) x 2 dB, written into the modem's mic gain at each FM transmit start | the board's DMR level plus the same offset, at each DMR entry and DMR transmit start; the samples to the vocoder are not scaled |
| without it | not changed | the mic samples are scaled before the vocoder (`ambeVoiceSetMicGain`, with saturation) |

The board only stores the offset in the hook; a change takes effect at the next transmission. On an HR_C7000 radio
the register is clamped to 0-42 dB (`hrc7000_mic_db`). The VOX meter reads the same codec ADC, so on these radios
the mic gain also moves the VOX level. Without `CONFIG_DMR` and without the hook the row is dimmed.

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
starts the board's PCM audio stream itself and stops it two silent blocks after the pattern. If that stream delivers no
block for 200 ms (8 radio polls), its clock has stopped: the beep stops the stream, gives the speaker route back and
drops the pattern, so a codec that goes quiet can't leave the analog receive audio muted. A start that fails (no codec
clock) is stopped the same way, in case a block already ran. Counters in the view:
`beep.played` (started), `beep.no_stream` (no audio stream could be started: on a board that can't stream PCM in FM
mode this counts instead), `beep.blocks` (audio blocks with beep in them), `beep.stalls` (streams given up that way),
`beep.busy`.

### 4.1 Vibration (CONFIG_VIBRATE)

On a radio with a vibration motor (`hal_power_t.vibrate` not NULL), the alerts that already reach the user also
vibrate. The UI decides, as it does for the message tone: `handle_event` (`core/ui/ui_app.c`) calls
`ui_svc_vibrate()` for each event below when Settings > Sound > Vibrate is on, and `ui_svc_radio.c` passes it to the
radio core as `radio_req_vibrate()` (`RREQ_VIBRATE`).

| Pattern | Pulses | When (UI event) |
|---|---|---|
| message | 150 ms, 150 ms rest, 150 ms | a text message arrived (`UI_EV_MESSAGE`) |
| call | 500 ms | a call alert (`UI_EV_CALL_ALERT`) or a private call to us (`UI_EV_PRIVATE_CALL`) |
| emergency | 400 ms four times, 200 ms rests | someone's emergency heard (`UI_EV_EMERGENCY`, `dmr-emergency.md`) |

The board's `vibrate(ms)` runs the motor for one pulse and returns at once (the board stops it, from a timer). `vibe.c` times the pattern from the radio poll (25 ms): each pulse starts when the previous one's on and rest
times have passed, measured from that pulse's start, so the timing doesn't drift with the poll. A new pattern replaces
one still running and starts at the next poll. A vibration request is not user activity: it doesn't reset the battery
save idle time, abort an APRS transmission or cancel a roaming search, like a beep. On a radio without a motor the
pattern is dropped at the next poll.

The setting is `SETTINGS_UI_NO_VIBRATE` in `settings_t.ui_flags` (`settings-store.md` 3), on by default: an "off" bit,
so records saved before it existed keep it on. The Settings row is in Sound when the image has `CONFIG_VIBRATE`, and
is dimmed ("This radio has no vibration motor") when the board has no motor (`UI_FEAT_VIBRATE`, reported by
`ui_svc_feat_radio`). `CONFIG_VIBRATE` is off by default and on in the config of radios with a motor
([build-config.md](../build-config.md)). The debug link shows `vibe.built vibe.motor vibe.played vibe.busy` in the
`features` view and plays a pattern with RADIO op 29 (`debuglink.md` 3.7, 3.8), whatever the setting.

### 4.2 Speaker volume

Where the MCU can set the speaker level, the radio core owns it. The board gives it a `hal_radio_t.volume(percent)`
hook; `hal_power_t.volume()` says where the level comes from (`radio_volume_src_t`, the `volume.src` view key):

| `volume.src` | Board | Level in force |
|---|---|---|
| `none` | No hook (the MD-380, the DM-32UV: the volume control works in analog hardware, after the chip) | Not set by ODR. The debug link operation answers "unavailable" |
| `knob` | A hook and a knob the MCU reads (the DM-1701) | The knob's 0..100 % |
| `soft` | A hook and no knob (`hal_power_t.volume()` = -1, the DB25-D); `radio_volume_soft()`, reported to the UI as `UI_FEAT_VOLUME` | The stored step |
| `override` | As `knob`, with the debug link's level in force | The step the debug link set, whatever the knob does |

| Item | Value |
|---|---|
| Steps | 0 (mute) to `RADIO_VOLUME_MAX` (16), default `RADIO_VOLUME_DEFAULT` (8). The step count is that one constant: the stored form, the screen bar, Settings, the debug link, and the boards' maps follow it |
| Percent | The hook gets round(step * 100 / `RADIO_VOLUME_MAX`) (`radio_volume_percent`): step 1 is 6 %, step 8 is 50 %, step 16 is 100 %. The board decides what a percent means; step 0 must be silent |
| Stored | `settings_t.volume` (`settings-store.md` 3): 0 = the default, else `0x80 \| (step + 1)`; a byte 1..11 from the first version (steps 0..10) reads as round(step * 16 / 10). Settings > Sound > Volume (`UI_SET_VOLUME`) and the volume screen write it, `soft` radios only |
| To the board | `follow_volume` (radio poll) calls `hal_radio_t.volume(percent)` at start-up and after each change (the knob's changes of 2 % or more, and its ends); the same level twice calls nothing |
| A change | `ui_svc_setting_set(UI_SET_VOLUME)` saves the step and posts `RREQ_VOLUME` (`radio_req_volume`), so it is in force at once. The request counts as neither user activity nor an interruption: a transmission or a roaming search goes on |
| Override | `radio_req_volume_override(step)` (`RREQ_VOLUME_OVERRIDE`) holds a level over the knob until `-1`; RAM only, so a restart gives the knob back. Without a knob there is nothing to override, and the request changes nothing |
| Beeps | Key and status tones keep their own level (Tone volume, 4) and are mixed at the board's speaker path as before |

**Debug link.** RADIO op 30 (`debuglink.md` 3.8) sets a step 0..16, or `0xFF` for "back to the knob". On a `soft` radio
it stores the step as the UI does (`0xFF` stores the default); on a `knob` radio it sets the override; on a radio with
`volume.src=none` it answers unavailable. The `features` view shows `volume.src`, `volume.max`, `volume.step` (the
stored step), `volume.override` (-1 = none), `volume.sent` (the percent last given to the board, -1 = none yet) and
`volume.soft`. `odr_debug.py volume [N|knob]` prints them and sets.

The volume screen (`SCR_VOLUME`) opens on `ACT_VOLUME`, a board key map entry (a radio with a knob answers with a
toast). Up, Down and the encoder change the step by one and save it on each step; OK, Back or the same key close it,
and so does 3 s without a key. The screen shows the step as a large number over a bar of 16 segments (7 px pitch at
the smallest display, 5 px filled). On the radio without the feature the setting is left out of Settings.

## 5. Power

### 5.1 Battery

The radio core reads `battery_mv` every second (not while transmitting nor for 3 s after: the sag under TX is not the
battery's state) and shows `battery_mv`, `battery_pct` (the board's) and `battery` (ok / low / critical). Thresholds
are per board (`board.battery_low_mv`, `battery_critical_mv`; 0 means no warnings; values in each radio's hardware
page, e.g. `hardware-dm1701.md` 11.1). Three readings in a row
below a threshold lower the level; it goes back up 200 mV above it.

A board with `CAP_NO_BATTERY` (a mobile on a fixed supply) has no battery monitor. The core never reads
`battery_mv`, the level stays ok, `battery_pct` and `battery_mv` stay 0, and nothing warns or refuses a transmission
for the battery. The UI shows no battery icon, percentage, Tools > Battery page, Battery saver setting or Low battery
warning setting. The debug link reports `battery=none` (with `battery_pct=0` and `battery_mv=0`), `/status` leaves
`battery_pct` and `battery_mv` out, and SNMP reports odrBatteryVoltage 0, odrBatteryPercent 0 and odrBatteryState
ok(1) without ever sending odrLowBattery.

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
patterns and mixing, vibration patterns and their timing through the radio core, TOT warning, BCL, inhibit, roger beep, burst, VOX, battery save, DMR permit / call-end beeps.
On the radio (`docs/testing/on-air-coverage.md`, HIL 2026-10-10): zone scan stopping for DMR calls, VFO scan, dual
watch, the battery saver, the 1750 Hz burst and the roger beep on air; TX inhibit and the TOT warning on the radio's
state; VOX only in silence (the `mic_level` hook is unverified); whether PCM beeps play in FM mode is still open
(`beep.no_stream` / `beep.blocks` answer that).
