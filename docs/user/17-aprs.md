<!-- SPDX-License-Identifier: MIT -->

# APRS

APRS (Automatic Packet Reporting System) shares your position with other amateur stations and maps such as aprs.fi.
The radio sends APRS two ways:

- **On FM**: as 1200 baud AFSK packets on the APRS frequency of your region. The radio leaves your channel for about
  a second, sends, and comes back.
- **Over DMR**: as a position report to an APRS gateway ID (BrandMeister's 5057 by default), when you are on a DMR
  channel.

APRS is in every build that has FM. Packets the DM-1701 sends on FM and over DMR decode off air on the SDR bench. The DM-1701 can't hear APRS packets: its
received audio never reaches the processor, so it sends only. The MD-380 can't send APRS on FM yet (no tone path); it
sends positions over DMR.

## Before you start

- You need an amateur radio license. APRS sends only on amateur frequencies, and the radio refuses others.
- Your callsign is the first word of the radio name: Settings > My radio, or the codeplug's radio name. Without a
  valid callsign nothing is sent, and the APRS page says why.
- Sending is off until you turn it on. "Send position now" works at any time.

## The APRS page

**OK**, **5** (Tools), then **APRS** (the last row). The top of the page shows your callsign, the APRS frequency, your
position and where it comes from, and when the next beacon goes out. If the radio can't send, it says why.

| Row | What it does |
|---|---|
| Send position now | One position at once |
| Automatic beacons | On: positions go out by themselves (see Triggers) |
| Frequency | Your region: N America 144.390, Europe 144.800, Australia 145.175, New Zealand 144.575, Japan 144.660, China 144.640 MHz, or Custom (`tools/odr_debug.py aprs set region custom`, then `aprs set freq_hz 144390000`) |
| Interval | How often: Off, 1, 2, 5, 10 or 30 minutes |
| SmartBeaconing | More often when you move fast, at once after a turn, rarely when you stand still |
| Send via | Radio (FM), DMR, or both |
| Position | Auto, GPS, Computer (NMEA from the USB host), Fixed |
| Position in DMR calls | Your position in your DMR voice calls (see [DMR](08-dmr.md#position-in-calls)) |
| KISS on USB | The radio works as a KISS TNC for APRS programs on the computer |
| Stations heard | Stations the radio heard (radios that can receive APRS) |

The fixed position, symbol, path, comment and the DMR destination are set with the debug tool for now
(`tools/odr_debug.py aprs set ...`), later with qdmr.

## Triggers

Automatic beacons go out at the interval, or with SmartBeaconing by speed and turns. Never more often than every
30 seconds. The radio waits for a clear APRS frequency before it sends, and gives up after 10 seconds. Pressing PTT
always wins: an APRS transmission in progress stops and your channel comes back.

## Position

The DM-1701 has no GPS. Its position comes from:

- **Computer**: NMEA from a GPS on the computer, sent over USB by `tools/aprs_gate.py --nmea <GPS port>` (KISS on USB
  must be on).
- **Fixed**: a position you enter.

Auto takes a fresh GPS position, else the computer's, else the fixed one.

## With a computer (KISS and iGate)

Turn on **KISS on USB**, then run `tools/aprs_gate.py --port <radio port>`:

- `--kiss-tcp 8001`: APRS programs (APRSdroid, Xastir, YAAC) connect to TCP port 8001 and send through the radio. The
  radio sends only packets from your own callsign, at most one every 10 seconds after a burst of three.
- `--call <your call>`: connects to APRS-IS. On radios that can receive APRS the radio is an iGate: it passes what it
  hears to APRS-IS, and messages from APRS-IS to local stations back on the air if you allow it.
- In USB network mode use `--port udp:<radio address>`.

The radio can't be in hotspot mode at the same time.

In USB network mode the radio does both without the helper: APRS programs connect to the radio's address on TCP
port 8001 directly ([Services on the radio](14-usb-network.md#services-on-the-radio)), and the radio logs in to an
APRS-IS server itself ([APRS-IS from the radio](14-usb-network.md#aprs-is-from-the-radio)). KISS on USB must be on
for both.
