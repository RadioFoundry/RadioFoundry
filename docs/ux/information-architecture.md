<!-- SPDX-License-Identifier: MIT -->
# Information architecture

How the UI is organized: the Home screen, the logical input layer, what each key does in each context, quick settings, the six top-level destinations, settings grouped by intent, and the key maps for the four reference radios. Principles referenced as P1 to P10 are in `principles.md`.

Hardware details below (key labels, knob types, display sizes) are the designer's understanding and must be checked against `docs/spec/capabilities.md` and the board descriptors when those exist. The design does not depend on them: a wrong assumption only changes a row in a key map table.

---

## 1. Concepts the user sees

Keep the number of concepts small and name them the same everywhere.

| Concept | Meaning | Not called |
|---|---|---|
| **Channel** | A saved frequency plus mode and settings (FM or DMR) | memory, slot |
| **Zone** | A named list of channels | bank, group |
| **VFO** | Tune any frequency by hand | frequency mode, manual mode |
| **Talk target** | Where PTT goes right now: a talkgroup, a person, all-call or (FM) the channel | TX contact, contact, call target |
| **Talkgroup (TG)** | A DMR group number with an optional name | group contact |
| **Contact** | A person (DMR ID, name, callsign) | private contact, digital contact |
| **Listen list** | The talkgroups a channel listens to (the codeplug's RX group list) | RX group list (that phrase appears only in "Advanced" and in qdmr) |
| **Recent** | Stations heard and calls made, newest first | last heard, call log (both are filters inside Recent) |
| **Hear all** | Promiscuous monitoring: play every talkgroup on this timeslot | promiscuous (shown as a subtitle for experienced users) |

On-the-fly changes are **temporary** (P7). A temporary change shows a dot next to the changed field on Home and lasts until the channel changes or `BACK` is pressed on Home. Making it **permanent** writes it to the channel.

The talk target has three lifetimes:

1. **One-shot**: press PTT while a talkgroup or contact is focused in any list. Transmits to it now. It stays the talk target for the reply window (default 10 s after the last transmission or reception on it), then Home reverts to the channel default.
2. **Temporary**: `SELECT` on a talkgroup or contact ("Talk to"). Stays until the channel changes or `BACK` on Home.
3. **Permanent**: long `SELECT` on it ("Make default for this channel"). Writes the channel.

Replying to whoever just spoke is automatic during the reply window: if a station calls on TG 91 while the channel default is TG 3100, Home shows "Reply → TG 91 (8 s)" and PTT goes to TG 91 until the countdown ends. The setting "Reply window" (Talk & listen) allows 0 (off) to 30 s.

---

## 2. Logical actions

Screens consume only these. Each board maps physical inputs to them (`boards/<radio>/keymap`). Each action arrives with a gesture: `PRESS` (short, released before the long-press threshold), `LONG` (held past the threshold, default 600 ms, fires once at the threshold with a tick), `REPEAT` (navigation keys only, while held), and for `PTT` and `MONITOR` the raw `DOWN`/`UP`.

| Action | Meaning everywhere | Required? |
|---|---|---|
| `NAV_UP`, `NAV_DOWN` | Move focus; on Home and in value editors, change the value (channel, frequency, level) | yes |
| `NAV_LEFT`, `NAV_RIGHT` | Change the focused value / switch tabs. If absent, screens fall back to `NAV_UP`/`NAV_DOWN` after `SELECT` | optional |
| `SELECT` | Do or open the focused thing. Long: make it permanent (save) | yes |
| `BACK` | Back one level; on Home clear temporary state. Long: Home | yes |
| `QUICK` | Quick settings for the current context. Long: key lock | strongly recommended; fallback long `SELECT` |
| `CONTEXT` | Actions for the focused thing; on Home, the last or current caller. Long: Recent | strongly recommended; fallback: first item in Hub |
| `DIGIT_0` to `DIGIT_9` | Type (numbers, T9 letters). On Home: start Find. In quick settings and Hub: pick item by number. Long on Home: speed-dial slot | optional (keypad radios) |
| `PTT` | Transmit to the talk target (or the focused target, one-shot) | yes |
| `MONITOR` | While held: FM opens squelch; DMR plays all traffic (hear all) | recommended |
| `TALK_TARGET` | Open the talk target picker | assignable |
| `EMERGENCY` | Hold 1.5 s to start or cancel emergency (if enabled in codeplug) | optional |
| `CHANNEL_POS(n)` | Absolute channel selector position (1 to 16) | from knob |
| `ENC_STEP(+/-)` | Encoder detent. Forward (+, clockwise): next channel on Home (as `NAV_UP`), value up in editors, focus **down** in lists and quick tiles (the next row, like a scroll wheel). Back (-): the opposite | from encoder |
| `USER_1` to `USER_4` | Side and top keys; each has a short and a long function from the assignable list | optional |

Assignable functions for `USER_n` keys (short and long, chosen in Settings > Keys or in qdmr): Monitor, Talk target, Quick, Context, Recent, Power cycle, Scan on/off, Hear all on/off, Privacy on/off, Zone picker, Speak status, Backlight, Mode FM/DMR, Lock, Reply, Radio check last caller, Silent (mute tones and prompts), None.

Rules for key maps (P3, P10):

- Every board maps `NAV_UP`, `NAV_DOWN`, `SELECT`, `BACK`, `PTT`. Everything else must be reachable through a fallback on boards that lack it.
- Text keys (`*`, `#`, digits) inside a text field are typing keys: `*` = shift/case/symbols, `#` = space and mode (T9 / multi-tap / digits), long digit = the literal digit. `BACK` deletes a character, long `BACK` still goes Home (after an "unsent text kept as draft" banner).

---

## 3. Home

Home answers four questions at a glance (P2, P5): **Where am I** (zone, channel), **What will PTT do** (talk target, mode, TS/CC or tone, power, privacy), **What is happening** (TX/RX, caller), **Is the radio OK** (battery, signal, GPS, messages, lock).

Regions, top to bottom, the same on every display class (sizes differ):

1. **Status strip**: state pill (TX red / RX green / scan / hotspot / idle), RSSI, battery %, GPS fix, unread messages, lock, privacy. Drawn on every screen.
2. **Place**: zone name (small) and channel number + name (large). In VFO: the frequency (large) and "VFO".
3. **Talk target**: "PTT →" + TG or contact name and number (large), with chips: mode, TS, CC (DMR) or tone and bandwidth (FM), power. Temporary fields have a dot; reply window shows a countdown.
4. **Activity**: idle = the last heard station (name, callsign, TG, time ago). RX = the current caller big (name, callsign, ID, TG, talker alias, RSSI bar, distance if a position is known). TX = timer and target.
5. **Hint row**: what the nearest keys do here, for example `OK Menu · * Quick · # Last caller`. Hidden in large-text mode and after the user turns hints off.

Home keys:

| Input | Short | Long |
|---|---|---|
| `NAV_UP` / `NAV_DOWN`, encoder forward / back | Channel +1 / -1 in zone (VFO: frequency step) | repeat |
| `SELECT` | Open Hub (top-level destinations) | Make temporary changes permanent ("Saved to channel") |
| `BACK` | Clear temporary state: end reply window, revert temporary TG and fields, dismiss banner | Already Home: tick |
| `QUICK` | Quick settings | Lock keys |
| `CONTEXT` | Actions for the last/current caller | Recent |
| Digits | Find (channels, TGs, contacts, settings; in VFO, frequency first) | Speed-dial slot 0-9 |
| `PTT` | Transmit to talk target | - |
| `MONITOR` | Monitor while held | - |

Large-text mode (accessibility, or chosen by the user) shows only the status strip, the channel name and the talk target, each as large as fits.

### Banners and interruptions

- Group call heard: Activity region shows it; no focus change on any screen. Outside Home, the status strip shows `RX TG 91 · K1ABC`.
- Text message: tone, banner "Message from K1ABC: first words…", `CONTEXT` while the banner is visible opens it; the banner goes away after 8 s and the message waits in Messages with an unread count on the status strip.
- Private call to me, call alert, radio check request: banner with tone; Home shows the caller with "PTT to reply". A private call does not take over a screen the user is editing; it shows in the status strip and the reply is one `CONTEXT` away.
- Emergency received: full-screen, alert tone, repeated until acknowledged with `SELECT` or dismissed with `BACK`.

---

## 4. Hub: six top-level destinations

`SELECT` on Home opens the Hub. Items are numbered; digits pick directly. Order is fixed so muscle memory works.

| # | Destination | What is inside |
|---|---|---|
| 1 | **Contacts** | Search field on top. Tabs (or filter chips): Talkgroups, People, Listen lists. Item actions: Talk to, Private call, Message, Radio check, Call alert, Request position, Add to speed dial, Edit |
| 2 | **Messages** | Inbox (unread first), New message, Canned messages, Sent, Drafts |
| 3 | **Recent** | Everyone heard and every call made, newest first, filters: All / Calls to me / Missed / Talkgroups. Item actions are the same as Contacts, plus "Save as contact" and "Show position" |
| 4 | **Channels** | Zone picker, current zone's channels, VFO, Scan lists, Edit channel, New channel from current settings |
| 5 | **Tools** | Scan, Position (GPS), Hotspot, USB network, Single frequency repeater, ID lookup, Signal meter, DTMF, APRS, Registration (ARS), Battery |
| 6 | **Settings** | Grouped by intent (below), with search |

Contacts, Recent and Messages are the "people" side, Channels and Tools the "radio" side, Settings the preferences. Nothing operational lives only in Settings (P1).

---

## 5. Quick settings

`QUICK` opens a 3x3 grid (240x320 and 160x128) or a numbered list (128x64). Positions are fixed; digits 1-9 act directly, matching the keypad layout. Positions that mean the same in FM and DMR are in the same place.

| Key | DMR channel | FM channel |
|---|---|---|
| 1 | Power (each press cycles levels) | Power |
| 2 | Timeslot 1/2 | Squelch level |
| 3 | Color code | Tone (CTCSS/DCS, RX and TX) |
| 4 | Mode → FM | Mode → DMR |
| 5 | Hear all (promiscuous) on/off | Monitor latch on/off |
| 6 | Privacy on/off | VOX on/off |
| 7 | Scan on/off | Scan on/off |
| 8 | Talk target | Bandwidth 12.5/25 kHz |
| 9 | Brightness | Brightness |
| 0 | All settings | All settings |

- Toggle tiles change on one press; cycle tiles (power, timeslot, bandwidth) advance one value per press; value tiles (CC, squelch, tone, brightness) open an inline editor (`NAV_UP`/`NAV_DOWN` or digits, `SELECT` or `QUICK` to close).
- Every change is temporary on a channel (dot on Home). Long `SELECT` on a tile makes it permanent. In VFO, changes apply to the VFO directly.
- Each tile shows its current value. The voice prompt reads "Power, high" etc.
- `BACK` or `QUICK` again closes and returns to where the user was.

Mode tile behavior on a DMR channel with no codec installed: the DMR tiles show "DMR needs the codec (see docs)" instead of failing silently.

---

## 6. Settings by intent

Settings has a search field on top (T9). Groups, in this order:

| Group | Contents |
|---|---|
| **Sound** | Key tones, tone volume, talk permit tone, voice prompts (on/off, speed, verbosity), message tone, call alert tone |
| **Display** | Brightness, backlight timeout, theme (light/dark/high contrast), large text, orientation (240x320 boards), hint row, Home secondary fields |
| **Talk & listen** | Reply window, TX timeout, default squelch, VOX sensitivity and delay, mic gain, hear all default, talker alias show/send, name source (contacts, ID database, alias) |
| **Privacy** | Basic Privacy keys (per channel and per contact), lock behavior (auto-lock, what stays active when locked: PTT, side keys), PIN for settings |
| **Location** | GPS on/off, position format, send position (UDT, LRRP, APRS) and interval, reply to position requests |
| **Connections** | Hotspot defaults, USB network (TFTP codeplug write), single frequency repeater role, ARS registration, radio check auto-reply, call alert answer |
| **Power** | Battery saver, auto power off, low-battery warning level, power level defaults |
| **Keys** | Side/top key functions (short and long), long-press time, key repeat, Up/Down on Home (channel / zone / talk target), speed dial slots |
| **My radio** | DMR ID, callsign, name, talker alias text, radio name, region/band limits, time and date |
| **Advanced** | Calibration view, codeplug info, firmware info, factory reset (confirmed, P7), diagnostics |

Rule: each setting lives in exactly one group, and Find also reaches it by name and synonyms ("promiscuous" finds Hear all; "RX group" finds Listen lists).

---

## 7. Key maps for the reference radios

Notation: `short / long`. "-" means none. All maps also follow the text-field rules in section 2.

### 7.1 Baofeng DM-1701 (and TYT MD-UV380 class): keypad, channel encoder, volume knob, 2 side keys, top key, 160x128 color

| Physical | Logical (short / long) |
|---|---|
| Green Menu key | `SELECT` / save |
| Red Back key | `BACK` / Home |
| Up / Down | `NAV_UP` / `NAV_DOWN` (repeat) |
| `*` | `QUICK` / lock |
| `#` | `CONTEXT` / Recent |
| 0-9 | digits / speed dial |
| Channel encoder detent | `ENC_STEP` (Home: channel; editors: value; lists: focus, forward = down) |
| Upper side key (row 2 D5) and middle side key (row 2 D7): see hardware-dm1701.md 20 | `MONITOR` (while held) on the middle key |
| SK2 (lower side) | `TALK_TARGET` / Scan on/off |
| Top (orange) key | Press: power cycle. Hold 1.5 s: `EMERGENCY` where it is built (hold again to cancel), else speak status |
| PTT | `PTT` |

Notes: the channel knob is a quadrature encoder with detents, not a 16-position switch (hardware-dm1701.md 10.3), so
zones of any size step through it. No `NAV_LEFT`/`NAV_RIGHT`.

### 7.2 Baofeng DM-32UV: keypad, channel encoder, volume knob, side keys, color display

| Physical | Logical |
|---|---|
| Menu / OK | `SELECT` / save |
| Back / Exit | `BACK` / Home |
| Up / Down | `NAV_UP` / `NAV_DOWN` |
| Encoder detent | `ENC_STEP` (Home: channel; editors: value; lists: focus, forward = down) |
| Encoder push (if present) | `SELECT` / save |
| `*` | `QUICK` / lock |
| `#` | `CONTEXT` / Recent |
| 0-9 | digits / speed dial |
| SK1 | `MONITOR` |
| SK2 | `TALK_TARGET` / Scan |
| Top / alarm key (if present) | `EMERGENCY` or assignable |
| PTT | `PTT` |

The encoder has no hidden modes (survey: Yaesu knob complaints). Forward is "next" everywhere: the next channel on
Home (as Up), a higher value in an editor (as Up), the next row down a list (as Down). Lists follow the knob the way a
scroll wheel does; Home and editors follow the number. (Until 2026-10-10 the knob moved list focus up, which read as backward on the radio.) Display resolution comes from the board descriptor; the layouts for its class apply.

### 7.3 Radioddity GD-77: keypad, no encoder, channel knob absent, 128x64 monochrome

| Physical | Logical |
|---|---|
| Green key | `SELECT` / save |
| Red key | `BACK` / Home |
| Up / Down arrows | `NAV_UP` / `NAV_DOWN` |
| Left / Right arrows (if present on the model) | `NAV_LEFT` / `NAV_RIGHT` |
| `*` | `QUICK` / lock |
| `#` | `CONTEXT` / Recent |
| 0-9 | digits / speed dial |
| SK1 | `MONITOR` |
| SK2 | `TALK_TARGET` / Scan |
| Top orange key (if present) | assignable (default Power cycle / Speak status) |
| PTT | `PTT` |

128x64 has no room for a 3x3 grid; quick settings are a numbered list with the same numbers. Home uses two text sizes only.

### 7.4 TYT MD-9600 (mobile): front panel encoder and a few keys, hand mic with keypad, 160x128 color

| Physical | Logical |
|---|---|
| Front encoder detent | `ENC_STEP` |
| Front encoder push (if present) | `SELECT` / save |
| Front Menu / Back keys (or P keys, per board) | `SELECT` / `BACK` |
| Front P1 | `QUICK` / lock |
| Front P2 | `CONTEXT` / Recent |
| Mic Up / Down | `NAV_UP` / `NAV_DOWN` |
| Mic 0-9, `*`, `#` | as on handhelds (`*` = `QUICK`, `#` = `CONTEXT`) |
| Mic function keys (A-D or P1-P4, per mic) | `TALK_TARGET`, `MONITOR`, Scan, Power cycle by default; assignable |
| Mic PTT | `PTT` |

Mobile-specific: while driving, the "driving" profile (Settings > Display, or automatic when the radio is a mobile and the mic is the input) hides the text keyboard (P5, NHTSA lockout: no manual text entry), offers canned messages only, and enlarges Home. Voice prompts are on by default on mobiles.

### 7.5 Few-key radios (example: four front keys and a knob)

| Missing action | Fallback |
|---|---|
| `QUICK` | long `SELECT` (save then moves into `CONTEXT` > "Save to channel") |
| `CONTEXT` | Hub item 0 "Last caller" above Contacts |
| Digits | Find uses a letter wheel (encoder or Up/Down) with recent and frequent items first |
| `TALK_TARGET` | Quick tile 8 |
| `MONITOR` | Quick tile 5 latch |

---

## 8. Screen inventory

| Screen | Reached from | Depth |
|---|---|---|
| Home | power on, long `BACK` | 0 |
| Quick settings | `QUICK` | 1 |
| Talk target picker | `TALK_TARGET`, quick 8, `CONTEXT` > Talk to | 1 |
| Find | digits on Home | 1 |
| Caller actions | `CONTEXT` | 1 |
| Hub | `SELECT` | 1 |
| Contacts / Messages / Recent / Channels / Tools / Settings | Hub | 2 |
| Item actions (contact, message, channel) | from a list | 3 |
| Message composer | Contacts/Recent/Messages actions | 3 |
| Settings group / setting | Settings | 3 (setting editors are inline, not a fourth level) |
| Hotspot, Network, SFR status | Tools | 2, and shown on Home while active |
