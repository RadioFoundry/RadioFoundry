<!-- SPDX-License-Identifier: MIT -->

# Capabilities of the current firmwares

What the firmwares that OpenDigitalRadio replaces can do today, per radio, as user-level capabilities. This is the
feature target for the new firmware and the baseline users will compare it with. It says nothing about how the old
firmware presents these features (no menus, screens or keys): the new UI is designed separately in `docs/ux/`.

Sources: the old trees' status files and docs (as of 2026-10-09, build `bf08887`), commit history, bench notes.
The chip column is summarized; public sources and confidence for each chip are in [radios.md](radios.md).

## Radios

| Column | Radios | MCU / DMR chip | Notes |
| --- | --- | --- | --- |
| **1701** | Baofeng DM-1701 / Retevis RT-84; TYT MD-UV380 / Retevis RT-3S (same firmware family) | STM32F405, HR-C6000, AT1846S | The bench radio. The only radio with on-air results. MD-UV380 builds exist but haven't been tested by us |
| **9600** | TYT MD-9600 / Retevis RT-90 mobile (board versions V1, V2, V4, V5 need different builds) | STM32F405, HR-C6000, per-band synthesizers | Never flashed by us. Upstream calls it experimental |
| **380** | TYT MD-380 / MD-390 / Retevis RT3, one band per radio (UHF or VHF models), with or without GPS | STM32F405, HR-C5000, SKY72310 PLL | Never flashed. FM base known working upstream; our DMR bring-up is untested |
| **GD77** | Radioddity GD-77 (the same firmware family also covers GD-77S, DM-1801, RD-5R, which we don't build) | NXP MK22, HR-C6000, AT1846S | Never run on a radio by us. Flash nearly full (about 6 KB left with SFR) |
| **DM32** | Baofeng DM-32UV | HR_C7000 (C-SKY CK803S, built-in DMR modem), FD6818 RF | Never flashed. Only about 20 KB of program flash free |

## Status codes

| Code | Meaning |
| --- | --- |
| **T** | Tested on air (or on hardware for non-RF features) on that radio |
| **U** | Upstream feature, imported unchanged and used on the radio, but not formally tested by us |
| **B** | Built (and host tested where shared code allows), never run on that radio |
| **P** | Partial: works with notable gaps (see the limitations) |
| **X** | Known broken |
| **off** | In the source but switched off at build time by default (flash space) |
| **—** | Not available on that radio |

For 9600, 380, GD77 and DM32, **B** covers both our additions and upstream features: nothing has been run on those
radios by us. Upstream (the legacy firmware) reports most of its own features working on the GD-77, MD-UV380 and DM-1701; the
MD-9600 is upstream beta.

## FM (analog)

| Capability | 1701 | 9600 | 380 | GD77 | DM32 | Limitations |
| --- | --- | --- | --- | --- | --- | --- |
| FM voice TX/RX, 12.5 and 25 kHz bandwidth | T | B | B | B | B | DM32 FM worked in another firmware's base build |
| CTCSS and DCS encode and decode, separate RX and TX tones | U | B | B | B | B | |
| CTCSS / DCS tone scan (find the tone in use) | U | B | B | B | B | |
| Squelch: per-band default and per-channel override | U | B | B | B | B | |
| Squelch tail elimination (STE / non-STE per channel) | U | B | B | B | B | |
| 1750 Hz tone burst for repeater access | U | B | B | B | B | |
| DTMF transmit (manual digits and stored DTMF contacts) | U | B | B | B | B | No DTMF decode |
| VOX (threshold and tail, enabled per channel) | U | B | B | B | B | |
| Mic gain per mode (FM, DMR) | U | B | B | B | B | |
| Repeater reverse (listen on the input) | U | B | B | B | B | |
| Monitor (open squelch) | U | B | B | B | B | |
| APRS position beacons (AX.25 1200 baud AFSK, 300 baud option; fixed or GPS position; manual, periodic, on PTT, SmartBeaconing with decay; position ambiguity; QSY info; via satellite) | U | off | — | off | off | Transmit only: no APRS receive or decode. 380 base has no APRS. Off by default on 9600, GD77, DM32 to save flash |
| Analog filter (receive only with the right tone) | U | B | B | B | B | |

## DMR voice

| Capability | 1701 | 9600 | 380 | GD77 | DM32 | Limitations |
| --- | --- | --- | --- | --- | --- | --- |
| DMR Tier II voice, direct mode (DMO) | T | B | B | B | B | 380: C5000 RX burst numbering and PA timer phase unverified. DM32: from OpenDM32 disassembly only |
| Through a repeater (BS activation / wake-up, RMO) | T | B | B | B | B | |
| Timeslot and color code per channel, manual timeslot override | U | B | B | B | B | |
| Force direct mode on a channel | U | B | B | B | B | |
| Talkgroups, RX group lists (hear a set of talkgroups), per-contact timeslot override | U | B | B | B | B | |
| Manual talkgroup / private call ID entry | U | B | B | B | B | |
| Private calls (make, receive, accept or refuse incoming private calls) | U | B | B | B | B | |
| Group all-call reception | U | B | B | B | B | |
| Receive filters: by talkgroup / contact / RX group, by color code and timeslot, or none ("promiscuous", hear everything) | U | B | B | B | B | Can also ignore bad CRC (for weak signals) |
| Talker alias: receive and display; transmit own alias (text, APRS position, or both) | U | B | B | B | B | |
| DMR ID database on the radio (ID to callsign and name), loaded by the CPS | U | B | B | B | B | Size limited by the flash region |
| Last heard list | U | B | B | B | B | |
| Caller display priority (database, contacts, talker alias) | U | B | B | B | B | |
| Optional per-channel DMR ID (another own ID per channel) | U | B | B | B | B | |
| DMR AGC, DMR TX and RX beeps (per channel no-beep option) | U | B | B | B | B | |
| Capture timeout (how long a heard talkgroup is kept for reply) | U | B | B | B | B | |
| Repeater roaming: channels in a zone ordered by distance from the current position, re-ordered after moving 5/10/20 km, or manual | U | B | — | B | B | Needs channel locations in the codeplug and a position (GPS or fixed) |
| AMBE de-interleave fix (one table entry was wrong upstream) | T | B | B | B | B | Upstream images still have the bug |

## DMR data (our work)

All of this is our own code (`shared/`), the same on every radio, with the chip-specific burst engine per radio. See
`docs/dmr_data.md` in the old tree for protocol detail; `docs/spec/` for the new specs.

| Capability | 1701 | 9600 | 380 | GD77 | DM32 | Limitations |
| --- | --- | --- | --- | --- | --- | --- |
| Motorola TMS text receive (UDP 4007, IPv4 or compressed headers, all three block rates), 4-message inbox, automatic TMS ACK | T | B | B | B | B | Inbox in RAM only (lost at power off). UTF-16 shown as Latin-1. GD77: inbox readable over USB only, no on-radio display |
| TMS text send (from a host over USB or the network adapter) | T | B | B | B | B | No compose on the radio. Group TMS to the selected talkgroup accepted, others ignored |
| Radio check (answered) | T | B | B | B | B | Layout from MMDVMHost; not checked against a real Motorola radio |
| Call alert (answered and rings, optional loud ring above the volume setting) | T | B | B | P | B | 9600: no loud ring (analog volume knob). GD77: answered, no on-radio alert screen |
| Listen before talk for every data transmission (data, ACKs, CSBK answers) | T | B | B | B | B | Not applied in hotspot mode |
| Motorola ARS presence registration (to a configured ARS ID), refresh, refusal handling, deregistration at power off | T | B | B | B | B | No ARS user login, no per-channel ARS. Deregistration only on a power-switch off, not when the supply is cut. On-air test was against a server that never answers (retries ran) |
| Basic Privacy: Motorola BP (key 1-255) and Anytone 16-bit BP, for private calls with one chosen contact | T | B | B | — | B | Group calls, data and hotspot traffic stay clear. Only one contact. 380: depends on the unverified burst numbering |
| ETSI UDT NMEA GPS reports (periodic 30 s to 60 min, to an ID or talkgroup; and after each own call) | — | B | B | — | B | Needs a GPS. Positions older than 30 s go without the fix flag. UDT block CRC unmasked (RadioDesk's assumption) |
| Motorola LRRP: immediate location, triggered reports (periodic, distance), stop | — | B | B | — | B | Needs a GPS. No altitude or accuracy elements (fixed 10 m radius), no emergency triggers, no protocol version answer. Triggers lost at power off |
| Send and receive any CSBK, MBC, data header, Rate 1/2, 3/4 and 1 block (raw bursts from a host), up to 40 bursts per transmission | T | B | B | B | B | Rate 1 has no FEC: one bit error loses the packet |
| Unconfirmed packet data TX (built by the radio at any rate); confirmed data RX with ETSI ACK | T (TX), T (RX unconfirmed) | B | B | B | B | No confirmed TX (no retries or selective ACK). Confirmed RX not yet on air |
| USB host commands for data (send, status, receive queue, inbox, network mode) | T | B | B | B | B (over UART) | |
| USB network adapter (CDC-NCM): DHCP, the host gets 12.<own DMR ID>; UDP, ICMP and SCTP between the host and the air with Motorola CAI addresses (12.x individual, 225.x talkgroup multicast) | T | B | B | B | — | macOS on air; Windows and Linux untested. Serial and network are exclusive on STM32 (endpoints); GD77 is a composite device (untested). No TCP, no IPv6, no fragments, about 450 bytes max over the air. Sometimes macOS doesn't activate the link after a cold boot (re-enumeration mitigates it) |
| Raw DMR data port (UDP 40078, MQTT-SN framing) and monitor port (UDP 40077, every data burst heard) | B | B | B | B | — | Host tested only |
| Ping another radio's host over the air | T | B | B | B | — | About 2.25 s round trip |

## Hotspot and repeater

| Capability | 1701 | 9600 | 380 | GD77 | DM32 | Limitations |
| --- | --- | --- | --- | --- | --- | --- |
| MMDVM hotspot (simplex, slot 2), DMR voice both ways, over USB serial | T | B | B | B | B (programming cable UART) | DMR only via MMDVM; duplex not supported; most of SET_CONFIG ignored (no deviation control); retune drops what is in flight; power only in coarse steps |
| MMDVM over UDP on the network adapter (MMDVMHost `Protocol=udp`) | T | B | B | B | — | One client at a time |
| Hotspot data passthrough both ways (CSBK, MBC, headers, all block rates), every RF burst passed up as heard, back-to-back calls, NAK when full | T | B | B | B | B | Data RX in hotspot mode not on air yet |
| Carrier detect in status (from the receiver noise level) | T | B | B | B | B | |
| Hotspot frequencies outside amateur bands (band limit removed in hotspot) | T | B | B | B | B | |
| Standalone SFR, user roles: standard (Hytera / Retevis style) and Motorola ERDM style (inbound/outbound color codes); hears both copies, transmits aligned to the repeater | B | B | — | B | — | Not on air yet in any role |
| Standalone SFR, repeater roles: the radio repeats one slot on the other, 90 ms later, on one frequency (standard and Motorola style, TS or MS or BS framing, hang 1/2/4 s) | B | B | — | B | — | Standalone repeater untested. While keyed the HR-C6000 misreports heard bursts: headers rebuilt from the call's LC, data calls held and repeated after they end. BS framing has no CACH. 9600: synthesizer relock per burst may not fit the guard time. 380 (C5000) and DM32 (C7000) lack the chip glue |
| MMDVM SFR: the hotspot (repeats locally, links to a network host) | T | B | — | B | — | On air 10/10 calls clean on the DM-1701; per-channel SFR settings aren't stored by the CPS (spare channel bytes) |

## GPS and location

| Capability | 1701 | 9600 | 380 | GD77 | DM32 | Limitations |
| --- | --- | --- | --- | --- | --- | --- |
| GPS receiver: position, time, satellites, NMEA, clock set from GPS | — (DM-1701 has no GPS; MD-UV380 GPS models U) | B | B | P (external module mod) | B | 380 GPS power modes Off / On / Auto (Auto powers it only around due reports). DM32 and 9600 have no Auto mode |
| GPS NMEA logging to flash | U | B | — | — | — | |
| Fixed (manual) location and time zone, for distance and satellite use | U | B | B | B | B | |
| Distance to channel locations | U | B | — | B | B | |
| Satellite pass prediction (TLE) and Doppler-corrected tuning, APRS via satellite | U | off | B | off | off | Off by default on 9600, GD77, DM32 (flash) |

## Radio, power and battery

| Capability | 1701 | 9600 | 380 | GD77 | DM32 | Limitations |
| --- | --- | --- | --- | --- | --- | --- |
| Channels in zones, extended zones, an all-channels view; zone and all-channels scan skip | U | B | B | B | B | |
| Dual VFO with frequency entry, step, split TX/RX, TX/RX lock | U | B | B | B | B | |
| Dual watch (two frequencies) | U | B | B | B | B | |
| Channel scan, VFO range scan, scan modes (hold, pause, stop), dwell and delay, scan on boot | U | B | B | B | B | No priority channel |
| Spectrum / band sweep display of a VFO range | U | B | B | B | B | |
| RSSI / S-meter | U | B | B | B | B | |
| TX power levels (named steps plus a user-defined power), per channel or global; force 10 W on mobiles | U | B | B | B | B | 9600 power calibration poor on some units upstream. DM32 levels from factory calibration, unchecked |
| TX timeout timer with warning beep; RX-only channels; global TX inhibit | U | B | B | B | B | |
| Band limits (amateur bands only, or off) | U | B | B | B | B | 435-438 MHz refused when on |
| PTT latch | U | B | B | B | B | |
| Receive power saving (economy levels, per channel opt-out), auto power off (with or without RF activity), suspend at power off | U | B | B | B | B | |
| Battery voltage display (percent or volts) and calibration; temperature calibration | U | B | B | B | B | |
| Safe power on (no TX until confirmed), auto keypad lock, channels read-only | U | B | B | B | B | |
| Factory calibration used (TX/RX, power) and an on-radio calibration editor | U | B | B (read only) | B | P | DM32: reads its own factory table (TX center, modulation, PA drive); the editor still edits the wrong (MD-UV380) table |

## Voice prompts, display, languages

| Capability | 1701 | 9600 | 380 | GD77 | DM32 | Limitations |
| --- | --- | --- | --- | --- | --- | --- |
| Voice prompts (AMBE-encoded prompt pack loaded by the CPS), three verbosity levels, beep-only and silent modes | U | B | B | B | B | Prompt files built per language by upstream tools |
| Keypad and other beeps, beep volume, timeout beep | U | B | B | B | B | |
| Color themes (day / night), auto night mode, backlight modes and levels, contrast, double-height text, inverse video | U | B | — | B (mono) | B | 380 and GD77 monochrome or no themes |
| Boot image, boot melody or Morse, boot password | U | B | B | B | B | |
| 20 UI languages (two at a time: primary and secondary) | U | B | B | B | B | Strings are the legacy firmware's; must be rewritten |
| Clock display in the status line | U | B | B | B | B | |

## CPS / programming

| Capability | 1701 | 9600 | 380 | GD77 | DM32 | Limitations |
| --- | --- | --- | --- | --- | --- | --- |
| legacy CPS: read/write codeplug, DMR ID database, voice prompts, boot image, settings; backup of flash | T | B | B | B | B (programming cable UART, 3 ms idle framing) | DM32 not tried with the CPS; a USB serial adapter that pauses more than 3 ms mid-request breaks framing |
| qdmr support (GD-77/UV380 open-firmware drivers) | U | B | B | B | — | See `legacy-cps-interop.md` |
| Firmware update through the stock bootloader (DFU on STM32, .sgl on GD-77, Baofeng packed image on DM-32) | T | B | B | B | B | The AMBE codec must come from each radio's own stock firmware |
| Channel edits on the radio saved to the codeplug | U | B | B | B | B | |
| Screen capture and audio/AMBE access over USB (CPS tools) | U | B | B | B | B | |

## Security

| Capability | 1701 | 9600 | 380 | GD77 | DM32 | Limitations |
| --- | --- | --- | --- | --- | --- | --- |
| Basic Privacy (see DMR data) | T | B | B | — | B | Static keystream only: no ARC4, AES or DES (no PI header support yet) |
| Boot password, keypad lock, channels read-only, TX inhibit | U | B | B | B | B | Boot password is weak (no real protection of the codeplug) |

## Known limitations across radios

- Only the DM-1701 has been on the air. Every other radio is built only.
- Text messages live in RAM (4 messages), are lost at power off and can't be written on the radio.
- Hotspot: simplex slot 2 only; deviation not adjustable; retuning drops anything in flight; power maps to uneven
  steps from the MMDVM RF level.
- USB on the STM32 radios is either serial or network, not both.
- GD-77 and DM-32 have almost no flash left; satellite and APRS are switched off to make room.
- The SFR settings live in spare channel bytes that a CPS may clear (role goes back to off).
- MD-9600 power-off deregistration needs the power button; MD-9600 board version must be guessed (wrong build: no
  TX/RX, no harm).
- DM-32: its data area overlaps a region stock firmware uses (voice recording / extra contacts), so switching
  between stock and our firmware may lose data.

## Commonly wanted, missing

Capabilities users of these radios generally expect that no current build has:

- Compose, reply to, store and resend text messages on the radio; message history in flash; ETSI and Hytera text
  formats; DMR SMS through Brandmeister.
- Confirmed data transmission (retries, selective ACK).
- Stronger encryption: ARC4 (EP), AES-128/256, Hytera EP, per-channel keys, group call privacy.
- Dual Capacity Direct Mode (DCDM) and on-air-tested SFR / ERDM.
- Emergency: emergency alarm and call, man-down / lone worker, remote stun / revive / kill.
- APRS receive and decode (message display, station list); APRS messaging.
- DTMF decode, ANI, 2-tone / 5-tone selective calling, analog voice scrambler.
- Priority scan and priority channel; DMR dual-slot / dual-watch monitoring on digital; scan lists separate from zones.
- Talkaround on repeater channels as a one-step switch; per-channel ARS.
- Voice recording and playback.
- NOAA weather alert (where the RF covers it); FM broadcast receive on radios that have it in stock.
- GPS on DM-1701-class radios via an external receiver, and location reports (UDT, LRRP) on the GPS-equipped
  MD-UV380.
- Bluetooth audio and programming (on hardware that has it), Wi-Fi none.
- Over-the-air programming and remote monitor.
- IP site connect style roaming by signal strength (RSSI-based repeater roaming rather than GPS distance). Ours now:
  `CONFIG_DMR_ROAMING`, `docs/spec/dmr-roaming.md` (standard DMR only, no vendor site signaling).
- A native qdmr driver (beyond the legacy protocol shim) with every setting the radio has, including SFR and data
  options.
- Persistent ARS state and LRRP triggers across power cycles; LRRP altitude and accuracy.
