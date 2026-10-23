<!-- SPDX-License-Identifier: MIT -->

# APRS

APRS (Automatic Packet Reporting System) shares your position with other amateur stations and maps such as aprs.fi.
The radio sends APRS two ways:

- **On FM**: as 1200 baud AFSK packets on the APRS frequency of your region. The radio leaves your channel for about
  a second, sends, and comes back.
- **Over DMR**: as a position report to an APRS gateway ID (BrandMeister's 5057 by default). This works on DMR
  channels.

Not every radio does both. Some send only, some can't send APRS on FM yet, and a build can leave APRS out for flash
room. The [Radio reference](23-radios.md) lists them and says where APRS is verified.

## Before you start

- You need an amateur radio license. APRS sends only on amateur frequencies, and the radio refuses others.
- Your callsign is the first word of the radio name (Settings > My radio, or the codeplug's radio name). Without a
  valid callsign nothing is sent, and the APRS page says why.
- Sending is off until you turn it on. "Send position now" works at any time.

## The APRS page

Press **OK**, **6** (Tools), then **APRS** (the last row). The top of the page shows your callsign, the APRS
frequency, your position and its source, and when the next beacon goes out. If the radio can't send, it says why.

| Row | What it does |
|---|---|
| Send position now | One position at once |
| Automatic beacons | On: positions go out by themselves (see Triggers) |
| Frequency | Your region: N America 144.390, Europe 144.800, Australia 145.175, New Zealand 144.575, Japan 144.660, China 144.640 MHz, or Custom (`tools/rfoundry_debug.py aprs set region custom`, then `aprs set freq_hz 144390000`) |
| Power | As channel: APRS transmits at the radio's current power level. Or pick a level of its own, for example a low one for a nearby digipeater. Dimmed on radios that can't send APRS over the air |
| Path | How far digipeaters pass your packets on: None, WIDE1-1, WIDE1-1,WIDE2-1 (the default), WIDE2-1 or WIDE2-2. See Path below. Dimmed on radios that can't send APRS over the air |
| Interval | How often: Off, 1, 2, 5, 10 or 30 minutes |
| SmartBeaconing | More often when you move fast, at once after a turn, rarely when you stand still |
| Send via | AFSK (on FM), DMR, or both |
| Position | Where the position comes from, and the fixed position (see Position below) |
| Position in DMR calls | Your position in your DMR voice calls (see [DMR](08-dmr.md#position-in-calls)) |
| KISS on USB | The radio works as a KISS TNC for APRS programs on the computer |
| Stations heard | Stations the radio heard (radios that can receive APRS) |

The symbol, comment and the DMR destination are set with the debug tool for now
(`tools/rfoundry_debug.py aprs set ...`), later with qdmr.

## Path

The path says how many digipeater hops a packet may take on FM. Each **WIDEn-N** entry allows N more hops. Press
**OK** on Path to step to the next choice.

| Path | Hops | Use it |
|---|---|---|
| None | 0 | Only stations that hear you directly (an iGate nearby) |
| WIDE1-1 | 1 | A fill-in digipeater or one wide digipeater |
| WIDE1-1,WIDE2-1 | 2 | Mobile and handheld use in most of North America and Europe (the default) |
| WIDE2-1 | 1 | Fixed stations that reach a wide digipeater directly |
| WIDE2-2 | 2 | Sparse areas; skips fill-in digipeaters |

More hops mean more copies on a busy frequency. Use the fewest that reach an iGate. The path doesn't apply to
positions sent over DMR.

The path is also in the codeplug's first APRS configuration, which qdmr edits. A change on the radio is written
there too, and a codeplug written with a different path changes the radio's. Whichever changed last is used.

## Receiving

On radios that can receive APRS, the radio decodes packets while you listen on the APRS frequency. Make a channel on
the frequency shown in **Frequency** on the APRS page (your region's, 144.390 MHz in North America) in FM, or tune
the VFO there, and leave the radio on it. If the packets are sent on another frequency, set **Frequency** to Custom
and enter it first. Heard
stations go into **Stations heard**, messages to your callsign are kept and acknowledged, and the digipeater (if you
turned it on) passes packets on.

- It listens only on the APRS frequency, in FM, and the tuned frequency must equal the APRS **Frequency** exactly.
  On other channels nothing is decoded.
- The volume knob doesn't matter: a radio turned all the way down still decodes.
- On the DM-1701 a packet must be strong enough to open the squelch. Keep the squelch at its usual level, not tight.
- Battery saving pauses while the radio listens for APRS: a sleeping receiver misses packets.
- Receiving is on by default. Turn it off with `tools/rfoundry_debug.py aprs set rx 0`.

## Triggers

Automatic beacons go out at the interval, or with SmartBeaconing by speed and turns. Never more often than every
30 seconds. The radio waits for a clear APRS frequency before it sends, and gives up after 10 seconds. Pressing PTT
always wins: an APRS transmission in progress stops and your channel comes back.

## Position

The position comes from:

- **GPS**: on radios that have one ([Radio reference](23-radios.md)).
- **Computer**: NMEA from a GPS on the computer, sent over USB by `tools/aprs_gate.py --nmea <GPS port>` (KISS on USB
  must be on).
- **Fixed**: a position you enter.

Auto takes a fresh GPS position, else the computer's, else the fixed one.

APRS > Position has three rows: **Source** (press OK to step through Auto, GPS, Computer, Fixed), **Latitude** and
**Longitude**. To set the fixed position:

1. Select Latitude and press OK. The value opens for editing.
2. Type the degrees with the digits, decimal places included: `3`,`3`,`7`,`4`,`9`,`0` is 33.7490. The decimal point is
   already there. Longitude has three places before the point: 84.3880 is `8`,`4`,`3`,`8`,`8`,`0` (the radio puts the
   leading 0 in for you), and 122.4194 is `1`,`2`,`2`,`4`,`1`,`9`,`4`. For 10 to 19 degrees, type the 0 yourself.
3. Press Up or Down (or turn the knob) to switch between N and S, or E and W. West longitudes are W.
4. Press OK to save. Places you didn't type count as zeros. BACK removes the last digit; with nothing typed it closes
   the value unchanged.

Four decimal places put you within about 11 m. You can also type "latitude" or "position" on Home to find the page.

## With a computer (KISS and iGate)

Turn on **KISS on USB**, then run `tools/aprs_gate.py --port <radio port>`:

- `--kiss-tcp 8001`: APRS programs (APRSdroid, Xastir, YAAC) connect to TCP port 8001 and send through the radio. The
  radio sends only packets from your own callsign, at most one every 10 seconds after a burst of three.
- `--call <your call>`: connects to APRS-IS. A radio that can receive APRS is then an iGate. It passes what it hears
  to APRS-IS. If you allow it, it also passes messages from APRS-IS to local stations back on the air.
- In USB network mode use `--port udp:<radio address>`.

The radio can't be in hotspot mode at the same time.

In USB network mode the radio does both without the helper. APRS programs connect to the radio's address on TCP
port 8001 directly ([Services on the radio](14-usb-network.md#services-on-the-radio)). The radio logs in to an
APRS-IS server itself ([APRS-IS from the radio](14-usb-network.md#aprs-is-from-the-radio)). KISS on USB must be on
for both.
