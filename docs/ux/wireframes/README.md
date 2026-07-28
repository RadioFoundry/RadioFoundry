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
- `▮▮▮▯` signal bars, `84%` battery, `✉2` unread messages, `⌖` GPS fix, `🔒` is drawn as a padlock glyph `[L]` on mono, `♦` privacy, `•` marks a temporary change.
- UPPERCASE in a drawing marks the large font, not uppercase text.
- The bottom hint row lists the keys that matter on that screen.

Region order is identical on every class (status strip, place, talk target, activity, hint row), so a user who moves between radios finds things in the same place.

Screens drawn in each file: Home idle (DMR and FM), RX, TX, incoming message banner and message view, quick settings, Find (contact search), caller actions, Hub, talk target picker.
