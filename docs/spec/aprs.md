<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The OpenDigitalRadio Authors -->

# APRS

APRS (Automatic Packet Reporting System) on OpenDigitalRadio: our position, status and messages as AX.25 UI frames in
Bell 202 AFSK on FM; positions over DMR to an APRS gateway ID; automatic beaconing from several trigger sources; a
KISS TNC on the host port (and on TCP port 8001 in USB network mode); the iGate rules, with the APRS-IS connection
held by a host helper or by the radio's own client (`tcp-services.md` 4, 7); and, on radios that can sample their
received audio, a demodulator, a station list, messages to us and a digipeater.

Code: `core/aprs` (portable, host tested), the RF part in `core/radio/radio_sm.c` (the APRS block), the board's tone
path behind `hal/hal_afsk.h`, the UI in `core/ui/ui_aprs.c`, the debug link in `core/debuglink/debuglink_aprs.c`, the
host helper `tools/aprs_gate.py`. Build switches: `CONFIG_APRS`, `CONFIG_APRS_RX`, `CONFIG_APRS_IGATE`, and on the
USB network link `CONFIG_NET_KISS` and `CONFIG_NET_APRSIS` (`docs/build-config.md`). The DMR GPS Info LC, which shares the position source, is in `dmr-voice.md` 3.1.

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

| Feature | Needs |
|---|---|
| FM AFSK transmit | `board.afsk->tx_*` (5.1) |
| FM AFSK receive, station list, digipeater, messages to us | `board.afsk->rx_*` and `CONFIG_APRS_RX` (5.3) |
| Positions over DMR (LRRP / UDT) | a DMR radio, on a DMR channel |
| KISS TNC on the host port (UDP 3334 and TCP 8001 in network mode) | nothing more (TX path; RX frames only with RX) |
| iGate | RX for RF -> IS; IS -> RF messages only once RX exists |
| GPS | `CAP_GPS` boards feed `aprs_nmea(..., gps=true)`; otherwise host NMEA or the fixed position |

What the boards provide today:

| Radio | AFSK transmit | AFSK receive | GPS |
|---|---|---|---|
| DM-1701 | yes: the AT1846S tone generator retuned per symbol (`hardware-dm1701.md` 5.10) | not yet: FM audio reaches the MCU (`hardware-dm1701.md` 21), no `rx_start` | none |
| MD-380 | not yet: needs a TX tone path (`boards/md380/config.mk`) | no | G models (`hardware-md380.md` 13) |
| DM-32UV | no (`board.afsk` NULL) | no | `CAP_GPS` (`hardware-dm32.md` 12) |

Everything is host tested. On the SDR bench (section 13, measured on the DM-1701): positions over DMR (LRRP and UDT)
decode, and FM AFSK decodes with a valid FCS since the voice source fix (`hardware-dm1701.md` 5.10).

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
- Path: none, WIDE1-1, WIDE1-1,WIDE2-1 (the default), WIDE2-1, WIDE2-2. Set on the APRS page (section 10), over the
  debug link, or in the codeplug's APRS record (section 11).

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

### 5.2 Transmit

The radio core keys the transmitter in FM, then the board switches the tone at each symbol boundary (a timer at the
baud rate), with phase continuity where the hardware allows, and keeps the RTOS out of its symbol interrupt. While
AFSK runs, the radio core leaves the transmitter alone (`status()` returns its cached reading). The level (0-100, 50
default) sets the deviation around the board's nominal (about 3 kHz peak at 25 kHz bandwidth). `tests/hil/test_aprs.py` measures on air the phase continuity across a tone change, the deviation the level
gives, the mark / space flatness and the TX delay (section 13). Each board's tone path is on its hardware page (the table in
section 1).

### 5.3 Receive: designed only

A board with a receive path sets `rx_start` and samples the received FM audio; no board does yet, so
`CONFIG_APRS_RX` defaults to n. The demodulator (`afsk.c`) is ready for any
board that can: 8-48 kHz samples, a correlator per tone with hysteresis and a PLL that tightens once a flag is seen. Host sweep (`aprsTest --sweep`, flat / pre-emphasized audio):
90 / 10 % of frames at 10 dB SNR, 97 / 61 % at 12 dB, 100 / 94 % at 14 dB.

## 6. RF transmission

The radio core's APRS block (`radio_sm.c`, `aprs_*`): when a frame waits (`aprs_rf_pending`) and the radio is idle
(no PTT, no call heard or sent, no scan, no hotspot, no monitor, no TX inhibit, battery not critical), it takes the
job (`aprs_rf_take`: our queued frames, 1 flag between them, a TX delay of flags before, 3 after), and:

1. LISTEN: tunes the APRS frequency (25 kHz, or 12.5 kHz when narrow is set), 50 ms to settle, then 100 ms slots with
   p-persistence 64/256 while the squelch is closed; gives up after 10 s (`rf_busy`, retried later).
2. KEYUP: keys the FM transmitter on the APRS frequency at APRS's own power level when one is set (`aprs_power`:
   n = the board's level n - 1), else at the radio's current level (a temporary change included, as for PTT).
   `aprs.tx_power` on the debug link is the level it will use. 25 ms.
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
filter, and passes lines both ways as KISS SetHardware 'G' frames (serial port or UDP 3334), or, in USB network mode,
in the radio's own APRS-IS client (`CONFIG_NET_APRSIS`, `tcp-services.md` 7), which carries the same lines.

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
144.575, Japan 144.660, China 144.640, Custom), Power (As channel, then each of the board's levels), Path (the
digipeater path of RF packets: None, WIDE1-1, WIDE1-1,WIDE2-1, WIDE2-1, WIDE2-2; OK steps to the next and WIDE2-2
wraps to None), Interval (Off, 1, 2, 5, 10, 30 min), SmartBeaconing, Send via (Radio, DMR, Both), Position (opens its page, below), Position in DMR calls (`dmr-voice.md` 3.1), KISS on USB, Stations
heard (RX builds: the list, most recent first, dimmed when heard through a digipeater). On a radio without
`board.afsk`, Power and Path are dimmed, and so is Frequency unless the build receives APRS. Rows 11 and 12 have no
digit key.

APRS > Position (`aprs-pos`; the APRS page's body shows the position in use). Rows: Source (Auto, GPS, Computer, Fixed),
Latitude and Longitude (the fixed position, "Not set" while both are 0). OK on Latitude or Longitude opens an editor in
decimal degrees to 4 places (about 11 m): the digits fill `DD.DDDD` or `DDD.DDDD` from the left (a longitude's first
digit of 2-9 gets its leading zero by itself), a digit that would pass 90 or 180 degrees is refused with a toast, Up /
Down or the knob switch N / S or E / W, BACK takes the last digit back (with none typed it closes the editor unchanged),
OK saves with the places not typed as zeros. The editor starts with the stored value shown; the first digit starts over.
Find knows the page as "APRS position".

The rest (symbol, comment, digital destination, ambiguity, iGate, digipeater, fixed altitude) is set over the
debug link (`odr_debug.py aprs set`) and later by qdmr through the native codeplug.

## 11. Settings

`settings_t` (`core/settings/settings.h`), taken only when `aprs_stored` is 1, else the defaults: `aprs_flags`
(TX, SmartBeaconing, compressed, after PTT, boot, KISS, narrow, RX), `aprs_flags2` (iGate, iGate TX, messaging, UDT,
to APRS-IS, DMR GPS Info), `aprs_region`, `aprs_ssid`, `aprs_freq_hz`, `aprs_lat` / `aprs_lon` (1e-7 degree),
`aprs_dmr_dst`, `aprs_alt_m`, `aprs_interval_s`, `aprs_distance_m`, `aprs_symbol`, `aprs_path`, `aprs_pos_src`,
`aprs_level`, `aprs_power` (0 = the radio's current level, n = the board's level n - 1), `aprs_txdelay`, `aprs_misc`
(via, digipeater, ambiguity). The codeplug's APRS record 1 (EEPROM 0x1588,
64 bytes each) gives the SSID, path, symbol, comment and ambiguity when present; its fixed position and frequency
encodings are unknown and not used. Of these the radio takes the comment and the path.

The path lives in both places, and the one changed last wins:

- Codeplug to radio: when the record's path differs from `aprs_cp_path` (the codeplug path last taken, `aprs_path_t` +
  1), it replaces `aprs_path` and becomes the new `aprs_cp_path`. Checked at start-up and whenever the codeplug
  serial changes (a CPS write). A path that isn't one of the five (another callsign, a second entry without a first) is
  left alone.
- Radio to codeplug: a change on the APRS page posts a storage job (`aprs_path_to_codeplug`) that rewrites the record's
  two path entries (callsign space padded, SSID byte) and sets `aprs_cp_path` to match. No record is made when there is
  none: the radio then keeps the path in the settings only. A change over the debug link stays in the settings.

Defaults: TX off, region N America, SSID 7, symbol `/[`, path WIDE1-1,WIDE2-1, compressed, interval 10 min,
SmartBeaconing on, level 50, TX delay 300 ms, via Radio, destination 5057; messaging and RX only on boards with a
receive path.

## 12. Size (measured on the DM-1701 build, arm-none-eabi-gcc 13.3 -Os)

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

HIL (opt-in, `tests/hil/test_aprs.py`, README there): `--allow-rf --allow-service-stop --aprs-call CALL` (a licensed
operator's callsign, set as the RAM-only test call; SSID `--aprs-ssid`, default 15).

Where the AFSK tests transmit, so nothing reaches the APRS network: the custom region on `--aprs-test-mhz`, default
145.090 MHz (US band plans put 2 m packet at 145.01-145.09; 144.390 would be digipeated and gated to APRS-IS, and every
regional APRS frequency is refused), no path, the radio's lowest power (checked through `aprs.tx_power` before keying;
6 RF says APRS keys at the radio's current level). Each transmission is about 0.6 s. Every APRS setting touched is
saved and put back once the queued frames and message retries have gone out on the test frequency.

1. Position, status, message: the bench's RTL-SDR records I/Q while the radio sends; `tools/aprs_afsk_check.py`
   FM demodulates it on the bench and decodes Bell 202 / AX.25 (its own demodulator: tone envelopes, a bit clock PLL,
   NRZI, HDLC, the X.25 CRC; `--self-test` checks it on synthesized I/Q and on `aprsTest --wav`). Asserted: a valid
   FCS, `CALL-SSID>APZODR` with no digipeaters, the payload (position within 1e-4 degree, `>status`,
   `:ADDRESSEE:text{N`), `aprs.rf_sent` +1, the radio back on its channel, deviation peak 2.0-4.5 kHz, twist under
   6 dB. Printed: mark and space deviation, TX delay (flags before the frame, and carrier to first frame bit), the
   phase residual at tone changes after the fitted switching delay (continuous phase: about 10 degrees RMS; random
   restarts: about 100). The position is sent from the DMR channel and from the FM VFO.
2. Digital: on the DMR test channel the position goes to `--aprs-dmr-dst` as an LRRP report and as a UDT location;
   the bench modem's listener hears the header and blocks, the test checks the CRCs, the IPv4 / UDP 4001 / LRRP or
   UDT NMEA layers and the position (within 2e-5 degree). Passed on air (DM-1701, 467.375 MHz CC 2, 50 mW).
3. Still manual: BrandMeister (`aprs set via dmr` through a BrandMeister repeater or hotspot; the position on aprs.fi
   with the 5057 SSID, else `dmr_udt 1`, and the LRRP request ID question, 8.1) and KISS (`aprs set kiss 1`,
   `tools/aprs_gate.py --port PORT --kiss-tcp 8001`, or in USB network mode the radio's own KISS port 8001; a position
   from APRSdroid / Xastir goes out on RF).

Results so far: DMR LRRP and UDT pass. AFSK from 23469a2 / 0229a72 keyed a 0.59 s carrier on 145.090 MHz with no
tones (about 25 Hz RMS deviation; the 1750 Hz burst likewise silent while CTCSS on the same carrier deviated 2 kHz):
the voice source bug in `hardware-dm1701.md` 5.10, fixed in 694249b. On ff6268b (HIL 2026-10-10) AFSK decoded off air with a valid FCS,
twist 0.4-1.4 dB, continuous phase (6.5 degrees residual), but 7.2 kHz peak deviation at tone level 0x40; 237211a set
0x1C (about 3.1 kHz, `hardware-dm1701.md`).

## 14. Open decisions

- Default triggers: SmartBeaconing on with a 10 min interval fallback, after-PTT and boot off.
- A registered tocall instead of APZODR.
- The LRRP request ID and token for unsolicited reports to BrandMeister; whether LRRP answers should use the APRS
  position when there is no GPS; whether Settings > Location > Send position should drive APRS.
- APRS messages merged into the Messages inbox (now: Stations heard and the debug link only).
- The codeplug APRS record's fixed position and frequency encodings.
- DM-1701 receive from the FM audio paths the bench found (`hardware-dm1701.md` 21).
- Settings and debug link section / command numbers may need renumbering when merged with other branches.

Future, not done:

- 9600 baud APRS (G3RUH FSK, scrambled NRZI direct FM): needs a flat (discriminator / modulator direct) audio path
  and an FSK modem; a tone generator path can't do it. Not planned for now.
- Messages in the Messages inbox; Settings > Location tied to APRS; GPS Info in the hotspot's own transmissions.
