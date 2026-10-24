<!-- SPDX-License-Identifier: MIT -->

# The user interface

How `core/ui` implements the first-principles UX in `docs/ux` (`principles.md`, `information-architecture.md`,
`task-flows.md`, `wireframes/`, `usability-tests.md`). The UX documents say what the radio does; this page says how
the code is laid out, who owns the data behind each screen, and how the UI is tested without a radio.

| Contents | |
|---|---|
| 1, 2 | File layout, screens and the stack |
| 3 | Key meanings and side key assignment |
| 4, 5, 6 | Data other packages own, build switches, debug link |
| 7, 8 | Host simulator and tests, decisions and open points |

## 1. Layout

| File | What |
|---|---|
| `gfx.c`, `fonts.c`, `fmt.c` | drawing in a viewport inside the display's safe-area insets, fonts, number / text formatting |
| `ui_screens.c` | drawing only: status strip, title, hint row, lists, Home, call screens, Quick tiles, banners, emergency |
| `ui_app.c` | behavior without the RTOS: the navigation stack, global keys, Home and Quick keys, filters, Recent, events, banners |
| `ui_menus.c` | every list screen and the screen table `ui_scr[]`: Hub, Actions, Compose, Contacts, Messages, Recent, Zones, Zone, the zone editor's (2.2), Channels, Channel, Tools, Settings, Picker, Info, Confirm, Scan, Roaming |
| `ui_find.c` | type-to-find (T9 word starts, numbers, channel positions) |
| `ui_svc.h`, `ui_svc_stub.c` | the thin interface to data other packages own, with weak "not in this build" stubs (section 4) |
| `ui_svc_radio.c`, `ui_svc_data.c`, `ui_svc_storage.c` | the strong implementations on the radio core, the DMR data service and the settings store / codeplug editing |
| `ui.c` | the UI task: splash (the mark, beside the name on a panel too short to stack them (128x64), the name, the two CPS boot text lines from the codeplug, board and version; `ui_draw_splash` in ui_screens.c), 50 ms frame pacing, backlight, status copy for the debug link |

`ui_app.c` and `ui_menus.c` never call the RTOS. The firmware drives them from `ui.c`; the host simulator
(section 7) drives the same files.

Spacing on the displays under 200 px high (`UI_TALL_PX`): the status strip and, from 100 px high, each list row take
one pixel more than the text cell, so lines never touch (easy to read comes before one more row; principles.md 5).
The board's safe-area insets (`hal_display_t.inset_*`) are outside the drawing area (`gfx_width` / `gfx_height`),
for panel edges hidden by the case (for example 5 px left and right under a lens). At 160x120i5 a list shows 7 rows, at 160x128 7, at
240x320i10x5 11, at 128x64 5. The 240 px class keeps 6 px between rows and a separate title row;
`uiSim --layout` fails a class whose row pitch drops below these. A narrow portrait panel (120x160) keeps the 128 px
class's small font (24 characters a line) and takes the 160 px class's channel and talk target fonts. Home's Activity region shows the last caller on two lines
(name, then "TG 91 11s ago") where it has the rows for them, else one line where the name gives way first; a single
line ("No calls heard yet") is centered in the region. Text that doesn't fit is shortened (an alternate, the name
dropped, then an ellipsis), never drawn in a smaller font.

The hint row is groups separated by two spaces (`"OK open  # more  Back"`). The group for the Back key (it starts with
"Back") is drawn against the right edge, under the key; the other groups run from the left, and those that don't fit
are left out whole from the right. A toast is one centered line instead. A banner or a two-line toast starts in a gap
between lines of the screen under it (a row all one color), never through a line of text.

Text is shortened with a middle ellipsis only as a last resort (P5 allows it for names on Home). Before that:

- A list row keeps its label whole and gives the value the room left: values carry text alternates (`font.h`) down to
  a short form or nothing (Hub's zone name, Find's kind, Zones' "Here, 16 channels" / "Here, 16 ch" / "Here", ages: "11s ago" / "11s").
  A row marked `UI_ITEM_PREVIEW` (a message) is cut at its end, at a word.
- Hints and toasts take the longest alternate that fits; a toast that still doesn't fit takes a second line above the
  hint row. Banners give the title and the text two lines each ("Call alert from" / "Jodi Ruiz"), and a message
  banner shows two lines of the message from the inbox.
- Home's talk target line (DMR target or FM frequency) drops to the small font as a whole, then takes the target's
  short form; a frequency is never shortened. Large-text Home picks the largest font each line fits in, and takes the
  short form before it would go below the medium font. The short form of a talkgroup is its number ("TG 91 World"
  becomes "TG 91"); of a private contact, its name ("Joe Smith 3101234" becomes "Joe Smith"). A contact's ID alone
  says nothing at a glance, so it is what goes. An ID without a contact is shown as the ID.
- The RX and TX screens put the number in the title ("TG 91") and the talkgroup's or contact's name on a line of its
  own; a talker alias takes two lines when it needs them. A private call's title says "Private" where a group call's
  says "TG": "Private 3101234" while transmitting (just "Private" where that doesn't fit), "Private to you" or
  "Private to Joe Smith" while receiving. The TX screen's target line follows the short form rule above, and its
  detail line ends in "Private" or "Group". The word is the cue, not a color: the privacy (encryption) marker is
  "Priv" in blue, and a private call is not encrypted.

`uiSim --ellipsis` (section 7) lists what is still shortened; `uiSim --layout` fails the build on a middle ellipsis
with ordinary content.

Core code never includes a board header and never tests a radio define. What differs by radio comes from the board
descriptor: the display geometry and insets (`hal_display_t`), and the key map (`board.keymap`), which turns keys
into logical actions (`hal_input.h`).

## 2. Screens and the stack

The stack holds up to 6 frames; Home is frame 0. Each screen is a `ui_scr_def_t` (`ui_nav.h`): a name (the debug
link's `ui.screen`), flags, and callbacks for list rows, SELECT, CONTEXT, the talk target of the focused row, a title,
a body, raw keys, drawing and entry. List screens need only `count` and `item`; the generic list code does focus,
scrolling, numbered shortcuts (`SF_NUMBERED`), T9 filtering (`SF_FILTER`), and the long-press target.

| Screen | Name | Reached by |
|---|---|---|
| Home | `home` | start, long BACK from anywhere |
| Quick settings | `quick` | QUICK (`*`), or long SELECT on a radio without a QUICK key |
| Hub | `hub` | SELECT on Home: 1 Contacts, 2 Messages, 3 Recent, 4 Zones, 5 Channels, 6 Tools, 7 Settings (0 Last caller on a radio without CONTEXT) |
| Find | `find` | a digit on Home |
| Actions | `actions` | CONTEXT (`#`) on a person, a talkgroup, the last caller, a message |
| Recent | `recent` | Hub 3 |
| Zones | `zones` | Hub 4: the VFO, then every zone with its channel count. OK goes to the zone, CONTEXT opens its channels (`zone`; long OK on a radio without CONTEXT) |
| Zone | `zone` | CONTEXT in Zones: the zone's channels in order, OK goes to one. With `CONFIG_ZONE_EDIT` also the zone's editor (2.2): "Add channel" first, "Zone options" last, CONTEXT on a channel opens Channel actions |
| Channel actions | `zchannel` | CONTEXT (long OK without CONTEXT) on a channel in a zone's list (2.2) |
| Add channel | `zpick` | "Add channel" in a zone's list: the stored channels the zone doesn't hold, T9 filtered (2.2) |
| Zone options | `zoneedit` | "Zone options" in a zone's list: Rename, Copy as new zone, Delete zone (2.2) |
| Delete zone | `zonedel` | Zone options 3: the confirmation, "No, keep it" first (P7) |
| Channels | `channels` | Hub 5: every codeplug channel as number and name, in number order, filtered by T9. The value (also in a zone's list): RX frequency and talkgroup, "467.375 TG3100", or FM; beside a long name "467.375 3100", then "TG3100" (the name is never cut for it). The first row is "New from current", New channel from current settings (2.1). OK opens the channel (`channel`), CONTEXT goes to it |
| Channel | `channel` | OK in Channels: the channel's settings, editable (2.1). The last rows go to it (in the zone in use if it holds the channel, else the first zone that does; in none, "Not in any zone"), save, and save as a new channel |
| Text entry | `text` | OK on a text row (a channel's name): keypad letters, 2.1 |
| Unsaved changes | `discard` | BACK in the channel editor with unsaved changes (2.1) |
| List picker | `chpick` | OK on a channel's TX contact or RX group list row: "None", then every stored contact or list, T9 filtered |
| Talk target picker | `picker` | long CONTEXT on a DMR channel (else "Talk target: DMR channels only"), TALK_TARGET (a side key set to it), Quick 8 |
| Diagnostics | `diag` | Tools 7, Settings > Advanced |
| Roaming | `roaming` | Tools 8 (7 without diagnostics): roaming on for this zone, roam now, lock to this site ([dmr-roaming.md](dmr-roaming.md) 6.2) |
| About | `about` | Settings > Advanced > About: a text page (`ui_draw_text_page`) in the smallest font, word-wrapped and never shortened, Up / Down / the knob scroll a line. `SF_NO_STRIP`: no status strip while the radio is idle (the owner's request); RX or TX draws it again, so the TX / RX pill still shows (P2) |

The gallery (section 7) shows every screen at every display class.

### 2.1 Channel editor

The Channel screen edits a working copy of the channel. Nothing is written until the user saves.

Rows by kind. A row with two values flips on OK; a row with more opens the spinner, so the knob picks the value
instead of OK pressed once per step. The rows shown follow the copy's mode, so a mode change adds or removes rows at once.

| Kind | OK does | Rows |
|---|---|---|
| Toggle | the other value, at once | Mode (FM / DMR, with `CONFIG_DMR`), RX only, Bandwidth (FM), Timeslot (DMR), VOX (FM), Direct mode (DMR), Talk permit beeps (DMR), Talker alias TS1 / TS2 (DMR), Scan, Battery saver |
| Spin | opens the spinner: the knob or Up / Down step through the values, shown as they change; digits type where the value is a number; OK keeps, BACK cancels (the quick-tile and Settings-number editor) | Power (the board's levels, then "Radio's"), Color code (0-15), TX time-out (0 = "Radio's", 15-495 s in steps of 15), RX tone and TX tone (None, the CTCSS (PL) tones, the DCS codes normal then inverted; in a build without `CONFIG_SUBTONE`, such as a small-flash board's, the tone rows only show the stored tone and OK toasts "Tones: not in this build") |
| Direct entry | opens an editor that takes digits | RX and TX frequency (as VFO entry: MHz digits, then decimals, refused out of band as typed), Own ID (0 = "Radio's", up to 16777215) |
| Text | opens Text entry | Name |
| Pick | opens the List picker | TX contact, RX group list |
| Read only | nothing | Number, Zones |
| Action | runs it | Go to it, Save, Save as new channel |

- A changed row shows the temporary mark (`UI_ITEM_TEMP`, the dot Home uses).
- Long SELECT anywhere saves, as long SELECT makes a quick setting permanent. The Save row does the same.
- BACK with unsaved changes opens Unsaved changes (`discard`): "No, keep editing" first (the default, P7), then "Yes,
  discard". Long BACK still goes Home (P3) and drops the copy without asking.
- Save checks the copy first and names the first problem in a toast: an empty name, RX out of every band, TX out of
  the transmit bands on a channel that isn't RX only, a DMR channel without a TX contact.
- Save writes with `codeplug_set_channel` as a storage job (`ui_svc_write_channel`), like Save to channel, so the UI
  never waits for a flash erase. A failed write raises `UI_EV_SAVE_FAILED`. The radio core re-reads the channel in
  use when `codeplug_serial()` changes, so any save drops its temporary changes, whichever channel was written.
  Whether the written channel is the one in use is decided when the job is posted, on the UI task.
- Save as new channel writes the copy to the lowest free number (`codeplug_free_channel`) and opens that channel.
  "No free channel" when all 1024 are used. The new channel is in no zone until a zone's Add channel takes it (2.2);
  Copy here makes a copy that goes in a zone as it is saved.
- New from current (the first row in Channels: New channel from current settings) fills a copy from what the radio uses now: the channel in use
  with its temporary changes, or the VFO. It opens as an unsaved channel at the lowest free number, named after the
  source with a number appended (or the frequency, from the VFO).

Text entry supports both keypad conventions radios use: more presses of the same key (2: A B C 2 a b c) and `#` for
the input mode, with BACK to delete (and to leave once the field is empty). In ABC and abc, keys 2-9 cycle their
letters, the digit, then the letters in the other case; 1 cycles `. - / , ' & ( ) @ _ ! ? 1`, 0 a space, 0, `+ # *`.
The same key again within 1 s takes the next character; another key or the pause moves on. The title shows the mode
("Name ABC"). In 123 a digit types itself, and a held digit types itself in any mode (principles 3).
CONTEXT (`#`) cycles ABC → 123 → abc (ABC first). BACK deletes the last character and, on an empty field, leaves
without changing the name (the phone convention, the one exception to P4); QUICK (`*`) and Left delete too, and in
text entry QUICK doesn't open quick settings. OK keeps the name. Without a keypad (class F): Up / Down step the last character (down past the space deletes it), OK adds the next
one, long OK keeps. Names stop at `CODEPLUG_NAME_LEN` (16).

Typed rows (frequencies, the ID): BACK takes back the last digit, and with none typed closes the editor; OK keeps what
is typed, and with nothing typed leaves the row as it was. A new RX frequency moves TX with it, keeping a repeater's
offset. Where that offset would put TX outside every band, TX becomes the new RX (simplex). Own ID refuses a digit
that would take it past 16777215, and takes at most 8 digits.

### 2.2 Zone editor

With `CONFIG_ZONE_EDIT` a zone's list (`zone`) edits the zone. Every change is written at once, as a storage job
(`ui_svc_zone_edit`), so there is nothing to save or discard and the UI never waits for a flash erase. Changes made
faster than the flash takes them (a digit pressed again in Channel actions) wait in a queue of 4 and are written in
order by one job; a fifth is refused. The job runs `zone_edit_apply` (`core/codeplug/zone_edit.h`), the same layer an
import uses. A failed change raises `UI_EV_SAVE_FAILED` with the reason ("Zone full", "No room for another zone",
"Already at the end", "That zone is gone", "That channel is gone", "Couldn't write the codeplug") and drops the changes
queued after it, since they were made on a list that didn't happen. A change is checked whole before anything is
written: a new zone with a missing or repeated channel is refused, and a Copy here whose zone can't be written deletes
the channel it wrote. The lists read the codeplug again when `codeplug_serial()` changes and the queue is empty, so
they show the change within a fraction of a second.

| Where | Row or key | Does |
|---|---|---|
| A zone's list | "Add channel" (first row) | opens Add channel (`zpick`): every stored channel the zone doesn't hold, number and name, T9 filtered. OK inserts it after the channel in use when this is the zone in use, else at the end |
| | a channel, OK | goes to it (as before) |
| | a channel, CONTEXT (long OK without CONTEXT) | opens Channel actions (`zchannel`) on it |
| | "Zone options" (last row) | opens Zone options (`zoneedit`) |
| Channel actions | 1 Move up, 2 Move down | swaps it with its neighbor. The menu stays open, so the digit again moves it on; the toast gives the position ("Position 4 of 16"), and "Already first" / "Already last" at the ends |
| | 3 Copy here | the channel editor (2.1) on an unsaved copy at the lowest free number, named with " 2" appended (a copy of the channel in use keeps its temporary changes). Save writes it and inserts it after its source in one job (`ZONE_OP_ADD_NEW`); BACK asks as for any unsaved channel. Refused first with "No free channel" or "Zone full" |
| | 4 Edit channel | the channel editor on the channel |
| | 5 Remove from zone | takes it out of this zone at once, "Removed from <zone>". The channel stays in Channels and any other zone |
| Zone options | 1 Rename | text entry on the zone's name (2.1); OK writes it, an empty name is refused |
| | 2 Copy as new zone | a new zone with the same channels, named with " 2" appended, then back to Zones |
| | 3 Delete zone | asks first (`zonedel`): "Delete <name>? Channels stay." with "No, keep it" focused (P7). Yes deletes it and returns to Zones |

- Channel actions and Edit channel replace their menu on the stack, so the deepest path (Home, Hub, Zones, the zone,
  the channel editor, text entry) fits the six frames.
- A zone holds at most `codeplug_zone_capacity()` channels (80, or 16 with the old layout) and a channel once; there
  are at most `CODEPLUG_ZONES_MAX` zones.
- With no zones stored the radio shows one zone, "All channels", that holds every channel. The first change stores it
  as a real zone 0, so the user keeps what they saw; it can't be deleted until then ("It holds every channel").
- A new zone takes the lowest free slot, so it can come ahead of zones already there, and deleting a zone moves the
  ones after it up. `zone_edit.c` logs each such shift (`zone_edit_shifts`, the last `ZONE_SHIFT_LOG` = 8 kept), and
  every holder of a zone index maps it with `zone_edit_map` once no zone write is under way (`zone_edit_busy`). An
  import that writes zones through the same layer is followed the same way. A reader more than 8 shifts behind keeps
  its indexes.
- What follows a moved zone, and what happens when the zone itself was deleted:

  | Holder | Moved | Deleted |
  |---|---|---|
  | The zone in use (radio core, saved as the settings' zone) | follows; the radio stays on its channel | the zone that took its index (the last zone when it was the last), on the same channel if that zone holds it, else its first usable channel |
  | Dual watch's channels (radio core and `ui_dual.c`) | follow | the pane's channel is found by number in another zone; when no zone holds it the watch ends ("Dual watch off") |
  | The last B and A kept while the watch is off | found again by channel number | found again by channel number |
  | Roaming zone flags (settings `roam_zones`) | follow, saved again | the flag is cleared |
  | Open screens: Zones' focus, a zone's list, Zone options, Channel actions | follow | the zone's screens close, "That zone was deleted" |

- Saved references by channel number (dual watch's B setting, scan lists) need no change.
- A zone scan ends when the zone in use's channel list changes under it (its skipped channels are kept by position); a
  rename doesn't end it.
- Removing the channel in use from the zone in use moves the radio to the next usable channel in the zone. A move in
  the zone in use keeps the radio on its channel at its new position.
- The debug link's `zone.edits` counts the zone edits written since start-up and `zone.edit_last` is the last result
  (`zone_edit_result_t`, 0 = OK).

## 3. Key meanings

The same on every screen (information-architecture.md, key maps 7.1 to 7.5):

| Action | Press | Long press |
|---|---|---|
| BACK | up one level, or clear one thing on Home (entry, banner, scan, reply, digital monitor, temporary change) | Home |
| SELECT | open / choose | the alternative (save, permanent, Quick on radios without a QUICK key) |
| QUICK | Quick settings (closes them again) | lock / unlock the keys |
| CONTEXT | the focused item's actions; a banner's target while one is up | Recent |
| TALK_TARGET | the talk target picker | |
| digits | Find on Home, numbered rows in lists, T9 letters in filters; nothing while PTT keys the radio; on FM the input task gives them, \* and # to the radio core as DTMF and the UI never sees them (`radio-features.md` 3.7) | speed dial (a toast for now); nothing while PTT keys the radio |
| PTT | talk to the focused person or talkgroup once (one-shot), else the channel's target | |

Radios without some keys fall back as the key maps say (few-key map F in the tests). Up / Down on Home change the
channel (Settings > Keys > Up/Down on Home: or the zone, or on a DMR channel the talk target, stepping through the
channel's contact and its listen list; the arrow keys only: the channel knob, ENC_STEP, always steps the channel or the
VFO frequency, #25), USER_1 / USER_4 the zone, USER_3 the power, long USER_3 speaks the status (a
toast until voice prompts exist).

VFO_TOGGLE is the VFO / MR key's short press (a board key map entry, not a side key function): between the channel
and the VFO, "VFO" and "Back to channels". Its long press, COPY_VFO, copies the channel to the VFO (`dmr-voice.md`
2.2). A board with a dedicated VFO/MR key gives its long press to COPY_VFO,
and `keyassignTest` checks every board's key map for it. The key is a key map entry with fixed meanings (not one of the
`board.side_keys`), so Settings > Keys doesn't change it.

EMERGENCY is hold-to-activate (principles P2): a key whose long action is EMERGENCY needs `INPUT_HOLD_MS` (1.5 s)
whatever the long press time (`input_long_threshold`, core/input). Which key carries it is the board's key map
(`board.keymap`): for example the top side key's long action in builds with `CONFIG_DMR_EMERGENCY` (else long USER_3 as above), or a dedicated SOS key; the short press stays USER_3. A hold toggles:
"EMERGENCY sent: hold again to cancel", "Emergency canceled" (`ui_svc_emergency_active` reads the radio core's state,
so a cancel from elsewhere counts). A board whose key map has no EMERGENCY entry has no emergency key (a board whose top key isn't located yet).

PTT while the keys are locked works unless Settings > Privacy > PTT when locked is off: the UI then tells the input
layer (`ui_svc_ptt_block`, `input_set_ptt_block`), which keeps PTT DOWN from the radio core (UP always passes), and the
UI gives an error beep and "PTT is off while locked: hold * to unlock".

The state pill follows `radio_view_t.state`. RX also shows while the monitor holds an FM squelch open (MON stays beside
the mode). DMR's monitor key is digital monitor and leaves it IDLE.

On DMR the pill shows TX whenever the stack is on the air, with or without PTT. Data burst lists (ARS, SMS, a
position, a call alert or its ACK, the IP gateway, a roaming probe) set `RS_TX` with `RDMR_TX_DATA` (`dmr_follow`).
Home keeps its own screen for them, and the activity line says SENDING instead of TALKING (in dual watch, "SENDING on
A"). The pill shows RX while data or CSBK bursts arrive on our slot and color code, until the call fade time
(`DMR_CALL_FADE_MS`) after the last one, even with no call running (`dmr_call.c` `data_receiving`). The green LED
stays for a call being played.

Small displays (under 200 px tall) merge a screen's title into the status strip: the state pill shows only when not
idle, the bars and battery icon give way, the battery percent stays (on a radio without a battery, `CAP_NO_BATTERY`,
neither is drawn anywhere, and Tools > Battery, Settings > Power > Battery saver and Low battery warning are left
out of their lists, so the other Tools rows move up), and only the name part of the title is
shortened with a middle ellipsis (the prefix stays whole: "Quick COUNTY DMR", not "Quick ..tions").

### 3.1 Side key assignment

Switch: `CONFIG_KEY_ASSIGN`. The board lists the keys a user may reassign (`board.side_keys`, role order: side key 1,
side key 2, top key; boards have two or three, see the board's `side_keys`). Each role has a press and a
long press, and each takes one function:

| Function (`keyfn_t`) | Action sent | Press | Long press |
|---|---|:-:|:-:|
| Default (0) | what the board's key map gives | yes | yes |
| Nothing (1) | none | yes | yes |
| Monitor (2) | MONITOR as raw edges; the radio core picks by mode (#19): FM, the squelch open while held; DMR, a short press toggles digital monitor (the hear all latch) on its release | yes | no (but the key's hold slot works with it: on DMR channels a hold runs the hold function and doesn't toggle; on FM holding is the squelch break and the UI drops it) |
| Talk target (3) | TALK_TARGET | yes | yes |
| Power level (4) | POWER: the next level | yes | yes |
| Next zone (5), Previous zone (6) | ZONE_UP, ZONE_DOWN | yes | yes |
| Scan (7) | SCAN: on / off | yes | yes |
| Emergency (8) | EMERGENCY, hold to activate (1.5 s) | no: P2 | yes |
| (9) | retired (was Hear all / Digital monitor): reads as Default. The Monitor function toggles digital monitor on DMR (#19) | no | no |
| (10) | retired (was Monitor on/off): reads as Default. The Monitor function does both jobs (#19) | no | no |
| Key lock (11) | LOCK: lock, and unlock while locked (as long QUICK) | yes | yes |
| Call back (12) | CALL_BACK: talk for now to the last call heard, its talkgroup or, for a private call, its caller (`dmr-voice.md` 2) | yes | yes |
| Home zone (13) | HOME_ZONE: the codeplug's first zone (the CPS's zone order picks it; no setting, the settings record has no byte to spare). Leaves the VFO; "Home zone: <name>" | yes | yes |
| Copy to VFO (14) | COPY_VFO: copy the channel in effect to the VFO and open it (`dmr-voice.md` 2.2). In the VFO: "Already in VFO". A channel the VFO can't hold gets a "VFO: ..." toast and nothing changes. Offered in builds with a VFO (`CONFIG_VFO_FM` or `CONFIG_VFO_DMR`) | yes | yes |

POWER, ZONE_UP, ZONE_DOWN, SCAN, CALL_BACK, HOME_ZONE and COPY_VFO do the same on a press and a long press and
work on any screen. LOCK also gets through while the keys are locked, to unlock them. TALK_TARGET opens the picker on
a long press too. A monitor key given a press of its own loses its hold: its long press is nothing until set. A Monitor press with a hold
function: the input engine sends DOWN, the LONG (value 1) at the threshold and UP (value 1 when the LONG ran); the
radio core then doesn't toggle digital monitor (RREQ_MONITOR 2), and the UI drops the LONG on FM.

Storage: the open-firmware codeplug's ten quick-key slots (EEPROM 0x7524, 2 bytes LE each,
`open-firmware-cps.md` 7.2). Slot 0 is the marker 0x4B01 ("K", layout 1); slot 1 + 2k is role k's press and slot
2 + 2k its long press; slots 7-9 are spare. Without the marker the slots are another firmware's and every key is the
board's; the first change clears them and writes the marker. A value that isn't a `keyfn_t`, or isn't allowed in its
slot (monitor on a long press, emergency on a press), reads as Default. The values are append-only (gotchas 9.11).

Both places edit the same slots, so there is one copy:

- Settings > Keys (rows Side key 1, Side key 1 hold, ..., Top key hold): the change applies at once
  (`keyassign_build`, then `input_set_keymap`, which the input task takes when no key is held) and a storage job
  writes the slots. A failed write shows "Couldn't save the keys: for now only". A row for a key the radio doesn't
  have is dimmed ("This radio has no such key"), and so is a hold row whose press is monitor.
- The codeplug (a CPS or TFTP write): `ui_svc_keys_poll` reads the slots again when `codeplug_serial()` changes and
  rebuilds the key map. qdmr has no editor for them; a CPS that writes back an old image brings back its keys.

## 4. Data other packages own

The UI asks through `ui_svc.h`. Each call has a weak definition in `ui_svc_stub.c` that behaves as "not in this
build" (screens dim what isn't there and say why on SELECT); the owning package's strong definition replaces it at
link time. `ui_svc_features()` ORs the bits each package reports.

| Calls | Owner | State in this branch |
|---|---|---|
| `ui_svc_talk_to` temporary / default | dmr-voice (radio core) | `ui_svc_radio.c`: `radio_req_dmr_target`, `radio_req_dmr_target_clear` |
| `ui_svc_talk_to` permanent (`ui_svc_save_talk_target`), `ui_svc_save_channel`, `ui_svc_setting_get/set`, `ui_svc_feat_storage` | storage-cps | `ui_svc_storage.c`: the channel is rewritten with `codeplug_set_channel` (power, bandwidth, tones; a temporary squelch goes to the default squelch preference); settings are read from where they are kept: the radio core's settings (`radio_get_settings`, saved by the radio core), `settings_t` (brightness, default squelch, hotspot allowed), the data service's answers (`dmr_data_get_settings`); the reply window, mic gain, mic AGC and mic noise reduction (`accent` bits 6-7), auto lock, long press and repeat times, the GPS options, theme, Up/Down on Home, low battery level, auto power off and the `ui_flags` bits (large text, hint row, message tone, talker alias, PTT when locked, hear all by default, vibrate as an off bit) have `settings_t` fields (appended, settings-store.md); the talk permit tone is the radio core's (`radio_settings_t.talk_permit`); the privacy key type and key (Settings > Privacy) are key slot 1 of the key store (`privacy_type[0]`, `privacy_key[0]`, `privacy_active`), and a change while privacy is on goes to the radio core at once (`radio_req_dmr_privacy`); only the ones without a field stay in RAM (voice prompts). Both saves run as a storage job on the housekeeping task (`storage_job_post`), not on the UI task: the change takes effect for now at once, the temporary mark goes when the write lands, and a failed write raises `UI_EV_SAVE_FAILED` ("Couldn't save: still for now only"). Save to channel also writes the temporary mode, color code and timeslot |
| `ui_svc_factory_reset` | storage-cps | `ui_svc_storage.c`, as a storage job: `settings_t` back to its defaults and saved, then `storage_native_erase` (the native area, only when it carries the native magic or is already blank; never the open-firmware codeplug, calibration or a foreign region), then a restart (`board.system->reboot`), or a radio core reload where the board can't restart |
| `ui_svc_focus_target` (one-shot PTT to the focused row), `ui_svc_dmr_timeslot`, `ui_svc_dmr_color_code`, `ui_svc_mode`, `ui_svc_hear_all` | dmr-voice / radio core | `ui_svc_radio.c` on the new requests `radio_req_dmr_focus`, `radio_req_dmr_timeslot`, `radio_req_dmr_color_code`, `radio_req_mode`, `radio_req_dmr_hear_all` (radio-features.md, dmr-voice.md). Timeslot, color code and mode are temporary changes (temp bits 0x20, 0x40, 0x80) that BACK on Home undoes and Save to channel keeps; the hear-all latch ends with the channel, a mode change or BACK on Home (not with the call) |
| `ui_svc_privacy`, `ui_svc_monitor`, `ui_svc_vox` | radio core | `ui_svc_radio.c`: `radio_req_dmr_privacy` on the active privacy slot (else the first set one), `radio_req_monitor` as a latch, the `vox` setting (all / off) with `radio_view_t.vox_on` for the tile |
| `ui_svc_emergency`, `ui_svc_feat_ext` | dmr-ext | weak seams: the EMERGENCY key action calls `ui_svc_emergency`, and dmr-ext reports `UI_FEAT_EMERGENCY` through its own strong `ui_svc_feat_ext`; without it the key says "Emergency isn't in this build" |
| `ui_svc_scan`, `ui_svc_scan_skip`, `ui_svc_feat_radio`; scanning state from `radio_view_t.scan` | radio-features | `ui_svc_radio.c`: `radio_req_scan` (toggle, stop, nuisance) |
| `ui_svc_roam` (`UI_FEAT_ROAMING`); roaming state from `radio_view_t.roam` (the Home chip, `ROAM` in the strip, Tools > Roaming, the "Moved to" toast) | radio core (CONFIG_DMR_ROAMING) | `ui_svc_radio.c`: `radio_req_roam` |
| `hostlink_network_mode`, `board.netif->up`: the strip's USB word (`ui_state_t.usb_net`, polled in `ui_app_tick` so a change without a key press redraws): `IP` while USB is a network adapter, and right after it an up and a down arrow standing close (as U+21C5, a phone's data link mark) once the computer has brought the adapter up (the debug link's `usb.net_up`, [usb-network.md](usb-network.md) 1); after SCAN / ROAM / MON, which win while they last; not on screens that merge their title into the strip | host link and the board's netif (CONFIG_USB_NETWORK, CONFIG_HOSTLINK) | direct calls; Tools > USB network uses the first too |
| `ui_svc_beep` | radio-features | `ui_svc_radio.c`: `radio_req_beep(BEEP_KEY)` on every press and long press |
| `ui_svc_apply` (key tones, tone volume, TX timeout, backlight time, battery saver, talk permit tone, mic gain, mic AGC, mic noise reduction) | radio-features | `ui_svc_radio.c`: `radio_set_settings`, at start-up and on each change; mic gain goes to `radio_set_mic_gain` (2 dB a step, 5 = the board's levels: the board's FM and DMR mic gain where it has `hal_radio_t.mic_gain`, else DMR scales the samples before the vocoder; radio-features.md 3.6); mic AGC and mic noise reduction to `ambeVoiceSetEncodeOptions` (tone frames always, plus each option that is on; `dmr-voice.md` 6), and with AGC on the mic gain is step 5 |
| `ui_svc_beep` kinds (key, error, message, battery low) | radio-features | `ui_svc_radio.c`: a table from `UI_BEEP_*` to `BEEP_*` (`BEEP_MESSAGE` is the new-message tone) |
| `UI_FEAT_VOLUME`, `UI_SET_VOLUME` | radio-features | `ui_svc_radio.c`: the feature bit when `radio_volume_soft()` (no knob, a board volume hook); `ui_svc_storage.c`: the step in `settings_t.volume`, and `radio_req_volume` so it is in force at once. The UI side is the volume screen (`SCR_VOLUME`, opened by `ACT_VOLUME`) and Settings > Sound > Volume, both only with the feature (`radio-features.md` 4.2) |
| `ui_svc_vibrate` patterns (message, call, emergency), `UI_FEAT_VIBRATE` | radio-features | `ui_svc_radio.c` (CONFIG_VIBRATE): a table from `UI_VIBE_*` to `VIBE_*` for `radio_req_vibrate`; the feature bit when the board has `hal_power_t.vibrate` (`radio-features.md` 4.1). Weak no-op otherwise |
| `ui_svc_ptt_block`, `ui_svc_emergency_active` | input / dmr-ext | `ui_svc_radio.c`: `input_set_ptt_block`; `ui_svc_emergency.c`: `dmr_emergency_state` |
| backlight dim / off | radio-features | `ui.c`: `radio_backlight_pct` (dims on a low battery too) |
| `ui_svc_msg_*`, `ui_svc_radio_check`, `ui_svc_call_alert`, `ui_svc_poll`, `ui_svc_feat_data` | dmr-data | `ui_svc_data.c` on `dmr_data_api.h`; `ui_svc_msg_delete` is `sms_inbox_delete` |
| `ui_svc_position` | GPS (no owner yet) | stub: "No GPS on this radio"; the GPS settings are dimmed with the same reason on boards without `CAP_GPS` |
| USB mode (Tools > USB network) | hostlink | `hostlink_network_mode`, `hostlink_request_usb_network` (with `CONFIG_USB_NETWORK` and `CONFIG_HOSTLINK`) |
| talker alias, private call start | dmr-voice | read from `radio_view_t.dmr`; `UI_EV_PRIVATE_CALL` is not raised by anyone yet |

Auto power off is the UI's: `ui_app_power_off_due()` turns true after Settings > Power > Auto power off without use
(keys, RX, TX, scan and hotspot count as use), with a banner and beep one minute before; the housekeeping loop
(core/app) then powers off as for the knob, except that if the power is still on 2 s after releasing the latch it
latches again, cancels the radio core's power off (`radio_req_power_off_cancel`) and raises `UI_EV_POWER_OFF_FAILED`
("Couldn't switch off / Turn the knob off"). The low battery warning is the UI's too: a banner (and the low beep
while the radio core's level is still OK) at Settings > Power > Low battery warning, repeated every 5 minutes, reset
3 % above it.

Inbound events go through `ui_notify()` (a ring of 4, safe from any task) or, for packages the UI polls, through
`ui_svc_poll()` once per UI pass. They become banners for 8 s: message received, delivered / not delivered, call
alert, alert answered, radio check answered (with the round trip) or not, private call, emergency, hotspot started.

## 5. Build switches

| Switch | Default | Off |
|---|---|---|
| `CONFIG_UI_SETTINGS` | y | the Settings menu is left out (about 5.3 KB, `docs/feature-matrix.md`); settings keep their stored values and are set with qdmr; Settings shows one row saying so, Find finds no settings |
| `CONFIG_DIAG` | y | no diagnostics page (Tools has no Diagnostics row; Roaming moves up to 7) |
| `CONFIG_FONT_XL` | n | the 16x32 font for 240 px and wider displays |
| `CONFIG_FONT_AA` | n | the bitmap fonts (Spleen) on color panels; on, the sizes from 6x12 up are anti-aliased (JetBrains Mono Bold, coverage 0..3 a pixel, blended with what is under them; `gfx.c`). Mono radios leave it off. On a 4 bpp panel (every color board) the edge grays share the 16 palette entries with the screen's own colors; a screen that already uses many colors loses some of them, and some font sizes or colors can then be slightly harder to read |
| `CONFIG_FONT_BIG` | n | on (needs `FONT_AA`), the 240 px class draws lists, labels, the status strip and hints in a 9x18 font instead of 8x16, and its small print in 8x16 instead of 6x12 |
| `CONFIG_ZONE_EDIT` | = `CHANNEL_EDIT` | a zone's list only goes to its channels: no Add channel, Channel actions or Zone options (2.2). Needs `CONFIG_CHANNEL_EDIT` |
| `CONFIG_UI_COLOR` | y (n on a board with a monochrome panel) | Settings > Display has no Theme, Highlight color or Filled status bar row and the screen keeps the dark theme's colors (about 600 B). For radios whose only screen is monochrome |

Screens for features that are off (`CONFIG_DMR`, `CONFIG_HOTSPOT`, `CONFIG_VFO_FM`, `CONFIG_VFO_DMR`, `CONFIG_SCAN`, data features) are
left out or dimmed with the feature. Some radios' default images have only a few KB free (`docs/feature-matrix.md`); the UI was about 50 KB when last measured (`gfx` + fonts about 11 KB, screens and behavior the rest). The frame buffer (40 KB by default) bounds the display: 240x320 at
16 bpp renders only in the simulator, which overrides `GFX_FB_BYTES` and `GFX_MAX_ROWS`. Every color board builds the 16-color frame buffer instead
(`GFX_INDEXED`, bpp 4): 37.5 KB at 240x320, 9.6 KB at 160x120 and 10 KB at 160x128, against 150, 37.5 and 40 KB at
16 bpp. A mono board sizes the buffer to its panel (128x64 at 1 bpp is 1 KB). Drawing still takes RGB565 colors, and
`gfx.c` keeps a 16-entry palette of the ones on the screen. An entry gets a new color
only once nothing drawn since the last clear uses it; anti-aliased edges never take the last 4 free entries and
otherwise get the nearest color already there. Screen captures (debug link, `/api/screen.bmp`) read it as RGB565.

Without `CONFIG_FONT_AA` a screen uses at most 9 colors (the gallery, all 93 screens), so 4 bpp renders it exactly as
16 bpp does. With `CONFIG_FONT_AA` the edge grays add up to 20 colors on the busiest screen (Messages); on 14 of the
93 screens some edge grays and colors are replaced by the nearest palette entry, and some font sizes or colors can be
slightly harder to read. The 160 px boards leave `FONT_AA` off for that reason.

## 6. Debug link

The link section (`docs/spec/debuglink.md`) carries the UI's state for HIL tests: `ui.screen`, `ui.depth`,
`ui.focus`, `ui.count`, `ui.locked`, `ui.banner`, `ui.toast`, `ui.query`, `ui.draws`. Tests check these rather than
pixels, so they hold on any display.

## 7. Host simulator and tests

`tests/host/sim/` runs the real `core/ui` files against a simulated board (any display geometry, any of the key
maps K for a three-key board, M for a GD-77 style map, V for a two-key map, F few keys), radio core, the usability-test codeplug (3 zones,
40 channels, 30 contacts) and the services of section 4.

```
make -C tests/host run-ui                                      # gfxTest, uiFlowTest, the layout check
make -C tests/host ui-gallery                                  # docs/ux/screens/<class>/<screen>.png, sheets/<class>.png
make -C tests/host ui-sheets GEOM=128x64                       # contact sheet only, tests/host/out/sheets/
tests/host/uiSim --geom 160x120i5 --script "ok 4 down ok expect:zone=1" --out zone.png --scale 3
```

The gallery draws a color page (Home with its filled status bar, the Channels list) once more in each highlight color
`UI_GALLERY_COLORS` names, as `<screen>__<color>.png` right after the page, on color displays only; screens that only
differ in color (`home_plain`) are left out at 128x64. The list is set in
`.github/workflows/ui-gallery.yml` (`docs/dev/ci.md`), and `make ui-gallery` reads it from there. Home (filled and plain
status bar), RX, TX, the Channels list and Settings > Display are also drawn in the light and high-contrast themes, as
`<screen>__light.png` and `<screen>__high_contrast.png` (color displays).


Geometries: `160x120i5` (160x120 visible, 5 px inset left and right), `160x128` (both 160 px classes render through the 4 bpp frame buffer, as the boards do),
`240x320i10x5` (16 colors, insets 10 px left and right, 5 px top and bottom), `128x64` (mono), `240x320`, or any
`WxH`, `WxHiN` (inset N all round), `WxHiNxM` (N left and right, M top and bottom), `WxHm` (mono). The 240 px classes
draw the anti-aliased fonts (`CONFIG_FONT_AA`), and `240x320i10x5` the larger text too
(`CONFIG_FONT_BIG`); the others the bitmap ones. Script tokens:

| Token | Meaning |
|---|---|
| `up down left right ok back quick ctx 0..9 tt mon user1..user4 emerg enc+ enc-` | a short press (counted) |
| `long:<key>` | a long press (counted) |
| `ptt`, `ptt:<ms>`, `pttdown`, `pttup` | PTT (counted) |
| `mondown`, `monup` | the monitor key held, then released (FM: the squelch held open) |
| `color:<name>`, `strip:on\|off` | Settings > Display > Highlight color (`red orange yellow teal light_blue purple pink default`) and Filled status bar, set directly |
| `wait:<ms>` | time passes |
| `rx:<src>,<dst>,g\|p[,name]`, `rxend`, `call:<src>,<dst>,g\|p` | a DMR call heard |
| `msg:<src>,<text>`, `alert:<src>`, `emergency:<src>`, `privcall:<src>`, `hotspot:on\|off`, `battery:<pct>` | events |
| `shot:<name>` | save the screen |
| `offfailed` | the auto power off didn't cut the power (`UI_EV_POWER_OFF_FAILED`) |
| `worst` | worst-case codeplug content from here on (`sim_worst`: 16-character names, 8-digit talkgroups, 7-digit IDs, channels 985-1024, 80-channel zones) |
| `power:<n>` | the radio's power level index |
| `expect:<key>=<value>` | check: `screen focus depth count zone pos vfo freq mode tx txid txpriv talk locked query banner toast power sq ts cc temporary writes checks sent text scanning privacy hearall monitor hotspot beeps radio_tot radio_backlight beep pttblock poweroff usbnet set<id>` (`beep` the last beep kind, `set<id>` a setting's stored value; `_` = space, `*` = anything) |

`uiFlowTest` runs every task flow of `task-flows.md` with the press budgets of `usability-tests.md` on four display
and key map pairs (K at 160x120i5, M at 128x64, V at 160x128, K at 240x320) plus the few-key fallbacks: 160 flow runs.
A crawler then visits every reachable screen and checks the rules that hold everywhere: BACK leaves every screen, long
BACK reaches Home, long QUICK locks on Home and nowhere else, long CONTEXT opens the talk target picker on a DMR channel, the status strip is drawn (except About while idle),
PTT shows TX, and every field-use screen is at most 3 presses deep. The About check scrolls the page to its end at
every display class and fails if any text was shortened.

`uiSim --ellipsis [GEOM]` lists every text shortened with a middle ellipsis on the gallery screens and the settings
pages (lists scrolled through): `gfx_text_fit` reports each one through `gfx_fit_trace` in host builds
(`GFX_FIT_TRACE`). Use it after changing wording or layout.

`uiSim --layout [GEOM]` (run by `run-ui`, so CI) is the layout check: the gallery screens, the settings pages and the
worst-case screens at every geometry, with ordinary and worst-case content. `gfx_draw_trace` (host builds) reports
every text, fill and clear with its rectangle; the check fails on a text outside the safe area or cut by its clip box,
overlapping texts, words with no space between them, a box over part of a text, an overlay cutting a line in half, an
invisible text (its cells one color after it is drawn), and a middle ellipsis with ordinary content
(`docs/ux/wireframes/README.md`). On color displays it runs in every theme. `RFOUNDRY_LAYOUT_SHOTS=DIR` saves every screen
checked.

HIL: `tests/hil/test_ui.py` (`tests/hil/README.md`) replays the main sequences on the radio over the debug link.

## 8. Decisions and open points

- Task flow 12 budgets 3 presses into the VFO, but flow 2 opens Zones on the current zone (so a zone two away is
  2 presses); the VFO row is first, so entering it takes 4 (OK 4 Up OK). The test uses 4; the owner decides.
- Power on radios with many levels: the Quick tile cycles three presets (low, the board's default, highest); the full
  list is in Settings.
- Long BACK on Home no longer opens diagnostics (P3: one meaning per key); they are in Tools 7. The firmware boots
  into Home.
- The long-press and key-repeat times are stored in `settings_t` and handed to the input layer (`input_set_timing`,
  clamped to 200..3000 ms and 40..1000 ms); auto lock locks the keys after its time without a key press (counted on
  Home and in menus alike; injected keys count).
- Mic AGC and mic noise reduction are the vocoder's (DMR only). Each is dimmed ("Not on this radio") when the board's
  vocoder lacks the option (`hal_vocoder_t.options`), and in a build without DMR. While Mic AGC is on, Mic gain shows
  "AGC", is dimmed, OK says "Mic AGC sets the level"
  and the gain is step 5, the board's levels (FM too on a radio whose board sets the gain).
- Mic gain acts on FM and DMR where the board sets it in its audio path (`hal_radio_t.mic_gain`), else on DMR only,
  in software before the vocoder (radio-features.md 3.6). The
  GPS options are stored but act on nothing (no GPS owner); they are dimmed on boards without `CAP_GPS`.
- Voice prompts are dimmed ("Voice prompts aren't built yet: no recorded speech").
- Theme switches the color palette (dark, light, high contrast). Highlight color (Default, Red, Orange, Yellow, Teal,
  Light blue, Purple, Pink) and Filled status bar (Settings > Display, `settings_t.accent`) are for color displays
  too. On a 1 bpp display the three rows are dimmed ("Colors are for color screens"); a build without
  `CONFIG_UI_COLOR` (section 5) has no such rows. A color other than
  Default is the list and tile focus (with black text) and Home's channel number (on the light theme the number stays
  the text color: the light shades don't read on white). The filled bar (on by default) is reverse video: the status
  strip's background in the color (light gray with Default, white in the high-contrast theme), its text and icons
  black, the IDLE pill inverse on it; RX, TX and NO TX keep their own pill colors and words (P5). On it the pill is the strip's full height (a one-cell
  pill left a line of the strip's color under it) against the bezel, its corners rounded by two pixels. Left of the
  pill's middle the strip is the screen's background, so no strip color shows beside its left end. On the right a
  one-pixel black edge follows the curve: the pill colors are close in lightness to the strip colors, so hue alone
  would not separate them. Warnings on the strip (SCAN, MON, ROAM, unread messages, the lock, a low battery) keep
  a color: a dark shade picked per strip color, dark amber where it reads and dark blue on red, orange and pink, at
  least 5:1 on its strip after RGB565 rounding. The shades are light so black
  text reads on them (at least 7.5:1 after RGB565 rounding); the red is lighter and pinker than TX's red, and there is
  no green, which would hide the RX pill. Gallery: `home_dmr*`, `home_plain`, `channels*` (section 7).
- The VFO has a mode of its own (`dmr-voice.md` 2.1). The Mode tile (tile 4 in the VFO only) switches it FM / DMR ("VFO: DMR", no "for now": it is kept), and in a build with only one VFO kind the tile is dimmed ("VFO: one mode in this build"). On a channel tile 4 is "To VFO" (`dmr-voice.md` 2.2): it copies what is in effect on the channel to the VFO and opens it on Home ("Copied to VFO"); a channel the VFO can't hold is refused with a toast. A channel has no temporary mode change any more. In DMR the quick menu is the DMR one: Timeslot, Color and Talk to change the VFO's own settings, with no dot on Home. The talk target picker lists the VFO's current target first ("Current"); both OK and hold OK keep the target for the VFO.
- The privacy key is typed in decimal (Anytone 0..65535). Number rows take typed digits; a value outside the range or
  between steps is refused with the range. Only key slot 1 is reachable from the menu.
- Switching USB to network has no confirmation (P7: reversible); the info page says what stops and how to get back.
- A failed message isn't kept for a retry: the banner says "Not delivered: send it again".
- Compose offers canned texts only ("OK", "On my way", "Call me", "Can't talk now"); free text is dimmed.
- Deleting a message has no undo (`sms_inbox_delete` removes it from the RAM inbox).
- Save to channel and the permanent talk target write the codeplug as a storage job on the housekeeping task, so the
  UI no longer stalls for the flash write. A permanent target must be a stored contact: a typed ID is set for now at
  once and the job then says "Not a stored contact: for now only".
- One-shot PTT: the row with the focus in the picker or a list is handed to the radio core (`radio_req_dmr_focus`);
  PTT then talks to it once, and the radio follows it for a reply window (the reply time setting, else the
  codeplug's hang time) before going back to the channel's talk target. A channel change, the call ending outside
  the window, Reply cancel or a new manual target end it.
