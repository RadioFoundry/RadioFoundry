<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# UI design principles

These ten principles decide every UI question. Each has rules concrete enough to check, and a **Test** line that names
the check. The automated checks run in the host simulator ([Usability tests](usability-tests.md) 2); the others are
tasks in a moderated session. Code cites the principles as P1 to P10.

Terms used on this page:

| Term | Meaning |
|---|---|
| Home | The idle screen ([Information architecture](information-architecture.md)) |
| Logical action | A radio-independent input (`SELECT`, `BACK`, `QUICK`, and so on) that each board maps to its physical keys |
| Talk target | Where PTT transmits right now: a talkgroup, a private contact, all-call, or an FM channel |
| Press | One short or long press of a key, or one detent of a knob or encoder |

## 1. The main tasks are one or two presses from Home

The top 20 tasks ([Task flows](task-flows.md)) each show their first useful result within two presses from Home, and
finish within their budget (most within four, none above six).

- Home has direct keys for changing the channel, quick settings, actions on the last caller, Find (type), and
  monitor.
- The top-level destinations are few (seven), and each is a noun the user already knows: Contacts, Messages, Recent,
  Zones, Channels, Tools, Settings.
- No task requires going through Settings. Settings is for preferences, not for operating the radio.

**Test:** `uiFlowTest` runs each task flow as a script, counts the presses, and fails the build when a flow exceeds its
budget.

## 2. You always know what PTT does, and whether the radio is talking or listening

- Home always shows the talk target, mode (DMR or FM), channel and zone, timeslot and color code (DMR) or tone (FM),
  power level, and privacy state.
- A status strip at the top of every screen, including menus and lists, shows TX (red, with the word "TX"), RX (green,
  with the word "RX" and the caller), or idle. A menu never hides TX or RX. On the small displays, a screen can merge
  the strip into its title row, but the TX/RX pill stays.
- When the talk target differs from the channel's default (reply window, temporary TG), Home says so in words
  ("Reply → TG 3100", "Temporary TG 91") and says how to go back.
- PTT works from every screen. When the radio can't transmit (out of band, RX-only channel, no codec for DMR, locked),
  PTT shows why and what to do, with an error tone.
- Leaving a screen never changes the channel, the talk target, or the TX settings unless the user chose that.

**Test:** the `uiFlowTest` crawler checks on every screen that the status strip (or the title row that carries the
state pill) is drawn and that PTT shows TX. It also checks that the Transmitting screen names a private call's contact
and tells it apart from a group call.

## 3. Every key means one thing everywhere

- Inputs go through a logical action layer. Screens see only logical actions, never physical keys.
- `SELECT` does or opens the focused thing. `BACK` goes to where the user was. `NAV_UP` and `NAV_DOWN` move focus or
  change the value that Home is tuned to. `QUICK` opens quick settings for the current context. `CONTEXT` opens
  actions for the thing in focus (on Home: the last caller). Digits type (numbers and T9 letters).
- **Long press has one fixed meaning per key across all screens:**

  | Long press | Meaning |
  |---|---|
  | `BACK` | Home |
  | `SELECT` | Save the focused change. Where a radio has no `QUICK` key, long `SELECT` is `QUICK` |
  | `QUICK` (`*`) | Key lock. It locks from Home only, not in a menu mid-task; it unlocks from anywhere |
  | `CONTEXT` (`#`) | The talk target picker on a DMR channel: pick a TG or type an ID. It matches the `#` keycap's legend on many keypad radios |
  | A digit on Home | The speed dial slot |

  Inside a text field, the keypad is a typewriter: a long digit types the digit itself, and a long `*` deletes like a
  short one. Those are the only exceptions. Navigation keys repeat when held. The `USER_n` keys and the side keys
  listed in Settings > Keys are the assignable exception: they carry one function per gesture from the assignable
  list. A board's VFO/MR key has fixed ones (short: channel or VFO; long: Copy to VFO).
- Long press never transmits, never deletes, and never resets. The long-press threshold is one value (default 600 ms,
  settable), and a short tick confirms that the press reached it.
- Nothing requires a double press, a triple press, or a chord (two keys at once). Chords can exist as optional
  shortcuts only.

**Test:** `uiFlowTest` checks on every screen that long `BACK` goes Home, long `QUICK` locks, and long `CONTEXT` opens
the talk target picker on a DMR channel.

## 4. One back stack, no dead ends

- There is one navigation stack. `BACK` pops one level. At Home, `BACK` clears temporary state (dismisses a banner,
  ends a reply window, undoes temporary changes), never something harmful.
- One exception follows the convention of phones and most radio makers: in text entry, `BACK` deletes the last
  character, and leaves (without a change) once the field is empty.
- Hold `BACK` from anywhere to reach Home, in one press, always.
- Menu depth from Home is at most three levels for anything a user does in the field.
- Incoming events (call, message, alert) show a banner that doesn't take focus or keys. A private call addressed to
  the user and an emergency take the screen, and `BACK` still dismisses them.
- Modes (hotspot, network, SFR, scan) show on Home and in the status strip. One obvious press (`BACK` or the key that
  started the mode) leaves them; a power cycle is never the only way out.
- No screen waits for input it doesn't ask for. Every screen shows its way out.

**Test:** the `uiFlowTest` crawler visits every screen reachable from Home and checks that `BACK` leaves it, that long
`BACK` reaches Home, and that field-use screens are at most three levels deep.

## 5. Glanceable: one look, under two seconds

- Home is readable at arm's length in one glance: the channel name and the talk target are the two largest text items.
  Secondary items are smaller and in fixed places.
- Each screen region has one job and always the same position: the status strip at the top, the hint row at the
  bottom, the content in between.
- Easy to read beats one more item. Fonts never shrink and line spacing never squeezes to fit more; a fit problem is
  fixed with shorter text first (an alternate, a dropped name, an abbreviation). `uiSim --layout` fails a display
  class whose list or strip spacing drops below its normal pitch.
- Home has no scrolling text. Long names are shortened in the middle with an ellipsis, not marquee-scrolled.
- Color carries meaning only (TX red, RX green, warning amber, privacy blue). Every color cue also has a word or icon,
  so monochrome and color-blind users get the same information.
- The glance budget follows vehicle HMI guidance: no screen needs a look longer than 2 s to understand, and no common
  task needs more than 12 s of eyes on the screen.

**Test:** `uiSim --layout` checks the spacing on each display class. In a moderated session, the five-second test shows
Home for 2 s and asks "What channel? What does PTT do? Is the battery OK?"; the target is 90% correct answers.

## 6. Find by typing, configure by intent

- On Home, digits start **Find**: one list that matches channels (by number and name), contacts and talkgroups (by ID
  and by name through T9), recent callers, and settings. Typing `3100` offers TG 3100, contact 3100xxx, and channel
  31; typing `7476` offers T9 name matches such as "PIRATE".
- A number that isn't in the codeplug is still usable: type a TG or ID and press PTT (or `SELECT`) to talk to it
  temporarily.
- Settings are grouped by what the user wants (Sound, Display, Talk & listen, Privacy, Location, Connections, Power
  saving, Keys, My radio), never by chip or protocol layer. Find also searches settings.
- Quick settings hold the handful of settings people change in the field, in fixed positions that map to the keypad
  (1-9 as a 3x3 grid).

**Test:** in a moderated session, a participant finds each of 30 named settings in under 15 s without help.

## 7. Temporary first, save on purpose, confirm only what can't be undone

- Changes made while operating (TG, TS, CC, power, squelch, tone, privacy on or off) apply at once and are
  **temporary**: they last until the channel changes or the user returns to the channel's default. Home marks a
  changed channel with a dot, and `BACK` on Home undoes the changes.
- Making a change permanent is a deliberate act: long `SELECT` on Home, or "Save to channel" in `CONTEXT`. The radio
  never writes the codeplug silently.
- Defaults are safe: low power on first boot, and no auto-transmit features (VOX, GPS reports, ARS) until the user
  turns them on. The radio-check reply is on: it answers a check that another station asked for. Privacy is off;
  promiscuous receive is on.
- Only destructive, irreversible actions ask for confirmation. The confirmation names the thing and the consequence
  ("Reset all settings to factory defaults? Channels stay. This can't be undone.") and defaults to No. Factory reset
  has that confirmation; deleting a zone asks "Yes, delete".
- Emergency is hold-to-activate (about 1.5 s) instead of a confirmation dialog, and the same hold cancels it.

**Test:** `uiFlowTest` checks that the flows that only operate the radio never write the codeplug.

## 8. Every press gets an answer

- Every press gets a visual response within 50 ms: focus moves, a value changes, or a "not available here" hint shows.
  A full redraw can take longer, but something changes at once.
- Key tones are on by default and distinct from protocol tones: key click, invalid key, long-press threshold, talk
  permit, channel busy, call alert, message, low battery, and emergency each sound different. Their volume is separate
  from the receive volume.
- A press that does nothing on the current screen gives the invalid tone and a one-line hint ("Hold * to unlock" when
  locked).

## 9. Messages say what happened and what to do

- Every error or refusal says what happened (and why, if known) and what to do, in at most two short lines on the
  smallest display (about 2 x 20 characters). For example, the pill says NO TX, the Activity line "Bands 136-174
  400-480 MHz", and the hint "Pick another channel". A longer explanation goes on an information page or in the
  manual, never in a toast.
- A feature left out of the build says so the same way everywhere: "Scan: not in this build".
- No error codes alone, no "Invalid", no "Error 3". A code can follow the text for support.
- Input limits show before the user reaches them ("TG 1 to 16777215", "Message 0 of 144 characters").
- Success is confirmed quietly (a tone and a brief banner). Failure is never silent ("Radio check: no answer from
  Q1AAA after 3 tries").

## 10. One design for every radio and every user

- Layouts are defined per **display class** (240x320 color, 160x128 color, 128x64 mono), not per radio. A new radio
  picks a class and a key map; it gets no new screens.
- Every logical action is reachable on every supported key set. Radios with few keys get fallbacks (for example, long
  `SELECT` = `QUICK`), listed in the key map.
- The UI works with gloves: no timing-critical presses, no chords required, and large hit tolerance on encoders (one
  detent = one step, no acceleration below 4 detents a second).
- Accessibility is built in, not a setting buried away: large-text mode (Home with two lines: channel and talk
  target), a high-contrast theme, adjustable long-press time, key-repeat rate, and tone volume.
- Text is plain US English and short, with no abbreviations beyond the standard radio ones (TG, TS, CC, ID, FM, DMR,
  GPS, SQL, MON, Priv), and is translated through string tables.

**Wording rules:**

- One term per thing. A talkgroup is "TG" before a number ("TG 91", always with a space) and "talkgroup" in sentences.
  "Contact", "channel", "zone", "talk target", and "hear all" mean the same everywhere.
- Labels start with the action or the noun ("Make channel default", "Key type") and don't repeat the menu they are in
  (Settings > Privacy > "Key", not "Privacy key").
- Hint groups are a key and a verb in its plain form: "OK send", "Back cancel", "# skip". A lone "Back" means Back
  only goes back.
- Values start with a capital ("On", "Off", "Here", "Scanning"). Units have a space where they are words ("30 s",
  "-71 dBm", "5 min") and none in compact radio forms ("5W", "25k", "TS1", "SQ5"). Ages use one style everywhere:
  "11s ago", "4m ago", shortened to "11s".
- No filler, no chat, no marketing. Use "for now" only where it means a temporary change ("Timeslot 2 for now"), and
  "yet" only where something is expected to come ("No calls heard yet"), never as a promise about the firmware ("not
  in this build", not "not in this build yet").

**Test:** `uiFlowTest` runs every task flow on the reference key maps; a task that's impossible on a key map that has
it fails the build.

## Checklist for a new screen

1. Which logical actions does it use, and do they keep their global meanings?
2. Does `BACK` leave it, and does long `BACK` reach Home?
3. Is the status strip visible?
4. What does it look like at 128x64?
5. Does any change it makes write the codeplug? If so, is that deliberate?
6. Does every error say what to do?
7. Does a `uiFlowTest` flow open it and check `screen=<name>`?
