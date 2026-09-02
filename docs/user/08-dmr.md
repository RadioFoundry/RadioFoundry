<!-- SPDX-License-Identifier: MIT -->

# DMR

DMR voice receive and transmit are verified on the DM-1701. DMR voice needs the codec
([Installing](02-installing.md)) and a DMR ID in the codeplug.

## Terms

| Term | Meaning |
|---|---|
| Talkgroup (TG) | A DMR group number with an optional name |
| Contact | A person: DMR ID, name, callsign |
| Talk target | Where **PTT** goes right now: a talkgroup, a person, or all call |
| Listen list | The talkgroups a channel listens to (the codeplug's RX group list) |
| Recent | Stations heard and calls made, newest first |
| Hear all | Play every talkgroup on this timeslot (promiscuous mode) |
| Timeslot (TS) | One of the two time slots of a DMR channel |
| Color code (CC) | A number from 0 to 15 that a DMR channel uses, like a tone on FM |

## Who you hear

The radio plays a group call when its talkgroup is the talk target's, all call, in the channel's listen list, the
reply target during the reply window (below), or when hear all is on. It plays a private call addressed to your DMR
ID. It shows every other call on the slot as "Other TG: not played" and treats the channel as busy while it lasts.

## Who you talk to

**PTT** sends to, in this order:

1. the talkgroup or person with the focus in a list, once (one-shot);
2. the reply target, while the reply window runs;
3. a talk target chosen for now;
4. the channel's talk target from the codeplug; with none, talkgroup 9.

Home shows the result after "PTT→".

## Choosing a talk target

A talk target has three lifetimes.

| Lifetime | How | Lasts |
|---|---|---|
| Once | Focus a talkgroup or person in any list (Contacts, Recent, Find, the picker, actions) and press **PTT** | This call and its reply window |
| For now | Focus it and press **OK**. The radio shows "Talking to ..." | Until you change the channel or press **Back** on Home |
| Permanent | Focus it and hold **OK**. The radio shows "... is the default now" | Written to the channel |

A permanent talk target must be a contact in the codeplug. A typed number is used for now, and the radio then shows
"Not a stored contact: for now only".

### The talk target picker

Press **SK2** on a DMR channel (or **\***, **8**). The picker lists, in order:

| Row | Tag |
|---|---|
| The channel's talk target | default |
| The last call heard: its talkgroup, or the caller for a private call | reply |
| The talkgroups of the channel's listen list | listen |
| Talkgroups from Recent | recent |
| Type a number... | |

The focus starts on the reply row when there is one. **PTT** talks to the focused row once, **OK** uses it for now,
holding **OK** makes it the channel's default, **#** opens its actions. "Type a number..." opens Find: type the
talkgroup or ID, focus the line you want and press **PTT** or **OK**.

<img src="../ux/screens/160x120i5/talk_target_picker.png" width="200" alt="The talk target picker">

On an FM channel **SK2** shows "Talk target: DMR channels only".

### Typing a talkgroup or ID

On Home, type the number. Find lists "Talk to TG 3100" and "Private call 3100" (with a name when the ID is a contact).
Focus one and press **PTT** for one call, or **OK** for now.

## Replying

After a call the radio played, the reply window opens: **PTT** answers it, to the same talkgroup for a group call or
to the caller for a private call. After your own call, **PTT** goes to the same target again. Home shows "Reply→" with
the target and a countdown.

<img src="../ux/screens/160x120i5/home_reply.png" width="200" alt="Home during the reply window">

The window lasts **Settings > Talk & listen > Reply window** (0 to 30 s; 0 turns it off). When that setting is left at
its start value, the codeplug's hang time applies, or 10 s when the codeplug has none. **Back** on Home ends the
window ("Reply ended").

## Calling back later

On Home, **#** opens the actions for the current or last caller. A digit picks an action.

<img src="../ux/screens/160x120i5/caller_actions.png" width="200" alt="Actions for the last caller">

| # | Action | What it does |
|---|---|---|
| 1 | Talk to | The talkgroup they were heard on (after a private call: them) for now; **PTT** on this row talks to it once |
| 2 | Private call | Them, privately, for now; **PTT** on this row calls them once |
| 3 | Message | Opens the message composer ([Messages](09-messages-and-signaling.md)) |
| 4 | Radio check | Checks that their radio is on and in range |
| 5 | Call alert | Pages their radio |
| 6 | Position | Not built yet |
| 7 | Save as contact | Not built yet: add contacts in qdmr |
| 8 | Speak status | Not built yet |
| 9 | Save changes to channel | Shown only when the channel has temporary changes |

For a talkgroup the actions are: Talk to, Make default here, Message the group.

Without anyone heard yet, **#** shows "No one heard yet: callers show up here".

## Recent

Hold **#** (or **OK**, **3**). Recent lists the last 12 stations heard and calls made, newest first. Calls you made
start with an arrow. Calls to you are highlighted, and private calls show "you" or "priv". Recent is kept in memory
and starts empty after a restart.

<img src="../ux/screens/160x120i5/recent.png" width="200" alt="Recent">

**PTT** talks to the focused entry (the talkgroup a station was heard on), **OK** uses it for now, holding **OK** makes
it permanent, **#** opens its actions.

## Contacts

**OK**, **1** lists the codeplug's contacts and talkgroups. Type letters of the name, or digits of the ID, to filter.
**OK** talks to the focused entry for now, holding **OK** makes it the channel's default, **PTT** talks to it once,
**#** opens its actions.

<img src="../ux/screens/160x120i5/contacts.png" width="200" alt="Contacts">
<img src="../ux/screens/160x120i5/contact_actions.png" width="200" alt="Actions for a contact">

Contacts can't be added or edited on the radio yet. Use qdmr.

## Private calls

- From a contact: **OK**, **1**, find the person, **#**, **2** (Private call), then **PTT**. Or focus the person and
  press **PTT**.
- By ID: type the ID on Home, focus "Private call ..." and press **PTT**.
- To answer a private call, press **PTT** during the reply window.

## Timeslot and color code

On a DMR channel, **\*** opens these tiles:

<img src="../ux/screens/160x120i5/quick_dmr.png" width="200" alt="Quick settings on a DMR channel">

| Key | Tile | What it does |
|---|---|---|
| **1** | Power | Each press: low (50 mW), the default (1 W), high (5 W) |
| **2** | Slot | Switches between timeslot 1 and 2, for now |
| **3** | Color | Opens the color code editor |
| **4** | Mode | Listen and talk in FM on the same frequencies, for now |
| **5** | All | Hear all on or off |
| **6** | Privacy | Privacy on or off |
| **7** | Scan | Scan on or off |
| **8** | Talk to | Opens the talk target picker |
| **9** | Bright | Opens the brightness editor |
| **0** | | Opens Settings |

Color code: **\***, **3**, then **Up** or **Down**, or type the code (0 to 15), and press **OK** to apply it. The new
color code takes effect when the editor closes.

<img src="../ux/screens/160x120i5/quick_cc_editor.png" width="200" alt="Editing the color code">

Timeslot, color code and mode are temporary. They aren't changed while transmitting, and not in the VFO. Hold **OK**
to save them to the channel.

## Hear all

- Hold **SK1**: the radio plays every talkgroup on this frequency and timeslot while you hold it.
- **\***, **5** latches hear all on ("Hear all: every talkgroup"). It stays on until you turn it off, change the
  channel or the mode, or press **Back** on Home.

While hear all is on, the reply window lets you answer any call you heard with **PTT**.

## Privacy

ODR has basic privacy: Motorola basic privacy (keys 1 to 255) and Anytone 16-bit basic privacy. Basic privacy is a
fixed scrambling of the voice. It keeps casual listeners out but isn't encryption. Not tested on air yet.

- The radio plays an encrypted call only when it has the matching key. Without the key the call shows as
  "Encrypted: not played".
- Calls with stronger encryption (they send a PI header) are never played.
- **\***, **6** turns privacy on for your calls ("Privacy on for now"). Home shows "Priv". **Back** on Home turns it
  off again.

Setting the key on the radio: **Settings > Privacy** (**OK**, **6**, **4**).

1. **Privacy key type**: press **OK** until it shows Motorola or Anytone.
2. **Privacy key**: press **OK**, type the key with the number keys (Motorola 1 to 255, Anytone 0 to 65535, in
   decimal), and press **OK** again.

The radio keeps the key across restarts. Setting the type back to Off removes the key and turns privacy off. qdmr
can't write privacy keys yet. Developers can also load a key over USB with the debug tool:

```
tools/odr_debug.py radio privacy moto 5          # Motorola basic privacy, key 5
tools/odr_debug.py radio privacy anytone 4660    # Anytone basic privacy, key 4660
tools/odr_debug.py radio privacy off
```

Without a key, the Privacy tile shows "No privacy key: set one in Settings > Privacy". The keys are stored without
protection in the radio's flash.

## Talker alias

The radio shows the talker alias other radios send, as it arrives, on the receive screen (unless **Settings > Talk &
listen > Show talker alias** is off). It sends your callsign from
the codeplug (8 characters) as your own alias when the channel's talker alias setting in the codeplug asks for it.
