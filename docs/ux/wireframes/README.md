<!-- SPDX-License-Identifier: MIT -->
# Wireframes

ASCII wireframes of the main screens for the three display classes. They show layout, hierarchy and content, not final pixels.

| File | Display class | Grid used in the drawings |
|---|---|---|
| `240x320.md` | 240x320 color, portrait (landscape variant included) | 30 columns x 26 rows of an 8x12 cell; LARGE text = 2x |
| `160x128.md` | 160x128 color | 20 columns x 10 rows of an 8x12 cell (some rows 8x8) |
| `128x64.md` | 128x64 monochrome | 21 columns x 8 rows of a 6x8 cell; LARGE = 2x height |

Conventions:

- `[TX]` `[RX]` pills: TX is red with the word, RX green with the word, idle gray. The word is always present (P5).
- `▮▮▮▯` signal bars, `84%` battery, `✉2` unread messages (an envelope icon and the count), `⌖` GPS fix, `🔒` the key lock (a padlock icon, on mono too), `♦` privacy, `•` marks a temporary change.
- UPPERCASE in a drawing marks the large font, not uppercase text.
- The bottom hint row lists the keys that matter on that screen.

Region order is identical on every class (status strip, place, talk target, activity, hint row), so a user who moves between radios finds things in the same place.

The drawings are the design intent. What the firmware draws is in `../screens/<geometry>/` (`make -C tests/host ui-gallery`), one folder per display: `160x120i5` the DM-1701 (160x120, the lens hides 5 px left and right; top and bottom are used to the edge), `160x128` the MD-380 class, `240x320i10x5` the DM-32 (16 colors, anti-aliased fonts, lists and labels in 9x18; the bezel hides 10 px left and right, 5 px top and bottom), `128x64` mono, `240x320` full resolution. `../screens/sheets/<geometry>.png` shows every screen of a display on one page. Words on the screens follow the firmware, not these sketches (for example "TG 91" with a space, ages as "2m ago", hints as `Back` alone where Back just goes back).

## The layout check

`uiSim --layout` (part of `make -C tests/host`, so CI) draws every gallery screen, every settings page and a set of worst-case screens at every geometry (in each theme on color ones), once with the usability-test codeplug and once with worst-case content: 16-character channel, zone and contact names, 4-digit channel numbers, 8-digit talkgroups, 7-digit IDs, 31-character talker aliases, 143-character messages, 80-channel zones, every status mark at once (lock, unread, scan, low battery, hear all, privacy, a 5-character power level). It fails on:

- a text outside the safe area, or cut by its box (a quick settings tile);
- two texts overlapping, or side by side with no space between them;
- a box drawn over part of a text, or an overlay (banner, toast) cutting a line in half;
- a text drawn invisibly: its cells one color after it is drawn (text the color of what is under it, or a blank glyph);
- a text shortened in the middle with an ellipsis on ordinary content (worst-case content may be shortened; a message preview cut at its end is fine).

`ODR_LAYOUT_SHOTS=DIR uiSim --layout` saves every screen it checked; `uiSim --ellipsis` lists every shortened text.

Screens drawn in each file: Home idle (DMR and FM), RX, TX, dual watch Home, incoming message banner and message view, quick settings, Find (contact search), caller actions, Hub, talk target picker.
