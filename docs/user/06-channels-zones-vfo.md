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
"TG" goes first, then the frequency. The channel you are on is marked. Type letters to filter the list.

<img src="../ux/screens/160x120i5/channels.png" width="200" alt="Channels: every channel by number">

- **OK** on a channel opens its settings, where you can change them (below).
- **#** in the list goes to the channel: in the current zone if it holds it, otherwise in the first zone that does.
  A channel in no zone says "Not in any zone".
- The first row, **New from current**, starts a new channel from what the radio uses now (below).

## Editing a channel

The channel's page lists every setting: name, mode, RX and TX frequency, power, RX only, then bandwidth, tones and
VOX on FM, or color code, timeslot, TX contact, RX group list, own ID, direct mode, talk permit beeps and talker
alias on DMR, then TX time-out, scan, battery saver and the zones that hold it. The list scrolls.

<img src="../ux/screens/160x120i5/channel.png" width="200" alt="A channel's settings">

Focus a row and press **OK**:

| Row | What OK does |
|---|---|
| Two choices (mode, timeslot, bandwidth, RX only, scan, VOX, direct mode, beeps, talker alias, battery saver) | switches to the other one |
| Power, RX tone, TX tone, color code, TX time-out | opens a picker: turn the knob (or Up / Down) to the value, **OK** keeps it, **Back** cancels. Color code and time-out also take digits |
| RX, TX, own ID | type the digits, **OK** keeps them, **Back** takes the last digit back. Digits outside the radio's bands are refused as you type. A new RX moves TX with it, so a repeater keeps its offset. If that would put TX outside the radio's bands, TX is set to the new RX |
| Name | opens text entry (below) |
| TX contact, RX group list | opens the list of contacts or group lists; type letters to find one |

<img src="../ux/screens/160x120i5/channel_power.png" width="200" alt="Picking the power level with the knob">

A changed row is marked until you save. **Hold OK** anywhere on the page to save, or pick the **Save** row. The radio
writes the channel in the background and says "Saved channel 3". **Back** with unsaved changes asks whether to
discard them; the answer starts on "No, keep editing". Saving any channel drops the quick-settings changes on the
channel you are on: the radio reloads it from the codeplug.

The radio won't save a channel without a name, with RX outside its bands, with TX where it can't transmit (set RX only
instead), or a DMR channel without a TX contact. It says which.

**Save as new channel** stores the edited copy at the lowest free number and leaves the original as it was.
**Go to it** tunes the channel; save or discard your changes first.

### Text entry

It works like a phone keypad. Keys **2** to **9** type their letters, then the digit, then the letters in the other
case: press **2** once for A, twice for B, four times for 2, five times for a. Press the same key again within a
second for the next character; another key, or a pause, moves on. **1** types punctuation (`. - / , ' & ( ) @ _ ! ?`
and 1), **0** a space, 0, `+`, `#` and `*`. The title shows the mode.

**#** switches the mode: ABC (capitals, the start), then 123 (each key types its digit), then abc (lower case), then
back to ABC. Holding a digit key types the digit in any mode. **Back** deletes the last character (**\*** does too).
**OK** keeps the name. To leave without changing it, press **Back** until the field is empty and once more. Names hold
up to 16 characters.

On a radio without a keypad, Up / Down change the last character, **OK** adds another, and holding **OK** keeps the
name.

<img src="../ux/screens/160x120i5/channel_name.png" width="200" alt="Typing a channel name">

### New channel from current settings

**OK**, **5**, then the first row, **New from current**. It opens an unsaved copy of the channel you are on, with any
quick-settings changes, or of the VFO, at the lowest free channel number. The name gets " 2" added (from the VFO, the
frequency). Change what differs, then hold **OK** to save it. A new channel is in no zone yet: add it to one with qdmr
([Programming with qdmr](15-programming-qdmr.md)).

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
