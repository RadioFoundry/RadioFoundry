<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Documentation style

This guide covers how to write and organize RadioFoundry documentation. It follows the
[Google developer documentation style guide](https://developers.google.com/style); where this page is silent, that
guide applies.

## Where a page goes

Every page has one audience and one job. The [documentation map](../README.md) lists every page and the topic it
owns.

| Directory | Audience | Kind of page |
|---|---|---|
| `docs/user/` | People who use the radio | The user manual: tasks and the settings reference. Built to the site and a PDF |
| `docs/dev/` | Contributors | How to build, test, release, and extend the project |
| `docs/spec/` | Contributors | Reference: what the firmware does and the protocols it speaks |
| `docs/testing/` | Contributors | Test plans, bench procedures, and run records |
| `docs/ux/` | Contributors | User interface design: principles, task flows, wireframes |
| `docs/` (top level) | Contributors | Build switches, the feature matrix, the clean-room rules |

## One home for each fact

- Before you add a fact, find its home in the [documentation map](../README.md). Put it there, and link to it from
  other pages instead of repeating it.
- A new page gets a row in the map in the same commit. `tools/check_doc_links.py` fails on a page the map doesn't
  list.
- A page that describes behavior of the firmware links to its spec in `docs/spec/`. The spec holds the detail.
- Values that come from the board registry (`boards/<id>/board.toml`) or the build (`config/features.mk`) appear only
  in generated blocks. Edit the source, then run `python3 tools/boards.py docs`.

## Describe what works

Documentation describes the firmware as it is on `main`. Track work in progress in GitHub issues, not in the docs.

- Don't write status notes such as "untested," "not confirmed on the radio," "planned," "for now," "TODO," or "not
  yet." To say which radios have a feature, link to the [feature matrix](../feature-matrix.md).
- Don't add dates, "measured on," or version history. Release notes hold history.
- Don't name a radio in a generic page. Say what a board provides (`CAP_*`, `board_t` fields) and link to the feature
  matrix. `tools/check_generic.py` enforces this rule; for details, see [AGENTS.md](../../AGENTS.md).
- State hardware facts as facts: "register `0x13` is `0x0396`." Don't describe where a fact came from.

## Voice and tone

- Address the reader as "you." Use the imperative for instructions: "Press **Menu**," not "You should press Menu."
- Use active voice and present tense. Say who acts: "The radio sends an acknowledgment."
- Use *must* for requirements, *can* for options, and *might* for possible outcomes. Avoid *should*.
- Don't attribute thought to software. The radio *detects*, *sends*, or *ignores*; it doesn't *see*, *know*, or
  *want*.
- Cut filler: *simply*, *just*, *easy*, *please*, *note that*, *in order to*, *basically*.
- Avoid time words that go stale: *currently*, *now*, *new*, *soon*, *at present*.
- Use contractions, including negative ones: *don't*, *isn't*, *can't*.
- Write US English: color, center, behavior.

## Sentences and paragraphs

- Keep sentences short, with one idea each. Put the subject and verb near the start.
- Put a condition before the instruction: "To save the channel, hold **OK**."
- Keep one term for one thing across all pages. The [glossary](glossary.md) sets the terms.
- Start each paragraph with its most important sentence.

## Headings

- Use sentence case, with no closing period.
- Start a task heading with a verb: "Program a channel." Use a noun phrase for a concept: "Channel modes."
- Don't start a heading with an *-ing* word, and don't skip heading levels. Each page has one H1.
- **Exception for specs.** Spec headings carry section numbers (`## 4. Frames`, `### 4.2 Tocall`). Code cites them
  as `docs/spec/aprs.md 4.2`, so they're stable identifiers. Don't renumber a section unless you update every
  citation (`git grep "aprs.md 4\.2"`).

## Procedures

- Use a numbered list, one action per step. Start each step with an imperative verb.
- Name the place before the action: "On the **Home** screen, press **\***."
- Put the result of a step in its own sentence after the action.
- Begin an optional step with "Optional:".
- A procedure with one step is a sentence, not a numbered list.

## Lists and tables

- Introduce a list with a complete sentence.
- Use numbered lists for sequences, and bullets for everything else.
- Keep list items parallel. End an item with a period when it's a sentence.
- Use a table for facts that share attributes: settings and their defaults, keys and their actions, fields and their
  sizes. Give every table a header row.

## Formatting

| Element | Format | Example |
|---|---|---|
| Keys, menu items, screen names, buttons | Bold | Press **Menu**, then select **Settings > Display**. |
| Files, paths, commands, flags, code symbols, registers | Code | Edit `config/features.mk`. Set `CONFIG_APRS=n`. |
| A term on first definition | Italic, once | A *codeplug* holds the radio's channels and contacts. |
| Commands to run | Fenced code block with a language | ` ```sh ` |

Don't use bold for emphasis. Don't use underline.

## Links

- Write link text that makes sense out of context: the page title or a short description. Never "here" or "this
  page."
- Use relative links inside the repository, so they work on GitHub, the site, and in the PDF manual.
- For a cross-reference, write "For more information, see *Page title*," with the title as the link.
- Link to a section with its anchor (`radio-features.md#3-scan`), and check it with `tools/check_doc_links.py`.

## Numbers and units

- Use numerals with units, separated by a space: 12.5 kHz, 1 W, 3.3 V, 20 ms.
- Name transmit power in watts (1 W, 100 mW), not by level names or numbers.
- Write hexadecimal as `0x` with uppercase digits in code font: `0x1FF000`.
- Use hyphens for ranges: 400-480 MHz.

## Words

| Use | Instead of |
|---|---|
| use | utilize, leverage |
| lets you | allows you to |
| to | in order to |
| for example | e.g. |
| that is | i.e. |
| through, with, by | via |
| stop, cancel | abort |
| run | execute (a program) |
| allowlist, denylist | whitelist, blacklist |
| primary, secondary | master, slave |
| doesn't respond | hangs |
| a commercial portable | brand names of commercial systems |

## Images

- Give every image alt text that says what it shows.
- Use the generated gallery screens (`docs/ux/screens/`) for the radio's display. Don't commit hand-made
  screenshots of the display.

## Translations

The user manual has translations in `docs/i18n/<lang>/`. Each translated page mirrors the English page with the same
path. When you change an English page, update the translations, or list them as an open task in the pull request.
For details, see [Translating](translating.md).

## File basics

- Start every page with the SPDX header:

  ```markdown
  <!-- SPDX-License-Identifier: MIT -->
  <!-- Copyright (c) 2026 The RadioFoundry Authors -->
  ```

- Name files in lowercase with hyphens. User manual pages start with a two-digit number that sets the reading order.
- Wrap lines at 120 characters.

## Checks

Run these checks after you edit documentation. `tools/ci.sh` runs them too.

```sh
python3 tools/check_doc_links.py    # links, anchors, and the documentation map
python3 tools/check_generic.py      # no radio names in generic pages
python3 tools/i18n.py --check       # translation catalogs
```
