<!-- SPDX-License-Identifier: MIT -->

# Translating

RadioFoundry has two sets of text to translate: the radio's (firmware) and the web site's (flasher, Codeplug Editor,
tools). They are separate catalogs with separate tools; a language can have one without the other. English is the
source of both. This page is the how-to; the rules are in [ui.md 2.5](../spec/ui.md#25-languages) (radio) and
[website.md, Translating](website.md#translating) (site).

| | Radio | Web site |
|---|---|---|
| Catalog | `core/ui/lang/<code>.po` (gettext) | `site/i18n/<code>.json` (flat JSON) |
| Language list | The catalogs that exist | `site/i18n/languages.json` |
| Tool | `tools/i18n.py` | `python3 -m rfoundrysite.i18n` (from `site/`) |
| Build switch | `CONFIG_LANG_<CODE>`, one per language; `CONFIG_LANG_DEFAULT_<CODE>` to start in it | None: every language is on the site |
| Missing string | Shown in English; fails CI | Shown in English; fails CI |
| Characters | ASCII and Latin-1 (U+00A0 to U+00FF) | Any Unicode |

## Translating the radio

### How it works

The source is English. A string the radio shows is marked where it is written:

| Mark | Where | Meaning |
|---|---|---|
| `TR("Text")` | In code | Returns the text in the language in use |
| `TRN("Text")` | In a static table (a `const` initializer) | Only marks the text for the tool; the code that shows it calls `tr()` on it |
| `tr(s)` | Where a table entry is shown | Looks `s` up; returns `s` when there is no translation |

`tools/i18n.py` reads the marks, keeps the catalogs in step and turns each catalog into a table the build links in.
With every language switched off, `TR()` is the text itself and the lookup is gone.

### Adding a language

1. Start the catalog: `python3 tools/i18n.py --new <code> <name>`. `<code>` is the ISO 639-1 code (`fr`), `<name>`
   the language's own name (`Français`). This writes `core/ui/lang/<code>.po` with every marked string, untranslated.
2. Add the build switches: `CONFIG_LANG_FR` builds the language in, `CONFIG_LANG_DEFAULT_FR` makes it the one the
   radio starts in (for `fr`):

   | File | Change |
   |---|---|
   | `config/features.mk` | `CONFIG_LANG_FR ?= n` and `CONFIG_LANG_DEFAULT_FR ?= n`, next to the others; both in `FEATURES`; the `$(error CONFIG_LANG_DEFAULT_FR=y needs CONFIG_LANG_FR=y)` line, as the other languages have |
   | `core/config.h` | The `#ifndef` defaults (0) of both, and `CONFIG_LANG_FR` added to `CONFIG_LANG_ANY` |
   | `docs/build-config.md` | A row for each in the user interface table |
   | `docs/feature-matrix.md` | A row for each in the user interface table, with the measured bytes (the default costs none) |
   | `tests/hil/features.toml` | `[feature.lang_fr]`, `switch = "LANG_FR"`, and `[feature.lang_default_fr]`, `switch = "LANG_DEFAULT_FR"` |

   `python3 tools/boards.py docs` then adds the switches to the generated tables of the feature matrix.

   `tools/i18n.py --check` fails until every one of these is there.
3. Translate (below), then run `python3 tools/i18n.py --check` and the host tests (`make -C tests/host`).
4. Look at it: build the gallery in your language (below), or flash a build and pick it in Settings > Display >
   Language.

A new language is off by default. It is turned on by default only when it is widely used and the radios have the
flash for it ([build-config.md](../build-config.md)): Spanish is, Italian isn't. A language that is off is still listed on the
radio, dimmed, and the web flasher offers to turn it on for a visitor whose browser asks for it. The web flasher also
asks which language the radio starts in (preset to the page's) and sets `CONFIG_LANG_DEFAULT_<CODE>` from it.

### Translating the catalog

Each entry is the English (`msgid`) and its translation (`msgstr`). Fill in the empty `msgstr` lines with any `.po`
editor (Poedit, Lokalize) or a text editor. An empty one shows the English, so the radio works with a catalog partly
done, but CI fails until every string is translated ([When the source changes](#when-the-source-changes)). In a text
editor, write a double quote inside a string as `\"`, as the `msgid` does.

```
#: core/ui/ui_menus.c
msgid "Backlight"
msgstr "Retroiluminación"
```

| Rule | Why |
|---|---|
| Keep it short. Most screens are 128 or 160 pixels wide: about 21 characters in the small font, 16 in the list font | A row too long is cut with an ellipsis; a toast or hint is cut |
| Keep leading and trailing spaces, and the colon at the end | The code glues a number or a name to the text: `"Volume "` + `"5"` |
| Keep `{a\|b}` groups as groups, with the same number of choices | A text alternate: the radio shows the first choice that fits, longest first. Write each choice shorter than the one before |
| Use only ASCII and Latin-1 letters (á é í ó ú ñ ü ç ß ...) and `… · → ° ⤒ ⤓` | The radio draws one byte a character; `--check` rejects anything else |
| Start every hint group that names the Back key with exactly your translation of `"Back"` | Hints keep the Back group at the right edge by matching that word: change it and every Back group must change with it |
| Status strip words (IDLE, NO TX, SCAN, DUAL, ROAM) and chips (Lock, DMon, Priv) no longer than the English; keep the English when nothing fits | They sit in fixed slots of the status strip |
| Don't translate the standard abbreviations the source leaves unmarked (TG, TS, CC, RX, TX, VFO, DMR, FM) | Radio operators know them; they aren't in the catalog |

Some strings are joined in English word order, a number or name before or after a piece of text (`"14m"` + `" ago"`).
Translate the piece so the joined text reads well ("14m atrás"), even when the usual phrase puts the words the other
way round.

To see where a string goes, search for it in the file its `#:` line names: `grep -n -F '"Backlight"'
core/ui/ui_menus.c`. The comments next to a `TR()` often say more. A string with a leading or trailing space is
always such a piece.

### When the source changes

`python3 tools/i18n.py --sync` adds the new marked strings to every catalog (untranslated) and drops the ones the
source no longer has. A change that adds or edits a marked string runs it and commits the catalogs with the code.

| Case | What `--sync` does | CI |
|---|---|---|
| A new string | Adds it untranslated | Fails until it is translated in every catalog |
| English edited a little (mostly the same words) | Keeps the old translation, still in use | A warning on the pull request: the old and new English and each language's text |
| English rewritten | A new entry, untranslated; the old one dropped | Fails until it is translated |
| A translation marked `#, fuzzy` (in doubt) | Kept, not used | Fails until a translator checks it and removes `#, fuzzy` (and the `#\|` line under it) |

A small edit of the English often leaves the translations right, so a changed string is a warning
(`tools/i18n_changes.py`), not a failure: the reviewer reads the warning and decides whether each language needs an
update. The same holds for the site's catalogs: a key whose English changed while a translation didn't is a warning.

A pull request that adds or rewrites text and can't translate it lists it as an open task in its description
(`- [ ] translations: es, it`), and the strings are translated before the merge. To build or test with strings still
untranslated, `python3 tools/i18n.py --check --allow-pending` skips them; CI doesn't.

`python3 tools/i18n.py --stats` shows how much of each catalog is translated.

### Marking text in new code

| Text | Mark |
|---|---|
| A word or sentence the user reads | `TR("...")`, or `TRN("...")` in a table and `tr()` where it is shown |
| A screen's title, hint, empty text, a menu row | `TRN()` in the table: `list_draw` and the menus translate them |
| Names, numbers, units, standard abbreviations | Not marked |
| Debug link names and values, protocol text sent off the radio, log text | Not marked: tools and tests read them |
| Canned text messages | Marked: the radio sends them in the language in use, so keep them short and plain |
| Search words (the second string of a settings row) | Not marked: Find matches the English words and the shown name |

A marked string is one C literal, or literals and the `font.h` glyph macros (`GLYPH_ELLIPSIS`, `TEXT_ALT_OPEN`, ...)
next to each other. Not a variable, no `\n` (write the line break with `sb_chr`). A macro whose value is a
`TRN(...)` is found like any other. Build text from pieces
with `sbuf_t`, not `strcpy`/`strcat`: a translation can be longer than the English, and `str_copy` and `sb_str`
cut it safely. Don't compare shown text with `strcmp`; compare the state it came from. After marking, run
`--sync` and commit the catalogs.

### Looking at the result

`make -C tests/host ui-gallery` renders the screens in English. To see a language, render the gallery with `UI_LANG`
set to its code, into a directory outside the repo, and look for text that is cut or runs into something:

```
make -C tests/host uiSim
cd tests/host && UI_LANG=es ./uiSim --gallery /tmp/gallery-es    # contact sheets in /tmp/gallery-es/sheets
```

Commit only the English gallery.

### Flash cost

The first language costs the most: the lookup and the Latin-1 glyphs of every font. Each further language adds only its
text, about the size of the catalog's translations. Measure a language like any switch
([building.md](building.md)) and put the bytes in the feature matrix.

## Translating the web site

1. Copy `site/i18n/en.json` to `site/i18n/<code>.json` and translate the values. Keep its format (one space of
   indent, UTF-8 as is: `json.dumps(data, ensure_ascii=False, indent=1)`). A key left out shows the English, but CI
   fails until every key is there (`python3 -m rfoundrysite.i18n` from `site/` lists them).
2. Add the language to `site/i18n/languages.json` (`code`, `name`, `dir`, `locale`), and its catalog to
   `tools/generic_allow.txt` next to `en.json`: it carries the same per-radio texts.
3. If its plural rule differs from English's, add it to `PLURAL_RULES` in `site/rfoundrysite/i18n.py`.
4. `python3 site/build.py --strict` checks the catalog. `python3 site/build.py --serve` and
   `http://localhost:8000/<code>/` shows it.

| Rule | Why |
|---|---|
| Keep `{name}` placeholders and HTML tags exactly, and anything else in angle brackets (`<radio address>`) | The check fails otherwise; the page fills them in |
| Plural keys (`key.one`, `key.other`, ...): give your language's forms. Most European languages need `.one` and `.other`; `PLURAL_RULES` lists the others | Picked by the count; `.other` is the fallback |
| Keep product and protocol names (RadioFoundry, DMR, DFU, CPS) and radio model names | They are names |

What the site leaves in English (radio data, error details, logs) is listed in
[website.md, What stays English](website.md#what-stays-english). Translated documentation pages go in
`docs/i18n/<code>/`.

## Checks

| Check | Runs |
|---|---|
| `python3 tools/i18n.py --check` | Catalogs in step with the source, none untranslated or fuzzy, drawable characters, text alternates, spaces, the switch in every file |
| `make -C tests/host` | `i18nTest`: lookups, switching, a language not built; `uiFlowTest`: the Language screen |
| `python3 site/build.py --strict` | The site catalogs: keys, placeholders, tags, `languages.json` |
| `python3 -m rfoundrysite.i18n` (from `site/`) | The same, and no key missing from a language |
| `tools/i18n_changes.py` | Warnings, not failures: English the branch changed while a translation didn't, radio and site |

`tools/ci.sh` runs all of them. In a pull request the warnings show as annotations on the changed catalog lines.
