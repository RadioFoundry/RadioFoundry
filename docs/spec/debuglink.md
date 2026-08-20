<!-- SPDX-License-Identifier: MIT -->

# USB debug link

A small framed protocol on the radio's host port (USB CDC-ACM on the STM32 radios) for bench automation: read the
firmware version, the diagnostics counters and the radio's state, reset counters, take screenshots, press keys, key
PTT with a firmware time-out, send radio requests, read a log ring, read the chip's identity and option bytes, read
internal flash and the other read-only memories, and reboot. It lives beside the MMDVM hotspot and the
legacy CPS protocol on the same port (`core/hostlink`), and it never changes the host link's mode.

- Firmware: `core/debuglink/` (switch `CONFIG_DEBUGLINK`, default on with `CONFIG_USB`), key injection in
  `core/input/input_task.c`, the hook in `core/hostlink/hostlink.c`.
- Chip commands: the portable checks in `core/debuglink/dl_chip.c` (host tested), the platform side in `hal/hal_system.h`
  (`board.system`) and `platform/stm32f4/sysboot.c`.
- Host: `tools/odr_debug.py` (command line and Python module), `tests/hil/` (pytest hardware-in-the-loop tests).

## 1. Coexistence on the host port

The host link sends each request to one protocol by its first byte (`hostlink.h`):

| First byte | Protocol |
|---|---|
| `0xE0` | MMDVM: enters hotspot mode |
| ASCII letters (`R`, `W`, `X`, `C`, `D`, ...) | legacy CPS (`legacy-cps-interop.md` 2) |
| `0xA5` followed by `0x5A` | the debug link |

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

### 3.1 VERSION

```
proto=1                 protocol version (this document)
view=1                  GET_VIEW record version (3.7)
git=5277f73a            firmware git hash
board=DM-1701
features=FM,SUBTONE,DMR,DMR_PRIVACY,...,USB,DEBUGLINK
display=160x120x16      panel width x height x bpp
inset=5,5,5,5           safe area insets: top, bottom, left, right
uptime_ms=123456
req_max=64
reply_max=1024
inject_max_ms=10000     longest hold of an injected key, PTT included
```

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
line was sent. The DM-1701's board lines start with the HR_C6000 slot engine counters (`V<voice> H<headers>
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
| 0 | `core` | `view state tx_deny mode vfo zone_index zone_pos zone_size zone_name channel_number channel_name rx_hz tx_hz power power_name power_count temporary temp squelch monitor tone_ok rssi_dbm noise tx_elapsed_ms tot_s last_rx_ms vfo_step_hz codeplug hotspot serial` |
| 1 | `channel` | `ch.name ch.rx_hz ch.tx_hz ch.mode ch.bw ch.rx_tone ch.tx_tone ch.power ch.rx_only ch.color_code ch.timeslot ch.contact ch.listen_list ch.dmr_id ch.dmo` |
| 2 | `dmr` | `dmr.built dmr.active dmr.codec dmr.state dmr.phase dmr.tx_error dmr.own_id dmr.cc dmr.ts dmr.rmo dmr.repeater_up dmr.target_id dmr.target_private dmr.target_name dmr.reply_active dmr.reply_s dmr.hear_all dmr.underruns dmr.target_manual dmr.privacy dmr.alias_tx` |
| 3 | `dmr_call` | `call.src call.dst call.group call.to_us call.encrypted call.emergency call.pi_header call.src_name call.dst_name call.late_entry call.alias_complete call.fid call.svc call.pi_alg call.pi_key call.alias call.rssi_dbm call.ms`, the same with `last.` (`src` to `dst_name`) and `last.ms` |
| 4 | `dmr_stats` | call stack (`call.rx_calls rx_calls_played rx_late_entries rx_voice_bursts rx_lost_frames rx_dup_dropped rx_enc_dropped tx_calls tx_denied tx_underruns evq_overflows rx_private_calls rx_emergency_calls rx_encrypted_calls rx_pi_headers rx_alias_complete tx_alias_lcs`), AMBE voice (`voice.rx_in rx_frames rx_concealed rx_silence rx_calls rx_queue_max rx_bad rx_lost rx_underruns rx_overflows rx_bit_errors tx_frames tx_overflows`) |
| 5 | `hotspot` | `hostlink` (idle / hotspot / cps), `hs.running hs.transmitting hs.receiving hs.rx_hz hs.tx_hz hs.cc hs.src hs.dst hs.group hs.from_net` and the engine counters `hs.net_frames ... hs.enter_attempts` |
| 6 | `link` | `uptime_ms ui.screen` (the screen's name, `docs/spec/ui.md` 2) `ui.depth ui.focus ui.count ui.locked ui.banner ui.toast ui.query ui.draws input.idle_ms input.source input.raw_keys input.injected input.encoder_total dl.frames dl.bad_crc dl.too_long dl.timeouts` |
| 7 | `features` | `scan.built scan` (off, zone, vfo, dual) `scan_phase` (listen, probe, hold, resume, paused) `scan_skipped tot_warning tone_burst roger vox_keyed rx_sleeping battery` (ok, low, critical) `battery_pct battery_mv power_off`, `beep.built beep.played beep.no_stream beep.blocks beep.busy`, and the radio settings `set.tot_s set.tot_warn_s set.bcl set.roger_beep set.tx_inhibit set.scan_resume set.scan_hold_s set.scan_delay_ms set.scan_dwell_ms set.scan_priority set.beep_level set.key_beep set.vox set.vox_level set.vox_tail_ms set.rx_save set.rx_save_idle_s set.battery_off_s` (`docs/spec/radio-features.md`) |
| 8 | `dmr_data` | `data.built data.busy data.sms_state data.signal_state data.signal_kind data.signal_dst data.ars_state data.inbox_count data.inbox_unread data.serial data.event data.event_serial data.event_id data.ars_server data.radio_check_reply data.call_alert_ack data.lrrp_reply data.has_gps data.lrrp_triggers`, counters `data.rx_bursts rx_csbk rx_headers rx_blocks rx_blocks_stale rx_crc_errors rx_crc_flag_mismatch rx_packets rx_for_us rx_confirmed sms_received sms_duplicates sms_sent sms_acked sms_failed tms_acks_sent responses_sent csbk_answers_sent call_alerts_received radio_checks_received call_alerts_sent call_alerts_acked radio_checks_sent radio_checks_answered ars_sent ars_acks lrrp_requests lrrp_answers lrrp_no_fix tx_lists tx_ok tx_failed tx_gave_up tx_last_error` (meanings: `core/dmr/dmrDataService.h` dmrDataStats_t). States: `idle queued sending ok failed`; ARS `off registering registered refused no_answer`; `data.event` the last event's name (`sms_received`, `call_alert`, `radio_check_answered`, ...), `data.event_id` its radio ID |
| 9 | `storage` | settings store (`settings.load` ram / defaults / loaded / upgraded, `settings.dirty seq saves erases failures sector slot skipped`), flash journal (`storage.sector_writes sector_skips replays errors`), `codeplug.source codeplug.serial`, legacy CPS (`cps.built requests reads commits refused overflows last session pending`; [settings-store.md](settings-store.md), [cps-protocol.md](cps-protocol.md)) |

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
| 24 | id u8, value u16 | one radio setting in RAM (`radio_set_settings`; not stored): 1 tot_s, 2 tot_warn_s, 3 bcl (0 off, 1 carrier, 2 wrong tone), 4 roger_beep, 5 tx_inhibit, 6 scan_resume (0 time, 1 carrier, 2 stop), 7 scan_hold_s, 8 scan_delay_ms, 9 scan_dwell_ms, 10 scan_priority, 11 beep_level (0-100), 12 key_beep, 13 vox (0 off, 1 channel, 2 all), 14 vox_level (0-10), 15 vox_tail_ms, 16 rx_save (0-3), 17 rx_save_idle_s, 18 battery_off_s, 19 scan_beep. Out of range: bad argument |

Ops 15-18 last until the channel changes (`dmr.target_manual`). Unavailable ops answer UNAVAILABLE.

### 3.9 INFO_CHIP

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
| 1 | SMS | destination u32, flags u8 (bit 0 group, bit 1 ACK requested), text (Latin-1, the rest of the payload, up to 58) | send a TMS text message |
| 2 | CALL_ALERT | target u32 | send a call alert (3 tries) |
| 3 | RADIO_CHECK | target u32 | send a radio check (3 tries) |
| 4 | SETTINGS | ARS server u32 (0 off), flags u8 (bit 0 answer radio checks, bit 1 acknowledge call alerts, bit 2 answer LRRP) | the service's settings, in RAM only (until the codeplug holds them) |
| 5 | INBOX | index u8 (0 newest) | reply: source u32, destination u32, group u8, unread u8, the text; UNAVAILABLE when there is no such message |
| 6 | MARK_READ | index u8 | mark an inbox message read |

RESET_COUNTERS also zeroes the data service's counters. `odr_debug.py data sms|alert|check|settings|inbox|read`.

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
- The debug link never enters hotspot or CPS mode, and never takes bytes that belong to an MMDVM frame or a CPS
  request (section 1).

## 5. Counters and resets

RESET_COUNTERS zeroes: the board's diagnostics counters (`board.diag_reset`: on the DM-1701 the HR_C6000 slot engine
counters, the RF switch counters and the DMR audio counters), the DMR call stack counters (`dmr_call_reset_stats`),
the AMBE voice counters (`ambeVoiceResetStats`), the beep counters (`beep_reset_stats`) and the DMR data service
counters (`dmrDataServiceResetStats`). States and "last" values (last CC, last sync, engine TX state) are kept. The
resets are not atomic with respect to the tasks and interrupts that count: an increment racing the reset may survive
it; a test that resets, waits a moment and then starts its traffic is not affected.

## 6. Adding a command

Add the number to `debuglink_proto.h`, a handler in `core/debuglink/debuglink.c` (short, non-blocking, reply at most
1024 bytes), a row here, and a method in `tools/odr_debug.py`. New view keys need only the renderer and the table
in 3.7.
