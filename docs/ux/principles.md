<!-- SPDX-License-Identifier: MIT -->
# UI design principles

Goal: some of the best usability of any radio firmware. These ten principles decide every UI question. Each has rules that are concrete enough to check, and a **Test** line that `usability-tests.md` turns into a measurement or an automated check in the SDL simulator.

Words used below:

- **Home**: the idle screen (see `information-architecture.md`).
- **Logical action**: a radio-independent input (`SELECT`, `BACK`, `QUICK`, …) that each board maps to its physical keys.
- **Talk target**: where PTT will transmit right now (a talkgroup, a private contact, all-call, or an FM channel).
- **Press**: one short or long press of a key, or one detent of a knob or encoder.

---

## 1. The main tasks are one or two presses from Home

The top 20 tasks (`task-flows.md`) each show their first useful result within two presses from Home, and finish within their budget (most within four, none above six).

- Home has direct keys for: change channel, quick settings, actions on the last caller, find anything (type), monitor.
- Top-level destinations are few (seven) and each is a noun the user already knows: Contacts, Messages, Recent, Zones, Channels, Tools, Settings.
- No task requires going through Settings. Settings is for preferences, not for operating the radio.

**Test:** scripted flows in the simulator count presses per task and fail the build if a flow exceeds its budget.

## 2. You always know what PTT will do, and whether the radio is talking or listening

- Home always shows the **talk target**, mode (DMR/FM), channel and zone, timeslot and color code (DMR) or tone (FM), power level, and privacy state.
- A **status strip** at the top of every screen, including menus and lists, shows TX (red, with the word "TX"), RX (green, with the word "RX" and the caller), or idle. TX/RX is never hidden by a menu. On the small displays a screen may merge the strip into its title row, but the TX/RX pill stays.
- If the talk target differs from the channel's default (reply window, temporary TG), Home says so in words ("Reply → TG 3100", "Temporary TG 91") and says how to go back.
- PTT works from every screen. If the radio can't transmit (out of band, RX-only channel, no codec for DMR, locked), pressing PTT shows why and what to do, with an error tone.
- Leaving a screen never changes the channel, the talk target or the TX settings unless the user chose that.

**Test:** for every screen in the simulator, a check asserts the status strip is drawn and shows TX within 100 ms of PTT. A list of screens with no status strip must be empty.

## 3. Every key means one thing everywhere

- Inputs go through a **logical action layer**. Screens only see logical actions, never physical keys.
- `SELECT` = do/open the focused thing. `BACK` = go to where I was. `NAV_UP`/`NAV_DOWN` = move focus or change the value that Home is tuned to. `QUICK` = quick settings for the current context. `CONTEXT` = actions for the thing in focus (on Home: the last caller). Digits = type (numbers and T9 letters).
- **Long press has one fixed meaning per key across all screens**: long `BACK` = Home, long `SELECT` = save/confirm the focused change (where a key for `QUICK` is missing, long `SELECT` = `QUICK`), long `QUICK` (`*`) = key lock (from Home only: in a menu it doesn't lock mid-task; it unlocks from anywhere), long `CONTEXT` (`#`) = the talk target picker on a DMR channel (TG pick or a typed ID, the `#` keycap's legend and the AnyTone 868/878/578 convention), long digit on Home = speed-dial slot. Inside a text field the keypad is a typewriter: long digit types the digit itself and long `*` deletes like a short one; those are the only exceptions. Navigation keys repeat when held.
- Long press never transmits, never deletes, never resets. Long-press threshold is one value (default 600 ms, settable), and a short tick confirms the threshold was reached.
- No double presses, no triple presses, no chords (two keys at once) are required for anything. Chords may exist as optional shortcuts only.

**Test:** the key map table in `information-architecture.md` is generated from the firmware's action tables; a lint fails if a screen binds a long press to something other than the key's global meaning.

## 4. One back stack, no dead ends

- There is one navigation stack. `BACK` pops one level; at Home, `BACK` clears temporary state (dismisses a banner, ends a reply window), never something harmful.
- One exception, the convention of phones and most radio makers: in text entry `BACK` deletes the last character, and leaves (without a change) once the field is empty.
- Hold `BACK` from anywhere = Home, in one press, always.
- Menu depth from Home is at most three levels for anything a user does in the field.
- Incoming events (call, message, alert) show a **banner** that doesn't steal focus or keys, except a private call addressed to the user and emergency, which take the screen but are still dismissed with `BACK`.
- Modes (hotspot, network, SFR, scan) are shown on Home and in the status strip and are left with one obvious press (`BACK` or the same key that started them), never only by power cycling.
- No screen waits for input it doesn't ask for. Every screen shows (or speaks, with voice prompts) its way out.

**Test:** a simulator crawler visits every reachable screen and verifies (a) `BACK` leaves it, (b) long `BACK` reaches Home, (c) depth is at most 3 for screens tagged "field use".

## 5. Glanceable: one look, under two seconds

- Home is readable at arm's length in one glance: the channel name and talk target are the two largest text items. Secondary items are smaller and in fixed places.
- Each screen region has one job and always the same position (status strip at top, hint row at bottom, content in between).
- Easy to read beats one more item: fonts are never made smaller and line spacing never squeezed to fit more; a fit problem is fixed with shorter text first (an alternate, a dropped name, an abbreviation). `uiSim --layout` fails a display class whose list or strip spacing drops below its normal pitch.
- No scrolling text on Home. Long names are shortened from the middle with an ellipsis, not marquee-scrolled.
- Color carries meaning only (TX red, RX green, warning amber, privacy blue); every color cue also has a word or icon, so monochrome and color-blind users get the same information.
- Glance budget follows vehicle HMI guidance: no screen requires a look longer than 2 s to understand, and no common task requires more than 12 s of eyes-on-screen.

**Test:** five-second test (show Home for 2 s, ask "what channel, what will PTT do, is the battery OK?"), target 90% correct answers. Render check: Home's channel and talk target text heights are the two largest on each display class.

## 6. Find by typing, configure by intent

- On Home, digits start **Find**: one list that matches channels (by number and name), contacts and talkgroups (by ID and name via T9), recent callers, and settings. Typing `3100` offers TG 3100, contact 3100xxx and channel 31; typing `7476` offers "PIRATE" style T9 name matches.
- A number that isn't in the codeplug is still usable: typing a TG or ID and pressing PTT (or `SELECT`) talks to it temporarily.
- Settings are grouped by what the user wants (Sound, Display, Talk & listen, Privacy, Location, Connections, Power, Keys, My radio), never by chip or protocol layer. Settings are also searchable from Find.
- Quick settings hold the handful of settings people change in the field, in fixed positions that map to the keypad (1-9 as a 3x3 grid).

**Test:** for each of 30 named settings, a participant finds it in under 15 s without help; Find returns the intended item in the top 3 for 95% of a test corpus of queries.

## 7. Temporary first, save on purpose, confirm only what can't be undone

- Changes made while operating (TG, TS, CC, power, squelch, tone, privacy on/off) apply immediately and are **temporary**: they last until the channel changes or the user returns to the channel's default. Home marks a changed channel ("edited" dot and the changed field highlighted).
- Making a change permanent is a deliberate act: long `SELECT` on the changed field, or "Save to channel" in `CONTEXT`. The codeplug is never written silently.
- Defaults are safe: low power on first boot, no auto-transmit features (VOX, GPS reports, ARS, auto radio-check reply) enabled until the user enables them, privacy off, promiscuous on for RX only.
- Reversible actions don't ask; they offer **Undo** for 5 s (delete message, remove contact from a list, clear last heard).
- Only destructive, irreversible actions ask for confirmation (factory reset, erase codeplug, delete a whole zone, overwrite calibration). The confirmation names the thing and the consequence ("Erase all 1,024 channels? This can't be undone.") and defaults to No.
- Emergency is hold-to-activate (about 1.5 s) instead of a confirmation dialog, and cancel is the same hold.

**Test:** count of confirmation dialogs in the firmware is reviewed per release; each must be on the destructive list. Codeplug write counter in the simulator must not change during the "operate only" test script.

## 8. Every press gets an answer

- Visual response to every press within 50 ms (focus moves, value changes, or a "not available here" hint). A full redraw may take longer, but something changes at once.
- Key tones are on by default and distinct from protocol tones: key click, invalid key, long-press threshold, talk permit, channel busy, call alert, message, low battery, emergency each sound different. Volume is separate from the receive volume.
- Optional voice prompts announce what the focus moved to and what changed ("Channel 3, Base repeater", "Power high", "Timeslot 2"), and on request the full Home state ("Speak status", on an assignable key, default long press of the top key, and in the Home `CONTEXT` actions).
- A press that does nothing on the current screen gives the invalid tone and a one-line hint ("Hold * to unlock" when locked).

**Test:** simulator measures press-to-first-pixel latency; on hardware, a high-speed camera test per release. Target p95 below 50 ms, max below 150 ms.

## 9. Messages say what happened and what to do

- Every error or refusal says what happened (and why, if known) and what to do, in at most two short lines at the smallest display (about 2 x 20 characters): the pill says NO TX, the Activity line "Bands 136-174 400-480 MHz", the hint "Pick another channel". A longer explanation goes on an information page or in the manual, never in a toast.
- A feature left out of the build says so the same way everywhere: "Scan: not in this build".
- No error codes alone, no "Invalid", no "Error 3". A code may follow the text for support.
- Input limits are shown before the user hits them ("TG 1 to 16777215", "Message 0 of 144 characters").
- Success is confirmed quietly (a tone and a brief banner), failure is never silent ("Radio check: no answer from N0CALL after 3 tries").

**Test:** a string lint lists all user-facing error strings; each must contain an action verb or a "see" reference. Reviewed per release.

## 10. One design for every radio and every user

- Layouts are defined per **display class** (240x320 color, 160x128 color, 128x64 mono), not per radio. A new radio picks a class and a key map; it gets no new screens.
- Every logical action is reachable on every supported key set. Radios with few keys get fallbacks (for example long `SELECT` = `QUICK`), documented in the key map.
- Works with gloves: no timing-critical presses, no chords required, large hit tolerance on encoders (one detent = one step, no acceleration below 4 detents/s).
- Accessibility is built in, not a setting buried away: voice prompts, large-text mode (Home with two lines: channel and talk target), high-contrast theme, adjustable long-press time, key-repeat rate, tone volume.
- Text is plain US English, short, no abbreviations beyond the standard radio ones (TG, TS, CC, ID, FM, DMR, GPS, SQL, MON, Priv), with translations through string tables.

**Wording rules** (checked in the UI audit of 2026-10-11):

- One term per thing: talkgroup is "TG" before a number ("TG 91", always with a space) and "talkgroup" in sentences; "contact", "channel", "zone", "talk target", "hear all" are used the same everywhere.
- Labels start with the action or the noun ("Make channel default", "Key type"), and don't repeat the menu they are in (Settings > Privacy > "Key", not "Privacy key").
- Hint groups are a key and a verb in its plain form: "OK send", "Back cancel", "# skip". A lone "Back" means Back just goes back.
- Values start with a capital ("On", "Off", "Here", "Scanning"); units have a space where they are words ("30 s", "-71 dBm", "5 min") and none in compact radio forms ("5W", "25k", "TS1", "SQ5"). Ages are one style everywhere: "11s ago", "4m ago", shortened to "11s".
- No filler, no chat, no marketing. "For now" only where it means a temporary change ("Timeslot 2 for now"), "yet" only where something is expected to come ("No calls heard yet"), never as a promise about the firmware ("not in this build", not "not in this build yet").

**Test:** every task flow in `task-flows.md` is run on all four reference key maps in the simulator; a task that's impossible on any map fails the build. Accessibility pass: the top 10 tasks done blindfolded with voice prompts by at least one tester per release.

---

## Checklist for a new screen

1. Which logical actions does it use, and are they the global meanings?
2. Does `BACK` leave it, and does long `BACK` reach Home?
3. Is the status strip visible?
4. What does it look like at 128x64?
5. What does the voice prompt say on entry and on each focus change?
6. Does any change it makes write the codeplug? If so, is that deliberate?
7. Does every error say what to do?
