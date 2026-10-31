<!-- SPDX-License-Identifier: MIT -->

# Everyday radio features

Scan, the FM transmit extras, beeps and power management in the radio core. Code: `core/radio/scan.c` (the scan
engine, pure), `core/radio/beep.c` (beep patterns and synthesis), `core/radio/vibe.c` (vibration patterns),
`core/radio/battery.c` (battery levels, the backlight helper), all glued into `core/radio/radio_sm.c`; board hooks in
`hal/hal_radio.h` (`tx_tone`, `tx_dtmf`, `rx_sleep`, `mic_gain`, `ref_trim`) and `hal/hal_power.h` (`mic_level`, `vibrate`). Build switches: `docs/build-config.md`. Debug link: the `features` view section
and RADIO ops 20-24 (`docs/spec/debuglink.md` 3.7, 3.8). Host tests: `tests/host/radioFeaturesTest.c`; HIL tests:
`tests/hil/test_radio_features.py`.

## 1. Settings

The radio core keeps one `radio_settings_t` (`core/radio/radio.h`). It loads it from the settings store at
start-up (the `rs_*` fields, `settings-store.md` 3) and saves every change made with `radio_set_settings()`, so the
settings screen hands the whole struct over on a change and reads it back with `radio_get_settings()`. The debug
link's SETTING op goes through `radio_set_settings()` too, so its changes are saved.

Every caller reads the struct, changes its own fields and sets it again. The settings screen reads it afresh for each
change, so a change made elsewhere (the debug link, a host, the radio core) is kept. Until the radio task has taken
in the last `radio_set_settings()`, `radio_get_settings()` answers with those settings, not the ones in force. A run of
quick changes therefore never reads back a value from before its own last change.

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
| `dual_delay_ms` | 1000 | dual watch on one receiver: its wait after a signal ends, in place of `scan_delay_ms` (0-10000; section 2.1) |
| `beep_level` | 50 | 0 (silent) to 100: the loudest a beep gets (section 4) |
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
  link's) watches the current channel and position `pos` of the current zone. The UI on top of it (A and B, the
  screens, the keys) is in [dual-watch.md](../ux/dual-watch.md).
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

On a radio with one receiver, dual watch alternates it between two channels with the scan's timing (a pseudo dual
watch). A radio with a second receiver (2.2) hears both at once. `radio_req_dual(ref, sel)` names the two, A and B: each a
channel (zone index and position, any zone) or a VFO with its own settings. `ref[sel]` is the selected channel.
`NULL` stops the watch on the selected channel. The view shows `scan=dual`, and the tuned channel as usual: its
`zone_index`, `zone_pos` and zone name, or `vfo=1` and `vfo_index`, so the screens can tell which of the two the
receiver is on.

There are two VFOs, so each channel can have its own: a VFO ref's `pos` says which (0 or 1). The radio core keeps
the one in use in `vfo_ch` and the other in `vfo_other`; a hop to a VFO pane swaps them first. Both are saved, with
the one in use (`settings-store.md` 3, second VFO). Outside dual watch the VFO key opens the one last used. Copy to
VFO during the watch copies into a VFO the other channel isn't on: the one last used, else the other.

| Event | What happens |
|---|---|
| Start | Refused when both name the same channel, or one can't be read (an empty position, no such zone, a build without a VFO). The receiver listens where it is when that is one of the two, else it tunes the selected one |
| The same pair again | Only the selection changes: no retune, the stop goes on (cheap; the UI asks again on every change) |
| A new pair | The receiver stays where it is when that channel is still one of the two (a call it holds too), else it tunes the selected one |
| A signal | Holds the receiver as in a scan, then the wait (`dual_delay_ms`, below). Resume mode stop counts as carrier: a signal never ends the watch |
| The wait after a signal | The receiver stays on that channel for the reply, and after each dwell there looks at the other channel for one dwell (squelch shut). A signal found there sets its bit in the view's `dual_busy` (the pane shows Busy) and is not played or held: the receiver goes back and the wait goes on. A look starts only if a look and a dwell back fit in what is left of the wait. When the wait ends the watch alternates again, so a busy channel is held then. A look that hears nothing clears the bit, as does the watch's own hop to that channel and a stop. A hold (a signal, the DMR reply window) has no looks |
| DMR reply window | Holds the selected channel while it runs (PTT talks there). On the other channel it doesn't |
| PTT | Tunes the selected channel first if the receiver is on the other, and transmits there. The watch goes on: after the transmission (DMR: after its tail) the selected channel waits (`dual_delay_ms`, with looks at the other), and a reply holds it |
| Monitor down | Tunes the selected channel first if the receiver is on the other, then acts as without the watch (FM: squelch open; DMR: digital monitor on the release). The watch pauses there while the key is held |
| Power, squelch, Clear changes, a talk target or hear all (`RREQ_POWER`, `_SQUELCH`, `_CLEAR_TEMP`, `_DMR_TARGET`, `_DMR_CONTACT`, `_DMR_CALL_BACK`, `_DMR_TARGET_CLEAR`, `_DMR_HEAR_ALL`) | Tunes the selected channel first if the receiver is on the other, then applies the change there. The watch goes on |
| Each hop | The channel the receiver leaves keeps its changes for now (power, squelch, bandwidth, mode, color code, timeslot, tones, the manual talk target, hear all; `dual_keep[]`), and the channel it tunes gets its own back. A pair with a new channel drops that channel's; a stop drops both |
| A channel, zone, VFO, tone, bandwidth, mode, timeslot or color code change | Ends the watch on the selected channel, then applies the change there. The UI then asks for the watch again with the new channel (dual-watch.md 5) |
| Hotspot mode | Ends it where the receiver is |
| `radio_req_scan` stop, cancel or toggle (the debug link) | Ends it on the selected channel and counts it in the view's `dual_stops`: the UI takes it as the user's stop and doesn't start the watch again |
| `radio_req_dual` with the same pair and the other selected (the debug link's select) | The view's `dual_sel` changes; the UI follows a change it didn't ask for, so the next channel, zone or VFO request changes that channel |
| A VFO request, a frequency or Copy to VFO with a channel selected | The selected channel goes into a VFO the other channel isn't on: the one last used, else the other one |
| Power off | The saved position is the selected channel's, wherever the receiver is: the radio powers up there |

`radio_get_dual()` returns the pair, the selected one and the one the receiver is on (the debug link's `dual.a dual.b
dual.sel dual.at`, [debuglink.md 3.7](debuglink.md#37-get_view)); the view's `dual_busy` is `dual.busy`. `radio_get_vfo(index)` returns a VFO's settings while something else is tuned (a VFO pane). Built without `CONFIG_DUAL_WATCH`
only the debug link's dual watch is left: two channels of the current zone, and PTT ends it as it ends any scan.

### 2.2 A second receiver

A board with a second receiver sets `board.rx2` (`hal/hal_rx2.h`; a second SoC, over the link in
[rx2-link.md](rx2-link.md)). In dual watch the main receiver then stays on the selected channel and the second one gets
the other. Nothing alternates, and the scan's hold and resume timing aren't used. The view's `rx2` is 1 while this is
so (the debug link's `dual.both`).

| Event | What happens |
|---|---|
| Start, a new pair, a new selection | The second receiver gets the channel not selected: its frequency, mode, bandwidth, RX tone and the squelch level; DMR: color code, timeslot, our ID, the talk group and the RX group list's groups (up to 16). The main receiver tunes the selected one |
| Its settings change (a codeplug edit, the squelch) | Sent again every 2 s; the link drops repeats |
| Audio | The selected channel's always wins. The second receiver has the speaker only while the main one plays nothing (FM squelch closed, no DMR call) and the radio isn't transmitting |
| PTT | On the selected channel, where the main receiver already is. The second receiver keeps listening; its audio is muted while the radio transmits |
| Talk back (the UI setting, [12-scan.md](../user/12-scan.md)) | A call that ends on the other channel makes it the selected one, so the main receiver retunes there, the second receiver gets the old channel, and PTT answers on the new one. The second receiver never transmits: `hal_rx2_t` has no transmit call, and a receive-only SoC's bands are all `tx = false` |
| It doesn't answer, or isn't on its settings 1.5 s after them | The core falls back to alternating the main receiver (2.1) and tries the second receiver again every 10 s. PTT, a new selection and talk back work as in 2.1 meanwhile |
| The watch ends | The second receiver stops and its audio goes off |

`ui_svc_receivers()` is 2 on such a radio, and `ui_svc_dual_rx(pane)` reports each pane's own receiver while the
second one has its channel: the dual watch screens then show a call on each pane at once.

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

The board restores the microphone path on `tx_tone(0)` and on every return to receive. An AT1846S board makes the
tone with the voice source select (tone 1, tone 2 or both) in place of the microphone. Verified on air on an HR_C6000 + AT1846S board (HIL 2026-10-10): the burst, the roger beep and APRS
AFSK (`docs/testing/on-air-coverage.md`).

### 3.3a DCS turn-off tail (CONFIG_SUBTONE)

A DCS transmission ends with the turn-off code: 134.4 Hz (the DCS bit rate) as a steady tone in place of the code.
Receivers set to the code close their squelch on it while the carrier is still up, so they don't play the noise burst
of the carrier dropping.

| Step | Behavior |
|---|---|
| PTT release (after the roger beep, if on) | the core calls the board's `tx_tail(true)`. True: the radio stays keyed `RADIO_SUBTONE_TAIL_MS` (300 ms), then `tx_tail(false)` and unkeys. False (no DCS, or no tail on the radio): unkeys at once |
| PTT again during the tail | `tx_tail(false)`: the code again, the transmission goes on |
| Time-out during the tail | the tail ends with the transmission |

The board maps the hook to its chip; the core owns the timing.

| Chip | Tail | Measured (SDR bench, D023N, 12.5 kHz) |
|---|---|---|
| BK4819 | 0x52 bit 15 | 134.4 Hz for 0.30 s |
| HR_C7000 | FM_PTT off with FM_MOD b9 (`modem-hrc7000.md` 8.2) | 134.4 Hz for 0.20 s, then 0.10 s of carrier without modulation (one board measured) |
| AT1846S | TX select from CDCSS to CTCSS, 0x4A already 134.4 Hz | not yet measured |
| software (PWM), HR_C5000 boards | the generator's 134.4 Hz sine (`subaudio_tx_tail`) | not yet measured |

`tools/sdr_fm_check.py` reports the tail on every DCS segment ("sub-audio tail: 134.4 Hz for 0.30 s, ...");
`test_fm_tx_dcs` requires at least 0.15 s.

### 3.4 VOX (CONFIG_VOX; the `vox` setting is off by default)

On FM channels (all, or those with the codeplug VOX flag) the microphone level (`mic_level()`, 0-100 logarithmic) at
or above `100 - 8 x vox_level` for 2 polls (50 ms) keys up; below it for `vox_tail_ms` unkeys (with the roger beep
if on). Received audio, MONITOR, and 500 ms after a reception never key VOX (the speaker into the microphone). PTT
works as usual and wins. The level comes from the board's `board.power->mic_level` hook; a board without it has no
VOX. Boards put their reading on the hal's scale with `miclevel_log()` in `drivers/audio/miclevel/miclevel.h` (100 / 12
steps per bit, about 0.72 dB per step), so a threshold `100 - 8 x vox_level` sits `(8 x vox_level) x 0.72` dB below
the top of the scale: about 6 dB at sensitivity 1, 29 dB at 5, 58 dB at 10.

| Board kind | Where the level comes from | Checked |
|---|---|---|
| STM32 boards with an analog level tap | an ADC channel on the microphone level, 12 bits, passed straight in | in silence only (`test_vox_quiet`) |
| HR_C7000 boards | the codec ADC's mic PCM, 80 samples every 10 ms (`platform/hrc7000/audio_pcm.c`, `drivers/audio/miclevel`): the peak of each half from its mean, 100 = a full-scale 16-bit peak; the level follows a louder half at once and falls 4 steps per half. These radios have no analog level tap. The first `mic_level()` call starts the meter (AUDIO_CONTROL b4 and the PCM interrupt, shared with the PCM stream) and returns 0; 1 s without a call stops it. The meter's interrupt clears b4 itself, so every AUDIO_CONTROL update in the driver (FM receive, transmit and idle, DMR entry) runs with interrupts off (`hrc7000_set_crit`, set by `hrc_audio_pcm_attach`). Needs `CONFIG_DMR` (the PCM stream); without it there is no hook | not on the bench |

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
| with `hal_radio_t.mic_gain` (HR_C7000 boards) | the board's FM level for the channel bandwidth plus (step - 5) x 2 dB, written into the modem's mic gain at each FM transmit start | the board's DMR level plus the same offset, at each DMR entry and DMR transmit start; the samples to the vocoder are not scaled |
| without it | not changed | the mic samples are scaled before the vocoder (`ambeVoiceSetMicGain`, with saturation) |

The board only stores the offset in the hook; a change takes effect at the next transmission. On an HR_C7000 radio
the register is clamped to 0-42 dB (`hrc7000_mic_db`). The VOX meter reads the same codec ADC, so on these radios
the mic gain also moves the VOX level. Without `CONFIG_DMR` and without the hook the row is dimmed.

### 3.7 DTMF transmit (CONFIG_DTMF)

Touch tones on FM while PTT is held (#54, step 1: manual digits). Stored DTMF codes and DTMF decode are not built.

**Keys.** With PTT held on an FM channel, 0-9, \* and # (the QUICK and CONTEXT keys on keypad radios) send their DTMF
pair for as long as the key is held. A key map entry names its digit (`keymap_entry_t.dtmf`); a digit key's is its
own digit. A microphone with extra keys (P4-P7) sends A-D. The input engine hands these keys' down and up edges to the input task
before any gesture. When the radio core's `dtmf_ready` is set (`radio_dtmf_ready()`), the input task sends the edge
to the radio core (`radio_req_dtmf(digit, down)`), as it does for PTT, and the engine sends none of that hold's
gestures: no Find, Quick settings or key lock under the TX screen, and the tone follows the key, not the UI. Locked
keys send nothing (`input_set_dtmf_block`, from the UI's key lock). On DMR the digits stay inert during TX, and \* and
# keep their usual meaning.

**Accepted** only while `dtmf_ready`: state TX on an FM channel, PTT held (the radio's key or the host's), no roger
beep or 1750 Hz burst on the air, no host audio (USRP), not a hotspot, and a board with `tx_dtmf`. Anything else is
ignored, never queued. PTT release and the time-out end a digit still sounding before the roger beep and the DCS
turn-off tail (3.3a) start, so a digit never sounds over either; PTT is up during the tail, so keys send nothing then.

**Timing** (radio core, every poll):

| Item | Value | Why |
|---|---|---|
| Shortest tone | `RADIO_DTMF_MIN_MS` 100 ms | a quick tap still decodes (decoders want 40 ms or more) |
| Shortest gap between two digits | `RADIO_DTMF_GAP_MS` 60 ms | the same digit twice decodes as two (decoders want a 40 ms pause) |
| A key pressed while a tone sounds or within the gap | waits (one digit); the sounding tone ends once it has had its shortest length, the next starts after the gap | rollover from a fast thumb |
| PTT released, time-out, any end of TX | the tone stops with the transmitter | no tone outlives the transmission |

The microphone is muted while a tone sounds and comes back between digits. The speaker plays no sidetone; the TX
screen's second line shows "DTMF" and the last digits sent in this transmission instead of the time left.

**View** (debug link `features` section): `dtmf_ready` and, only while they are not at rest (the view is near the reply's size cap), `dtmf` (the digit
sounding), `dtmf_sent` (digits sent since start-up) and `dtmf_digits` (the last 8 of this transmission). The RADIO op `DTMF` (`docs/spec/debuglink.md`
3.8) presses and releases a digit as a key would, including A-D, which no key gives.

**Board hook** `hal_radio_t.tx_dtmf(digit)`: `'0'`-`'9'`, `'*'`, `'#'`, `'A'`-`'D'` on, 0 = off (the microphone
again); false when the board can't (not in FM TX). The pairs (`hal/hal_dtmf.h`, ITU-T Q.23):

| Low \ high | 1209 Hz | 1336 Hz | 1477 Hz | 1633 Hz |
|---|---|---|---|---|
| 697 Hz | 1 | 2 | 3 | A |
| 770 Hz | 4 | 5 | 6 | B |
| 852 Hz | 7 | 8 | 9 | C |
| 941 Hz | \* | 0 | # | D |

| Chip | How | Deviation |
|---|---|---|
| HR_C7000 | the DTMF sender as for `tx_tone` (`modem-hrc7000.md` 8.3): the low tone's step in FM_TONE1's high half, the high tone's in FM_TONE3's, digit "1" sent; TONE_SEND_CTRL b6 = 0 mutes the microphone | SUBVOICE_COEF b15:8 as for the 1750 Hz burst (`hrc7000_dtmf_dev`) |
| AT1846S | tone1 = low, tone2 = high, voice_sel 011 (both tones), the HR_C6000 microphone input muted | reg 0x59 bits 15:6 = `DTMF_DEV` |
| BK4819 | tone 1 and tone 2 on (0x70), frequencies in 0x71 / 0x72, microphone ADC off | levels 0x70 = 0x9090, 0x40 = 0x348A |
| HR_C5000 boards | none: no FM TX tone path yet | |

**Twist** (the high tone's level over the low tone's) is judged on the audio a receiver hears, after its de-emphasis
(6 dB per octave, 750 us), where the usual DTMF receiver limits apply: the high tone -8 to +4 dB from the low one (the
North American telephone criteria; other countries' are similar or wider). A tone that goes through the transmitter's
pre-emphasis reads 3 to 7 dB higher on the carrier than it is received. Measured on the SDR bench (2026-10-21 and -22,
12.5 and 25 kHz; `sdr_fm_check.py --dtmf` prints both; digits 1, #, 5, D, 0):

| Chip | On the carrier | Received | Why |
|---|---|---|---|
| AT1846S | -0.1 to +0.2 dB | -4.6 to -2.8 dB | equal tones, injected after the pre-emphasis. "A" (697 + 1633 Hz) would be about -7 dB |
| HR_C7000 | +5.0 dB, every digit | +0.4 to +2.1 dB | the chip's DTMF sender sets the low tone below the high one by design (`modem-hrc7000.md` 8.3), after the pre-emphasis |
| BK4819 | +3.6 to +7.0 dB | +0.7 to +2.3 dB | equal tones (0x70 = 0x9090) through the microphone's pre-emphasis, as a DTMF microphone's would be |

None of them needs a correction in the driver. The HIL test (`test_fm_tx_dtmf`) checks the received twist;
`test_fm_tx_dtmf_stop` checks on air that a held digit ends with the carrier at PTT release and at the time-out.

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
scale so a beep mixed over voice doesn't clip. Beeps follow the radio's volume up to `beep_level`. A radio with an analog
knob (no `hal_radio_t.volume`) and one whose volume hook also scales the PCM path (`volume_pcm`: an HR_C6000
board's RX gain, an HR_C7000 board's line out gain) play the beep at `beep_level`, and the volume acts on it in the
speaker path. On a radio where the MCU sets the volume somewhere the PCM path doesn't pass, the core plays each beep at the
volume percent last given to the board, capped at `beep_level` (`radio_beep_level`): volume 0 gives no beep. When the DMR voice stream runs, the beep is mixed into it (`ambeVoiceSetMixer`); otherwise the beep
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
| `none` | No hook (the volume control works in analog hardware, after the chip) | Not set by RadioFoundry. The debug link operation answers "unavailable" |
| `knob` | A hook and a knob the MCU reads | The knob's 0..100 % |
| `soft` | A hook and no knob (`hal_power_t.volume()` = -1); `radio_volume_soft()`, reported to the UI as `UI_FEAT_VOLUME` | The stored step |
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
`volume.soft`. `rfoundry_debug.py volume [N|knob]` prints them and sets.

The volume screen (`SCR_VOLUME`) opens on `ACT_VOLUME`, a board key map entry (a radio with a knob answers with a
toast). Up, Down and the encoder change the step by one and save it on each step; OK, Back or the same key close it,
and so does 3 s without a key. The screen shows the step as a large number over a bar of 16 segments (7 px pitch at
the smallest display, 5 px filled). On the radio without the feature the setting is left out of Settings.

## 5. Power

### 5.1 Battery

The radio core reads `battery_mv` every second (not while transmitting nor for 3 s after: the sag under TX is not the
battery's state) and shows `battery_mv`, `battery_pct` (the board's) and `battery` (ok / low / critical). Thresholds
are per board (`board.battery_low_mv`, `battery_critical_mv`; 0 means no warnings; values in each radio's hardware
page). Three readings in a row
below a threshold lower the level; it goes back up 200 mV above it.

A board with `CAP_NO_BATTERY` (a mobile on a fixed supply) has no battery monitor. The core never reads
`battery_mv`, the level stays ok, `battery_pct` and `battery_mv` stay 0, and nothing warns or refuses a transmission
for the battery. The UI shows no battery icon, percentage, Tools > Battery page, Battery saver setting or Low battery
warning setting. The debug link reports `battery=none` (with `battery_pct=0` and `battery_mv=0`), `/status` leaves
`battery_pct` and `battery_mv` out, and SNMP reports rfoundryBatteryVoltage 0, rfoundryBatteryPercent 0 and rfoundryBatteryState
ok(1) without ever sending rfoundryLowBattery.

Such a board may measure its DC input instead: it sets `hal_power_t.supply_mv`, and `board_volts()` (`hal/board.h`)
says `BOARD_VOLTS_SUPPLY`. The radio core reads it once a second, outside transmissions like the battery, into the
view's `supply_mv`. Nothing acts on it: no level, percentage, warning, TX refusal or power off, since no supply
thresholds are defined yet. The UI's last tool is then Supply, showing the reading in volts (13.8 V), with no battery
settings. The debug link has `supply_mv` in place of `battery_pct` and `battery_mv`, `/status` has `supply_mv`, and
SNMP reports as for no battery. The feature matrix and the web site show each radio's reading: Battery, Supply or
None.

### 5.2 Low and critical

Low: the battery low beep, again every 2 minutes; the backlight is capped at 30 % (5.3). Critical: the critical beep
every 30 s, every transmission refused (`tx_deny=battery`), and with `battery_off_s` set the radio core asks for
power off after that long (`power_off=1`, `radio_power_off_wanted()`); `core/app/app.c` then powers off as with the
switch.

### 5.3 Backlight

`radio_backlight_pct(brightness, idle_ms)`: the UI's brightness, dimmed to a fifth (at least 5 %) after
`backlight_dim_s` without input, off after `backlight_off_s`, capped at 30 % while the battery is low or critical.
The UI owns the backlight and calls it; the radio core doesn't drive the backlight itself.

Idle time restarts on input and while `radio_view_wakes(view)` holds: something the user would want to see.

| State | Wakes | Why |
|---|---|---|
| `RS_TX`, `tx_auto` 0 | yes | the user's PTT (or VOX) |
| `RS_TX`, `tx_auto` 1 | no | the radio's own: a data list (ARS, ACK, LRRP answer, IP), a roaming probe, an APRS beacon |
| `RS_TX_DENIED` | yes | PTT held: the screen says why |
| `RS_RX`, FM | yes | the squelch opened: something plays |
| `RS_RX`, DMR, `dmr.state` `RDMR_RX` | yes | a call that plays |
| `RS_RX`, DMR, otherwise | no | data heard (ARS ACK, others' CSBKs), a call not played (other talkgroup, encrypted) |
| `RS_RX`, `bcast` | no | a broadcast program plays continuously |
| `hotspot` | no | the host's traffic |

What does reach the user (a message, call alert, private call, radio check result, emergency) comes as a UI banner
or screen, and the UI wakes the backlight for it (`ui_app_take_wake`). `last_tx_ms` in the view records when the
last transmission began, so an unattended one can be placed afterwards (debuglink.md 3.7).

### 5.4 Receiver battery save (CONFIG_BATTERY_SAVE, `rx_save` off by default)

On an idle FM channel after `rx_save_idle_s` without activity (keys, requests, receptions, PTT), the receiver sleeps
200 / 400 / 800 ms and listens 100 ms (`rx_sleeping` in the view). Any signal in the listen window keeps it awake;
anything else (a key, a request, MONITOR, scan, VOX, DMR channel, hotspot, a channel with the codeplug "no eco" flag,
APRS receive listening on the APRS frequency, `aprs.md` 5.3) stops it. The board's `rx_sleep` hook turns the receiver off and back on. On air
on an AT1846S board since 2026-10-10 (`test_battery_save*`: cycles when idle, MONITOR and a call wake it); the setting stays off by default.

### 5.5 Power switch

On boards with a volume knob that has a switch, the switch doesn't cut the supply: the MCU holds its own with the power
latch, and the switch only breaks the battery sense line, so the unfiltered battery reading drops to about 0 V the
moment the knob turns off. `core/radio/power_switch.c` turns that
into the board's `switch_on()`: three readings in a row below 5.5 V, at least 10 ms apart, mean off; one low reading
doesn't. The boards ignore it for the first 1.5 s after power-up while the readings settle. `core/app/app.c` polls
`switch_on()` and powers off (settings saved, latch released). 5.5 V sits well under a flat 2S pack (critical
6.0 V), so a weak battery never reads as the switch.

### 5.6 Time of day

RadioFoundry keeps no wall clock. `hal_time.h` gives only time since start-up, and that is what every record uses: the crash
log's `uptime_ms`, messages (`ui_msg_t.age_ms`), call history and the APRS station list ("14m ago"). Nothing on the screen shows a time of day, so a
radio never shows a made-up one after power-on.

The only time of day is the UTC time a GPS or host NMEA fix carries. It travels with that position into DMR GPS and
LRRP reports. A fixed position carries none, and the reports leave the time out (`utc_hms` 0xFFFFFFFF, date 0).

Some radios have a real-time clock, and some keep it running through power-off. `board.toml` `[hardware] clock`
records which, with its source (`backed`, `unbacked`, `none`, or `unknown`), and the feature matrix and the site show
it. A clock driver arrives behind a hardware switch (`CONFIG_RTC`, `hardware` in `tests/hil/features.toml`). Until GPS,
the network or the user sets it, the radio shows "no time", and records keep using uptime or mark the time unknown.

## 6. TX power, calibration, band limits

Checked, unchanged: the board clamps the power level, uses the calibration table's drive value for the band and level,
and refuses a TX frequency outside the board's bands (`tx_deny=band`). Without a calibration the board either drives
a low fixed value or refuses to transmit; the board's hardware page says which.

### 6.1 Reference trim

Settings > Advanced > Freq trim: a fraction of the reference, -10.0 to +10.0 ppm in 0.1 ppm steps, 0 by default
(`core/radio/ref_trim.c`). It is for a radio whose reference oscillator is off even with its calibration: it moves
receive and transmit together, FM and DMR. A fraction of the reference moves every carrier by the same fraction, so
one value holds on every band; the Hz it comes to grows with the frequency.

| Trim | At 440 MHz | At 144 MHz |
|---|---|---|
| 0.1 ppm (one step) | 44 Hz | 14.4 Hz |
| 4.5 ppm | 1980 Hz | 648 Hz |
| 10.0 ppm (the limit) | 4.4 kHz | 1.44 kHz |

The value is held in parts per billion (ppb, 100 a step), an int16: `hal_radio_t.ref_trim`, `settings_t.ref_trim_ppb`,
the debug link. The UI stores it and applies it at start-up and on each change with `radio_set_ref_trim()`, which
clamps, rounds to the step and calls `hal_radio_t.ref_trim` when the value differs from the one the board has (the
board starts at 0, so a start-up with no trim calls nothing). The debug link's SETTING op 35 sets it in RAM only.

| Board | Trim |
|---|---|
| with a reference control (`hal_radio_t.ref_trim` sets it) | adds the trim to the control its calibration sets, at every tune; its hardware page gives the scale |
| with synthesizers only (`hal_radio_t.ref_trim` keeps the value) | programs each RF chip at f x (1 + trim) instead of f, at every tune and burst (`ref_trim_tune_hz`). A superhet's LO takes f x (1 + trim) + IF and its IF stays as it is: both chips run from one reference, so the IF's share of the error cancels |
| without the hook, or built with `CONFIG_REF_TRIM=n` | the row is hidden; nothing changes |

The hook returns whether the board takes the trim at the next tune. A board that applies it at once returns false;
one that applies it when it tunes returns true, and the caller (the UI, the debug link) then asks the radio task to
tune again (`radio_req_retune`, the `RREQ_RETUNE` request). A tune ends a reception in progress, so the radio task tunes
once the trim has held still `RADIO_RETUNE_SETTLE_MS` (400 ms), not at each step: when idle, after an APRS
transmission, or at the end of a transmission or the hotspot. Until then the hotspot or a transmission in progress
keeps the trim it started with on receive. The RF chips' bus stays the radio task's.

The value is a correction to enter by hand: measure the carrier against a reference (a frequency counter, a service
monitor, or a hotspot's MMDVMCal BER test) and enter the error with its sign reversed. A radio 2 kHz high at 448.7 MHz
is 4.5 ppm high and takes -4.5 ppm. [feature-matrix.md](../feature-matrix.md) lists which radios have the hook.

## 7. Verification status

Host-tested (`radioFeaturesTest`): scan engine and glue (zone, skip flags, nuisance, resume modes,
priority, dual watch with its wait and looks and the debug link's requests during it, VFO band scan, mixed FM / DMR, a DMR call already in progress with a direct mode carrier and a
late entry at every superframe phase, PTT and retune behavior), battery levels and power off, beep
patterns and mixing, vibration patterns and their timing through the radio core, TOT warning, BCL, inhibit, roger beep, burst, DTMF timing and refusals (`inputTest` for the key routing), VOX, battery save, DMR permit / call-end beeps.
On the radio (`docs/testing/on-air-coverage.md`, HIL 2026-10-10): zone scan stopping for DMR calls, VFO scan, dual
watch, the battery saver, the 1750 Hz burst and the roger beep on air; TX inhibit and the TOT warning on the radio's
state; VOX only in silence (the `mic_level` hook is unverified); whether PCM beeps play in FM mode is still open
(`beep.no_stream` / `beep.blocks` answer that).
