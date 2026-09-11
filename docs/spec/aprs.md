<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The OpenDigitalRadio Authors -->

# APRS

APRS (Automatic Packet Reporting System) on OpenDigitalRadio: our position, status and messages as AX.25 UI frames in
Bell 202 AFSK on FM; positions over DMR to an APRS gateway ID; automatic beaconing from several trigger sources; a
KISS TNC on the host port; the iGate rules with a host helper holding the APRS-IS connection; and, on radios that can
sample their received audio, a demodulator, a station list, messages to us and a digipeater.

Code: `core/aprs` (portable, host tested), the RF part in `core/radio/radio_sm.c` (the APRS block), the board's tone
path behind `hal/hal_afsk.h`, the UI in `core/ui/ui_aprs.c`, the debug link in `core/debuglink/debuglink_aprs.c`, the
host helper `tools/aprs_gate.py`. Build switches: `CONFIG_APRS`, `CONFIG_APRS_RX`, `CONFIG_APRS_IGATE`
(`docs/build-config.md`). The DMR GPS Info LC, which shares the position source, is in `dmr-voice.md` 3.1.

Sources (all public, read for the formats only; no implementation was used as a source, `CLEANROOM.md`):

- AX.25 Link Access Protocol for Amateur Packet Radio, version 2.2 (TAPR, 1998): address field, UI frames, FCS,
  bit stuffing (sections 3, 6).
- APRS Protocol Reference 1.0 (APRS Working Group, 2000), "APRS101", and the APRS 1.1 and 1.2 addenda: data types,
  positions (plain and compressed), Mic-E, status, messages and acks, the third-party format, tocalls, paths,
  position ambiguity.
- The KISS TNC protocol (Chepponis and Karn, 1987): framing, escapes, the command bytes.
- aprs-is.net: "IGate Design", "q Construct", "Connecting to APRS-IS" (login, passcode, filters).
- SmartBeaconing as published by Tony Arnerich KD7TA and Steve Bragg KA9MVA (HamHUD), released for anyone to use.
- BrandMeister's APRS documentation: the location service IDs (5050-5059, SSID and icon by ID) that turn LRRP and
  ETSI location reports into APRS positions.
- Bell 202: 1200 baud, mark 1200 Hz, space 2200 Hz, NRZI (a 0 bit is a tone change).

Black-box oracles (never read for code): Direwolf decodes the WAV that `aprsTest --wav` writes
(`tests/host/aprs_wav_check.sh`) and the radio's transmission on the SDR bench (section 13); APRSdroid, Xastir or
aprx can stand in.

## 1. What each radio does

| | DM-1701 | Other radios |
|---|---|---|
| FM AFSK transmit | yes: AT1846S tone generator retuned per symbol (5.2) | when the board provides `board.afsk->tx_*` |
| FM AFSK receive, station list, digipeater, messages to us | designed only: no received audio reaches the MCU (5.3) | when the board provides `board.afsk->rx_*` and builds `CONFIG_APRS_RX` |
| Positions over DMR (LRRP / UDT) | yes, on a DMR channel | every DMR radio |
| KISS TNC on USB (and UDP 3334 in network mode) | yes (TX path; RX frames only with RX) | yes |
| iGate | IS -> RF messages only after RX exists; RF -> IS needs RX (designed only) | with RX |
| GPS | none: host NMEA or the fixed position | `CAP_GPS` boards feed `aprs_nmea(..., gps=true)` |

Everything is host tested; nothing has been on the air yet (section 13 lists the bench steps).

## 2. Model

The engine (`core/aprs/aprs.c`) runs in the radio task: `aprs_tick` from `radio_sm_poll` every 25 ms. The UI, the
debug link and the host link reach it through the "any task" calls, which take a lock the radio task sets up
(`aprs_set_lock`, the scheduler suspended for short copies). Settings and codeplug reads take the storage mutex and so
happen outside that lock.

Pure parts, each with host tests in `tests/host/aprsTest.c`: `ax25.c` (addresses, frames, TNC2 text), `hdlc.c` (FCS,
bit stuffing, NRZI symbols, the receive deframer), `afsk.c` (the tone plan for TX, a correlator demodulator with a PLL
for RX), `kiss.c`, `aprs_fmt.c` (APRS formats in and out), `aprs_nmea.c`, `aprs_beacon.c` (when to send),
`aprs_igate.c` (gating and digipeater rules). `tests/host/aprsRadioTest.c` runs the engine with the radio core on a
fake board whose AFSK output is demodulated back to frames.

## 3. Rules

- Transmit is off by default. Automatic beacons need Tools > APRS > Automatic beacons; "Send position now" works
  without it.
- A callsign is required: the debug link's test call, else Settings > My radio's radio name, else the codeplug's
  radio name; its first word, upper case, 3-6 letters and digits with a digit after the first character, not N0CALL.
  Our SSID comes from the APRS settings (7 by default). No callsign: nothing goes out, the page says why.
- RF only on amateur frequencies: the APRS frequency must be in 144-148, 222-225 or 420-450 MHz and in a band the
  board transmits in; else the reason is "not amateur" and nothing is keyed.
- A hard minimum of 30 s between automatic beacons (`AUTO_MIN_S`), 10 s for any configuration
  (`APRS_MIN_INTERVAL_FLOOR_S`), and "send now" pressed again within 5 s is ignored.
- Frames from the host (KISS) must carry our callsign (any SSID) as the source and are rate limited: a burst of 3,
  then one per 10 s.
- CSMA before every RF transmission (section 6); our own PTT always wins (an APRS transmission is aborted, the
  channel restored).
- iGate transmit (IS -> RF) only for messages to stations heard directly in the last 30 minutes (section 9.1).

## 4. Frames

### 4.1 What we send

- Position: `!` (or `=` when messaging is on), plain `DDMM.mmN/DDDMM.mmW` with the symbol, or compressed (base 91,
  the default), course / speed when known, altitude `/A=` when known, then the comment (the codeplug APRS record's
  comment if any). Position ambiguity 0-4 blanks digits as APRS101 chapter 8 says.
- Status: `>text` (40 characters).
- Messages: `:ADDRESSEE:text{NN` (67 characters), acks `:ADDRESSEE:ackNN`; ours retried at 30, 60 and 120 s
  (3 tries) until the ack.
- Path: none, WIDE1-1, WIDE1-1,WIDE2-1 (the default), WIDE2-1, WIDE2-2.

### 4.2 Tocall

Destination `APZODR`: the APZ block is the experimental range anyone may use. A registered tocall (APxxxx from the
tocalls list) is an open decision.

### 4.3 Received (RX builds)

Positions (plain, compressed, with and without timestamps), Mic-E (the destination field's latitude and message bits,
longitude, speed and course from the information field), objects, items, status, messages, third-party packets,
queries, weather and telemetry recognized by type (not decoded further).

## 5. The radio side

### 5.1 hal_afsk

`hal/hal_afsk.h`: `tx_start(tones, nbits, baud, mark, space, level)` plays a symbol string (one bit per symbol,
1 = mark), `tx_busy`, `tx_stop`; `rx_rate_hz`, `rx_start`, `rx_read`, `rx_stop` deliver received audio samples.
`board.afsk` is NULL on boards without a tone path; then the reason is "no AFSK" and only DMR positions go out.

### 5.2 DM-1701 transmit

The AT1846S FM transmitter's tone generator is the voice source (as for the roger beep). TIM6 (APB1 timer clock
72 MHz, prescaler 2, `ARR = clk / 2 / 1200 - 1`, interrupt priority 5, no RTOS calls) interrupts at every symbol
boundary and writes the next tone to register 0x35 when it changes (one I2C write, about 0.1 ms, out of an 833 us
symbol). The radio core keeps the I2C bus free meanwhile (`status()` returns its cached reading while AFSK runs) and
force-writes 0x35 at the end because the interrupt bypassed the driver's cache. The level (0-100, 50 default) scales
the tone deviation register 0x59 around the driver's value.

Not verified on the bench: whether the tone stays phase continuous across a retune, the deviation the level gives,
the mark / space flatness (section 13).

### 5.3 DM-1701 receive: designed only

FM audio goes AT1846S -> HR_C6000 line in -> speaker DAC; the I2S stream from the HR_C6000 carries the microphone
only, and no ADC pin sees the discriminator. So `rx_start` is NULL and `CONFIG_APRS_RX` defaults to n on this board.
A bench experiment worth trying: route the line in to the HR_C6000's record path (register 0xE0 family) and read it
over I2S. The demodulator (`afsk.c`) is ready for any board that can: 8-48 kHz samples, a correlator per tone with
hysteresis and a PLL that tightens once a flag is seen. Host sweep (`aprsTest --sweep`, flat / pre-emphasized audio):
90 / 10 % of frames at 10 dB SNR, 97 / 61 % at 12 dB, 100 / 94 % at 14 dB.

## 6. RF transmission

The radio core's APRS block (`radio_sm.c`, `aprs_*`): when a frame waits (`aprs_rf_pending`) and the radio is idle
(no PTT, no call heard or sent, no scan, no hotspot, no monitor, no TX inhibit, battery not critical), it takes the
job (`aprs_rf_take`: our queued frames, 1 flag between them, a TX delay of flags before, 3 after), and:

1. LISTEN: tunes the APRS frequency (25 kHz, or 12.5 kHz when narrow is set), 50 ms to settle, then 100 ms slots with
   p-persistence 64/256 while the squelch is closed; gives up after 10 s (`rf_busy`, retried later).
2. KEYUP: keys the FM transmitter on the APRS frequency at the radio's default power level, 25 ms.
3. SEND: `board.afsk->tx_start`; at most 6 s.
4. Back: transmitter off, the user's channel retuned exactly as it was.

PTT, a channel change or any request other than beeps, DMR focus and settings aborts it (`rf_failed`, the frame
retried after 15 s, 3 tries). The settings save waits while a job is pending.

## 7. Position and triggers

Position source (setting: Auto, GPS, Computer, Fixed): Auto takes a board GPS fix younger than 60 s, else a host NMEA
fix younger than 60 s (`$GPRMC`, `$GPGGA` with valid checksums, from KISS SetHardware 'N' or a bare `$` line on the
host port), else the fixed position (settings, or the debug link). GPS and host fixes carry speed and course.

Triggers (`aprs_beacon.c`), all under the hard minimum:

| Trigger | Default | |
|---|---|---|
| Interval | 10 min when beacons are on | 0 = none |
| SmartBeaconing | on | slow 5 km/h -> 30 min, fast 90 km/h -> 3 min, linear in between; corner pegging: a turn sharper than 28 degrees + 400 / speed sends after 30 s |
| Distance | off | after moving N meters |
| After PTT | off | after our own transmission, if the last beacon is 2 min old |
| Manual | | "Send position now" (UI, debug link) |
| Boot | off | the first position after power-up |

Without a known speed SmartBeaconing falls back to the interval. The defaults are an open decision (section 14).

## 8. Positions over DMR

Send via (Radio / DMR / Both): DMR positions go out when the radio is on a DMR channel with the call stack running,
as a private data call to the destination ID (default 5057, BrandMeister's APRS service, "person" icon):

- Motorola LRRP triggered location report (the default): UDP port 4001, built by `dmrLrrp.c`.
- ETSI UDT location (`dmr_udt` on): `dmrDataBuildUDTLocation`.

### 8.1 LRRP details

An unsolicited report carries request ID 00000001 (no request came). Which request ID and token BrandMeister's
gateway expects for unsolicited reports is unverified: an open decision, checked with the HIL step in section 13.
BrandMeister maps the ID to an SSID and icon (5050-5059); the APRS settings on BrandMeister's self care page set the
comment.

## 9. Host port: KISS TNC

With Tools > APRS > KISS on USB, the host link claims KISS frames (0xC0) and `$` lines on the USB serial port (and on
UDP port 3334 in USB network mode, where the host port runs over the network). The host link's TNC mode
(`HOSTLINK_TNC`) holds while frames flow and ends after 30 s idle, like the hotspot and CPS modes.

Port 0 only. DATA frames are checked (section 3) and queued for RF; TXDELAY, PERSIST and SLOTTIME are accepted and
ignored (the engine's own CSMA applies); SetHardware 'G' carries an APRS-IS line, 'N' an NMEA sentence. To the host:
received frames (RX builds) as KISS DATA, and lines for APRS-IS as SetHardware 'G'.

### 9.1 iGate

The rules are in firmware (`aprs_igate.c`, after aprs-is.net "IGate Design" and "q Construct"); the APRS-IS TCP
connection is in the host helper `tools/aprs_gate.py`, which logs in with the callsign and passcode, applies a
filter, and passes lines both ways as KISS SetHardware 'G' frames (serial port or UDP 3334).

- RF -> IS: never a packet whose path holds TCPIP, TCPXX, NOGATE or RFONLY, nor a query; a third-party packet is
  gated as the packet it carries unless that one came from the Internet; the line gets `,qAR,<our call>`; each
  packet once per 30 s.
- IS -> RF (`igate_tx`, off): only messages and acks, to a station heard directly in the last 30 minutes, from a
  station not heard on RF in the last 10 minutes, not seen in the last 30 s; sent as `}SRC>DST,TCPIP,OURCALL*:info`.
- Our own beacons to APRS-IS (`to_is`): `CALL>APZODR,TCPIP*:info` over the same channel.
- Digipeater (RX builds, off): fill-in (WIDE1-1 only) or wide (WIDE1-1 and WIDE2-N), also our own call in the path;
  our call inserted before the WIDE entry, marked used (traced), the hop count decremented; duplicates suppressed
  for 30 s.

## 10. UI

Tools > APRS (`ui_aprs.c`): the body shows the callsign and frequency, why nothing can be sent (no callsign, no
frequency, not amateur, no AFSK), the position and its source, the next beacon and the count sent. Rows: Send
position now, Automatic beacons, Frequency (region: N America 144.390, Europe 144.800, Australia 145.175, New Zealand
144.575, Japan 144.660, China 144.640, Custom), Interval (Off, 1, 2, 5, 10, 30 min), SmartBeaconing, Send via (Radio,
DMR, Both), Position (Auto, GPS, Computer, Fixed), Position in DMR calls (`dmr-voice.md` 3.1), KISS on USB, Stations
heard (RX builds: the list, most recent first, dimmed when heard through a digipeater).

The rest (symbol, path, comment, digital destination, ambiguity, iGate, digipeater, fixed position) is set over the
debug link (`odr_debug.py aprs set`) and later by qdmr through the native codeplug.

## 11. Settings

`settings_t` (`core/settings/settings.h`), taken only when `aprs_stored` is 1, else the defaults: `aprs_flags`
(TX, SmartBeaconing, compressed, after PTT, boot, KISS, narrow, RX), `aprs_flags2` (iGate, iGate TX, messaging, UDT,
to APRS-IS, DMR GPS Info), `aprs_region`, `aprs_ssid`, `aprs_freq_hz`, `aprs_lat` / `aprs_lon` (1e-7 degree),
`aprs_dmr_dst`, `aprs_alt_m`, `aprs_interval_s`, `aprs_distance_m`, `aprs_symbol`, `aprs_path`, `aprs_pos_src`,
`aprs_level`, `aprs_txdelay`, `aprs_misc` (via, digipeater, ambiguity). The codeplug's APRS record 1 (EEPROM 0x1588,
64 bytes each) gives the SSID, path, symbol, comment and ambiguity when present; its fixed position and frequency
encodings are unknown and not used.

Defaults: TX off, region N America, SSID 7, symbol `/[`, path WIDE1-1,WIDE2-1, compressed, interval 10 min,
SmartBeaconing on, level 50, TX delay 300 ms, via Radio, destination 5057; messaging and RX only on boards with a
receive path.

## 12. Size (DM-1701, arm-none-eabi-gcc 13.3 -Os)

| Build | Flash | RAM |
|---|---|---|
| `CONFIG_APRS=n` | 224.6 KB | 114.0 KB |
| default (APRS, no RX) | 247.5 KB | 116.8 KB |
| `CONFIG_APRS_RX=y` | 257.9 KB | 120.9 KB |

APRS costs about 21 KB flash and 2.8 KB RAM with its UI and debug link; RX about 10 KB flash and 4 KB RAM more.
`CONFIG_DMR_GPS` about 2.2 KB flash.

## 13. Tests

Host: `make -C tests/host` runs `aprsTest` (spec vectors: AX.25, FCS, stuffing, KISS, every APRS format, NMEA, the
triggers, the iGate rules, a modulate / demodulate loop; `--sweep` prints the decode rate against SNR, `--wav DIR`
writes `aprs_test_44k1.wav` with three frames) and `aprsRadioTest` (the engine with the radio core: beacons on the
air decoded back, the minimum interval, CSMA on a busy channel, PTT cutting a transmission, the not-amateur block,
manual and after-PTT triggers, KISS and NMEA from the host, messages with acks, the station list, digipeating and
the iGate both ways, the DMR position source). `tests/host/aprs_wav_check.sh` feeds the WAV to Direwolf's `atest`
when it is installed (a black-box check).

HIL (opt-in, `tests/hil/test_aprs.py`, `--allow-rf`; the lead runs it): on the DM-1701 with an SDR or a second
receiver decoding (Direwolf on the SDR's FM audio):

1. Debug link: `odr_debug.py aprs call N0CALL-like-yours` (your callsign), `aprs set region na` (144.390; `eu` for
   144.800), `aprs set lat 33.7490`, `aprs set lon -84.3880`, `aprs set pos_src fixed`, low power.
2. `odr_debug.py aprs send`: the radio leaves the channel, listens, keys about 1 s, returns. `view aprs`:
   `aprs.rf_sent` + 1, `aprs.last_info`.
3. On the receiver: the frame decodes (`CALL-7>APZODR,WIDE1-1,WIDE2-1:...`), no FCS errors. Check the deviation
   (about 3 kHz peak at level 50; adjust `aprs set level`), the mark / space amplitude difference, and the tone
   phase at symbol changes on the SDR waterfall / audio.
4. Digital: on a DMR channel through a BrandMeister repeater or hotspot, `aprs set via dmr`, `aprs send`; the
   position shows on aprs.fi under the radio's DMR callsign with the 5057 SSID. If not: try `aprs set dmr_udt 1`,
   then capture the LRRP report for the request ID question (8.1).
5. KISS: `aprs set kiss 1`, then `tools/aprs_gate.py --serial PORT --kiss-tcp 8001` and APRSdroid / Xastir on TCP
   8001 send a position; it goes out on RF.

## 14. Open decisions

- Default triggers: SmartBeaconing on with a 10 min interval fallback, after-PTT and boot off.
- A registered tocall instead of APZODR.
- The LRRP request ID and token for unsolicited reports to BrandMeister; whether LRRP answers should use the APRS
  position when there is no GPS; whether Settings > Location > Send position should drive APRS.
- APRS messages merged into the Messages inbox (now: Stations heard and the debug link only).
- The codeplug APRS record's fixed position and frequency encodings.
- DM-1701 receive through the HR_C6000 record path (5.3).
- Settings and debug link section / command numbers may need renumbering when merged with other branches.

Future, not done:

- 9600 baud APRS (G3RUH FSK, scrambled NRZI direct FM): needs a flat (discriminator / modulator direct) audio path
  and an FSK modem; not on the DM-1701's tone generator. Not planned for now.
- Messages in the Messages inbox; Settings > Location tied to APRS; GPS Info in the hotspot's own transmissions.
