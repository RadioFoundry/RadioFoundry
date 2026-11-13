<!-- SPDX-License-Identifier: MIT -->

# APRS

APRS (Automatic Packet Reporting System) shares your position with other amateur stations and maps such as aprs.fi.
The radio sends APRS two ways:

- **On FM**: as 1200 baud AFSK packets on the APRS frequency of your region. The radio leaves your channel for about
  a second, sends, and comes back.
- **Over DMR**: as a position report to an APRS gateway ID (BrandMeister's 310999 by default; it works from any master, but your own master's MCC plus 999 is recommended, for example 204999). This works on DMR
  channels. BrandMeister forwards Motorola (LRRP) and Hytera position reports to APRS, so keep the default LRRP format
  (not UDT). A radio that already saved APRS settings keeps its stored ID: run `aprs set dmr_dst 310999` (US) or
  `aprs set dmr_dst <MCC>999`. The position appears on APRS-IS as your callsign with the SSID from your BrandMeister
  profile (aprs.fi may take a minute to show it).

Not every radio does both. Some send only, some can't send APRS on FM yet, and a build can leave APRS out for flash
room. The [Radio reference](23-radios.md) lists them and says where APRS is verified.

## Before you start

- You need an amateur radio license. APRS sends only on amateur frequencies, and the radio refuses others.
- Your callsign is the first word of the radio name (Settings > My radio, or the codeplug's radio name). Without a
  valid callsign nothing is sent, and the APRS page says why.
- Sending is off until you turn it on. "Send position now" works at any time.

## The APRS page

Press **OK**, **6** (Tools), **8** (APRS). The top of the page shows your callsign, the APRS
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
| KISS on USB | The radio works as a KISS TNC for APRS programs on the computer |
| Stations heard | Stations the radio heard (radios that can receive APRS), newest first. A station that sent its position shows how far away and which way it was from yours, when the radio has a position too; the age on that row is then the age of that position. Other rows show when the station was last heard. Objects and items a station sends don't count as its position, and a position more than a day old is dropped. Select one and choose Message to fill its callsign |

A firmware built with only one way to send APRS shows only the rows for it. With APRS over DMR only there is no
Frequency, Power, Path, Send via or Stations heard, and KISS on USB is called NMEA on USB (the computer can still give
the radio its position).

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
stations go into **Stations heard**, messages to your callsign appear in **Messages** and are acknowledged, and the digipeater (if you
turned it on) passes packets on. Messages from internet stations (an APRS messaging service, for example) reach you
through a nearby iGate. They appear under the sender's callsign, and the acknowledgment goes back to that sender.

- It listens only on the APRS frequency, in FM, and the tuned frequency must equal the APRS **Frequency** exactly.
  On other FM channels nothing is decoded.
- The volume knob doesn't matter: a radio turned all the way down still decodes.
- On a radio that decodes only what opens the squelch ([Radio reference](23-radios.md)), a packet must be strong enough to open it. Keep the squelch at its usual level, not tight.
- Battery saving pauses while the radio listens for APRS: a sleeping receiver misses packets.
- Receiving is on by default. Turn it off with `tools/rfoundry_debug.py aprs set rx 0` (FM only).

On a DMR channel, a DMR APRS position another radio sends (some makes send it as an NMEA position) also goes into
**Stations heard**, whoever it is addressed to: usually the APRS gateway, as on FM every station heard is listed. It
is listed under the sender's callsign when its DMR ID is in the radio's DMR ID database, else under the DMR ID. This
works whatever the receive setting. **Stations heard** shows the station and when it was heard, not its position.

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

The position is not only APRS's: DMR calls and location requests (LRRP) use the same one. Its settings are under
**Settings > Location**: **Position source** (Auto, GPS, Computer, Fixed) and **Fixed position**, a page with
**Latitude** and **Longitude**. To set the fixed position:

1. Open Settings > Location > Fixed position, select Latitude and press OK. The value opens for editing.
2. Type the degrees with the digits, decimal places included: `3`,`3`,`7`,`4`,`9`,`0` is 33.7490. The decimal point is
   already there. Longitude has three places before the point: 84.3880 is `8`,`4`,`3`,`8`,`8`,`0` (the radio puts the
   leading 0 in for you), and 122.4194 is `1`,`2`,`2`,`4`,`1`,`9`,`4`. For 10 to 19 degrees, type the 0 yourself.
3. Press Up or Down (or turn the knob) to switch between N and S, or E and W. West longitudes are W.
4. Press OK to save. Places you didn't type count as zeros. BACK removes the last digit; with nothing typed it closes
   the value unchanged.

Four decimal places put you within about 11 m. You can also type "latitude" or "fixed" on Home to find the page.

## GPS status

**Tools > Position** shows what the radio's GPS is doing, on four pages. Press Up or Down (or turn the knob) for the
next page, or press 1 to 4. On a radio with a tall screen, Status also shows the grid locator and degrees, minutes and
seconds, so there are three pages.

| Page | What it shows |
|---|---|
| 1 Status | 3D Fix, 2D Fix (Fix when the receiver doesn't say which) or Searching; satellites used and seen; position, altitude, speed, HDOP (accuracy: good, fair, poor) and the UTC time of the fix |
| 2 Sky | where each satellite is: the outer ring is the horizon, the middle ring 45 degrees up, north at the top. A filled dot is used in the fix. The colors and the legend show the system: GPS, GLONASS, Galileo, BeiDou, QZSS |
| 3 Signal | each satellite's signal strength as a bar; filled bars are used in the fix |
| 4 Grid | your Maidenhead grid locator (FN42li), and the position in degrees, minutes and seconds |

"No GPS, or it is off" means the radio has no GPS or **Settings > Location > GPS** is off. A first fix outdoors takes
from a few seconds to a few minutes; indoors there may be none.

## With a computer (KISS and iGate)

Turn on **KISS on USB**, then run `tools/aprs_gate.py --port <radio port>`:

- `--kiss-tcp 8001`: APRS programs (APRSdroid, Xastir, YAAC) connect to TCP port 8001 and send through the radio. The
  radio sends only packets from your own callsign, at most one every 10 seconds after a burst of three.
- `--call <your call>`: connects to APRS-IS. A radio that can receive APRS is then an iGate. It passes what it hears
  to APRS-IS. If you allow it, it also passes messages from APRS-IS to local stations back on the air.
- In USB network mode use `--port udp:<radio address>`.

The radio can't be in hotspot mode at the same time.

In USB network mode the radio does both without the helper. APRS programs connect to the radio's address on TCP
port 8001 directly ([Services on the radio](15-usb-network.md#services-on-the-radio)). The radio logs in to an
APRS-IS server itself ([APRS-IS from the radio](15-usb-network.md#aprs-is-from-the-radio)). KISS on USB must be on
for both.
