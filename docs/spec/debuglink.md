<!-- SPDX-License-Identifier: MIT -->

# USB debug link

A small framed protocol on the radio's host port (USB CDC-ACM on radios with USB, a UART otherwise) for bench
automation: read the
firmware version, the diagnostics counters and the radio's state, reset counters, take screenshots, press keys, key
PTT with a firmware time-out, send radio requests, read a log ring, read the chip's identity and option bytes, read
internal flash and the other read-only memories, and reboot. It lives beside the MMDVM hotspot and the
open-firmware CPS protocol on the same port (`core/hostlink`), and it never changes the host link's mode.

- Firmware: `core/debuglink/` (switch `CONFIG_DEBUGLINK`, default on with `CONFIG_HOSTLINK`), key injection in
  `core/input/input_task.c`, the hook in `core/hostlink/hostlink.c`.
- Chip commands: the portable checks in `core/debuglink/dl_chip.c` (host tested), the platform side in `hal/hal_system.h`
  (`board.system`) and the platform's implementation (`platform/stm32f4/sysboot.c`, `platform/hrc7000/system.c`).
- Host: `tools/odr_debug.py` (command line and Python module), `tests/hil/` (pytest hardware-in-the-loop tests).

## 1. Coexistence on the host port

The host link sends each request to one protocol by its first byte (`hostlink.h`):

| First byte | Protocol |
|---|---|
| `0xE0` | MMDVM: enters hotspot mode |
| ASCII letters (`R`, `W`, `X`, `C`, `D`, ...) | open-firmware CPS (`open-firmware-cps.md` 2) |
| `0xA5` followed by `0x5A` | the debug link |
| `0x7E` | PPP (`CONFIG_NET_PPP`): the port goes to PPP, `ppp.md` 4; the debug link then answers over UDP |

`0xA5` is not printable ASCII and not `0xE0`, so it can't start a request of the other two. A debug frame is
recognized where a request may begin:

- **Idle** (no session): always.
- **CPS session**: only after the 3 ms of quiet that starts a new CPS request (bytes inside a CPS write may be
  anything).
- **Hotspot mode**: only between MMDVM frames, when the MMDVM framer is waiting for `0xE0` and would discard the byte
  as junk anyway. MMDVM frames before and after a debug frame in the same USB packet are handled as usual. The debug
  link answers in hotspot mode; its replies go out between MMDVM replies (one task writes both, so they never
  interleave). Debug frames do not count as MMDVM traffic: they don't keep hotspot mode alive and don't enter it.
  In practice only a test tool that speaks both protocols on one open port sees this (the port has one owner).

Once `A5 5A` has been seen, the following bytes belong to the debug frame until it is complete or until
200 ms pass without a byte (the partial frame is then dropped and counted as a time-out). If the byte after `0xA5` is
not `0x5A`, only the `0xA5` is consumed and the next byte goes to the normal dispatch.

Commands run in the host link task. Each is short: no command waits for another task (radio requests are queued
to the radio core, key holds are timed by the input task), and a reply is at most 1032 bytes on the wire.

## 2. Framing

Both directions use the same frame:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `0xA5` |
| 1 | 1 | `0x5A` |
| 2 | 1 | command; replies: command \| `0x80` |
| 3 | 1 | sequence number, chosen by the host, echoed in the reply |
| 4 | 2 | payload length, little endian. Requests at most 64, replies at most 1024 |
| 6 | n | payload |
| 6+n | 2 | CRC, little endian |

The CRC is CRC-16/CCITT-FALSE (polynomial `0x1021`, initial value `0xFFFF`, no reflection, no final XOR; the check
value of `123456789` is `0x29B1`) over the bytes from the command to the end of the payload.

Every reply payload starts with a status byte:

| Status | Meaning |
|---|---|
| 0 | OK |
| 1 | unknown command |
| 2 | bad argument (wrong length, value out of range) |
| 3 | busy: try again shortly (an injected key is still held, the 40 ms gap after it, or the radio request rate limit) |
| 4 | unavailable: not built in, or nothing to act on (no display, no key for that action) |
| 5 | bad CRC (error frames only) |
| 6 | request too long (error frames only) |
| 7 | refused: a safety check said no (READ_MEM outside the readable ranges, the REBOOT gate; 3.11) |

A request that can't be decoded gets an **error frame**: command `0xFF`, the request's sequence number as far as
it was received, payload = status (5 or 6) and the command byte received. A request longer than 64 bytes is
answered as soon as its length field arrives; the parser then resynchronizes on the bytes that follow (they are not
skipped, since they may belong to another protocol).

All multi-byte numbers are little endian. Text is ASCII, lines end with `\n`.

## 3. Commands

| Cmd | Name | Request payload | Reply payload after the status |
|---|---|---|---|
| `0x01` | PING | anything, up to 64 bytes | the same bytes |
| `0x02` | VERSION | none | key=value text (3.1) |
| `0x03` | GET_DIAG | [first line u8], default 0 | ms u32, next u8, text lines (3.2) |
| `0x04` | RESET_COUNTERS | none | ms u32: the firmware clock at the reset |
| `0x05` | SCREENSHOT | none, or offset u32 + length u16 | the frame buffer header and a chunk (3.3) |
| `0x06` | KEY | kind u8 + arguments (3.4) | none |
| `0x07` | PTT | down u8 [+ max ms u16] | the time-out applied, ms u16 (3.5) |
| `0x08` | GET_LOG | none | dropped u32, more u8, text lines (3.6) |
| `0x09` | GET_VIEW | section u8 | ms u32, next section u8, key=value text (3.7) |
| `0x0A` | RADIO | operation u8 + arguments (3.8) | none |
| `0x0B` | INFO_CHIP | none | chip ID, flash size, UID, option bytes decoded (3.9) |
| `0x0C` | READ_MEM | address u32 + length u16 | address u32, the bytes (3.10) |
| `0x0D` | REBOOT | mode u8 + token u32 | reason u8 (3.11) |
| `0x0E` | DMR_DATA | operation u8 + arguments (3.12) | none, or an inbox message (3.12) |
| `0x0F` | STORAGE | operation u8 + arguments (3.13) | region info, SPI flash bytes, or a reason (3.13) |
| `0x10` | AUDIO_PROBE | operation u8 + arguments (3.14); `CONFIG_AUDIO_PROBE` builds only, else status 4 | levels, a register value, or a PCM block (3.14) |
| `0x11` | APRS | operation u8 + arguments (3.16) | none (3.16) |
| `0x15` | SPLIT | operation u8 + arguments (3.15); only in `CONFIG_HOTSPOT_SPLIT` builds, unknown otherwise | none, or the split view text (3.15) |
| `0x19` | SNMP | operation u8 + arguments (3.17) | none (3.17) |
| `0x1A` | STUB | operation u8 + arguments (3.18); `CONFIG_STUB_ENTRY` builds on a supported SoC, else status 4 | region info, progress, or a reason (3.18) |

### 3.1 VERSION

Example from a DM-1701:

```
proto=1                 protocol version (this document)
view=2                  GET_VIEW record version (3.7)
git=5277f73a            firmware git hash
board=DM-1701
features=FM,SUBTONE,DMR,DMR_PRIVACY,...,USB,HOSTLINK,DEBUGLINK
codec=dm1701-v0206-csv  the codec profile found in flash (below)
display=160x120x16      panel width x height x bpp
inset=5,5,5,5           safe area insets: top, bottom, left, right
uptime_ms=123456
req_max=64
reply_max=1024
inject_max_ms=10000     longest hold of an injected key, PTT included
```

`codec=` is the board's own check of the codec in flash, the same one that decides whether DMR voice runs: a
profile id (`dm1701-v0206-csv`: signature word and the CRC-32 of both codec windows match stock V02.06 CSV;
`dm32-049`, `dm32-046`: the DM-32 codec slot is valid and was carved from that stock file), `absent` (the board has a
vocoder but the check failed: erased, another stock version, damaged), `present` (a valid codec whose board names no
profile) or `none` (built without DMR). The first request after boot may take some 20 ms longer while the board
computes the CRCs. It reads flash only. `tools/odrflash` asks for it before flashing to decide whether the codec
sectors can be left alone ([docs/dev/release.md](../dev/release.md)).

Unknown keys must be ignored by hosts; keys may be added without a version change.

### 3.2 GET_DIAG

The diagnostics as text lines, numbered from 0, with the firmware's millisecond clock at the time of reading, so a
host can compute rates between two reads:

| Lines | Content |
|---|---|
| 0, 1 | the radio core's summary: `radio FM MEM ch 12 'Name' 462.56250 RX`, `radio rssi -95 nz 30 sq 3 pw 1W tx 0ms` |
| 2-5 (DMR builds) | the DMR voice path: `call rx= play= late= tx= deny=`, `call vb= lost= dup= enc= txu= evq=`, `voice in= play= conc= sil= und=`, `voice ovf= bad= lost= bits= q= calls=` (the same counters with full names are in the `dmr_stats` view section) |
| then | the board's diagnostics lines (`board.diag_line(0..)`, the radio's diagnostics page) |

The reply holds as many whole lines as fit from `first`; `next` is the line to ask for next, `0xFF` when the last
line was sent. The board lines are each board's own. On the HR_C6000 boards they start with the slot engine counters (`V<voice> H<headers>
T<terminators> I<idle>`, `C<csbk> D<data> crc<errors> rc<rc>`, ...) and end with the readings and the audio counters
(`aud blk ov dr`).

### 3.3 SCREENSHOT

Request: none (header only), or offset u32 and length u16 (at most 1000 bytes per reply). Reply after the status:

| Offset | Size | Field |
|---|---|---|
| 1 | 2 | width (the whole panel, insets included) |
| 3 | 2 | height |
| 5 | 1 | bpp |
| 6 | 1 | format: 1 = RGB565 little endian, row-major; 2 = 1 bpp rows of (width + 7) / 8 bytes, MSB left |
| 7 | 4 | insets: top, bottom, left, right |
| 11 | 4 | total frame buffer bytes |
| 15 | 4 | offset of this chunk |
| 19 | n | the chunk |

The chunks are read live from the UI's frame buffer, without a lock (the UI task owns it, and a 38 KB copy would
not fit in RAM). A redraw while the host reads the chunks can tear the image. `odr_debug.py screenshot` reads the
whole buffer twice and repeats until two reads agree (`--tries`, default 4), which gives a consistent image unless
the screen changes continuously (a running timer); it says so when it gives up.

While a capture is read the UI does not redraw: every SCREENSHOT request holds redraws for 1 s, and the request that
returns the last chunk for 250 ms, long enough for the host's check pass to start. A key or radio request in that
time changes the UI's state (`ui.screen` in GET_VIEW) at once, but its screen is drawn only when the hold lapses, so
a host that captures a new screen waits for the hold to end (`tests/hil/odr_hil.py`: `Radio.screenshot` and
`shot_until`). Firmware before this rule held for 1 s after the last chunk too.

### 3.4 KEY

Injected keys go through the same path as the radio's own keys: their physical key bits are ORed into the input
task's scan, so debounce, the gesture engine (PRESS / LONG / REPEAT), the PTT and MONITOR edges to the radio core,
and the UI's idle and backlight time all behave as for a real key. `input_last_source()` tells the UI that the last
activity came from the host.

| Kind | Arguments | Effect |
|---|---|---|
| 0 | action u8, gesture u8, value i8 | a logical action (below) with a gesture: PRESS holds the key that gives it for 80 ms, LONG for 720 ms (released after the LONG event), REPEAT for 720 ms + value x 120 ms; DOWN / UP hold and release a raw-edge key (MONITOR; PTT goes to the PTT slot with the 10 s time-out); `enc` with DOWN adds value detents. An action no key gives (e.g. left / right on the DM-1701) goes straight to the UI's event queue |
| 1 | mask u32, hold ms u16 | hold the board's physical key bits for hold ms |
| 2 | none | wake: counts as key activity without an event, so the backlight comes on (the UI drops the first key after the backlight went off) |
| 3 | action u8, hold ms u16 | hold the key that gives the action for hold ms, whatever gesture that makes |

Actions (`hal_input.h`): 1 up, 2 down, 3 left, 4 right, 5 select, 6 back, 7 quick, 8 context, 9-18 digits 0-9,
19 ptt, 20 monitor, 21 talk target, 22 emergency, 23 enc (encoder step), 24-27 user 1-4. Gestures: 0 PRESS, 1 LONG,
2 REPEAT, 3 DOWN, 4 UP.

Rate limit: one injected key at a time. While one is held, and for 40 ms after its release (two scans for the
debounce to see it released), KEY answers busy (3). Every hold is capped at 10 s.

### 3.5 PTT

`down` = 1 holds the PTT key, 0 releases it. The optional max ms (default and maximum 10000) is a time-out kept by
the input task: the PTT is released when it expires even if the host never sends `down` = 0 (the host crashed, the
cable was pulled). Sending `down` = 1 again while held re-arms the time-out. The reply carries the time-out applied.
PTT goes through the radio core like the real key, so every transmit check applies (band, RX-only channel, TX
time-out, hotspot mode). The PTT slot is separate from KEY's, so KEY presses work while PTT is held.

### 3.6 GET_LOG

Firmware code logs with `dlog(fmt, ...)` (`core/debuglink/dlog.h`: `%d %i %u %x %X %s %c`, width, `0` flag; no
floats). Lines go into a 2 KB RAM ring as `<ms> text`; when it is full the oldest lines are dropped and counted.
GET_LOG takes whole lines, oldest first (the read removes them), and reports the lines dropped since the last read
and whether more remain. Built without the debug link, `dlog()` compiles to nothing.

### 3.7 GET_VIEW

The radio's state as key=value lines, one section per request, so each fits a reply. `next` is the following
section, `0xFF` after the last. Every section starts with `section=<name>`; a section cut short ends with
`truncated=1`. Hosts ignore keys they don't know; keys may be added at any time, and `view=` in the core section
(and in VERSION) is bumped only when a key changes meaning or goes away. Each request takes a fresh snapshot
(`radio_get_view()`), so sections may be a few ms apart: compare `serial=` in the core section when that matters.

| # | Section | Keys |
|---|---|---|
| 0 | `core` | `view state tx_deny mode vfo zone_index zone_pos zone_size zone_count zone_name channel_number channel_name rx_hz tx_hz power power_name power_count temporary temp squelch monitor tone_ok rssi_dbm noise tx_elapsed_ms tot_s last_rx_ms vfo_step_hz codeplug hotspot serial` |
| 1 | `channel` | `ch.name ch.rx_hz ch.tx_hz ch.mode ch.bw ch.rx_tone ch.tx_tone ch.power ch.rx_only ch.color_code ch.timeslot ch.contact ch.listen_list ch.dmr_id ch.dmo` |
| 2 | `dmr` | `dmr.built dmr.active dmr.codec dmr.state dmr.phase dmr.tx_error dmr.own_id dmr.cc dmr.ts dmr.rmo dmr.repeater_up dmr.target_id dmr.target_private dmr.target_name dmr.reply_active dmr.reply_s dmr.hear_all dmr.hear_all_latch dmr.focus_id dmr.focus_private dmr.one_shot dmr.underruns dmr.target_manual dmr.privacy dmr.alias_tx dmr.tx_alias` (the alias text our calls send, empty when none) |
| 3 | `dmr_call` | `call.src call.dst call.group call.to_us call.encrypted call.emergency call.pi_header call.src_name call.dst_name call.late_entry call.alias_complete call.fid call.svc call.pi_alg call.pi_key call.alias call.rssi_dbm call.ms`, the same with `last.` (`src` to `dst_name`) and `last.ms`. The caller's ETSI GPS Info ([dmr-voice.md](dmr-voice.md) GPS Info): `call.gps_valid` (and `last.`), when 1 `call.gps_lat call.gps_lon` (degrees, 7 decimals) `call.gps_err` (position error code 0-7), when our own position is known `call.gps_dist_m call.gps_bearing` (degrees from north) |
| 4 | `dmr_stats` | call stack (`call.rx_calls rx_calls_played rx_late_entries rx_voice_bursts rx_lost_frames rx_dup_dropped rx_enc_dropped rx_term_dropped tx_calls tx_denied tx_underruns evq_overflows rx_private_calls rx_emergency_calls rx_encrypted_calls rx_pi_headers rx_alias_complete tx_alias_lcs rx_gps tx_gps_lcs`), AMBE voice (`voice.rx_in rx_frames rx_concealed rx_silence rx_calls rx_queue_max rx_bad rx_lost rx_underruns rx_overflows rx_bit_errors tx_frames tx_overflows`) |
| 5 | `hotspot` | `hostlink` (idle / hotspot / cps), `hostlink.left` (why hotspot mode was last left: none / user / closed = DTR dropped / gone / idle), `hs.running hs.transmitting hs.receiving hs.rx_hz hs.tx_hz hs.cc hs.src hs.dst hs.group hs.from_net` and the engine counters `hs.net_frames ... hs.enter_attempts hs.starts hs.stops` (counted since boot, across sessions); USB network builds `usb.network usb.net_up` and with the IP gateway `usb.boot_network` and `net.*` (`docs/spec/usb-network.md` 3, 5.4: `net.passthrough net.passthrough_active net.fixed_link net.link_ip net.link_in_force` among them); `CONFIG_NET_PPP` builds `ppp.running ppp.ip_up ppp.lcp ppp.ipcp` (RFC 1661 state numbers, 9 = opened) `ppp.rx_frames ppp.tx_frames ppp.rx_errors` (`docs/spec/ppp.md`) (the SFR keys moved to section 19 in view version 2) |
| 6 | `link` | `uptime_ms ui.screen` (the screen's name, `docs/spec/ui.md` 2) `ui.depth ui.focus ui.count ui.locked ui.banner ui.toast ui.query ui.draws input.idle_ms input.long_ms input.source input.raw_keys input.injected input.encoder_total input.enc_detents input.enc_transitions input.enc_illegal input.enc_reversals input.enc_steps input.enc_deferred dl.frames dl.bad_crc dl.too_long dl.timeouts` |
| 7 | `features` | `scan.built scan` (off, zone, vfo, dual) `scan_phase` (listen, probe, hold, resume, paused) `scan_skipped scan_probes scan_synced tot_warning tone_burst roger vox_keyed vox_on vox.mic` (1: the radio reads its mic level, so VOX can work) `rx_sleeping battery` (ok, low, critical) `battery_pct battery_mv power_off`, `beep.built beep.played beep.no_stream beep.blocks beep.stalls beep.busy`, and the radio settings `set.tot_s set.tot_warn_s set.bcl set.roger_beep set.tx_inhibit set.scan_resume set.scan_hold_s set.scan_delay_ms set.scan_dwell_ms set.scan_priority set.beep_level set.key_beep set.vox set.vox_level set.vox_tail_ms set.rx_save set.rx_save_idle_s set.battery_off_s set.reply_s` (a number, or `codeplug` for the hang times; `docs/spec/radio-features.md`) |
| 8 | `dmr_data` | `data.built data.busy data.sms_state data.signal_state data.signal_kind data.signal_dst data.ars_state data.inbox_count data.inbox_unread data.serial data.event data.event_serial data.event_id data.ars_server data.radio_check_reply data.call_alert_ack data.lrrp_reply data.has_gps data.lrrp_triggers`; the counters are in section 11 (meanings: `core/dmr/dmrDataService.h` dmrDataStats_t). States: `idle queued sending ok failed`; ARS `off registering registered refused no_answer`; `data.event` the last event's name (`sms_received`, `call_alert`, `radio_check_answered`, ...), `data.event_id` its radio ID |
| 9 | `storage` | settings store (`settings.load` ram / defaults / loaded / upgraded, `settings.dirty seq saves erases failures sector slot skipped`), flash journal (`storage.sector_writes sector_skips replays errors`), `codeplug.source codeplug.serial`, the UI settings (`settings.auto_lock_s long_press_ms repeat_ms mic_gain theme ui_flags updown_home low_batt_pct auto_off_min`, `settings.privacy_type` of key slot 1 and `settings.privacy_active`; never the key), `storage.job_busy` (a save to channel, permanent talk target or factory reset pending on the housekeeping task), foreign region guard (`storage.region` none / ours / foreign, `storage.journal` none / ok / foreign, `storage.foreign_slots foreign_refusals claim_pending`; `settings.load` also `foreign`), open-firmware CPS (`cps.built requests reads commits refused overflows last session pending`; [settings-store.md](settings-store.md), [cps-protocol.md](cps-protocol.md)) |
| 10 | `dmr_ext` | DMR extensions ([dmr-emergency.md](dmr-emergency.md), [dmr-data.md](dmr-data.md) 5.2): the ETSI forms `data.sms_format` (etsi_ip / etsi / tms, the radio's default) `data.radio_check_etsi` `data.tms_built` (`CONFIG_DMR_TMS`), ETSI IP text `data.ip_text_received data.ip_text_sent`, control station passthrough `data.pt_active data.pt_yielded` ([usb-network.md](usb-network.md) 5.2), counters `data.etsi_sms_received etsi_sms_sent etsi_responses emergency_alarms_heard emergency_alarms_received emergency_alarms_sent`; emergency `em.built em.state` (idle / alarm / hot_mic / active / failed) `em.serial em.mode` (alarm / alarm_call / call) `em.dst em.dst_private em.tries em.hot_mic_s em.etsi_fid em.rx_tone`, the newest alert `em.alert` (1 = not dismissed) `em.alert_kind` (none / alarm / call) `em.alert_src em.alert_dst em.alert_group em.alert_pending em.alert_serial`, counters `em.started failed alarms_started hot_mic_keyed alerts alerts_alarm alerts_call tones`; hotspot builds the CW ID `hs.cw_requests hs.cw_sent hs.cw_refused hs.cw_last_ms hs.cw_wpm hs.cw_active` |
| 11 | `dmr_data_stats` | the DMR data service counters, split from `dmr_data` because both together exceed one reply: `data.rx_bursts rx_csbk rx_headers rx_blocks rx_blocks_stale rx_crc_errors rx_crc_flag_mismatch rx_packets rx_for_us rx_confirmed sms_received sms_duplicates sms_sent sms_acked sms_failed tms_acks_sent responses_sent csbk_answers_sent call_alerts_received radio_checks_received call_alerts_sent call_alerts_acked radio_checks_sent radio_checks_answered ars_sent ars_acks lrrp_requests lrrp_answers lrrp_no_fix tx_lists tx_ok tx_failed tx_gave_up tx_last_error` |
| 12 | `roam` | DMR repeater roaming ([dmr-roaming.md](dmr-roaming.md) 6.3): `roam.built roam.zone_on roam.state` (off / watch / search / survey / locked) `roam.why roam.pending` (none / weak / tx_fail / manual / check / stale) `roam.blocked` (none / mode / hotspot / scan / tx / call / reply / quiet / override / emergency) `roam.sites roam.site roam.home roam.channel roam.rssi_dbm roam.weak`, counters `roam.searches roams stays aborts probes triggers`, `site.<i>=<channel>:<rssi>` per site (rssi 0 = not measured), the survey `roam.visit` (site being visited, -1 at home) `roam.win_conf` (0 no beacon schedule, 1..3) `roam.win_next_ms` (to the next beacon window, -1 unknown) `roam.confirm` (a confirm visit is pending), counters `roam.surveys visits cuts beacons confirms`, `surv.<i>=<dbm>:<age_s>:<phase_ms>:<fresh>` per site (dbm 0 = never surveyed, age and phase -1 = unknown), the settings `set.roam_rssi_dbm set.roam_hyst_db set.roam_probe` (passive / wake) `set.roam_check_s set.roam_beacon_s set.roam_beacon_len_s set.roam_survey` (off / sync / site) `set.rpt_beacon`. Built without roaming: `roam.built=0` |
| 13 | `aprs` | APRS ([aprs.md](aprs.md) 13): `aprs.built aprs.rx_built aprs.igate_built aprs.call` (with SSID, empty = none) `aprs.block` (ok / no_call / no_freq / not_amateur / no_position / no_afsk / busy: why RF can't send) `aprs.freq_hz aprs.pos_src` (none / gps / host / fixed) `aprs.pos_valid`, when valid `aprs.lat aprs.lon` (degrees, 7 decimals) `aprs.pos_age_s`, `aprs.next_s` (-1 = no time based beacon) `aprs.last_why` (none / interval / smart / turn / distance / ptt / manual / boot) `aprs.last_ms aprs.rf_active aprs.last_info`, counters `aprs.rf_sent rf_failed rf_busy dmr_sent dmr_failed rx_frames rx_bad host_in host_out host_dropped igated is_to_rf digi_sent`, `aprs.stations messages msg_pending msg_tries msg_acked`, `aprs.rx_listening` (the receive path runs: on the APRS frequency in FM with receive on, [aprs.md](aprs.md) 5.3), RX builds `aprs.rx_peak` (largest sample since the path started: 0 = only silence) `rx_bits` (tone decisions) `rx_hdlc_frames rx_fcs_errors rx_aborts` (the HDLC decoder's), `aprs.tx_power` (the power level an APRS RF transmission keys at: the radio's current one, `power` in `core`). Built without APRS: `aprs.built=0` |
| 14 | `aprs_set` | the APRS settings, split from `aprs` for the reply size: `aprs.built set.aprs_tx aprs_region aprs_ssid aprs_symbol aprs_path aprs_interval_s aprs_smart aprs_distance_m aprs_after_ptt aprs_boot aprs_level aprs_txdelay aprs_via` (rf / dmr / both) `aprs_dmr_dst aprs_dmr_udt aprs_pos_src aprs_kiss aprs_rx aprs_igate aprs_igate_tx aprs_digi aprs_dmr_gps aprs_to_is aprs_power` |
| 15 | `dmr_ip` | IP packets over DMR ([dmr-data.md](dmr-data.md) 12.5): `ip.built ip.compress_built ip.confirmed_built`, the settings `ip.compress` (off / auto / all) `ip.confirmed` (off / rules / all) `ip.tries ip.rule0 ip.rule1` (none, `id:N` or `port:N`), the gateway packet in the service `ip.busy ip.result` (idle / ok / failed ...), compression `ip.sap3 ip.sap4 ip.octets_saved ip.blocks_saved`, the gateway's packets `ip.packets ip.sent ip.failed`, confirmed `ip.cnf ip.cnf_tx ip.cnf_retries ip.cnf_acks ip.cnf_sacks ip.cnf_nacks ip.cnf_resyncs ip.cnf_timeouts ip.cnf_failed ip.cnf_too_long`, confirmed data to us `ip.rx_sacks_sent ip.rx_nacks_sent ip.rx_duplicates ip.rx_crc9_mismatch`. Built without the data features: `ip.built=0` |
| 16 | `net` | the radio's own services on the USB network link: with TCP (`tcp-services.md`) `tcp.open tcp.segs_in tcp.segs_out tcp.retransmits tcp.rst_out tcp.bad_checksum tcp.accepted tcp.refused tcp.no_slot tcp.timeouts` and the TCP services' keys, `net.link_up`; with mDNS `mdns.state mdns.name mdns.queries mdns.answered mdns.sent mdns.busy mdns.conflicts mdns.bad` (`mdns.md` 5), with TFTP `tftp.state tftp.reads tftp.writes tftp.errors tftp.timeouts tftp.retransmits tftp.last_size tftp.blksize tftp.write_allowed` (`tftp.md` 5); `net.built=0` without any of them |
| 17 | `snmp` | the SNMP agent ([snmp.md](snmp.md) 7): `snmp.built snmp.flags snmp.write_set snmp.trap_port`, counters `snmp.rf_requests rf_answers rf_dropped traps_usb traps_rf traps_dropped` (the protocol counters and the other settings: the MIB). Built without the agent: `snmp.built=0` |
| 18 | `usrp` | USRP voice over UDP ([usb-audio.md](usb-audio.md) 9.1): `usrp.peer` (the address that last sent a valid frame, 0.0.0.0 = none: the host's address is used) `usrp.keyed` (the peer's keyup) `usrp.tx` (we asked the radio to transmit) `usrp.cos` (the last frame sent had keyup 1) `usrp.rx_on` (the receive audio goes to the host: network mode is up), counters `usrp.frames voice lost out_of_order bad keys timeouts tx_limits sent send_busy refused ptt_downs`, the shared stream `mux.blocks rx_samples rx_overflow tx_blocks fm_pulls starts start_fails tx_fill`, the jitter buffer `slip.pushed underruns drops repeats primes`. Built without it: `usrp.built=0` |
| 19 | `sfr` | The MMDVM hotspot's SFR mode (`docs/spec/sfr.md`): `sfr.on sfr.keyed sfr.busy sfr.slots sfr.rx sfr.repeated sfr.ignored sfr.echo sfr.bad_crc sfr.tx_bursts sfr.tx_idle sfr.tx_starts sfr.up sfr.up_lost sfr.tx_beacons sfr.tx_gaps`. Built without it: `sfr.built=0` |

Value forms: numbers in decimal (`temp`, `input.*keys` in hex with `0x`), states as lower-case words, tones as
`type:value` (`ctcss:1000` = 100.0 Hz, `dcs:19` = D023), strings as they are with control characters replaced by
`?`. Built without DMR the DMR sections hold `dmr.built=0`; built without the DMR data features `dmr_data` holds
`data.built=0`. The DMR sections are rendered in
`core/debuglink/debuglink_view_dmr.c`, apart from the rest, so DMR work can extend them.

### 3.8 RADIO

Radio requests, queued to the radio core exactly as the UI sends them (`core/radio/radio.h`); the radio core may
refuse them as it would refuse the UI (e.g. a bandwidth change on a DMR channel). Read GET_VIEW to see the effect.
At most one per 20 ms (busy otherwise; the radio core's queue holds 16).

| Op | Arguments | Request |
|---|---|---|
| 1 | dir i8 | `radio_req_channel_step`: next / previous channel in the zone |
| 2 | pos u16 | `radio_req_zone_pos`: position in the zone, 0-based |
| 3 | index u16 | `radio_req_zone` |
| 4 | on u8 | `radio_req_vfo` |
| 5 | hz u32 | `radio_req_vfo_set` (simplex) |
| 6 | dir i8 | `radio_req_vfo_step` |
| 7 | dir i8 | `radio_req_power` (a temporary change, wraps) |
| 8 | dir i8 | `radio_req_squelch` |
| 9 | bw u8 | `radio_req_bandwidth`: 0 12.5 kHz, 1 25 kHz |
| 10 | which u8, type u8, value u16 | `radio_req_tone`: which 1 TX, 2 RX, 3 both; type 0 none, 1 CTCSS (Hz x 10), 2 DCS, 3 DCS inverted |
| 11 | on u8 | `radio_req_monitor` |
| 12 | none | `radio_req_clear_temporary` |
| 13 | none | `radio_req_reply_cancel` (DMR builds) |
| 14 | none | `hostlink_hotspot_exit`: leave hotspot mode |
| 15 | target u32 | `radio_req_dmr_target`: ID or talkgroup in bits 23:0, bit 24 = private call (DMR builds) |
| 16 | index u16 | `radio_req_dmr_contact`: a codeplug contact as the talk target |
| 17 | none | `radio_req_dmr_call_back`: talk to the last call heard (its ID if private, its talkgroup if group) |
| 18 | none | `radio_req_dmr_target_clear`: back to the channel's talk target |
| 19 | type u8, key u16 | `radio_req_dmr_privacy`: basic privacy key in RAM, type 0 off, 1 Motorola BP (key 1-255), 2 Anytone BP (CONFIG_DMR_PRIVACY builds; never stored) |
| 20 | cmd u8 [, pos u16] | `radio_req_scan`: 0 stop (stay where it is), 1 zone, 2 VFO (the band of the VFO frequency), 3 dual watch (pos = the second channel, required), 4 cancel (back to where it started), 5 nuisance delete (skip the current channel until the scan ends), 6 toggle (CONFIG_SCAN builds) |
| 21 | lo u32, hi u32 | `radio_req_scan_vfo`: VFO scan from lo to hi Hz in the VFO step |
| 22 | beep u8 | `radio_req_beep`: 1 key (only with `set.key_beep`), 2 error, 3 TX permit, 4 TX deny, 5 call end, 6 time-out warning, 7 battery low, 8 battery critical, 9 scan hold (CONFIG_BEEP builds) |
| 23 | on u8 | `radio_req_tone_burst`: the 1750 Hz burst, keys up while on (FM channels, CONFIG_FM_TX_TONES builds) |
| 24 | id u8, value u16 | one radio setting in RAM (`radio_set_settings`; not stored): 1 tot_s, 2 tot_warn_s, 3 bcl (0 off, 1 carrier, 2 wrong tone), 4 roger_beep, 5 tx_inhibit, 6 scan_resume (0 time, 1 carrier, 2 stop), 7 scan_hold_s, 8 scan_delay_ms, 9 scan_dwell_ms, 10 scan_priority, 11 beep_level (0-100), 12 key_beep, 13 vox (0 off, 1 channel, 2 all), 14 vox_level (0-10), 15 vox_tail_ms, 16 rx_save (0-3), 17 rx_save_idle_s, 18 battery_off_s, 19 scan_beep, 20 reply_s (0-30, 255 = the codeplug's hang times), 21 roam_rssi_dbm (-125..-60, two's complement), 22 roam_hyst_db (1-30), 23 roam_probe (0 passive, 1 wake), 24 roam_check_s (0 or 30-3600), 25 roam_beacon_s (0 or 10-600), 26 roam_beacon_len_s (1-10), 27 roam_survey (0 off, 1 synced, 2 per site), 28 rpt_beacon (0/1). Out of range: bad argument |
| 25 | on u8: 1 network, 0 serial | switch the USB personality once the reply has gone out (`CONFIG_USB_NETWORK`; status 4 without it). The radio re-enumerates; in network mode this link answers over UDP to the radio's address, port 3334 (`docs/spec/usb-network.md`) |
| 26 | cmd u8 | `radio_req_roam` (CONFIG_DMR_ROAMING builds; status 4 without): 0 zone off, 1 zone on, 2 zone toggle (the current zone's roaming flag, stored), 3 roam now, 4 lock to this site, 5 unlock, 6 lock toggle ([dmr-roaming.md](dmr-roaming.md) 6.1) |
| 27 | flags u8, link IP u32 | the USB network settings, stored and applied at once (`docs/spec/usb-network.md` 5.4): bit 0 the network personality at power-on, bit 1 control station passthrough, bit 2 the fixed link pair; the radio's link address (0 = 192.168.10.1). Bits 1 and 2 need `CONFIG_DMR_CONTROL_STATION` (ignored without); UNAVAILABLE without `CONFIG_USB_NETWORK`. `odr_debug.py radio usb-settings [boot] [passthrough] [fixed[=IP]]` |
| 28 | on u8: 1 start, 0 end | PPP on the serial host port once the reply has gone out (`CONFIG_NET_PPP` builds; status 2 without): 1 starts it actively (our LCP Configure-Request goes out at once; ignored while the USB network adapter is in use), 0 ends it with LCP Terminate (send it over UDP while PPP runs). pppd starts PPP by itself, so 1 is only for peers that wait. While PPP runs this link answers over UDP to the radio's address, port 3334, as in USB network mode (`ppp.md` 4). `odr_debug.py radio ppp on\|off` |

Ops 15-18 last until the channel changes (`dmr.target_manual`). Unavailable ops answer UNAVAILABLE.

### 3.9 INFO_CHIP

INFO_CHIP, READ_MEM (3.10) and REBOOT (3.11) go through `board.system` (`hal/hal_system.h`). The fields, ranges and
modes below are the STM32F4 platform's (`platform/stm32f4/sysboot.c`), the only one whose `chip()` reports anything
today. On the HR_C7000 platform (DM-32UV, DB25-D) `chip()` reports nothing, so INFO_CHIP and READ_MEM answer
UNAVAILABLE and REBOOT is refused with reason 4.

Read-only. Reply after the status (29 bytes in all):

| Offset | Size | Field |
|---|---|---|
| 1 | 4 | DBGMCU IDCODE: device ID bits 11:0 (`0x413` = STM32F405/407), revision bits 31:16. May read 0 on some STM32F40x revisions when no debugger is attached |
| 5 | 2 | flash size in KB (the flash size register at `0x1FFF7A22`) |
| 7 | 12 | 96-bit unique ID (`0x1FFF7A10`), three u32, lowest word first. The lowest word is REBOOT's confirmation token |
| 19 | 4 | `FLASH_OPTCR` as read |
| 23 | 1 | RDP level: 0 (byte `0xAA`), 2 (byte `0xCC`), 1 (any other byte) |
| 24 | 1 | the RDP byte (`FLASH_OPTCR` bits 15:8) |
| 25 | 2 | write-protected sectors, bit n = sector n (from nWRP bits 27:16, where 0 means protected) |
| 27 | 1 | BOR level field (bits 3:2): 0 = level 3 (2.7 V), 1 = level 2, 2 = level 1, 3 = off |
| 28 | 1 | user bits: bit 0 WDG_SW, bit 1 nRST_STOP, bit 2 nRST_STDBY |

`odr_debug.py chip` prints all of this, and prints a loud warning when RDP is not level 0 or when any application
sector (3..11) is write protected.

Example (the bench DM-1701, 2026-10-10): idcode `0x0009A413`, 1024 KB, `FLASH_OPTCR` `0x0FFF55ED`: **RDP level 1**
(byte `0x55`), no write protection, BOR off, user bits all 1.

### 3.10 READ_MEM

Request: address u32, length u16 (1 to 1000). Reply after the status: the address u32, then the bytes. The whole range
must lie inside one of these, otherwise the answer is refused (7):

| Range | Contents |
|---|---|
| `0x08000000` up to the flash size (at most `0x080FFFFF`) | internal flash, the stock bootloader at `0x08000000-0x0800BFFF` included |
| `0x1FFF0000-0x1FFF77FF` | system memory (ST's ROM bootloader) |
| `0x1FFF7800-0x1FFF7A0F` | OTP and its lock bytes |
| `0x1FFFC000-0x1FFFC00F` | option bytes |

RAM, CCM and peripheral addresses are always refused: a read there can have side effects (clearing a status flag,
popping a FIFO). The UID and the flash size register come through INFO_CHIP instead.

RDP and reads: read protection level 1 does **not** stop the application from reading flash. Level 1 blocks flash
access only from a debugger (SWD/JTAG), from code running in RAM and when the chip boots from system memory (the
BOOT0 pin); code running from user flash, as this firmware does, reads every flash address. So READ_MEM works at
RDP level 1, and that is how the stock bootloader was saved on the bench unit.

The web flasher and odrflash read the bootloader sectors this way to find the bootloader key by its SHA-256
(`hardware-dm1701.md` 15.3), so updating a radio that runs ODR needs no stock file. The bytes stay in memory; only the
key is kept.

`odr_debug.py dump --addr A --len N [-o FILE]` reads in 1000-byte pieces. It refuses to write inside any git work
tree; the default file is `the external reference folder<addr>_<len>.bin`. The stock bootloader is proprietary:
a dump of it is never committed or published.

### 3.11 REBOOT

Request: mode u8, token u32. Reply: the status and a reason byte.

| Mode | Effect |
|---|---|
| 0 | normal reset (`NVIC_SystemReset`): the stock bootloader runs, then the application |
| 1 | ROM DFU: reset, then straight into ST's system-memory bootloader (USB DfuSe, `0483:DF11`). Only at RDP level 0 |

The token must be the low 32 bits of the chip UID (INFO_CHIP offset 7), so a stray frame can't reboot the radio.
The checks, at the request and again just before the reset:

| Reason | Refused when |
|---|---|
| 1 | the token is not the UID's low word (both modes) |
| 2 | mode 1 and RDP is not level 0. At level 1 the ROM bootloader refuses flash access, and its read-unprotect command mass-erases all of flash, the stock bootloader included, which would leave the radio unable to load firmware without SWD. Level 2 disables the ROM bootloader |
| 3 | transmitting: radio state TX, or the hotspot sending (both modes) |
| 4 | the chip info could not be read |
| 5 | (logged only) the ROM DFU flag could not be stored in the backup register |

On OK the reply goes out first and the firmware resets 250 ms later. If a check fails at that point (a PTT came
in), it logs `debuglink: reboot ... aborted` and stays up. How mode 1 enters the ROM:
`docs/spec/hardware-dm1701.md` 15.4.

`odr_debug.py reboot [--rom-dfu]` reads INFO_CHIP first, refuses `--rom-dfu` itself unless RDP is level 0, sends
the token and prints the radio's reason when it refuses.

### 3.12 DMR_DATA

Requests to the DMR data service (`docs/spec/dmr-data.md`), through the same mailbox as the UI's
(`core/dmr/dmr_data_api.h`): the radio task takes them at its next tick, so OK means queued, not sent. Follow the
result in the `dmr_data` view section (`data.sms_state`, `data.signal_state`, the counters). IDs are u32 little
endian (24 bits used). BUSY: one of the kind is still queued; UNAVAILABLE: built without the data features.

| Op | Name | Arguments | Effect |
|---|---|---|---|
| 1 | SMS | destination u32, flags u8 (bit 0 group, bit 1 ACK requested, bit 2 ETSI short data, bit 3 the radio's default format, bit 4 ETSI IP text; none of them: TMS, BUSY without `CONFIG_DMR_TMS`), text (Latin-1, the rest of the payload, up to 58) | send a text message |
| 2 | CALL_ALERT | target u32 | send a call alert (3 tries) |
| 3 | RADIO_CHECK | target u32 | send a radio check (3 tries) |
| 4 | SETTINGS | ARS server u32 (0 off), flags u8 (bit 0 answer radio checks, bit 1 acknowledge call alerts, bit 2 answer LRRP, bit 3 text messages in ETSI short data by default, bit 4 radio checks in the ETSI form, bit 5 text messages in ETSI IP text by default; neither bit 3 nor 5: TMS) | the service's settings (the radio core saves them in the settings store) |
| 5 | INBOX | index u8 (0 newest) | reply: source u32, destination u32, group u8, flags u8 (bit 0 unread, bit 1 received as ETSI short data, bit 2 received as ETSI IP text), the text; UNAVAILABLE when there is no such message |
| 6 | MARK_READ | index u8 | mark an inbox message read |
| 7 | EMERGENCY | u8: 0 cancel, 1 start (the emergency key), 2 dismiss the alert | [dmr-emergency.md](dmr-emergency.md); start answers BUSY while an emergency runs; UNAVAILABLE without `CONFIG_DMR_EMERGENCY` |
| 8 | EMERGENCY_SETTINGS | mode u8 (0 alarm, 1 alarm then call, 2 call), target u32 (0 the channel's), flags u8 (bit 0 the target is a radio, bit 1 ETSI FID, bit 2 no alert tone), alarm tries u8 (1-10), hot mic s u8 (0-60) | the emergency settings (saved by the radio core); out of range values are clamped |
| 9 | IP_SETTINGS | compression u8 (0 off, 1 auto, 2 all), confirmed u8 (0 off, 1 rules, 2 all), tries u8 (0 the default 4, 1-8), rule u32 x 2 (0 unused, a radio ID, or 0x80000000 \| a UDP port) | IP packets over DMR ([dmr-data.md](dmr-data.md) 11, 12; saved by the radio core); BAD_ARG out of range |
| 10 | IP_SEND | destination u32, flags u8 (bit 0 group), source port u16, destination port u16, payload (up to 54) | a UDP datagram into the IP gateway's air path as if from the host (`ipGatewayIPToAir`); BUSY while the gateway's mailbox holds one; UNAVAILABLE without `CONFIG_DMR_IP_GATEWAY` |

RESET_COUNTERS also zeroes the data service's counters. `odr_debug.py data sms|alert|check|settings|inbox|read|emergency|em-settings|ip-settings|ip-send`.

### 3.13 STORAGE

The settings ring / journal region (`settings-store.md` 5): its state, a read-only SPI flash window for dumping it,
and the claim of a foreign region.

| Op | Request | Reply after the status |
|---|---|---|
| 0 INFO | none | region u8 (0 none, 1 ours, 2 foreign), claim pending u8, then address u32 + size u32 for the settings ring, the journal, the calibration copy |
| 1 READ | address u32, length u16 (1-1000) | address u32, the bytes. Only inside the three ranges of INFO, else `DL_ST_REFUSED`. Taken under the storage lock |
| 2 CLAIM | token u32: the chip UID's low word (INFO_CHIP offset 7) | reason u8. `DL_ST_REFUSED` with reason 1 (bad token) or 4 (no chip info); `DL_ST_UNAVAILABLE` when the region isn't foreign. On OK the housekeeping task erases the region (10 sector erases, up to about 4 s, during which it does not watch the power knob); poll `storage.region` |

`odr_debug.py storage info | dump | claim` drives it; `claim` always dumps first, outside any git tree.

### 3.14 AUDIO_PROBE

A bench tool for the USB audio feasibility work ([hardware-dm1701.md](hardware-dm1701.md) 21.3), only in firmware built with
`CONFIG_AUDIO_PROBE=y` (off by default, never for a release). It reads and writes registers of the chip that owns
the audio path (`board.audio_tap->modem_reg`; on the DM-1701 the HR_C6000, pages and registers as in
`modem-hrc6000.md`), runs the board's PCM stream with its own block callback, which sends a test tone, and measures
both I2S channels (`core/debuglink/debuglink_audio_probe.c`, `hal/hal_audio_tap.h`). Only the DM-1701 provides
`board.audio_tap` today; on other boards the ops that need it answer UNAVAILABLE. The page, register and RAM
semantics in the table are the HR_C6000's.

| Op | Request | Reply after the status |
|---|---|---|
| 0 STATUS | none | flags u8 (bit 0 the probe owns the stream, bit 1 the stream runs), blocks u32, then for the left and the right channel: peak u16, RMS u16, peak since start u16 (of the last 20 ms block, full scale 32767) |
| 1 REG_READ | page u8, register u8 | value u8 |
| 2 REG_WRITE | page u8, register u8, value u8 | the value read back u8 (logged) |
| 3 START | tone Hz u16 (0-3800), amplitude u16 (0 = silence), flags u8 (bit 0: route the PCM to the speaker) | none. `DL_ST_BUSY` while DMR voice owns the stream; `DL_ST_UNAVAILABLE` when the stream can't start (no I2S clock) |
| 4 STOP | none | none |
| 5 SAMPLES | none | blocks u32, the last block's 160 left then 160 right samples, i16 little endian |
| 6 RAM_READ | page u8, address u16 (11 bit), length u16 (1 to reply_max - 1) | the bytes (`board.audio_tap->modem_ram`; HR_C6000 page 2 reads the RX RAM) |
| 7 RAM_WRITE | page u8, address u16, 1 to 60 bytes | none (page 2 writes the TX RAM) |
| 9 MUSIC | time ms u16 (1-2000), tone Hz u16 (0-3800), amplitude u16, flags u8 (bit 0 little endian samples) | FIFO writes u16, polls u16, elapsed ms u32. Feeds the HR_C6000's prompt tone FIFO (`modem-hrc6000.md` 8.1): 32 samples to page 3 address 0 whenever 0x88 bit 0 reads 0 (`board.audio_tap->modem_burst`); the caller opens the path (0x06 bit 1). Blocks the debug link for the time |
| 8 WATCH | time ms u16 (1-2000), flag page u8, flag register u8, clear register u8, mask u8, RAM page u8, RAM address u16 (0xFFFF = none) | elapsed ms u32, polls u16, for flag bits 0-7: count u16 and first ms u16, RAM word changes u16 |
| 10 MOD | time ms u16 (1-2000), page u8, register u8, tone Hz u16 (0-8000), amplitude u8 (register steps), optional flags u8 (bit 0: second order noise shaping) | writes u32, elapsed ms u32, the register's value u8. Writes the register = its signed value + amplitude x sine back to back (about 37,000 writes a second, measured on the DM-1701), the phase from the microsecond clock, then restores it. On the HR_C6000's 0x04 (two-point MOD1 offset) this is MCU audio on air ([hardware-dm1701.md](hardware-dm1701.md) 21.3.1). Blocks the debug link and lower priority tasks for the time |
| 11 RAMFEED | hold ms u16, gap ms u16, tone Hz u16, amplitude u16, n u8 (0-16), n x (page u8, register u8, xor mask u8); n x (hold + gap) + gap at most 9000 ms | half switches u16, writes u16, longest write lag ms u16, flips done u8. Keeps a big endian tone in the HR_C6000 TX RAM FM halves (written through page 2, watched through page 6) by rewriting each half as soon as the codec moves to the other, while applying each flip for hold ms with gap ms between. Blocks the debug link for the time |
| 12 PIN_TONE | hz u16 (0 = off) | none. A 50 % square wave on the board's spare output (DM-1701: PC8, the stock beep PWM, TIM8 CH3), to find out whether that net reaches the transmit audio; off returns the pin to its low output. Status 4 when the board has none |
| 13 PIN_SCAN | ms u16 (at most 2000) | n u8, n x changed-pin mask u16 (GPIOA, GPIOB, ...). Samples every GPIO input register back to back for ms; output pins read their own level, so two scans (a clock off, then on) are diffed. Blocks the debug link for the time |
| 14 PIN_PWM | top u16 (at least 15), tone Hz u16 (0 = off), amplitude per mille u16 | carrier Hz u32 (0: not running). PWM audio on the spare output: a carrier of top + 1 counts of 72 MHz (DM-1701: TIM8 CH3 on PC8), its duty a sine around 50 % rewritten each carrier period by DMA (DMA2 stream 2) from a table of whole tone periods |

WATCH polls the flag register about once a millisecond, counts each flag in the mask that it finds set and clears it
by writing it to the clear register, and counts the changes of the two-byte RAM word at the address. It blocks the
debug link for the whole time. On the DM-1701 in FM mode no handler serves the HR_C6000's interrupt lines, so it sees
every flag (`modem-hrc6000.md` 8.1).

The probe borrows the stream: a beep or a DMR channel in the meantime may take it back (STATUS bit 0 then reads 0).
Register and RAM writes change the chip under its driver; the radio's next mode change rewrites what it owns.
`odr_debug.py aprobe status | reg | wreg | start | stop | samples | ram | wram | watch | music | mod` drives it;
`tests/hil/test_audio_probe.py` runs the experiments.

### 3.15 SPLIT

The split duplex repeater (`split-duplex.md`), built only with `CONFIG_HOTSPOT_SPLIT` (without it the command answers
`DL_ST_UNKNOWN_CMD` as before; VERSION's feature list then lacks `HOTSPOT_SPLIT`). The number 0x15 leaves 0x10-0x14
to commands added meanwhile.

| Op | Request | Reply after the status |
|---|---|---|
| 1 VARIANT | variant u8 (0-3, 255 raw), n u8 (0-4), n x (page 4 register u8, value u8) | none. The BS framing experiment (`split-duplex.md` 10.1): what the TX half's next key-up writes and the unkey restores. `DL_ST_BAD_ARG` for n > 4 or a length that does not match n |
| 2 VIEW | none | the split view as key=value text |

View keys: `split.built`, `split.role` (normal, tx, rx), `split.offered` (INFO byte 1), `split.flags` (INFO byte 9),
`split.variant`, `split.cach_method` (none, ours, chip, unknown), `split.keyed_s`, `split.lease_expiries`,
`split.time_requests`; TX half: `split.buf1` / `split.buf2` (bursts buffered per slot), `split.underruns1` /
`split.underruns2`, `split.at_host` (the host's AT mask now), `split.at_on_air`, `split.cach` (CACH bursts sent),
`split.idle`, `split.silence`, `split.closed`, `split.slc_taken`, `split.slc_bad`; RX half: `split.epoch_us`,
`split.period_ns`, `split.epoch_age_s` (65535 never), `split.offset_us` (the last phase residual), `split.track_us`
(the anchor's correction), `split.busy`, `split.labeled1` / `split.labeled2`, `split.unlabeled`. No GET_VIEW section
is added, so section numbers stay the same in every build. `odr_debug.py split view | variant N [REG=VAL ...]`.

### 3.16 APRS

Bench control of APRS ([aprs.md](aprs.md) 13). Built without CONFIG_APRS every operation answers `DL_ST_UNAVAILABLE`.
The state is in the `aprs` and `aprs_set` view sections.

| Op | Request | Effect |
|---|---|---|
| 1 SEND_NOW | none | a position beacon on the configured way out; `DL_ST_REFUSED` when it can't go (no callsign, no position, RF blocked and not via DMR) |
| 2 STATUS | text | a status packet (`>text`); `DL_ST_REFUSED` as above |
| 3 MESSAGE | `CALL text` (addressee, one space, the text) | an APRS message with acks and retries; `DL_ST_BUSY` while one is still waiting |
| 4 TEST_CALL | callsign, 0-9 characters | a RAM-only callsign override for bench tests; empty = back to the radio name. Not saved |
| 5 SET | field u8, value u32 (signed as two's complement) | one APRS setting, saved like a UI change. Fields: 0 tx, 1 region (0 NA, 1 EU, 2 AU, 3 NZ, 4 JP, 5 CN, 6 custom), 2 freq_hz (custom), 3 ssid (0-15), 4 lat, 5 lon (fixed position, 1e-7 degree), 6 pos_src (0 auto, 1 GPS, 2 host, 3 fixed), 7 interval_s, 8 smart, 9 via (0 RF, 1 DMR, 2 both), 10 dmr_dst, 11 level (0-100), 12 path (0 none, 1 WIDE1-1, 2 WIDE1-1,WIDE2-1, 3 WIDE2-1, 4 WIDE2-2), 13 kiss, 14 dmr_udt, 15 after_ptt, 16 distance_m, 17 rx, 18 igate, 19 igate_tx, 20 digi, 21 txdelay (5-100, 10 ms units), 22 dmr_gps (our position in DMR voice calls), 23 to_is (our own packets to APRS-IS: the host helper or the radio's own client, `tcp-services.md` 7), 24 power (0 = the radio's current level, n = the board's level n - 1, up to its level count; `aprs.md` 9). Out of range: bad argument |
| 6 NMEA | an NMEA sentence | taken as a host position (`$GPRMC` / `$GPGGA` with a valid checksum) |

`odr_debug.py aprs send | status TEXT | msg CALL TEXT | call [CALL] | set FIELD VALUE | nmea SENTENCE`.

### 3.17 SNMP

The SNMP agent's settings ([snmp.md](snmp.md) 6, 7). Built without CONFIG_SNMP every operation answers
`DL_ST_UNAVAILABLE`. The state is in the `snmp` view section.

| Op | Request | Effect |
|---|---|---|
| 0 SET | field u8, value u32 | saved like a UI change. Fields: 0 flags (`SNMP_CFG_*`: 0x01 agent off, 0x02 no answers over RF, 0x04 SET over RF, 0x08 no notifications to the host, 0x10 authenticationFailure, 0x20 call notifications over RF), 1 rf_manager (DMR ID, 0 = none), 2 rf_gap_s (0 = 60, else 10-3600), 3 trap_port (the host's notification port, 0 = 162; RAM only). Out of range: bad argument |
| 1 COMMUNITY | which u8 (0 read, 1 write), the text (0-32 bytes) | sets the community (stored as its hash); empty: read back to "public", write none (SET refused) |
| 2 TEST_TRAP | none | queues a coldStart notification (the HIL test's check of the path) |

`odr_debug.py snmp status | set FIELD VALUE | read [TEXT] | write [TEXT] | trap` (flags as a number or names:
`off,rf_off,rf_write,usb_traps_off,auth_traps,rf_call_traps`).

### 3.18 STUB

Load the host's flash-access RAM monitor (the "C7 stub") into a free RAM span and hand the SoC to it at run time, no
power cycle (`CONFIG_STUB_ENTRY`; `docs/spec/hardware-dm32.md` 17.2 for the SoC side). Built without the switch, or on a
board whose platform has no `hal_system_t.enter_stub`, every operation answers `DL_ST_UNAVAILABLE`. All u32 are little
endian. The guard is a 32-bit magic key `'C7SB'` (`0x42533743`) on BEGIN and ENTER, plus the exact length and a CRC-32
of the whole image checked before the jump.

| Op | Request | Reply / effect |
|---|---|---|
| 0 INFO | none | magic u32, load_addr u32, region_size u32, max_chunk u16, armed u8, received u32: the span and transfer state |
| 1 BEGIN | magic u32, total_len u32, crc32 u32, entry u32 | received u32 (0): zero the span and arm a transfer. Bad magic: refused; total 0 / over the span or entry outside it: bad argument |
| 2 DATA | offset u32, bytes (<= max_chunk) | received u32: write a chunk (the frame's CRC-16 guards it). Not armed, or past total_len: bad argument |
| 3 ENTER | magic u32 | ok then (after the reply) quiesce and jump; the monitor answers next. Bad magic: refused; incomplete or the CRC-32 mismatches: bad argument |

The CRC-32 is the standard (zlib) CRC-32. Per-chunk integrity is the debug link frame's own CRC-16, so DATA carries no
second checksum; the span is zeroed at BEGIN, so a dropped chunk leaves zeros that fail the ENTER CRC-32. After an ok
ENTER the firmware sends the reply, draws a static banner on the display ("PC Loader Connected" / "Power-cycle to exit",
flushed synchronously so the panel does not sit on a stale or hung frame while the monitor runs), then quiesces and jumps
and stops answering; the host then speaks the monitor's own protocol (`02 op len chk 03`, the stub's `PROTOCOL.md`) on
the same UART at the same baud. Recover the radio with a power cycle.

## 4. Safety

- PTT: a firmware time-out on every injected PTT (10 s maximum); the host tool refuses longer holds; the radio
  core's own TX time-out and transmit checks still apply.
- Every injected key hold is capped at 10 s, one injected key at a time, 40 ms between them.
- Radio requests are rate limited (20 ms).
- INFO_CHIP and READ_MEM only read. READ_MEM reads flash, system memory, OTP and option bytes only, never RAM or
  peripherals. Nothing in the debug link writes or erases internal flash or the option bytes.
- REBOOT needs the chip UID's low word as a token, is refused while transmitting, and the ROM DFU mode is refused
  unless RDP is level 0. The host tool checks RDP too.
- Memory dumps are never written inside a git tree (the stock bootloader is proprietary).
- AUDIO_PROBE (diagnostic builds only) writes audio chip registers as asked; nothing else in the debug link writes
  chip registers.
- STUB (bench builds only) writes arbitrary RAM and then runs it, giving the host full control: it is gated by a magic
  key, the exact length and a CRC-32, loads only into the platform's declared free RAM span, and never returns (recover
  by a power cycle). Off in a release.
- The debug link never enters hotspot or CPS mode, and never takes bytes that belong to an MMDVM frame or a CPS
  request (section 1).

## 5. Counters and resets

RESET_COUNTERS zeroes: the board's diagnostics counters (`board.diag_reset`, whatever the board counts; on the DM-1701
the HR_C6000 slot engine counters, the RF switch counters and the DMR audio counters), the DMR call stack counters (`dmr_call_reset_stats`),
the AMBE voice counters (`ambeVoiceResetStats`), the beep counters (`beep_reset_stats`) and the DMR data service
counters (`dmrDataServiceResetStats`). States and "last" values (last CC, last sync, engine TX state) are kept. The
resets are not atomic with respect to the tasks and interrupts that count: an increment racing the reset may survive
it; a test that resets, waits a moment and then starts its traffic is not affected.

## 6. Adding a command

Add the number to `debuglink_proto.h`, a handler in `core/debuglink/debuglink.c` (short, non-blocking, reply at most
1024 bytes), a row here, and a method in `tools/odr_debug.py`. New view keys need only the renderer and the table
in 3.7.
