<!-- SPDX-License-Identifier: MIT -->
# UI survey: what good radio and device interfaces do

This is the background for `principles.md`. It looks at commercial two-way radios, amateur DMR radios, other open firmware, handheld gadgets, phones and vehicle/aviation HMI guidance, and notes for each what OpenDigitalRadio should borrow and what it should avoid.

Clean-room note: this survey deliberately leaves out the aftermarket firmware and its derivatives. Nothing here, and nothing in `docs/ux/`, is based on their menus, screens or key mappings.

Observations come from public user guides, reviews, forum threads and general product knowledge. Where something is the author's judgment and not a documented fact, it says so.

## 1. Commercial land mobile radios

### Motorola MOTOTRBO (XPR 7000/7000e, SL series) and APX

What they do well:

- **Home screen built around "where am I and what will PTT do".** The idle screen shows the zone, the channel alias and the current talk target, with a slim status icon row (RSSI, battery, scan, privacy, GPS, messages) across the top. Little else.
- **Programmable buttons with short press and long press.** The side buttons and the top button each carry two functions (short and long) set by the dealer, so the user's two or three most frequent jobs (monitor, scan, privacy, talkaround, power) never need the menu.
- **One Touch Access / One Touch Call.** A single button can start a group call, private call, call alert, text or phone call to a preset target. The guides list "make a group, private or All Call with the programmable number key": number keys double as speed dial on the keypad models.
- **Talkback in hang time.** After a call, pressing PTT during the system hang time replies to the group or person who just spoke. The display shows this.
- **Consistent menu navigation.** Up/down move, OK enters, Back leaves, and long-press Back goes Home on several models. The same pattern runs through contacts, call log, messages, scan lists and utilities.
- **Distinct alert tones.** Talk permit tone, channel busy, invalid key, low battery, call alert and emergency each have a different sound, so the radio can be used without looking.
- **APX: emergency is physically distinct** (orange top button, recessed, hold to activate) and the top display on APX models lets you read the channel with the radio on your belt or shoulder.
- **APX: a large, high-contrast font and glove-friendly key spacing**, plus a dedicated two- or three-position toggle (A/B/C) and a concentric channel knob for operation by feel.

What to avoid:

- **Dealer-locked behavior.** Much is only changeable in CPS, so the user can't fix a bad button assignment in the field. We should let users reassign keys on the radio.
- **Deep menus for occasional settings.** On keypad models, things like the talkaround toggle or power level are several levels down unless a button is programmed for them.
- **Icon-only status.** New users don't know what half the status icons mean, and there is no on-radio legend. Every icon should be explainable from the radio itself (a "what does this mean" view).

### Kenwood NX series (NX-1000/3000/5000)

Borrow:

- **Very short key repertoire** (often only a few front keys on non-keypad models) with consistent short/long semantics. Shows that a radio with four keys can be fully usable if the key layer is designed for it.
- **Voice announcement** of channel and zone on change, configurable, useful for blind operation and for operators who can't look down.
- **Scan with "priority" and "nuisance delete"**: a single press removes a busy channel from the current scan pass without editing the scan list.

Avoid:

- **Different meaning for the same key on different screens** (on some models the same key is "Scan" at idle and "Select" in a list, configured by the dealer). It saves keys but breaks muscle memory.

### Icom (IC-F series, IP/LTE, D-STAR amateur handhelds like ID-52)

Borrow:

- **ID-52/ID-50: the "DR" (D-STAR repeater) mode** picks repeaters and destinations from lists ("From" and "To"), the closest thing in amateur radio to a phone-like "who do I want to talk to" flow. Users regularly compare DMR radios unfavorably with it.
- **Quick menu (QUICK key)** with the context-appropriate few settings for the current screen. This is the direct ancestor of our QUICK action.
- **Call sign and name of the station heard, plus RX history** with one-key reply ("RX>CS" picks the last heard as the destination).

Avoid:

- **Very long settings lists** with flat, alphabetical-ish ordering and cryptic abbreviations. The quick menu helps, but the full menu is long.

### Hytera (PD/HP series)

Borrow:

- **Icon grid main menu** (Contacts, Messages, Call Records, Zone, Scan, Settings): a small set of top-level destinations, each one a noun the user already knows.
- **Clear call state screens**: "Group Call", "Private Call", "Emergency" banners with the far party.

Avoid:

- **Too many top-level icons on later models** (a dozen plus), many of them rarely used (Recording, Man Down, Lone Worker setup). Rare things belong one level down.

## 2. Amateur DMR radios

### Anytone AT-D878UV / AT-D578UV

Borrow:

- **Icon main menu** with Talk Group, SMS, Call Log, Zone, Scan, Settings, GPS, Digital Monitor. A reviewer who expected a "Chinglish" DMR menu found it "pretty easy".
- **Talkgroup entry on the radio** (type a TG number and transmit to it without programming it) and **digital monitor** (promiscuous) per timeslot. Users praise being able to "key up a talkgroup like TAC310 even if it isn't in the radio".
- **Roaming between repeaters** from a list.
- **Last call / call log with reply.**

Avoid (public complaints):

- **Steep learning curve**: a reviewer found it "much more difficult and less intuitive" than Icom D-STAR radios; others say the manual "tells you what controls do but gives no examples of how to do anything" and asks for step-by-step instructions for each setting. Our task flows and on-radio hints are the answer.
- **Fear of breaking the codeplug**: a vendor blog exists just because users fear "accidentally erasing their meticulously organized channel list" when using the list function to switch talkgroups. The UI must make it obvious when a change is temporary and when it writes the codeplug.
- **Range/format surprises**: a user's 8-digit TG got an "out of range" error with no explanation of the allowed range. Errors must say what is allowed.
- **Many hidden modes** (sub channel, A/B, talkgroup list vs digital contact list, "Talk Group" vs "Contact" vs "Receive group") whose interplay users find confusing.

### TYT MD-380 / MD-UV380, Baofeng DM-1701, Retevis RT3S / RT84 (stock firmware)

Avoid (public complaints, consistent across forums):

- **You can't hear or reply to other talkgroups without reprogramming.** The stock radio usually listens to one TG; a RadioReference newcomer couldn't hear active traffic and experienced users guessed at a wrong timeslot. md380tools and experimental firmware became popular largely for "promiscuous" and "talkgroup-free" modes that reply to whatever was heard. Promiscuous monitoring and reply-to-heard must be first class.
- **The channel is the only unit of choice**: TG, TS and CC are fixed per channel, so a hotspot user needs dozens of channels that differ only by TG.
- **Menus with abbreviated, inconsistent labels** and no way back to idle except repeated Back presses.
- **No DMR ID database on the radio** (or a truncated one): the screen shows a bare number.

### Retevis / Radioddity budget DMR in general

Avoid: slow screen redraws after key presses (users press again, overshoot), key beeps that can't be separated from talk permit tones, and settings whose effect only appears after a power cycle.

### Yaesu (FT-5D, FTM-500, System Fusion)

Borrow:

- **Touch-free "band scope" and "memory list" views** that show many channels at once. Useful for our scan view on 240x320.
- **Dedicated DX/"GM" (group monitor) screen** showing who is in range: an analog for our last-heard list.
- **Backtrack (GPS) screen**: a clear single-purpose page for "where is that station / where did I start".

Avoid:

- **Overloaded multifunction knobs** where push, push-and-hold, turn and push-and-turn all mean different things. Users regularly report not knowing which mode the knob is in.

## 3. Open firmware and open devices

### OpenRTX

Borrow:

- **Multi-radio from one core** with a small set of keys and a clear separation of radio-independent UI from per-device input. Same shape as our logical action layer.
- **Voice prompts for vision-impaired operators** (added November 2022): an existence proof that a small team can make a radio usable without looking at it.
- **M17 and modern codec support** shows the value of a clean UI layer that isn't tied to a single mode.

Avoid: the UI is still sparse (functional more than designed) and some radios have fewer features than their hardware allows. We should not wait for feature completeness to design the UX.

### Meshtastic devices (T-Deck, T-Echo, Heltec, RAK WisBlock with screens)

Borrow:

- **Paged home screens** on tiny OLEDs: one press cycles pages (nodes, messages, position, battery), each page with one job. Proven on 128x64.
- **Message-first design**: unread count, a single-line preview, and "reply" from the message itself.
- **Canned messages** selectable with an encoder or a single button. Directly useful for TMS on radios without a full keyboard.
- **Node list with distance and bearing** from GPS: a model for last heard with position.

Avoid:

- **Hidden single-button gestures** (triple press = GPS toggle, long press = shutdown on some boards). Users discover them by accident. We avoid triple presses and anything a pocket can do.
- **Settings only reachable from the phone app** on some boards. Every setting a user needs in the field should be on the radio.

## 4. Garmin handhelds (GPSMAP, eTrex, inReach)

Borrow:

- **Page/back model**: a fixed ring of main pages (map, compass, trip computer, main menu) navigated with one key, with "Quit"/Back always going up one level.
- **Customizable data fields** on the main pages. On our 240x320 home, the user could pick which secondary fields appear (GPS, battery volts, TX timer).
- **Glove-friendly physical keys** with a large tactile difference between the "enter" key and the others.
- **inReach: canned messages and a "preset" message** that can be sent in two presses.

Avoid: **long, nested setup menus** (Setup > System > ...) organized by internal subsystem rather than by what the user wants.

## 5. Phone patterns

Borrow:

- **Quick settings**: a single gesture shows the five to nine most toggled settings as tiles with their current state. Our QUICK action.
- **Universal search**: type anything (contact, app, setting) from the home screen. Old feature phones did this with T9 too: typing on the home screen dialed digits and simultaneously matched contact names. Our "type to find".
- **One back stack**: Back always means "where I was", Home always means "start". Feature phones (Nokia Series 40) used a left soft key = select, right soft key = back, and the center/OK key for the main action on every screen. People learned it once.
- **Notifications that don't steal focus**: a banner appears, the current task continues, and the notification waits in a list.
- **Settings search and settings grouped by intent** ("Sound & vibration", "Display", "Battery", "Connections", "Privacy") rather than by hardware module.
- **Undo instead of confirm** for reversible actions ("Message deleted. Undo").
- **Accessibility built in**: screen readers announce the focused item; large text and high contrast are system settings, not app settings.

Avoid:

- **Gestures with no visual affordance**, and **hamburger menus that hide primary actions**. A radio has no touch screen anyway, but the same applies to hidden long-presses.

## 6. Automotive and aviation HMI guidance

- **NHTSA Visual-Manual Driver Distraction Guidelines (2013, Phase 2 for portable devices 2016)**: no single glance away from the road of 2 s or more, total eyes-off-road time of 12 s or less per task, about six inputs per task. Some functions (manual text entry, scrolling text) are locked out while driving. For the MD-9600 mobile and for any radio used while walking or driving, task budgets in `task-flows.md` use these numbers: every common task completes in six or fewer presses, and every screen can be read in one glance under 2 s.
- **Aviation (FAA AC 25.1302 / human factors for avionics, Garmin G1000 style)**: consistent knob behavior (outer knob coarse, inner fine), a "direct-to" key that does the most common navigation task in one press, a fixed location for each kind of information, color reserved for meaning (red = warning, amber = caution, green = active/safe), and checklists that can't be left in an ambiguous state.
- **Gloves and vibration**: physical keys over touch, no double-press timing requirements, minimum target separation, auditory confirmation, and hold-to-activate for anything with consequences.

Borrow for us: glance budgets, color only for meaning (TX red, RX green, warnings amber, never the only cue), fixed screen regions, coarse/fine knob semantics on encoders, hold-to-activate for emergency.

## 7. Summary of the most common DMR usability complaints

1. Can't hear or reply to a talkgroup that isn't programmed (no promiscuous, no reply-to-heard, TS/CC fixed per channel).
2. Screen shows an ID number, not a name; no alias display.
3. Steep learning curve and manuals that list controls instead of tasks.
4. Fear that a field change will damage the codeplug.
5. Many presses to change power, TG or timeslot; settings buried in long flat menus.
6. Errors with no guidance ("Out of range", "Invalid").
7. Too many modes whose state isn't visible (A/B, sub channel, VFO vs channel, TG list vs contact list).
8. No easy way back to the idle screen.
9. Texting is painful (multi-tap only, no canned replies, inbox hard to find).
10. Hotspot and data modes require a computer to switch.

Every principle in `principles.md` exists to remove at least one of these.

## Sources

- [BridgeCom: Menu guide to the AnyTone 878 Plus](https://www.bridgecomsystems.com/blogs/bridgecom-tx-rx-blog/menu-guide-to-the-anytone-878-plus-by-hamradioconcepts)
- [BridgeCom: How to use the List function on your AnyTone radio](https://www.bridgecomsystems.com/blogs/bridgecom-official-blog/how-to-use-the-list-function-on-your-anytone-radio)
- [BridgeCom: SevenFortyOne overviews the AnyTone AT-D878UV](https://www.bridgecomsystems.com/blogs/bridgecom-tx-rx-blog/sevenfortyone-overviews-the-anytone-at-d878uv)
- [BridgeCom: AnyTone AT-D878UV PLUS review by Phil W](https://www.bridgecomsystems.com/blogs/bridgecom-brag-board-testimonials/anytone-at-d878uv-plus-is-a-great-radio-review-by-phil-w)
- [RadioReference: DMR basics thread](https://forums.radioreference.com/threads/dmr-basics.491080/)
- [VA3XPR: experimental firmware for the TYT MD-380](https://va3xpr.net/experimental-firmware-tyt-md-380/)
- [dmrhub: md380tools overview](https://dmrhub.ru/tech/en/md380tools)
- [Motorola Solutions: SL series user guide](https://www.motorolasolutions.com:443/content/dam/msi/docs/products/mototrbo/portable-radios/user-guides/user_guide_sl_series.pdf)
- [XPR 7550/7580 user guide](https://northgeorgiacommunications.com/wp-content/uploads/2022/05/xpr7550_xpr7580userguide.pdf)
- [FOSDEM 2024: OpenRTX slides](https://archive.fosdem.org/2024/events/attachments/fosdem-2024-2537-openrtx-an-open-source-firmware-for-ham-radio-devices/slides/22760/OpenRTX_ePTXH62.pdf)
- [hamradio.my: OpenRTX overview](https://hamradio.my/openrtx-open-source-firmware-powering-the-future-of-digital-radios/)
- [SWOV: Visual-manual NHTSA driver distraction guidelines for portable and aftermarket devices](https://swov.nl/en/publicatie/visual-manual-nhtsa-driver-distraction-guidelines-portable-and-aftermarket-devices)
- [National Academies Press: NHTSA guidelines chapter](https://nap.nationalacademies.org/read/26550/chapter/4)
- [Dashboard Symbols: Two second rule](https://dashboardsymbols.com/2013/04/two-second-rule-distraction-guidelines-seek-to-limit-eyes-on-text/amp/)
