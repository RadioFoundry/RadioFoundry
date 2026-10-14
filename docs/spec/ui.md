<!-- SPDX-License-Identifier: MIT -->

# The user interface

How `core/ui` implements the first-principles UX in `docs/ux` (`principles.md`, `information-architecture.md`,
`task-flows.md`, `wireframes/`, `usability-tests.md`). The UX documents say what the radio does; this page says how
the code is laid out, who owns the data behind each screen, and how the UI is tested without a radio.

## 1. Layout

| File | What |
|---|---|
| `gfx.c`, `fonts.c`, `fmt.c` | drawing in a viewport inside the display's safe-area insets, fonts, number / text formatting |
| `ui_screens.c` | drawing only: status strip, title, hint row, lists, Home, call screens, Quick tiles, banners, emergency |
| `ui_app.c` | behavior without the RTOS: the navigation stack, global keys, Home and Quick keys, filters, Recent, events, banners |
| `ui_menus.c` | every list screen and the screen table `ui_scr[]`: Hub, Actions, Compose, Contacts, Messages, Recent, Channels, Zone, Tools, Settings, Picker, Info, Confirm, Scan, Roaming |
| `ui_find.c` | type-to-find (T9 word starts, numbers, channel positions) |
| `ui_svc.h`, `ui_svc_stub.c` | the thin interface to data other packages own, with weak "not in this build" stubs (section 4) |
| `ui_svc_radio.c`, `ui_svc_data.c`, `ui_svc_storage.c` | the strong implementations on the radio core, the DMR data service and the settings store / codeplug editing |
| `ui.c` | the UI task: splash (the mark, beside the name on a panel too short to stack them (128x64), the name, the two CPS boot text lines from the codeplug, board and version; `ui_draw_splash` in ui_screens.c), 50 ms frame pacing, backlight, status copy for the debug link |

`ui_app.c` and `ui_menus.c` never call the RTOS. The firmware drives them from `ui.c`; the host simulator
(section 7) drives the same files.

Spacing on the displays under 200 px high (`UI_TALL_PX`): the status strip and, from 100 px high, each list row take
one pixel more than the text cell, so lines never touch (easy to read comes before one more row; principles.md 5).
The board's safe-area insets (`hal_display_t.inset_*`) are outside the drawing area (`gfx_width` / `gfx_height`),
for panel edges hidden by the case (DM-1701: 5 px left and right under the lens, `hardware-dm1701.md` 9.1). At 160x120i5 a list shows 7 rows, at 160x128 7, at 120x160 (the DM-32) 15,
at 128x64 5. The 240x320 class keeps 3 px per row and a separate title row; `uiSim --layout` fails a class whose row
pitch drops below these. The 120x160 portrait panel keeps the 128 px class's small font (24 characters a line) and
takes the 160 px class's channel and talk target fonts. Home's Activity region shows the last caller on two lines
(name, then "TG 91 11s ago") where it has the rows for them, else one line where the name gives way first; a single
line ("No calls heard yet") is centered in the region. Text that doesn't fit is shortened (an alternate, the name
dropped, then an ellipsis), never drawn in a smaller font.

The hint row is groups separated by two spaces (`"OK open  # more  Back"`). The group for the Back key (it starts with
"Back") is drawn against the right edge, under the key; the other groups run from the left, and those that don't fit
are left out whole from the right. A toast is one centered line instead. A banner or a two-line toast starts in a gap
between lines of the screen under it (a row all one color), never through a line of text.

Text is shortened with a middle ellipsis only as a last resort (P5 allows it for names on Home). Before that:

- A list row keeps its label whole and gives the value the room left: values carry text alternates (`font.h`) down to
  a short form or nothing (Hub's zone name, Find's kind, Channels' "Here, 16 channels" / "Here, 16 ch" / "Here", ages: "11s ago" / "11s").
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
| Hub | `hub` | SELECT on Home: 1 Contacts, 2 Messages, 3 Recent, 4 Channels, 5 Tools, 6 Settings (0 Last caller on a radio without CONTEXT) |
| Find | `find` | a digit on Home |
| Actions | `actions` | CONTEXT (`#`) on a person, a talkgroup, the last caller, a message |
| Recent | `recent` | Hub 3 |
| Talk target picker | `picker` | long CONTEXT on a DMR channel (else "Talk target: DMR channels only"), TALK_TARGET (SK2), Quick 8 |
| Diagnostics | `diag` | Tools 7, Settings > Advanced |
| Roaming | `roaming` | Tools 8 (7 without diagnostics): roaming on for this zone, roam now, lock to this site ([dmr-roaming.md](dmr-roaming.md) 6.2) |
| About | `about` | Settings > Advanced > About: a text page (`ui_draw_text_page`) in the smallest font, word-wrapped and never shortened, Up / Down / the knob scroll a line. `SF_NO_STRIP`: no status strip while the radio is idle (the owner's request); RX or TX draws it again, so the TX / RX pill still shows (P2) |

The gallery (section 7) shows every screen at every display class.

## 3. Key meanings

The same on every screen (information-architecture.md, key maps 7.1 to 7.5):

| Action | Press | Long press |
|---|---|---|
| BACK | up one level, or clear one thing on Home (entry, banner, scan, reply, digital monitor, temporary change) | Home |
| SELECT | open / choose | the alternative (save, permanent, Quick on radios without a QUICK key) |
| QUICK | Quick settings (closes them again) | lock / unlock the keys |
| CONTEXT | the focused item's actions; a banner's target while one is up | Recent |
| TALK_TARGET | the talk target picker | |
| digits | Find on Home, numbered rows in lists, T9 letters in filters | speed dial (a toast for now) |
| PTT | talk to the focused person or talkgroup once (one-shot), else the channel's target | |

Radios without some keys fall back as the key maps say (few-key map F in the tests). Up / Down on Home change the
channel (Settings > Keys > Up/Down on Home: or the zone, or on a DMR channel the talk target, stepping through the
channel's contact and its listen list; the arrow keys only: the channel knob, ENC_STEP, always steps the channel or the
VFO frequency, #25), USER_1 / USER_4 the zone, USER_3 the power, long USER_3 speaks the status (a
toast until voice prompts exist).

EMERGENCY is hold-to-activate (principles P2): a key whose long action is EMERGENCY needs `INPUT_HOLD_MS` (1.5 s)
whatever the long press time (`input_long_threshold`, core/input). Which key carries it is the board's key map
(`board.keymap`): on the DM-1701 the top side key's long action in builds with `CONFIG_DMR_EMERGENCY` (else long
USER_3 as above); the short press stays USER_3. A hold toggles:
"EMERGENCY sent: hold again to cancel", "Emergency canceled" (`ui_svc_emergency_active` reads the radio core's state,
so a cancel from elsewhere counts). A board whose key map has no EMERGENCY entry has no emergency key (the DM-32UV: its top key isn't located yet).

PTT while the keys are locked works unless Settings > Privacy > PTT when locked is off: the UI then tells the input
layer (`ui_svc_ptt_block`, `input_set_ptt_block`), which keeps PTT DOWN from the radio core (UP always passes), and the
UI gives an error beep and "PTT is off while locked: hold * to unlock".

Small displays (under 200 px tall) merge a screen's title into the status strip: the state pill shows only when not
idle, the bars and battery icon give way, the battery percent stays, and only the name part of the title is
shortened with a middle ellipsis (the prefix stays whole: "Quick COUNTY DMR", not "Quick ..tions").

### 3.1 Side key assignment

Switch: `CONFIG_KEY_ASSIGN`. The board lists the keys a user may reassign (`board.side_keys`, role order: side key 1,
side key 2, top key; the DM-1701 has all three, the MD-380 and DM-32UV the first two). Each role has a press and a
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

POWER, ZONE_UP, ZONE_DOWN, SCAN, CALL_BACK and HOME_ZONE do the same on a press and a long press and
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
| `ui_svc_talk_to` permanent (`ui_svc_save_talk_target`), `ui_svc_save_channel`, `ui_svc_setting_get/set`, `ui_svc_feat_storage` | storage-cps | `ui_svc_storage.c`: the channel is rewritten with `codeplug_set_channel` (power, bandwidth, tones; a temporary squelch goes to the default squelch preference); settings are read from where they are kept: the radio core's settings (`radio_get_settings`, saved by the radio core), `settings_t` (brightness, default squelch, hotspot allowed), the data service's answers (`dmr_data_get_settings`); the reply window, mic gain, auto lock, long press and repeat times, the GPS options, theme, Up/Down on Home, low battery level, auto power off and the `ui_flags` bits (large text, hint row, message tone, talker alias, PTT when locked, hear all by default) have `settings_t` fields (appended, settings-store.md); the talk permit tone is the radio core's (`radio_settings_t.talk_permit`); the privacy key type and key (Settings > Privacy) are key slot 1 of the key store (`privacy_type[0]`, `privacy_key[0]`, `privacy_active`), and a change while privacy is on goes to the radio core at once (`radio_req_dmr_privacy`); only the ones without a field stay in RAM (voice prompts). Both saves run as a storage job on the housekeeping task (`storage_job_post`), not on the UI task: the change takes effect for now at once, the temporary mark goes when the write lands, and a failed write raises `UI_EV_SAVE_FAILED` ("Couldn't save: still for now only"). Save to channel also writes the temporary mode, color code and timeslot |
| `ui_svc_factory_reset` | storage-cps | `ui_svc_storage.c`, as a storage job: `settings_t` back to its defaults and saved, then `storage_native_erase` (the native area, only when it carries the `ODRN` magic or is already blank; never the open-firmware codeplug, calibration or a foreign region), then a restart (`board.system->reboot`), or a radio core reload where the board can't restart |
| `ui_svc_focus_target` (one-shot PTT to the focused row), `ui_svc_dmr_timeslot`, `ui_svc_dmr_color_code`, `ui_svc_mode`, `ui_svc_hear_all` | dmr-voice / radio core | `ui_svc_radio.c` on the new requests `radio_req_dmr_focus`, `radio_req_dmr_timeslot`, `radio_req_dmr_color_code`, `radio_req_mode`, `radio_req_dmr_hear_all` (radio-features.md, dmr-voice.md). Timeslot, color code and mode are temporary changes (temp bits 0x20, 0x40, 0x80) that BACK on Home undoes and Save to channel keeps; the hear-all latch ends with the channel, a mode change or BACK on Home (not with the call) |
| `ui_svc_privacy`, `ui_svc_monitor`, `ui_svc_vox` | radio core | `ui_svc_radio.c`: `radio_req_dmr_privacy` on the active privacy slot (else the first set one), `radio_req_monitor` as a latch, the `vox` setting (all / off) with `radio_view_t.vox_on` for the tile |
| `ui_svc_emergency`, `ui_svc_feat_ext` | dmr-ext | weak seams: the EMERGENCY key action calls `ui_svc_emergency`, and dmr-ext reports `UI_FEAT_EMERGENCY` through its own strong `ui_svc_feat_ext`; without it the key says "Emergency isn't in this build" |
| `ui_svc_scan`, `ui_svc_scan_skip`, `ui_svc_feat_radio`; scanning state from `radio_view_t.scan` | radio-features | `ui_svc_radio.c`: `radio_req_scan` (toggle, stop, nuisance) |
| `ui_svc_roam` (`UI_FEAT_ROAMING`); roaming state from `radio_view_t.roam` (the Home chip, `ROAM` in the strip, Tools > Roaming, the "Moved to" toast) | radio core (CONFIG_DMR_ROAMING) | `ui_svc_radio.c`: `radio_req_roam` |
| `ui_svc_beep` | radio-features | `ui_svc_radio.c`: `radio_req_beep(BEEP_KEY)` on every press and long press |
| `ui_svc_apply` (key tones, tone volume, TX timeout, backlight time, battery saver, talk permit tone, mic gain) | radio-features | `ui_svc_radio.c`: `radio_set_settings`, at start-up and on each change; mic gain goes to `ambeVoiceSetMicGain` (DMR: 2 dB a step on the samples before the vocoder, 5 = unity) |
| `ui_svc_beep` kinds (key, error, message, battery low) | radio-features | `ui_svc_radio.c`: a table from `UI_BEEP_*` to `BEEP_*` (`BEEP_MESSAGE` is the new-message tone) |
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

Screens for features that are off (`CONFIG_DMR`, `CONFIG_HOTSPOT`, `CONFIG_VFO_FM`, `CONFIG_SCAN`, data features) are
left out or dimmed with the feature. Some radios' default images have only a few KB free (`docs/feature-matrix.md`); the UI was about 50 KB when last measured (`gfx` + fonts about 11 KB, screens and behavior the rest). The frame buffer (40 KB) bounds the display: 240x320 at
16 bpp renders only in the simulator, which overrides `GFX_FB_BYTES` and `GFX_MAX_ROWS`.

## 6. Debug link

The link section (`docs/spec/debuglink.md`) carries the UI's state for HIL tests: `ui.screen`, `ui.depth`,
`ui.focus`, `ui.count`, `ui.locked`, `ui.banner`, `ui.toast`, `ui.query`, `ui.draws`. Tests check these rather than
pixels, so they hold on any display.

## 7. Host simulator and tests

`tests/host/sim/` runs the real `core/ui` files against a simulated board (any display geometry, any of the key
maps K DM-1701, M GD-77 style, V MD-380 style, F few keys), radio core, the usability-test codeplug (3 zones,
40 channels, 30 contacts) and the services of section 4.

```
make -C tests/host run-ui                                      # gfxTest, uiFlowTest, the layout check
make -C tests/host ui-gallery                                  # docs/ux/screens/<class>/<screen>.png, sheets/<class>.png
make -C tests/host ui-sheets GEOM=128x64                       # contact sheet only, tests/host/out/sheets/
tests/host/uiSim --geom 160x120i5 --script "ok 4 down ok expect:zone=1" --out zone.png --scale 3
```

Geometries: `160x120i5` (DM-1701: 160x120 visible, 5 px inset left and right), `160x128` (MD-380 class), `120x160`
(DM-32: the 240x320 panel at half resolution), `128x64` (mono), `240x320`, or any `WxH`, `WxHiN` (inset N all round),
`WxHm` (mono). Script tokens:

| Token | Meaning |
|---|---|
| `up down left right ok back quick ctx 0..9 tt mon user1..user4 emerg enc+ enc-` | a short press (counted) |
| `long:<key>` | a long press (counted) |
| `ptt`, `ptt:<ms>`, `pttdown`, `pttup` | PTT (counted) |
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
BACK reaches Home, long QUICK locks, long CONTEXT opens the talk target picker on a DMR channel, the status strip is drawn (except About while idle),
PTT shows TX, and every field-use screen is at most 3 presses deep. The About check scrolls the page to its end at
every display class and fails if any text was shortened.

`uiSim --ellipsis [GEOM]` lists every text shortened with a middle ellipsis on the gallery screens and the settings
pages (lists scrolled through): `gfx_text_fit` reports each one through `gfx_fit_trace` in host builds
(`GFX_FIT_TRACE`). Use it after changing wording or layout.

`uiSim --layout [GEOM]` (run by `run-ui`, so CI) is the layout check: the gallery screens, the settings pages and the
worst-case screens at every geometry, with ordinary and worst-case content. `gfx_draw_trace` (host builds) reports
every text, fill and clear with its rectangle; the check fails on a text outside the safe area or cut by its clip box,
overlapping texts, words with no space between them, a box over part of a text, an overlay cutting a line in half, and
a middle ellipsis with ordinary content (`docs/ux/wireframes/README.md`). `ODR_LAYOUT_SHOTS=DIR` saves every screen
checked.

HIL: `tests/hil/test_ui.py` (`tests/hil/README.md`) replays the main sequences on the radio over the debug link.

## 8. Decisions and open points

- Task flow 12 budgets 3 presses into the VFO, but flow 2 opens Channels on the current zone (so a zone two away is
  2 presses); the VFO row is first, so entering it takes 4 (OK 4 Up OK). The test uses 4; the owner decides.
- Power on radios with many levels: the Quick tile cycles three presets (low, the board's default, highest); the full
  list is in Settings.
- Long BACK on Home no longer opens diagnostics (P3: one meaning per key); they are in Tools 7. The firmware boots
  into Home.
- The long-press and key-repeat times are stored in `settings_t` and handed to the input layer (`input_set_timing`,
  clamped to 200..3000 ms and 40..1000 ms); auto lock locks the keys after its time without a key press (counted on
  Home and in menus alike; injected keys count).
- Mic gain acts on DMR only, in software before the vocoder; FM's microphone path is the modem's (no hook yet). The
  GPS options are stored but act on nothing (no GPS owner); they are dimmed on boards without `CAP_GPS`.
- Voice prompts are dimmed ("Voice prompts aren't built yet: no recorded speech").
- Theme switches the color palette (dark, light, high contrast); on 1 bpp displays the row is dimmed.
- The VFO is FM only, so the Mode tile is dimmed in the VFO ("VFO: FM only") instead of a refusal.
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
