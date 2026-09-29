<!-- SPDX-License-Identifier: MIT -->

# Channels, zones and the VFO

A **channel** is a saved frequency with its mode (FM or DMR) and settings. A **zone** is a named list of channels.
The **VFO** tunes any frequency by hand. Channels and zones come from the codeplug ([Programming with
qdmr](15-programming-qdmr.md)).

## Changing the channel

- Turn the channel knob, or press **Up** or **Down** on Home, to step through the zone's channels.
- Type the channel's position on Home and press **OK**: **1**, **2**, **OK** goes to channel 12 of the zone.
- Type letters of the channel's name on Home (see [Find](04-keys-and-navigation.md#find)) and press **OK** on it.
- **OK**, **4** (Channels), focus the zone, **#**: the zone's channels. Focus one and press **OK**.

<img src="../ux/screens/160x120i5/zone_channels.png" width="200" alt="A zone's channels">

Changing the channel ends temporary changes, the reply window, hear all and a scan.

## Changing the zone

- Press **P2** for the next zone, **P1** for the previous one. The radio shows "Zone:" and the name.
- Or: **OK**, **4** (Channels). The list opens with the current zone focused ("here"). Focus another zone and press
  **OK**. The radio switches to it and returns to Home.
- Or type the zone's name on Home; Find lists zones too.

<img src="../ux/screens/160x120i5/channels.png" width="200" alt="Channels: the VFO and the zones">

The Channels list shows the VFO first, then every zone with its number of channels. Type letters to filter it.

## Saving changes to a channel

Change power, squelch, tone, bandwidth, timeslot, color code or mode with quick settings, then hold **OK** (on Home or
in quick settings). The radio writes the channel in the background and shows "Saved to channel". The squelch is saved
as the radio's default squelch, not in the channel.

A save that fails shows "Couldn't save: ..." and the change stays for now only.

## The VFO

The VFO is FM only. A DMR VFO isn't built yet.

1. **OK**, **4** (Channels). The VFO is the first row.
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
- A digit that can't lead to a frequency in the radio's bands is refused, and the radio shows the bands
  (DM-1701: 136-174 and 400-480 MHz).
- Up to eight digits (10 Hz). After 10 s without a key, the entry is dropped.

<img src="../ux/screens/160x120i5/home_vfo_entry.png" width="200" alt="Typing a VFO frequency">

### Stepping

**Up**, **Down** and the channel knob change the VFO frequency by its step. The step comes from the codeplug's VFO
settings; the quick settings Step tile (8) shows it but can't change it yet ("Step: set it in qdmr for now").

### Leaving the VFO

**OK**, **4**, then **OK** on the VFO row again ("Back to channels"), or pick a zone or channel.

## Channels the radio can't use

Channels outside the radio's bands don't transmit ("Outside ..."). Receive-only channels show "RX only" as the talk
target.
