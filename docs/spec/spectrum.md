<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->
<!-- Copyright (c) 2026 Sandro Kalatozishvili -->

# Spectrum analyzer and listening tuner

`CONFIG_SPECTRUM` needs FM. The shared radio task runs a sequential RSSI sweep through `board.radio->rx` / `rx_am` and
`status`; no board registers or RTOS calls belong to the engine (`core/radio/spectrum.c`).

## Receiver ownership

`RREQ_SPECTRUM` starts, stops, moves the tuner, or changes center, spacing, point count, squelch, dwell time,
automatic peak tuning or the display's dBm offset. Start refuses TX, held PTT, hotspot, trunking, satellite
tracking, an air-interface bench session and the separate broadcast receiver's range. Air band AM is allowed.
An armed diagnostic detour also refuses Start: its deadline owns the return to the home receiver. A detour request
while Spectrum runs closes the analyzer before the detour saves its home state.
Normal scan stops first. Dual watch instead parks on its selected pane and pauses: the pair, selection, VFOs and
each pane's changes for now remain intact. During the analyzer, `scan=dual` and `scan_phase=paused`; only the
analyzer polls or retunes the main receiver. A second receiver is released for the duration.
The original channel/VFO stays in the view. The DMR/air engines release the modem.
Every scan or listening retune chooses AM inside [108, 137) MHz when `radio_am_capable()` is true; otherwise FM.
It calls the existing `board.radio->rx_am(hz, bw)` and falls back to `rx()` if AM is refused, as a normal channel
does ([airband-and-broadcast.md](airband-and-broadcast.md)). This follows the tuned bin, including a window that
crosses 137 MHz, rather than the original channel or center frequency. At 108 MHz AM takes precedence over the
broadcast receiver's inclusive upper bound. FM has no receive tone and uses the selected filter: Wide is 25 kHz
(default), Narrow is 12.5 kHz. AM passes the same width request to the board, which owns its AM filter and gain loop.
`view.am` follows the active receiver; Stop restores the original channel's demodulator.
PTT and monitor requests are ignored. Normal radio ticks, automatic transmissions, channel squelch and battery
saving give way to the analyzer; volume, battery monitoring, scheduled beeps, audio-stream cleanup and vibration
alerts continue. The beep watchdog releases a stalled PCM route without requiring an analyzer exit. Other
operating requests close it first.

Stop or a failed retune restores the original frequency and mode, including the manual talk target and hear-all
latch. A paused dual watch resumes on that selected pane with a fresh listen/settle deadline, and the second
receiver is re-engaged where available. A watch that was off stays off. The UI retains its pair and saved On flag;
it records any interrupted call as ended without talk back and ignores analyzer readings when caching pane settings.
Repeated STOP is harmless. An explicit scan stop or channel change closes the analyzer first, then follows its
normal rules; it does not leave a pending restart behind. A codeplug reload closes the analyzer before applying its new channel. Leaving the analyzer's stack,
including Home from its settings menu, sends STOP. Opening or closing that menu does not stop or restart it.
No codeplug write occurs.

## Sweep and listening

The window has 65, 97 or 129 bins in one board band, initially 129. Spacing is 1.00 to 100.00 kHz in 0.01 kHz
increments, initially 12.50 kHz. The center is clamped to keep both endpoints in the band. Invalid or oversized
windows leave the previous one unchanged. Changing center can change bands and recenters the tuner. Spacing or
point-count changes preserve its absolute frequency when representable, otherwise clamp to the nearest endpoint.
All three preserve squelch, dwell time, automatic peak tuning, receive filter and the dBm offset.

Dwell time is 10..1000 whole milliseconds, initially 10. The engine ignores readings before its deadline; the
10 ms radio tick rounds other intervals up to the next tick. Each scanned bin waits that long after retuning.
A listening tuner instead uses the minimum 10 ms interval, so a long dwell does not delay squelch or chop audio.
Deadlines use signed differences across unsigned time wrap. Dwell and dBm offset changes do not retune or mute.

The receiver checks the selected tuner first. A carrier there is heard continuously. If it is quiet, **Auto peak**
in **After sweep** mode (default) sweeps the full window, then selects its strongest latest reading at or above squelch. It retunes
and confirms the carrier before opening audio: a peak that has ended cannot briefly open the speaker from a cached
reading. Ties prefer the lower-frequency bin. Automatic selection uses this sweep's readings, not the display's
five-second peak history. It keeps the window fixed and does not overwrite the saved manual tuner.
After the carrier ends and the release delay passes, scanning resumes at bin zero for the next sweep.

In **Immediate** mode, the first settled scanned reading at or above squelch becomes the tuner and opens audio
without another retune. It does not wait for a stronger signal later in the sweep. The next scan index advances
before listening, so after the carrier ends and the release delay passes, scanning resumes at the following bin
(bin zero after a capture at the final bin). Like Off, it also checks the tuner between batches of eight bins.

With Auto peak off, scanned signals only update the plot. They do not move the tuner or open their audio. Between
batches of eight bins and at each sweep end, the receiver checks the manually selected tuner. It can hold a carrier
there. Left/right and the encoder move the tuner one bin, clamp to the endpoints and immediately check it, in
any mode. Once listening, the receiver stays there rather than looking for a stronger signal. Changing automatic
mode does not retune or mute a carrier already being heard.

At the default 10 ms dwell, quiet sweeps of 65 / 97 / 129 points take about 0.7 / 1.0 / 1.3 s in After sweep mode, or
0.8 / 1.1 / 1.5 s in Immediate or Off with their extra tuner checks, plus retuning and scheduling costs. Actual settling and
throughput need verification on hardware. Short bursts can be missed; this is not an FFT and RSSI depends on board
calibration. The three point counts span 0.8 / 1.2 / 1.6 MHz at 12.50 kHz, or 0.4 / 0.6 / 0.8 MHz at 6.25 kHz.
BW is always `(bins - 1) * step_hz`, not a fixed MHz preset.

Squelch is a direct signed threshold from -160 to 0 dBm, default -80 dBm. A more negative value opens more easily;
-160 opens at the receiver floor, while 0 requires 0 dBm. Listening and automatic selection share
`spectrum_threshold_dbm`. Close has 3 dB hysteresis. Audio mutes when the carrier drops; the receiver waits 300 ms
after the last held reading before resuming. An open squelch keeps the receiver on the tuner.

Filter selects the HAL receive bandwidth on every scan bin and listening retune: Wide is `RADIO_BW_25K`, Narrow is
`RADIO_BW_12K5`. It defaults to Wide. Changing it preserves center, tuner frequency, spacing, point count, squelch,
dwell, automatic mode and dBm offset. It clears readings and peaks, retunes the same tuner with the new filter and
checks its carrier again before opening audio. The original channel's bandwidth is restored on exit.

Each bin keeps its latest reading and recent maxima as bytes (`dBm + 160`), covering -160..0 dBm. Unread is 255
(`SPECTRUM_EMPTY`), converted to `INT16_MIN` by `spectrum_level_dbm`. Five one-second buckets roll by elapsed time,
including during an audio hold. `spectrum_peak_level` takes their maximum: approximately the last five seconds,
with one-second age quantization (a sample lasts four to five seconds). Recent weaker readings survive expiry of an
older maximum; unvisited peaks cannot stay forever. The graph and Peak row share this history. Center, spacing and
point-count changes clear it.

## UI

Spectrum analyzer is the tenth Tools entry, with the ordinary dim `0` shortcut. The temporary received-zone entry
follows it, using Context (`#`) even after its banner is hidden. Without Spectrum it retains `0`.

The main screen keeps center at the left and the read-only tuner in a dim color at the right, followed by Peak,
the full-width graph and endpoints. All frequencies have five fractional MHz digits, including trailing zeros.
Narrow displays omit repeated MHz units when needed. It uses the layout's tiny font. Displays at least
`UI_TALL_PX` (200 px) high keep the title; smaller displays omit it.

Typing a digit starts center-frequency entry immediately. Reception and the window stay unchanged until OK
confirms it. Entry uses the VFO's positional MHz digits; # is an optional separator after three digits. Back deletes
one digit, then cancels an empty editor. Back outside entry closes the analyzer; long Back returns Home.

OK outside entry opens **Spectrum Settings** (`screen=spectrum-settings`), a standard list with these rows:

| Row | OK | Value |
|---|---|---|
| Squelch | Type a magnitude; the minus is automatic, OK confirms | -160..0 dBm; default -80 dBm; zero has no minus |
| Sweeping | Empty numeric editor; # for decimal, OK confirms | 1.00..100.00 kHz |
| Bandwidth | Cycle 65 / 97 / 129 points | MHz computed from point count and spacing |
| Dwell time | Empty numeric editor, digits then OK | 10..1000 ms |
| Auto peak | Cycle | After sweep (default) / Immediate / Off |
| Filter | Toggle | Wide (25 kHz, default) / Narrow (12.5 kHz) |

Up/down navigate that menu. Squelch entry accepts magnitude 0..160 and adds a minus to every nonzero value, including while editing. Zero displays as `0 dBm`, never `-0 dBm`. Numeric values outside their ranges are refused, never wrapped. Back in an editor
deletes, then cancels; Back in the menu returns to the running graph. Long Back closes both. Global quick settings
and banner actions do not consume numeric-editor keys.

Without `CAP_KEYPAD`, numeric editors start at the current value. Up/Down or encoder steps change the preview;
OK applies it and Back cancels it without changing reception or stored preferences. Squelch moves by 1 dB,
spacing by 10 Hz (0.01 kHz), and dwell by 1 ms; all clamp to their supported ranges. Spacing also respects the
current band's room for the selected point count. The six existing rows keep their order. Only these radios gain
a final **Center** (center frequency) row: the preview moves by the selected sweep spacing and crosses gaps to the next
active board band that can hold the window, excluding the separate broadcast receiver. It stops at the outer
limits. Optional mic digits switch the editor to direct numeric entry. Radios with a keypad retain their empty
digit editors and the six-row settings menu.

On the graph, Up raises and Down lowers the dBm window by 20 dB, clamped to -40..+40 dB offsets in 20 dB increments. Default grid
labels are -40, -80, -120 dBm. One Down gives -60, -100, -140; a second gives -80, -120, -160. Two Up presses from the default give 0, -40, -80. The plot
always spans 100 dB (`-130 + offset` to `-30 + offset`), clips readings outside it and keeps their actual Peak values.
This changes neither frequency nor the listening state. Labels sit over the grid lines, with only the middle label
on plots too short for three. A white marker follows the tuner below the plot; there is no center line or sweep cursor.

## Preferences and diagnostics

Manual changes update appended `spectrum_*` fields of `settings_t` through the normal two-second save delay
([settings-store.md](settings-store.md)). Power-off flushes pending changes. Automatic captures and RSSI samples
never update settings or postpone a save. Only manual tuner, center, spacing and BW edits can change the saved tuner.
Opening the analyzer restores its settings; it does not start automatically on Home after boot.

Records from before Spectrum retain their old fields and gain defaults: current-channel center, center tuner,
12.50 kHz, 129 points and squelch -80 dBm. The previous 428-byte record gains 10 ms dwell, Auto peak on and zero
dBm offset. The 432-byte record gains Wide filtering and a direct dBm threshold without changing the old field's
meaning. When `spectrum_squelch_dbm` is unset (`INT16_MIN`) or invalid, a valid legacy level is imported as
`legacy - 160`: old 80 becomes -80 dBm, old 40 becomes -120 dBm and old zero becomes -160 dBm (open).
An invalid legacy value uses old 80. Manual edits save the direct threshold and the closest legacy level, clamped
to the older build's 0..100 range, for a downgrade. `spectrum_peak_immediate` uses the former reserved byte at
offset 433: zero retains After sweep, one selects Immediate. `spectrum_no_peak == 1` takes precedence and keeps
Off. Earlier records and existing Off preferences retain their behavior; the record size stays 436 bytes.

Invalid frequencies fall back to the current channel; invalid spacing or count to 12.50 kHz / 129 points; invalid
dwell to 10 ms, automatic mode to After sweep, filter to Wide and offset to zero. The saved tuner rounds to the nearest bin
inside the restored window, otherwise starts at the center. Existing -40, 0 and +40 dB offsets remain valid.

GET_VIEW `channel` exposes `spectrum.active`, `listening`, `audio`, `center_hz`, `step_hz`, `index`, `tuner_hz`,
`squelch`, `threshold_dbm`, `am` (the active analyzer receiver is in AM), `bins`, `sweeps`, `dwell_ms`, `auto_peak` (0 Off, 1 either enabled mode),
`auto_peak_mode` (`after_sweep` / `immediate` / `off`), `scale_db`, `filter` (`wide` / `narrow`), `peak_hz` and `peak_dbm`
(0 Hz / `INT16_MIN` for an empty peak).

`spectrumTest` covers geometry, timing, dwell, full-sweep selection and confirmation, immediate capture and resume,
Auto off, squelch, audio hold,
expiry, settings retention and time wrap. `radioCoreTest` covers ownership, retuning, PTT inhibition, audio continuity
under configuration edits, restoration, reboot preferences and dual-watch pause/resume (A/B, another zone, a VFO,
changes for now, failed retunes and explicit stops), completed and stalled beep streams, delayed beeps, and both
orders of receiver ownership with a diagnostic detour. `radioFeaturesTest` also covers pausing/restoring a second
receiver and the legacy dual watch in a build without `CONFIG_DUAL_WATCH`. `radioListenBandsTest` covers AM sweeps and both
capture modes, filter requests, AM/FM boundary transitions, refused AM fallback, broadcast exclusion and restoration.
`settingsTest` checks 412-, 428- and 432-byte migration,
round trips and power cuts. `uiFlowTest` covers direct entry, the settings screen, editing, BW cycles, dwell,
automatic mode, filter choice, 20 dB navigation and Back/Home, plus keypad preservation, optional mic input and
arrow/encoder previews, cancellation and band changes without a keypad. Opt-in HIL covers controls and receiver restoration, with a
separate manual RF test for automatic peak listening.
