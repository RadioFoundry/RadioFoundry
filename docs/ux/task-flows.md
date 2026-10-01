<!-- SPDX-License-Identifier: MIT -->
# Task flows

Step-by-step flows for the 20 most frequent or most important tasks, with press counts per radio class. Keys use the logical names from `information-architecture.md`; the reference mapping is `SELECT` = green/Menu, `BACK` = red/Back, `QUICK` = `*`, `CONTEXT` = `#`, `TALK_TARGET` = SK2, `MONITOR` = SK1.

## Radio classes

| Class | Example radios | Input | Display |
|---|---|---|---|
| **K** | DM-1701, MD-UV380, DM-32UV | keypad, channel knob or encoder, 2 side keys | color |
| **M** | GD-77 | keypad, no knob/encoder, 2 side keys | 128x64 mono |
| **V** | MD-9600 (mobile) | mic keypad + mic function keys, front encoder | color |
| **F** | few-key radios | Up/Down/Select/Back, knob, PTT, no keypad | any |

## Counting rules

- One press = one short or long key press, or one knob/encoder detent. PTT counts as a press.
- "+n" means "plus n navigation presses that depend on list position". Budgets assume the target is in the first 5 items or found by typing.
- Text typing counts each key press (T9 predictive: one press per letter).
- Budget = the target maximum (P1). A flow that needs more is a design bug.

## Caller actions (opened by `CONTEXT` on Home)

Numbered, so a digit picks directly:

1. Talk to (their TG, or them privately if the call was private)
2. Private call
3. Message
4. Radio check
5. Call alert
6. Position (request or show)
7. Save as contact
8. Speak status
9. Save to channel (shown only when the channel has temporary changes)

---

## 1. Change channel

- K: turn the channel knob (an encoder on the DM-1701 and the DM-32: one detent per channel). Or `NAV_UP`/`NAV_DOWN`.
- M, V: `NAV_UP`/`NAV_DOWN` (V: or front encoder).
- By number or name (all keypad classes): type digits or T9 letters on Home, Find shows matches, `SELECT`.
- Feedback: Home redraws the channel name within 50 ms; voice prompt says "Channel 4, County fire".

| | K | M | V | F | Budget |
|---|---|---|---|---|---|
| Next channel | 1 | 1 | 1 | 1 | 1 |
| Channel 12 by number | 3 (`1`,`2`,`SELECT`) | 3 | 3 | 1+n | 3 |

## 2. Change zone

1. `SELECT` (Hub).
2. `4` (Channels). The zone list opens with the current zone focused.
3. `NAV_UP`/`NAV_DOWN` to the zone (or type its first letters).
4. `SELECT`: the radio switches to that zone (on the knob's channel, or channel 1 on encoder radios) and returns to Home. `CONTEXT` on a zone instead opens its channel list to pick a different channel.

Shortcut: type the zone name on Home (Find lists zones too). Assignable side-key function "Zone picker" makes it 1 + n + 1.

| | K | M | V | F | Budget |
|---|---|---|---|---|---|
| Zone 2 positions away | 5 (`SELECT`,`4`,`↓`,`↓`,`SELECT`) | 5 | 5 | 5 | 5 |

## 3. Switch talkgroup: this transmission only, or permanently

One-shot (this transmission and its replies):

1. `TALK_TARGET` (SK2): picker opens. Order: Channel default, Reply to last heard, the channel's listen list, recent talkgroups, speed dials, "Type a number".
2. `NAV_DOWN` to TG 91 (or type `9`,`1`).
3. `PTT`: transmits to TG 91. Home shows "PTT → TG 91 (reply 10 s)" and reverts to the channel default after the reply window.

Temporary (until channel change): step 3 is `SELECT` instead of PTT. Home shows "Temporary TG 91 · Back to revert".

Permanent: step 3 is long `SELECT`. Banner "TG 91 is now the default for County DMR". Revert is done the same way.

Without the side key: `QUICK`, `8` opens the same picker.

TG not in the codeplug: on Home type `3`,`1`,`0`,`0`; Find's first result is "Talk to TG 3100"; `PTT` (one-shot) or `SELECT` (temporary).

| | K | M | V | F | Budget |
|---|---|---|---|---|---|
| One-shot, TG in list (2nd item) | 3 | 3 | 3 | 4 (`SELECT` long → quick, `8`… via list) | 3 |
| One-shot, typed TG 3100 | 5 (`3100`,`PTT`) | 5 | 5 | n/a (picker list only) | 5 |
| Permanent | 3 | 3 | 3 | 4 | 4 |

## 4. Call back the last station heard

During the reply window (default 10 s): just `PTT`. Home already shows "Reply → TG 91" or "Reply → K1ABC (private)".

After the window:

1. `CONTEXT` (`#`): caller actions for the last heard.
2. `1` (Talk to): sets their TG (or them privately) as the temporary talk target; or `PTT` directly with item 1 focused.

Older station: long `CONTEXT` opens Recent, `NAV_DOWN` to the station, `PTT`.

| | K | M | V | F | Budget |
|---|---|---|---|---|---|
| Within reply window | 1 | 1 | 1 | 1 | 1 |
| After window | 2 (`#`,`PTT`) | 2 | 2 | 3 (Hub item 0, `PTT`) | 2 |
| 3rd station in Recent | 4 (long `#`,`↓`,`↓`,`PTT`) | 4 | 4 | 5 | 4 |

## 5. Private call a contact by name or ID

By name:

1. On Home type T9 letters (`5`,`6`,`3` for "Joe"). Find lists Joe Smith K1ABC first.
2. `CONTEXT`: contact actions. `2` (Private call) or simply `PTT` with "Private call" focused.

Shortcut: in Find, focus the contact and press `PTT`: talk target choice for a person is private call, for a TG group call.

By ID: type the ID (`3`,`1`,`0`,`1`,`2`,`3`,`4`); Find shows "Private call 3101234" (name from the ID database if present) and "Talk to TG 3101234"; focus the private call line and `PTT`.

| | K | M | V | F | Budget |
|---|---|---|---|---|---|
| By name (3 letters) | 4 (3 letters, `PTT`) | 4 | 4 | 2+n (Contacts list) | 4 |
| By 7-digit ID | 9 (7 digits, `↓`, `PTT`) | 9 | 9 | n/a | 9 (digits dominate) |

## 6. Send a text message

1. Pick the recipient: `CONTEXT` (last caller) or Find by name, then `CONTEXT`.
2. `3` (Message): the composer opens with Canned messages listed first ("OK", "On my way", "Call me", "QSY TG 91", user-defined), then "Write…".
3. Canned: `NAV_DOWN` to it, `SELECT` = Send. Written: `NAV_DOWN` to "Write…", `SELECT`, type (T9), `SELECT` = Send.
4. Status strip shows "Sending…", then "Delivered" (if confirmed data) or "Sent, no confirmation" with what to do on failure ("No answer from K1ABC. Retry with Select, or Back to keep in Drafts").

Long `BACK` from the composer keeps the text as a draft (P4).

| | K | M | V | F | Budget |
|---|---|---|---|---|---|
| Canned reply to last caller | 4 (`#`,`3`,`↓`,`SELECT`) | 4 | 4 | 5 | 4 |
| Canned to contact by name | 6 (3 letters, `#`,`3`,`SELECT`) | 6 | 6 (driving: canned only) | 3+n | 6 |
| Written, 10 letters | 15 | 15 | locked while driving | n/a | - |

## 7. Read a new message

Banner is up (8 s): `CONTEXT` opens it. Banner gone: the status strip shows an envelope with the count; `SELECT`, `2` opens Messages with the newest unread focused; `SELECT` opens it. In the message: `CONTEXT` = Reply (canned first), Call, Delete (with Undo), Save sender.

| | K | M | V | F | Budget |
|---|---|---|---|---|---|
| While banner visible | 1 | 1 | 1 | 1 (`SELECT` while banner) | 1 |
| Later | 3 | 3 | 3 | 3+n | 3 |

## 8. Radio check

1. Pick the target (last caller: `CONTEXT`; contact: Find, `CONTEXT`).
2. `4` (Radio check). Status strip "Checking K1ABC…"; tone and banner "K1ABC answered (0.8 s)" or "No answer from K1ABC after 3 tries. They may be off, out of range, or on another channel."

| | K | M | V | F | Budget |
|---|---|---|---|---|---|
| Last caller | 2 | 2 | 2 | 3 | 2 |
| Contact by name | 5 | 5 | 5 | 3+n | 5 |

## 9. Adjust volume, squelch and power

- Volume: the volume knob (analog on the reference radios). If the board reports the volume position, Home shows a volume bar for 1 s. No presses in firmware.
- Squelch (FM): `QUICK`, `2`, `NAV_UP`/`NAV_DOWN` (live: hear it change), `QUICK` or `BACK` to close. Temporary; long `SELECT` saves.
- Power: `QUICK`, `1` cycles levels (Low, Mid, High; each press advances, voice "Power high"). `BACK` closes.

| | K | M | V | F | Budget |
|---|---|---|---|---|---|
| Power one step | 3 (`*`,`1`,`BACK`) | 3 | 3 | 4 | 3 |
| Squelch 2 steps | 5 | 5 | 5 | 6 | 5 |

## 10. Enable privacy

DMR channel: `QUICK`, `6` toggles Privacy. If the channel or contact has no key: the tile says "No key for this channel. Add one in Settings > Privacy or in qdmr" and stays off. Home shows a privacy icon and the talk target chip "Private" (blue + icon, P5). Making it permanent: long `SELECT` on the tile.

Per-contact key: privacy follows the talk target automatically when the contact has a key; Home shows the icon whenever the next transmission will be encrypted.

| | K | M | V | F | Budget |
|---|---|---|---|---|---|
| Toggle privacy | 3 | 3 | 3 | 4 | 3 |

## 11. Scan

Start: `QUICK`, `7` (or long SK2). Home shows "Scanning: Zone Local" in the status strip and the channel name updating; when a channel stops the scan, Activity shows it.

While scanning (Home keys): `PTT` talks on the channel that last stopped (or the start channel if none, setting); `NAV_UP`/`NAV_DOWN` skip ahead; `CONTEXT` on a stopped channel gives "Skip channel for now" (nuisance delete), "Stay here" (stop scan on this channel), "Always skip" (edit scan list, permanent). `BACK` stops scanning and returns to the start channel.

| | K | M | V | F | Budget |
|---|---|---|---|---|---|
| Start | 2 (long SK2: 1) | 2 | 2 | 2 | 2 |
| Nuisance skip | 2 (`#`,`1`) | 2 | 2 | 3 | 2 |
| Stop | 1 | 1 | 1 | 1 | 1 |

## 12. Enter a VFO frequency

Enter VFO: `SELECT`, `4`, VFO is the first row in Channels: `SELECT`. (Or type "VFO" in Find.) Home shows the frequency large.

On Home in VFO, digits enter a frequency directly: `4`,`4`,`6`,`0`,`0`,`0` is 446.000 MHz (the decimal point is implied after the band's MHz digits; `*` inserts it explicitly). Preview shows "446.000 MHz" as you type, out-of-band digits are refused as you type with a hint ("UHF 400-480"). `SELECT` tunes. `NAV_UP`/`NAV_DOWN` step by the step size (quick tile, VFO only, replaces tile 8). Mode and tone are quick tiles 4 and 3. "Save as channel" is in `CONTEXT`.

| | K | M | V | F | Budget |
|---|---|---|---|---|---|
| Into VFO | 3 | 3 | 3 | 3 | 3 |
| 446.000 in VFO | 7 (6 digits,`SELECT`) | 7 | 7 | n/a (step with encoder) | 7 |

## 13. Lock and unlock keys

Lock: long `QUICK` (`*`). Banner "Keys locked. Hold * to unlock." PTT, monitor, volume and emergency stay active (configurable). Any other press shows the hint with the invalid tone. Unlock: long `*` again.

| | K | M | V | F | Budget |
|---|---|---|---|---|---|
| Lock / unlock | 1 | 1 | 1 | 1 (Lock on an assignable key; few-key boards ship with one) | 1 |

## 14. Check GPS

Status strip shows a GPS icon: hollow = searching, solid = fix, crossed = off. Details: `SELECT`, `5`, `2` (Tools > Position): fix state, satellites, lat/long, grid locator, altitude, last sent report and next one. `CONTEXT` there: Send position now, Turn GPS off. Position of another station: caller actions `6`.

| | K | M | V | F | Budget |
|---|---|---|---|---|---|
| See fix | 0 (strip) / 3 | 0 / 3 | 0 / 3 | 0 / 2+n | 3 |

## 15. Enter hotspot or USB network mode

1. Connect USB.
2. `SELECT`, `5` (Tools), `3` (Hotspot) or `4` (USB network).
3. `SELECT` = Start. Screen shows link state (host connected, mode, frequency, TS/CC, traffic and the current talker).

Option (Connections): "Start hotspot when a hotspot host connects", default off. When on, a banner says "Hotspot started by host. OK stops it."

While active: `BACK` goes Home; the hotspot keeps running and Home's status strip shows "HOTSPOT" with the traffic. Stop: `SELECT` = Stop on the hotspot screen or `CONTEXT` on Home > "Stop hotspot". PTT on the radio while in hotspot mode explains "Hotspot mode: the radio relays network traffic. Stop hotspot to talk locally."

| | K | M | V | F | Budget |
|---|---|---|---|---|---|
| Start | 4 | 4 | 4 | 3+n | 4 |
| Stop from Home | 2 | 2 | 2 | 3 | 2 |

## 16. See who is talking

No presses: on RX the Activity region shows, in order of what's known: name (contact, else ID database), callsign, talker alias (as it arrives), DMR ID, TG and TS, RSSI, distance and bearing if a position is known. On 128x64, the two most important lines (name or callsign, TG) are shown and `CONTEXT` > Speak status or the RX detail view (`CONTEXT`, then `SELECT` on the header) shows the rest. Outside Home, the status strip shows "RX TG 91 · K1ABC".

| | K | M | V | F | Budget |
|---|---|---|---|---|---|
| Name and TG | 0 | 0 | 0 | 0 | 0 |
| Full details on small screen | 2 | 2 | 2 | 2 | 2 |

## 17. Emergency (if enabled in the codeplug)

Hold `EMERGENCY` (top/orange key) 1.5 s. A rising tone during the hold, then "EMERGENCY" full screen in red with a word label, the radio sends the alarm and (per codeplug) opens the mic. Cancel: hold the same key 1.5 s. If emergency is not configured, the hold gives a hint "Emergency is not set up on this radio" instead of nothing. On radios without a top key it is an assignable long press only, never on `PTT` or a key used for typing.

| | K | M | V | F | Budget |
|---|---|---|---|---|---|
| Start / cancel | 1 (hold) | 1 | 1 (mic key) | 1 if a key is free | 1 |

## 18. Toggle FM / DMR

On a channel: `QUICK`, `4`. Temporary: the channel becomes its other mode with the same frequency (DMR → FM uses the channel's FM settings if it has any, otherwise no tone; FM → DMR uses CC 1 TS 1 and the last talk target, shown with a dot). In VFO it changes the VFO. Home's mode chip changes color and word.

| | K | M | V | F | Budget |
|---|---|---|---|---|---|
| Toggle | 2 | 2 | 2 | 3 | 2 |

## 19. Monitor and hear all

- Monitor (momentary): hold `MONITOR` (SK1). FM: squelch opens. DMR: hear all on this frequency and timeslot (all TGs, any CC with a "CC 3" note).
- Latched: `QUICK`, `5`. Home shows "Hear all" (DMR) or "Monitor" (FM) in the status strip until turned off or the channel changes.
- While hear all is on, every call shows its TG; the reply window lets you answer on it with `PTT`.

| | K | M | V | F | Budget |
|---|---|---|---|---|---|
| Momentary | 1 | 1 | 1 | 2 (quick tile) | 1 |
| Latched | 2 | 2 | 2 | 3 | 2 |

## 20. Change a channel's timeslot or color code on the fly

- Timeslot: `QUICK`, `2` (toggles 1/2), `BACK`.
- Color code: `QUICK`, `3`, type the code (`7`) or `NAV_UP`/`NAV_DOWN`, `SELECT`.
- Both temporary with a dot on Home; long `SELECT` in quick settings or `CONTEXT` > 9 on Home saves to the channel.
- Hearing traffic on the other timeslot or CC with hear all on shows a hint "Heard on TS2 CC3. Press # to switch" (`CONTEXT` > "Use TS2 CC3").

| | K | M | V | F | Budget |
|---|---|---|---|---|---|
| Timeslot | 3 | 3 | 3 | 4 | 3 |
| Color code 7 | 4 | 4 | 4 | 5+n | 4 |
| Save both to channel | +1 (long `SELECT`) | +1 | +1 | +2 | +1 |

---

## Summary

| Task | K | Budget met? |
|---|---|---|
| Change channel | 1 | yes |
| Change zone | 5 | yes |
| TG one-shot / permanent | 3 / 3 | yes |
| Reply to last heard | 1-2 | yes |
| Private call by name | 4 | yes |
| Canned text reply | 4 | yes |
| Read new message | 1-3 | yes |
| Radio check last caller | 2 | yes |
| Power / squelch | 3 / 5 | yes |
| Privacy | 3 | yes |
| Scan start / stop | 2 / 1 | yes |
| VFO 446.000 | 7 | yes |
| Lock | 1 | yes |
| GPS | 0-3 | yes |
| Hotspot start / stop | 4 / 2 | yes |
| Who is talking | 0 | yes |
| Emergency | 1 hold | yes |
| FM / DMR | 2 | yes |
| Monitor / hear all | 1 / 2 | yes |
| TS / CC | 3 / 4 | yes |

Every task except typed entries (IDs, frequencies, free text) completes in six presses or fewer, inside the NHTSA six-input guideline.
