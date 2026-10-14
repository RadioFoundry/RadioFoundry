<!-- SPDX-License-Identifier: MIT -->

# Channels, zones and the VFO

A **channel** is a saved frequency with its mode (FM or DMR) and settings. A **zone** is a named list of channels.
The **VFO** tunes any frequency by hand. Channels and zones come from the codeplug ([Programming with
qdmr](15-programming-qdmr.md)).

## Changing the channel

- Turn the channel knob, or press **Up** or **Down** on Home, to step through the zone's channels.
- Type the channel's position on Home and press **OK**. For example, **1**, **2**, **OK** goes to channel 12 of the
  zone.
- Type letters of the channel's name on Home (see [Find](04-keys-and-navigation.md#find)) and press **OK** on it.
- Press **OK**, **4** (Zones), focus the zone and press **#**. The zone's channels open. Focus one and press **OK**.

<img src="../ux/screens/160x120i5/zone_channels.png" width="200" alt="A zone's channels">

Changing the channel ends temporary changes, the reply window, digital monitor and a scan.

## Changing the zone

- Press **Next zone** or **Previous zone** ([Key roles](23-radios.md#key-roles) shows which keys these are on your
  radio). The radio shows "Zone:" and the name.
- Or press **OK**, **4** (Zones). The list opens with the current zone focused ("here"). Focus another zone and press
  **OK**. The radio switches to it and returns to Home.
- Or type the zone's name on Home. Find lists zones too.

<img src="../ux/screens/160x120i5/zones.png" width="200" alt="Zones: the VFO and the zones">

The Zones list shows the VFO first, then every zone with its number of channels. Type letters to filter it.

## Every channel

**OK**, **5** (Channels) lists every channel in the codeplug by number and name, whichever zones hold it. Each
row ends with the RX frequency and the talkgroup (DMR) or FM. A long name stays whole and the row shortens the rest:
"TG" goes first, then the frequency. Builds
without it (the DM-1701 and MD-380 by default, for flash room) say "Channels: not in this build". The channel
you are on is marked. Type letters to filter the list.

<img src="../ux/screens/160x120i5/channels.png" width="200" alt="Channels: every channel by number">

- **OK** on a channel shows its settings: mode, RX and TX frequency with the offset, power, bandwidth and tones (FM),
  color code, timeslot, TX contact and RX group list (DMR), TX time-out, scan, and the zones that hold it. The page is
  read only. Change a channel in the codeplug ([Programming with qdmr](15-programming-qdmr.md)) or save quick settings
  changes (below).
- **OK** on that page, or **#** in the list, goes to the channel: in the current zone if it holds it, otherwise in the
  first zone that does. A channel in no zone says "Not in any zone".

<img src="../ux/screens/160x120i5/channel.png" width="200" alt="A channel's settings">

## Saving changes to a channel

Change power, squelch, tone, bandwidth, timeslot, color code or mode with quick settings. Then hold **OK** on Home or
in quick settings. The radio writes the channel in the background and shows "Saved to channel". The squelch is saved
as the radio's default squelch, not in the channel.

A save that fails shows "Couldn't save: ..." and the change stays for now only.

## The VFO

The VFO is FM only. A DMR VFO isn't built yet.

1. **OK**, **4** (Zones). The VFO is the first row.
2. Press **OK**. Home shows the frequency large, and the radio says "VFO: type a frequency".

<img src="../ux/screens/160x120i5/home_vfo.png" width="200" alt="Home in the VFO">

### Typing a frequency

On Home in the VFO, type the frequency digits from the hundreds of MHz down, without a decimal point. The radio shows
what you typed as you go.

| Keys | Frequency |
|---|---|
| **4** **4** **6** **0** **0** **0** | 446.000 MHz |
| **1** **4** **6** **5** **2** | 146.520 MHz |
| **4** **6** **1** **8** **6** **2** **5** | 461.8625 MHz |

- Type at least the three MHz digits, then press **OK** to tune.
- **Back** deletes the last digit.
- A digit that can't lead to a frequency in the radio's bands is refused, and the radio shows the bands. The bands
  depend on the radio ([Radio reference](23-radios.md)).
- Up to eight digits (10 Hz). After 10 s without a key, the entry is dropped.

<img src="../ux/screens/160x120i5/home_vfo_entry.png" width="200" alt="Typing a VFO frequency">

### Stepping

**Up**, **Down** and the channel knob change the VFO frequency by its step. The step comes from the codeplug's VFO
settings. The quick settings Step tile (8) shows it but can't change it yet ("Step: set it in qdmr for now").

### Leaving the VFO

Press **OK**, **4**, then **OK** on the VFO row again ("Back to channels"). Or pick a zone or channel.

## Channels the radio can't use

Channels outside the radio's bands don't transmit ("Outside ..."). Receive-only channels show "RX only" as the talk
target.
