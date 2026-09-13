<!-- SPDX-License-Identifier: MIT -->

# The Home screen

Home answers four questions at a glance: where you are, what **PTT** will do, what is happening, and whether the radio
is OK.

<img src="../ux/screens/160x120i5/home_dmr.png" width="200" alt="Home on a DMR channel">
<img src="../ux/screens/160x120i5/home_fm.png" width="200" alt="Home on an FM channel">

## Regions, top to bottom

| Region | Shows |
|---|---|
| Status line | The state (IDLE, RX, TX, or NO TX when a transmission is refused), the mode (DMR or FM), SCAN or MON when scan or monitor is on; on the right, "L" when the keys are locked, "M" and a number for unread messages, the signal bars, the battery icon and the battery percentage |
| Place | The zone name and the channel's position in the zone ("3/16"). In the VFO: "VFO" |
| Channel | The channel number and name, as large as they fit. In the VFO: the frequency |
| Talk target | "PTT→" and where **PTT** goes: a talkgroup or person on DMR, the transmit frequency on FM. During the reply window: "Reply→" and a countdown |
| Chips | DMR: timeslot, color code, power. FM: tone, bandwidth, power, squelch level |
| Activity | Idle: the last station heard and how long ago, or "No calls heard yet". Receiving or transmitting: see below |
| Hint row | What the nearest keys do now |

A field changed for now (a temporary change) is drawn in a highlight color on Home, and the hint row says
"Back undo  Hold OK save".

<img src="../ux/screens/160x120i5/home_temporary.png" width="200" alt="Home with a temporary power change">

The battery percentage turns to the warning color at 15 % and below.

## Hint row messages

The hint row changes with the situation. What the **Back** key does is always at the right end of the row, above the
key. The most common ones:

| Hint | Meaning |
|---|---|
| OK Menu  \* Quick  # Joe | The normal hint. The name after # is the last caller: **#** opens actions for them |
| Back undo  Hold OK save | The channel has temporary changes |
| Back ends reply  # caller | The reply window is running: **PTT** answers the last call |
| Back: channel's talkgroup | A talk target chosen for now is in use |
| Hear all on  Back clears | Hear all is latched on |
| Back stops scan  # skip | Scan is running |
| No codec: voice off | DMR channel without the codec merged |
| 0-9 freq  \* Quick  Up/Dn | In the VFO |
| Keys locked: hold \* to unlock | The keys are locked ("Locked: hold \* to unlock" where the row is narrow) |

## Receiving

During a DMR call, Home shows the caller: name (from your contacts, else from the DMR ID database in the codeplug),
DMR ID, timeslot, talker alias as it arrives (unless **Settings > Talk & listen > Show talker alias** is off), the talkgroup or "Private to you", and the signal strength.

<img src="../ux/screens/160x120i5/rx_group.png" width="200" alt="Receiving a group call">
<img src="../ux/screens/160x120i5/rx_private.png" width="200" alt="Receiving a private call">

A call that the radio hears but doesn't play shows why: "Other TG: not played" (a talkgroup you don't listen to),
"Encrypted: not played", or "No codec: voice off". Hold **SK1** to hear a call on another talkgroup.

On FM, the activity region shows "Receiving" while the squelch is open, or "Monitor (squelch open)".

## Transmitting

While you hold **PTT**, the screen shows TALKING, the target, the timeslot and color code (DMR), the time on air and
the time left before the transmit timeout ("TOT 3:00").

<img src="../ux/screens/160x120i5/tx.png" width="200" alt="Transmitting">

On DMR the screen may first show WAITING (the radio waits for a free slot) or WAKING RPT (it wakes up the repeater).

When the radio refuses to transmit, the status line shows NO TX, the screen says why and the hint row says what to
do:

| Message | Cause | What to do |
|---|---|---|
| This channel is RX only | The channel is set receive only | Pick another channel |
| Outside ... | The frequency is outside the radio's transmit bands | Pick another channel, or tune into a band in the VFO |
| TX time-out reached | You talked longer than the transmit timeout | Release **PTT** and press it again |
| No TX frequency set | The channel has no transmit frequency | Fix the channel in qdmr |
| No codec: voice off | DMR voice without the codec | Merge the codec ([Installing](02-installing.md)) |
| Channel busy (polite) | Someone else is talking on the slot | Wait for the call to end |
| No repeater answered | The repeater didn't wake up | Check frequency, color code and range |
| Hotspot mode: PTT off | The radio is a hotspot | Press **OK** to leave hotspot mode |
| No DMR ID in codeplug | The codeplug has no DMR ID | Set your ID in qdmr |
| DMR modem not running | The DMR chip didn't start | Restart the radio; see [Troubleshooting](17-troubleshooting.md) |

The low and critical battery levels also limit transmitting: see [Battery and power](16-battery-and-power.md).

## Large text

**Settings > Display > Large text** shows only the status line, the channel name and the talk target, each as large as
it fits. The radio keeps the setting across power off, as it does **Settings > Display > Hint row**.

<img src="../ux/screens/160x120i5/home_large_text.png" width="200" alt="Home in large text">
