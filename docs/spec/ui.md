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
| `ui.c` | the UI task: splash (the mark, the name, the two CPS boot text lines from the codeplug, board and version; `ui_draw_splash` in ui_screens.c), 50 ms frame pacing, backlight, status copy for the debug link |

`ui_app.c` and `ui_menus.c` never call the RTOS. The firmware drives them from `ui.c`; the host simulator
(section 7) drives the same files.

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
| Recent | `recent` | long CONTEXT, Hub 3 |
| Talk target picker | `picker` | TALK_TARGET (SK2), Quick 8 |
| Diagnostics | `diag` | Tools 7, Settings > Advanced |
| Roaming | `roaming` | Tools 8 (7 without diagnostics): roaming on for this zone, roam now, lock to this site ([dmr-roaming.md](dmr-roaming.md) 6.2) |

The gallery (section 7) shows every screen at every display class.

## 3. Key meanings

The same on every screen (information-architecture.md, key maps 7.1 to 7.5):

| Action | Press | Long press |
|---|---|---|
| BACK | up one level, or clear one thing on Home (entry, banner, scan, reply, hear all, temporary change) | Home |
| SELECT | open / choose | the alternative (save, permanent, Quick on radios without a QUICK key) |
| QUICK | Quick settings (closes them again) | lock / unlock the keys |
| CONTEXT | the focused item's actions; a banner's target while one is up | Recent |
| TALK_TARGET | the talk target picker | |
| digits | Find on Home, numbered rows in lists, T9 letters in filters | speed dial (a toast for now) |
| PTT | talk to the focused person or talkgroup once (one-shot), else the channel's target | |

Radios without some keys fall back as the key maps say (few-key map F in the tests). Up / Down on Home change the
channel, USER_1 / USER_4 the zone, USER_3 the power, long USER_3 speaks the status (a toast until voice prompts exist).

Small displays (under 200 px tall) merge a screen's title into the status strip: the state pill shows only when not
idle, the bars and battery icon give way, the battery percent stays, and only the name part of the title is
shortened with a middle ellipsis (the prefix stays whole: "Quick COUNTY DMR", not "Quick ..tions").

## 4. Data other packages own

The UI asks through `ui_svc.h`. Each call has a weak definition in `ui_svc_stub.c` that behaves as "not in this
build" (screens dim what isn't there and say why on SELECT); the owning package's strong definition replaces it at
link time. `ui_svc_features()` ORs the bits each package reports.

| Calls | Owner | State in this branch |
|---|---|---|
| `ui_svc_talk_to` temporary / default | dmr-voice (radio core) | `ui_svc_radio.c`: `radio_req_dmr_target`, `radio_req_dmr_target_clear` |
| `ui_svc_talk_to` permanent (`ui_svc_save_talk_target`), `ui_svc_save_channel`, `ui_svc_setting_get/set`, `ui_svc_feat_storage` | storage-cps | `ui_svc_storage.c`: the channel is rewritten with `codeplug_set_channel` (power, bandwidth, tones; a temporary squelch goes to the default squelch preference); settings are read from where they are kept: the radio core's settings (`radio_get_settings`, saved by the radio core), `settings_t` (brightness, default squelch, hotspot allowed), the data service's answers (`dmr_data_get_settings`); the reply window, mic gain, auto lock, long press and repeat times and the GPS options have `settings_t` fields (appended, settings-store.md); only the ones without a field stay in RAM. Both saves run as a storage job on the housekeeping task (`storage_job_post`), not on the UI task: the change takes effect for now at once, the temporary mark goes when the write lands, and a failed write raises `UI_EV_SAVE_FAILED` ("Couldn't save: still for now only"). Save to channel also writes the temporary mode, color code and timeslot |
| `ui_svc_factory_reset` | storage-cps | `ui_svc_storage.c`, as a storage job: `settings_t` back to its defaults and saved, then `storage_native_erase` (the native area, only when it carries the `ODRN` magic or is already blank; never the legacy codeplug, calibration or a foreign region), then a restart (`board.system->reboot`), or a radio core reload where the board can't restart |
| `ui_svc_focus_target` (one-shot PTT to the focused row), `ui_svc_dmr_timeslot`, `ui_svc_dmr_color_code`, `ui_svc_mode`, `ui_svc_hear_all` | dmr-voice / radio core | `ui_svc_radio.c` on the new requests `radio_req_dmr_focus`, `radio_req_dmr_timeslot`, `radio_req_dmr_color_code`, `radio_req_mode`, `radio_req_dmr_hear_all` (radio-features.md, dmr-voice.md). Timeslot, color code and mode are temporary changes (temp bits 0x20, 0x40, 0x80) that BACK on Home undoes and Save to channel keeps; the hear-all latch ends with the channel, a mode change or BACK on Home (not with the call) |
| `ui_svc_privacy`, `ui_svc_monitor`, `ui_svc_vox` | radio core | `ui_svc_radio.c`: `radio_req_dmr_privacy` on the active privacy slot (else the first set one), `radio_req_monitor` as a latch, the `vox` setting (all / off) with `radio_view_t.vox_on` for the tile |
| `ui_svc_emergency`, `ui_svc_feat_ext` | dmr-ext | weak seams: the Emergency screen calls `ui_svc_emergency`, and dmr-ext reports `UI_FEAT_EMERGENCY` through its own strong `ui_svc_feat_ext`; until then the screen says "not in this build" |
| `ui_svc_scan`, `ui_svc_scan_skip`, `ui_svc_feat_radio`; scanning state from `radio_view_t.scan` | radio-features | `ui_svc_radio.c`: `radio_req_scan` (toggle, stop, nuisance) |
| `ui_svc_roam` (`UI_FEAT_ROAMING`); roaming state from `radio_view_t.roam` (the Home chip, `ROAM` in the strip, Tools > Roaming, the "Moved to" toast) | radio core (CONFIG_DMR_ROAMING) | `ui_svc_radio.c`: `radio_req_roam` |
| `ui_svc_beep` | radio-features | `ui_svc_radio.c`: `radio_req_beep(BEEP_KEY)` on every press and long press |
| `ui_svc_apply` (key tones, tone volume, TX timeout, backlight time, battery saver) | radio-features | `ui_svc_radio.c`: `radio_set_settings`, at start-up and on each change |
| backlight dim / off | radio-features | `ui.c`: `radio_backlight_pct` (dims on a low battery too) |
| `ui_svc_msg_*`, `ui_svc_radio_check`, `ui_svc_call_alert`, `ui_svc_poll`, `ui_svc_feat_data` | dmr-data | `ui_svc_data.c` on `dmr_data_api.h`; `ui_svc_msg_delete` is `sms_inbox_delete` |
| `ui_svc_position` | GPS (no owner yet) | stub: "No GPS on this radio" |
| talker alias, private call start | dmr-voice | read from `radio_view_t.dmr`; `UI_EV_PRIVATE_CALL` is not raised by anyone yet |

Inbound events go through `ui_notify()` (a ring of 4, safe from any task) or, for packages the UI polls, through
`ui_svc_poll()` once per UI pass. They become banners for 8 s: message received, delivered / not delivered, call
alert, alert answered, radio check answered (with the round trip) or not, private call, emergency, hotspot started.

## 5. Build switches

| Switch | Default | Off |
|---|---|---|
| `CONFIG_UI_SETTINGS` | y | the Settings menu is left out (about 3.7 KB); settings keep their stored values and are set with qdmr; Settings shows one row saying so, Find finds no settings |
| `CONFIG_DIAG` | y | no diagnostics page (Tools has no Diagnostics row; Roaming moves up to 7) |
| `CONFIG_FONT_XL` | n | the 16x32 font for 240 px and wider displays |

Screens for features that are off (`CONFIG_DMR`, `CONFIG_HOTSPOT`, `CONFIG_VFO_FM`, `CONFIG_SCAN`, data features) are
left out or dimmed with the feature. The DM-1701 default image is about 188 KB of 428 KB; the UI is about 50 KB of it
(`gfx` + fonts about 11 KB, screens and behavior the rest). The frame buffer (40 KB) bounds the display: 240x320 at
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
make -C tests/host run-ui                                      # gfxTest and uiFlowTest
make -C tests/host ui-gallery                                  # docs/ux/screens/<class>/<screen>.png
tests/host/uiSim --geom 160x120i5 --script "ok 4 down ok expect:zone=1" --out zone.png --scale 3
```

Geometries: `160x120i5` (DM-1701: 160x128 panel, 5 px inset), `160x128`, `128x64` (mono), `240x320`, or any
`WxH`, `WxHiN` (inset N), `WxHm` (mono). Script tokens:

| Token | Meaning |
|---|---|
| `up down left right ok back quick ctx 0..9 tt mon user1..user4 emerg enc+ enc-` | a short press (counted) |
| `long:<key>` | a long press (counted) |
| `ptt`, `ptt:<ms>`, `pttdown`, `pttup` | PTT (counted) |
| `wait:<ms>` | time passes |
| `rx:<src>,<dst>,g\|p[,name]`, `rxend`, `call:<src>,<dst>,g\|p` | a DMR call heard |
| `msg:<src>,<text>`, `alert:<src>`, `emergency:<src>`, `privcall:<src>`, `hotspot:on\|off`, `battery:<pct>` | events |
| `shot:<name>` | save the screen |
| `expect:<key>=<value>` | check: `screen focus depth count zone pos vfo freq mode tx txid txpriv talk locked query banner toast power sq ts cc temporary writes checks sent text scanning privacy hearall monitor hotspot beeps radio_tot radio_backlight` (`_` = space, `*` = anything) |

`uiFlowTest` runs every task flow of `task-flows.md` with the press budgets of `usability-tests.md` on four display
and key map pairs (K at 160x120i5, M at 128x64, V at 160x128, K at 240x320) plus the few-key fallbacks: 160 flow runs.
A crawler then visits every reachable screen and checks the rules that hold everywhere: BACK leaves every screen, long
BACK reaches Home, long QUICK locks, long CONTEXT opens Recent, the status strip is drawn, PTT shows TX, and every
field-use screen is at most 3 presses deep.

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
- Mic gain and the GPS options are stored but act on nothing yet (no HAL hook for mic gain, no GPS owner).
- Compose offers canned texts only ("OK", "On my way", "Call me", "Can't talk now"); free text is dimmed.
- Deleting a message has no undo (`sms_inbox_delete` removes it from the RAM inbox).
- Save to channel and the permanent talk target write the codeplug as a storage job on the housekeeping task, so the
  UI no longer stalls for the flash write. A permanent target must be a stored contact: a typed ID is set for now at
  once and the job then says "Not a stored contact: for now only".
- One-shot PTT: the row with the focus in the picker or a list is handed to the radio core (`radio_req_dmr_focus`);
  PTT then talks to it once, and the radio follows it for a reply window (the reply time setting, else the
  codeplug's hang time) before going back to the channel's talk target. A channel change, the call ending outside
  the window, Reply cancel or a new manual target end it.
