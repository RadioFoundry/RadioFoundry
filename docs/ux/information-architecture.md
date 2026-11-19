<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Information architecture

This page describes how the UI is organized: the concepts, the logical input layer, Home, the Hub, quick settings,
the settings groups, the key sets, and the screen inventory. P1 to P10 are the [design principles](principles.md). For
which radio has which keys, see [Radio reference, Key roles](../user/23-radios.md#key-roles).

## 1. Concepts the user sees

The UI keeps the number of concepts small and names each the same everywhere.

| Concept | Meaning | Not called |
|---|---|---|
| **Channel** | A saved frequency plus mode and settings (FM or DMR) | memory, slot |
| **Zone** | A named list of channels | bank, group |
| **VFO** | Tune any frequency by hand | frequency mode, manual mode |
| **Talk target** | Where PTT goes right now: a talkgroup, a person, all-call, or (FM) the channel | TX contact, contact, call target |
| **Talkgroup (TG)** | A DMR group number with an optional name | group contact |
| **Contact** | A person (DMR ID, name, callsign) | private contact, digital contact |
| **Listen list** | The talkgroups a channel listens to (the codeplug's RX group list) | RX group list (that phrase appears only in Advanced and in qdmr) |
| **Recent** | Stations heard and calls made, newest first | last heard, call log |
| **Digital monitor** | Promiscuous monitoring: play every talkgroup on this timeslot ("hear all" in the code) | promiscuous, hear all |

On-the-fly changes are **temporary** (P7). A temporary change shows a dot next to the changed field on Home and lasts
until the channel changes or the user presses `BACK` on Home. Making it **permanent** writes it to the channel.

The talk target has three lifetimes:

1. **One-shot**: press PTT while a talkgroup or contact is focused in any list. The radio transmits to it now. It stays
   the talk target for the reply window (default 10 s after the last transmission or reception on it); then Home
   reverts to the channel default.
2. **Temporary**: `SELECT` on a talkgroup or contact ("Talk to"). It stays until the channel changes or the user
   presses `BACK` on Home.
3. **Permanent**: long `SELECT` on it ("Make channel default"). It writes the channel.

Replying to whoever spoke last is automatic during the reply window. If a station calls on TG 91 while the channel
default is TG 3100, Home shows "Reply → TG 91 (8 s)", and PTT goes to TG 91 until the countdown ends. The "Reply
window" setting (Talk & listen) takes 0 (off) to 30 s.

## 2. Logical actions

Screens use only these actions. Each board maps its physical inputs to them. Each action arrives with a gesture:

| Gesture | Meaning |
|---|---|
| `PRESS` | Short: released before the long-press threshold |
| `LONG` | Held past the threshold (default 600 ms); fires once at the threshold, with a tick |
| `REPEAT` | Navigation keys only, while held |
| `DOWN`, `UP` | The raw edges, for `PTT` and `MONITOR` |

| Action | Meaning everywhere | Required? |
|---|---|---|
| `NAV_UP`, `NAV_DOWN` | Move focus; on Home and in value editors, change the value (channel, frequency, level) | Yes |
| `NAV_LEFT`, `NAV_RIGHT` | Change the focused value or switch tabs. Without them, screens fall back to `NAV_UP` and `NAV_DOWN` after `SELECT` | Optional |
| `SELECT` | Do or open the focused thing. Long: make it permanent (save) | Yes |
| `BACK` | Back one level; on Home, clear temporary state. Long: Home | Yes |
| `QUICK` | Quick settings for the current context. Long: key lock (on Home) | Strongly recommended; fallback: long `SELECT` |
| `CONTEXT` | Actions for the focused thing; on Home, the last or current caller. Long: the talk target picker on a DMR channel | Strongly recommended; fallback: Hub item 0 |
| `DIGIT_0` to `DIGIT_9` | Type (numbers, T9 letters). On Home: start Find. In quick settings and the Hub: pick an item by number. Long on Home: speed dial slot | Optional (keypad radios) |
| `PTT` | Transmit to the talk target (or the focused target, one-shot) | Yes |
| `MONITOR` | FM: squelch open while held. DMR: digital monitor (hear all) on or off on each press | Recommended |
| `TALK_TARGET` | Open the talk target picker | Assignable |
| `EMERGENCY` | Hold 1.5 s to start or cancel an emergency (when the codeplug enables it) | Optional |
| `CHANNEL_POS(n)` | Absolute channel selector position (1 to 16) | From a channel switch |
| `ENC_STEP(+/-)` | Encoder detent. Forward (+, clockwise): the next channel on Home (as `NAV_UP`), a higher value in editors, focus **down** in lists and quick tiles (the next row, like a scroll wheel). Back (-): the opposite | From an encoder |
| `USER_1` to `USER_4` | Programmable keys; each has a short and a long function from the assignable list | Optional |

Users choose the functions of assignable keys in Settings > Keys or in qdmr. For the list, see
[Settings, Keys](../user/17-settings.md#keys-key-8).

Rules for key maps (P3, P10):

- Every board maps `NAV_UP`, `NAV_DOWN`, `SELECT`, `BACK`, and `PTT`. On a board without one of the others, a fallback
  reaches it.
- Inside a text field, the text keys (`*`, `#`, digits) are typing keys: `*` is shift, case, and symbols; `#` is space
  and mode (T9, multi-tap, digits); a long digit is the literal digit. `BACK` deletes a character, and long `BACK`
  still goes Home.

## 3. Home

Home answers four questions at a glance (P2, P5):

| Question | Answer on Home |
|---|---|
| Where am I? | Zone, channel |
| What does PTT do? | Talk target, mode, TS and CC or tone, power, privacy |
| What is happening? | TX or RX, the caller |
| Is the radio OK? | Battery, signal, GPS, messages, lock |

The regions, top to bottom, are the same on every display class (sizes differ):

1. **Status strip**: the state pill (TX red, RX green, scan, hotspot, idle), the USB network word (IP, with link
   arrows once the computer has the link up), RSSI, battery %, GPS fix, unread messages, lock, privacy. It's drawn on
   every screen. On small displays, a page's title takes the strip's place, and only Home shows the battery.
2. **Place**: the zone name (small) and the channel number and name (large). In the VFO: the frequency (large) and
   "VFO".
3. **Talk target**: "PTT →" and the TG or contact name and number (large), with chips: mode, TS, CC (DMR) or tone and
   bandwidth (FM), power. Temporary fields have a dot; the reply window shows a countdown.
4. **Activity**: when idle, the last heard station (name, callsign, TG, time ago). During RX, the current caller, large
   (name, callsign, ID, TG, talker alias, RSSI bar, distance if a position is known). During TX, the timer and target.
5. **Hint row**: what the nearest keys do here, for example `OK Menu · * Quick · # Last caller`. Large-text mode hides
   it, and so does the Hint row setting.

Home keys:

| Input | Short | Long |
|---|---|---|
| `NAV_UP` / `NAV_DOWN`, encoder forward / back | Channel +1 / -1 in the zone (VFO: frequency step) | Repeat |
| `SELECT` | Open the Hub | Make temporary changes permanent; on a board without `QUICK`, quick settings |
| `BACK` | Clear temporary state: end the reply window, revert the temporary TG and fields, dismiss a banner | Already Home: tick |
| `QUICK` | Quick settings | Lock keys |
| `CONTEXT` | Actions for the last or current caller | Talk target picker (DMR channel) |
| Digits | Find (channels, TGs, contacts, settings; in the VFO, frequency first) | Speed dial slot 0-9 |
| `PTT` | Transmit to the talk target | - |
| `MONITOR` | Monitor while held | - |

Large-text mode (Settings > Display) shows only the status strip, the channel name, and the talk target, each as large
as fits.

### Banners and interruptions

| Event | What the UI does |
|---|---|
| Group call heard | The Activity region shows it; no screen changes focus. Outside Home, the status strip shows `RX TG 91 · Q1AAA` |
| Text message | A tone and the banner "Message from Q1AAA: first words…". `CONTEXT` while the banner shows opens the message. The banner goes away after 8 s; the message waits in Messages, with an unread count on the status strip |
| Private call to the user, call alert, radio check request | A banner with a tone; Home shows the caller with "PTT to reply". A private call doesn't take over a screen the user is editing: it shows in the status strip, and the reply is one `CONTEXT` away |
| Zone or channel share offered | A banner with a tone. It has no timer and stays until `CONTEXT` opens the offer, `BACK` hides it (on any screen), or the offer ends. The offer also waits under Tools |
| Emergency received | Full screen and an alert tone, repeated until `SELECT` acknowledges it or `BACK` dismisses it |

## 4. Hub: the top-level destinations

`SELECT` on Home opens the Hub. Items are numbered, and digits pick directly. The order is fixed, so muscle memory
works.

| # | Destination | What's inside |
|---|---|---|
| 0 | **Last caller** | Only on radios without a `CONTEXT` key: the caller actions (7.5) |
| 1 | **Contacts** | Contacts and talkgroups, with New contact. Item actions: Talk to, Private call, Message, Radio check, Call alert, Make channel default |
| 2 | **Messages** | Received messages, newest first, with Write... (and New APRS message where APRS is built). A message offers replies, Call, Delete, and Full message for a long text |
| 3 | **Recent** | Everyone heard and every call made, newest first. Item actions as in Contacts, plus Save as contact |
| 4 | **Zones** | The zone picker, the VFO, and the zones. A zone's list is also its editor: Add channel first, Zone options (Rename, Copy as new zone, Share zone to..., Delete zone) last, and Channel actions on a channel (Move up, Move down, Copy here, Edit channel, Share channel to..., Remove from zone; the Share rows with zone sharing) |
| 5 | **Channels** | Every channel by number, with New from current (a copy of the channel in use or the VFO) first; a channel's settings, edited in place and saved ([ui.md](../spec/ui.md) 2.1) |
| 6 | **Tools** | 1 Scan, 2 Position, 3 Hotspot, 4 USB network, 5 Single-freq repeater, 6 Roaming, 7 Diagnostics, 8 APRS, 9 Battery, 0 Zones/Chs Received |
| 7 | **Settings** | Grouped by intent (section 6) |

Each Tools item keeps its number on every radio and build, so a key path such as `SELECT`, `6`, `7` (Diagnostics) is
the same everywhere. An item that a radio might not have goes last (Battery, 9, on a mobile), so commonly used items
have no gaps.

Contacts, Messages, and Recent are the "people" side; Zones, Channels, and Tools the "radio" side; Settings the
preferences. Nothing operational lives only in Settings (P1).

## 5. Quick settings

`QUICK` opens a 3x3 grid (240x320 and 160x128) or a numbered list (128x64). Positions are fixed, and digits 1-9 act
directly, matching the keypad layout. Positions that mean the same in FM and DMR are in the same place.

| Key | DMR channel | FM channel |
|---|---|---|
| 1 | Power (each press cycles the levels) | Power |
| 2 | Timeslot 1/2 | Squelch level |
| 3 | Color code | Tone (CTCSS/DCS, RX and TX) |
| 4 | Copy to VFO (copy the channel to the VFO and open it) | Copy to VFO |
| 5 | Digital monitor (hear all) on/off | Monitor latch on/off (TX freq in the VFO) |
| 6 | Privacy on/off (TX freq in the VFO) | VOX on/off |
| 7 | Dual watch on/off (a zone scan is in Tools > Scan) | Dual watch on/off |
| 8 | Talk target | Bandwidth 12.5/25 kHz |
| 9 | Brightness | Brightness |
| 0 | All settings | All settings |

- Toggle tiles change on one press. Cycle tiles (power, timeslot, bandwidth) advance one value per press. Value tiles
  (CC, squelch, tone, brightness) open an inline editor (`NAV_UP` and `NAV_DOWN` or digits; `SELECT` or `QUICK`
  closes it).
- On a channel, every change is temporary (a dot on Home). Long `SELECT` on Home makes the changes permanent. In the
  VFO, changes apply to the VFO directly.
- Tile 4 depends on where the user is. On a channel, it is "Copy to VFO": the channel's settings in effect now
  (temporary changes included) become the VFO's own, and the VFO opens ("Copied to VFO"). The channel is unchanged,
  and a toast refuses a channel the VFO can't hold. In the VFO, tile 4 is Mode (FM or DMR), kept like the VFO's other
  settings. A channel has no temporary mode change.
- In the VFO, the tile that is Monitor on FM (5) and Privacy on DMR (6) is TX freq ("TXFreq" on a tile): the VFO's
  split, its TX frequency against RX ("Off", "+600k", "-5M"). Up and Down walk a line out from Off at once (Up +,
  Down -) through the band's usual offsets, most common first, stopping at the ends: 2 m 600 kHz, then 1 MHz;
  1.25 m 1.6 MHz; 70 cm 5, 7.6, then 1.6 MHz, from the ARRL, IARU Region 1, RSGB, and WIA band plans. The walk skips
  a TX outside RX's transmit band; a typed split goes to the first offset in its direction. While the tile edits, the
  hint row says what Up, Down, and the digits do; while digits are typed, it shows the whole TX frequency so far
  ("TX 147.02___"; the tile says "TX"). Any other split is typed: digits type a TX frequency, OK sets it (refused
  outside the transmit bands: "Can't transmit there"), and BACK takes a digit back. Monitor stays on its key; privacy
  is for channels.
- Home shows a split VFO's TX frequency: on FM after "PTT →"; on DMR in the top row ("VFO  TX 147.120"), or as the
  offset among the chips on displays without that row. While transmitting, the large frequency is the TX frequency,
  in the TX color; the DMR call screen has a "TX 147.120 MHz" line under the target.
- Each tile shows its current value.
- `BACK`, or `QUICK` again, closes quick settings and returns to where the user was.

On a DMR channel with no codec installed (any DMR tile, and the VFO's Mode tile switching to DMR), the DMR tiles show
"DMR needs the codec (see docs)" instead of failing silently.

## 6. Settings by intent

Settings groups follow what the user wants to do, never the chip or the protocol layer. The groups, in order:

Sound, Display, Talk & listen, Privacy, Location, Connections, Power saving, Keys, Key lock, My radio, Advanced.

Each setting lives in exactly one group, and Find also reaches it by name. Factory reset, under Advanced, is the one
setting that asks for confirmation (P7). For what each group holds, see [Settings](../user/17-settings.md).

## 7. Key sets

Each subsection is a key set that boards share. The notation is `short / long`; "-" means none. Every key set also
follows the text-field rules in section 2. For which radio has which key set and what its keys are labeled, see
[Radio reference, Key roles](../user/23-radios.md#key-roles).

### 7.1 Keypad, channel encoder, and three side keys (160x128 color)

| Physical | Logical (short / long) |
|---|---|
| Green Menu key | `SELECT` / save |
| Red Back key | `BACK` / Home |
| Up / Down | `NAV_UP` / `NAV_DOWN` (repeat) |
| `*` | `QUICK` / lock |
| `#` | `CONTEXT` / talk target picker |
| 0-9 | Digits / speed dial |
| Channel encoder detent | `ENC_STEP` (Home: channel; editors: value; lists: focus, forward = down) |
| SK1 (top side key) | `MONITOR` (while held) |
| SK2 | `AB` (dual watch A / B) / Dual watch on/off |
| SK3 (lowest side key) | Press: power cycle. Hold 1.5 s: `EMERGENCY` where it's built (hold again to cancel) |
| P1, P2 | Previous zone, Next zone |
| PTT | `PTT` |

The channel knob is a quadrature encoder with detents, not a 16-position switch, so zones of any size step through
it. This key set has no `NAV_LEFT` or `NAV_RIGHT`.

### 7.2 Keypad and channel encoder (color)

| Physical | Logical |
|---|---|
| Menu / OK | `SELECT` / save |
| Back / Exit | `BACK` / Home |
| Up / Down | `NAV_UP` / `NAV_DOWN` |
| Encoder detent | `ENC_STEP` (Home: channel; editors: value; lists: focus, forward = down) |
| Encoder push (if present) | `SELECT` / save |
| `*` | `QUICK` / lock |
| `#` | `CONTEXT` / talk target picker |
| 0-9 | Digits / speed dial |
| SK1 | `MONITOR` |
| SK2 | `AB` / Dual watch |
| Top or alarm key (if present) | `EMERGENCY` or assignable |
| PTT | `PTT` |

The encoder has no hidden modes ([survey](survey.md): knob complaints). Forward is "next" everywhere: the next channel
on Home (as Up), a higher value in an editor (as Up), the next row down a list (as Down). Lists follow the knob the way
a scroll wheel does; Home and editors follow the number. The display resolution comes from the board descriptor, and
the layouts for its class apply.

### 7.3 Keypad, no encoder (128x64 monochrome)

| Physical | Logical |
|---|---|
| Green key | `SELECT` / save |
| Red key | `BACK` / Home |
| Up / Down arrows | `NAV_UP` / `NAV_DOWN` |
| Left / Right arrows (if present) | `NAV_LEFT` / `NAV_RIGHT` |
| `*` | `QUICK` / lock |
| `#` | `CONTEXT` / talk target picker |
| 0-9 | Digits / speed dial |
| SK1 | `MONITOR` |
| SK2 | `AB` / Dual watch |
| Top key (if present) | Assignable (default: power cycle) |
| PTT | `PTT` |

A 128x64 display has no room for a 3x3 grid: quick settings are a numbered list with the same numbers. Home uses two
text sizes only.

### 7.4 Mobile front panel: keys and encoder, no keypad (128x64 monochrome)

The front panel has Enter (green), Back (red), Up, Down, P1-P4, an orange key, the power key, the channel encoder, and
a volume knob in the audio path. It has no `*`, `#`, or digit keys.

| Physical | Logical (short / long) |
|---|---|
| Enter (green) | `SELECT` / save |
| Back (red) | `BACK` / Home |
| Up / Down | `NAV_UP` / `NAV_DOWN` (repeat) |
| Channel encoder detent | `ENC_STEP` |
| P1 | `QUICK` / lock; hints call it "P1" |
| P2 | `CONTEXT` / talk target picker; hints call it "P2" |
| P3 | `MONITOR` (while held); assignable |
| P4 | `TALK_TARGET` / `USER_2`; assignable |
| Orange key | `USER_3`; assignable |
| Power key | Power on and off (standby); not in the key map |
| Microphone PTT | `PTT` |
| Microphone Up / Down | `NAV_UP` / `NAV_DOWN` |

Without digit keys, text entry uses Up and Down ([ui.md](../spec/ui.md) 2.1), and hints that start with a digit are
left out.

### 7.5 Few-key radios (example: four front keys and a knob)

| Missing action | Fallback |
|---|---|
| `QUICK` | Long `SELECT` on Home. Saving then moves to `CONTEXT` > "Save to channel" |
| `CONTEXT` | Hub item 0, "Last caller", above Contacts. In Zones and a zone's list, long `SELECT` opens what `CONTEXT` would (the zone's list, Channel actions) |
| `TALK_TARGET` | Quick tile 8 |
| `MONITOR` | Quick tile 5 latch |

### 7.6 Keypad with direction keys, VFO/MR, and an SOS key, no encoder (128x64 monochrome)

| Physical | Logical (short / long) |
|---|---|
| Menu | `SELECT` / save |
| Back | `BACK` / Home |
| Up / Down | `NAV_UP` / `NAV_DOWN` (repeat) |
| Left / Right | `NAV_LEFT` / `NAV_RIGHT`; long: `USER_1` / `USER_4` |
| VFO/MR | `VFO_TOGGLE` (switch between the channel and the VFO) / `COPY_VFO` (copy the channel to the VFO) |
| A/B | `ZONE_UP` |
| `*` | `QUICK` / lock |
| `#` | `CONTEXT` / talk target picker |
| 0-9 | Digits / speed dial |
| SK1 | `MONITOR` (while held) |
| SK2 | `AB` / `USER_2` |
| SOS (top) | Hold: `EMERGENCY` where it's built |
| PTT | `PTT` |

The layouts are the 128x64 ones of section 7.3.

## 8. Screen inventory

| Screen | Reached from | Depth |
|---|---|---|
| Home | Power on, long `BACK` | 0 |
| Quick settings | `QUICK` | 1 |
| Talk target picker | `TALK_TARGET`, quick 8, long `CONTEXT`, `CONTEXT` > Talk to | 1 |
| Find | Digits on Home | 1 |
| Caller actions | `CONTEXT` | 1 |
| Hub | `SELECT` | 1 |
| Contacts, Messages, Recent, Zones, Channels, Tools, Settings | Hub | 2 |
| Item actions (contact, message, channel) | A list | 3 |
| A zone's list (channels, Add channel, Zone options) | `CONTEXT` in Zones | 3 |
| Channel actions, Add channel picker, Zone options | A zone's list | 4 |
| Copy here (the channel editor), Edit channel | Channel actions (replaces it) | 4 |
| Rename (text entry), Delete zone (confirmation) | Zone options | 5 |
| Message composer | Contacts, Recent, or Messages actions | 3 |
| Settings group, setting | Settings | 3 (setting editors are inline, not a fourth level) |
| Hotspot, Network, SFR status | Tools | 2, and shown on Home while active |
