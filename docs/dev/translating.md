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
| Characters | ASCII and Latin-1 (U+00A0 to U+00FF), with separate support for [CJK catalogs](#chinese-and-other-cjk-languages) | Any Unicode |

## Translating the radio

### How it works

The source is English. A string the radio shows is marked where it is written, with a key that names it and its
English:

| Mark | Where | Meaning |
|---|---|---|
| `TR(KEY, "Text")` | In code | Returns the text in the language in use |
| `TRN(KEY, "Text")` | Where the text is stored now and shown later: an initializer of a `static const` array or struct, a field, an event | A `tr_id_t` for the text (a name for it, not the text); the code that shows it calls `tr()` on it |
| `TRX(KEY, "Text")` | In the same places, for text never translated (`"RX"`, `"12.5 kHz"`) that sits beside translated text | A `tr_id_t` like `TRN`; no catalog has it |
| `tr(id)` | Where a table entry is shown | The text in the language in use; the English when there is no translation |
| `tr_en(id)` | Where the English is needed too (Find matches both) | The English |

```c
ui_toast(TR(ZONE_TOAST_FULL, "Zone full"));           // shown right here: TR gives the text

static const tr_id_t modes[] = { TRN(VALUE_OFF, "Off"), TRN(VALUE_AUTO, "Auto"), TRX(WORD_VOX, "VOX") };
sb_str(&b, tr(modes[i]));                             // stored first, shown later: TRN or TRX, then tr()
```

`TR` cannot be used in a `static const` initializer: it is a function call, and which language is in use is known only
when the text is shown. That is what `TRN` is for. `TRX` exists because every entry of such an array is a `tr_id_t`,
so text that is never translated needs one too.

The key is a C identifier, upper case. One key always has the same English (the tool fails otherwise); the same English
may have more than one key, where it needs different translations. The English at the mark is the source of truth: a
catalog entry is the key's (its `msgctxt`), and the tool reads the English from the mark.

#### Key names

A key says where the text is shown and what it is for, not what the English says: `AREA_ROLE_MEANING`
(`ZONE_TOAST_RENAMED`, `CHAN_LABEL_BANDWIDTH`, `DENY_FIX_ADD_CODEC`). The English can then change without the name
going stale, and a translator who reads the key in a catalog knows what kind of text it is.

| Part | What it is |
|---|---|
| Area | The screen or feature the text belongs to, as in the code: `HOME`, `ZONE`, `CHAN`, `MSG`, `SHARE`, `QUICK`, `SET`, `DUAL`, `APRS`, `ROAM`, ... Reuse one from the catalogs before making a new one. Left out for text several areas share (`VALUE_OFF`, `HINT_OK_PICK_BACK`, `WHY_SCAN_NOT_IN_BUILD`) |
| Role | How the text is shown, from the list below |
| Meaning | What it says, in a few words. Left out when the area has one such text (`QUICK_TITLE`, `GPS_HINT`) |

| Role | The text is |
|---|---|
| `TITLE` | A screen's title; also on the row that opens that screen |
| `ROW` | A list row that does something when picked: an action, a choice, a sub-menu |
| `LABEL` | The name beside a value: a settings row, a field, a caption |
| `VALUE` | A value shown for a setting or field |
| `HINT` | The hint row: what the keys do |
| `TOAST` | A short message after a key press |
| `BANNER` | A line of a banner (an event from outside) |
| `BODY` | Running text on a page, or a piece of it |
| `STATE` | A status word a screen shows while something goes on (`TALKING`) |
| `CHIP` | A short tag: the status bar, a list row's right edge |
| `EMPTY` | What a list or page shows when it has nothing |
| `WHY` | Why something can't be done: a refusal, a missing feature |
| `FIX` | What to do about it |
| `ASK` | A question to confirm |
| `ERR` | A failure reported by a background job |
| `UNIT` | A unit joined to a number (` channels`) |
| `PART` | A piece joined to other text, with no role of its own (` of `) |
| `WORD` | A term used as title, label and value alike (`VFO`, `Scan`) |

A piece of a sentence (`"Message from "`) takes the role of the sentence. A number in a name is the number in the
text (`VALUE_10_MIN`, `HINT_1_5_PICK_BACK`), never a counter: two texts that would get the same name differ in
meaning, so say how.

A `tr_id_t` is opaque. Code passes it to `tr()` or `tr_en()`, or compares it with `TR_NONE` (none: zero, so a field
left out of an initializer is none), and nothing else. With a language built it is the key's number (`TR_<KEY>` in
the generated `tr_keys.h`), and `tr()` reads it from a table: no search. With every language switched off it is the
English text itself and costs nothing. Code compiles the same either way.

`tools/i18n.py` reads the marks, keeps the catalogs in step and turns them into the tables the build links in: the
English of each key, and each language's translations by key.

Each build packs only the strings of the code the build compiles. Make preprocesses each source with the build's
flags and collects the `TR_<KEY>` names left in the output (a `.tr` file beside the object); `tools/i18n.py --gen
--used` then leaves every other key's text out of the tables. A mark under a switch that is off costs no text:

| | Comes from |
|---|---|
| Key numbers (`tr_keys.h`) | Every key in the source: a key has the same number in every build |
| Text in the tables | The keys this build compiles. Any other key costs 1 byte per table (its empty string) |

The host tests and the simulator build the full tables (`--gen` without `--used`). The catalogs and `--sync`/`--check`
read the whole source as before: a translator translates every string, built in or not.

### Adding a language

1. Start the catalog: `python3 tools/i18n.py --new <code> <name>`. `<code>` is the ISO 639-1 code (`fr`), `<name>`
   the language's own name (`Français`). This writes `core/ui/lang/<code>.po` with every marked string, untranslated.
   A CJK language adds its Latin name: `python3 tools/i18n.py --new zh 中文 Chinese` ([below](#chinese-and-other-cjk-languages)).
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
asks which language the radio starts in (preset to the page's when the build has it) and sets
`CONFIG_LANG_DEFAULT_<CODE>` from it. A language picked there is built in: its `CONFIG_LANG_<CODE>` goes on with it.

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
| Use only ASCII and Latin-1 letters (á é í ó ú ñ ü ç ß ...) and `… · → ° ⤒ ⤓` | The radio draws one byte a character; `--check` rejects anything else. A CJK catalog is the exception, and says so ([below](#chinese-and-other-cjk-languages)) |
| Start every hint group that names the Back key with exactly your translation of `"Back"` | Hints keep the Back group at the right edge by matching that word: change it and every Back group must change with it |
| Status strip words (IDLE, NO TX, SCAN, DUAL, ROAM) and chips (Lock, DMon, Priv) no longer than the English; keep the English when nothing fits | They sit in fixed slots of the status strip |
| Don't translate the standard abbreviations the source leaves unmarked (TG, TS, CC, RX, TX, VFO, DMR, FM) | Radio operators know them; they aren't in the catalog |

Some strings are joined in English word order, a number or name before or after a piece of text (`"14m"` + `" ago"`).
Translate the piece so the joined text reads well ("14m atrás"), even when the usual phrase puts the words the other
way round.

To see where a string goes, search for its key (the `msgctxt` line) in the file its `#:` line names: `grep -n
'BACKLIGHT,' core/ui/ui_menus.c`. The comments next to a `TR()` often say more. A string with a leading or trailing space is
always such a piece.

### Chinese and other CJK languages

The Latin fonts have no glyph for Chinese, so such a catalog is UTF-8 and brings its own face. Everything else works as
above: one catalog, one switch, the same checks.

| Step | What |
|---|---|
| The catalog | `tools/i18n.py --new zh 中文 Chinese` writes it with an `X-Glyphs: utf8` header and an `X-Language-Name-Latin` one. A catalog with that header holds UTF-8: ASCII, `font.h`'s glyphs, and characters the CJK face has. Two-byte characters (Latin-1 letters with an accent) are not allowed: the text layer decodes only three-byte sequences, so that Latin-1 names, messages and the Spanish catalog keep working in a build with the CJK face. `--check` lists every character it rejects or the face lacks |
| The list | A build without the CJK face (`CONFIG_LANG_ZH` off) still lists the language, dimmed: it shows `X-Language-Name-Latin` ("Chinese"), because the Latin font would draw the UTF-8 name as boxes |
| The face | `tools/cjkfont.py` rasterizes every character the catalogs use plus a reviewed list of Chinese province, prefecture-level city and common repeater words (`tools/cjk_names_zh.txt`) into a 16x16 one-bit glyph and writes the table the firmware links. The current table has 802 characters and is about 27 KB; with the text, Chinese costs about 44 KB. Channel names that use a character outside this list draw a box. Run it whenever a translation or the names list adds a character (`--check` fails until it does) and commit both |
| The font | One whose license allows embedding bitmaps: an SIL OFL font such as Noto Sans CJK SC or Source Han Sans. The generated file carries the font's copyright, its full OFL text and the font file's SHA-256, because that data is under the font's license, not MIT (see the header of `core/ui/cjk_font_data.c`). Do not use a vendor font that forbids it |
| Size | A CJK character is 16 px wide and 16 tall, the height of the 8x16 font: 8 characters a line on a 128 px screen, 10 on a 160 px one, 15 on a 240 px one. **A line must be at least 16 px high.** A 240 px color radio draws its words in 8x16 (a CJK build moves its text pages up to that font too) and shows Chinese everywhere; the 160 px class draws them in 6x12 and moves lists, labels, the strip, the hints and the text pages up to the 8x16 font while the language in use is wide, so its lines and rows get taller (`ui_layout_init`, [ui.md 2.5](../spec/ui.md#25-languages)). A 128x64 panel has no room for 16 px lines: it lists `LANG_ZH` and `LANG_DEFAULT_ZH` in `BOARD_UNSUPPORTED`, and CI's maximal build and the flasher leave the language out there. Flash is the other limit: the face and the catalog add about 44 KB, so compare that with the board's app region (`boards/<radio>/memory.ld`) and [the feature matrix](../feature-matrix.md#whole-images). A board without room leaves `CONFIG_LANG_ZH` off in its `config.mk`; the language packs otherwise build side by side |
| Cutting | A string too long for its space is cut at the end, whole characters only (`gfx_text_fit_end` for a line that keeps its start, `gfx_text_fit` for a middle cut), because Chinese has no spaces to cut at a middle. A line wraps after any CJK character (`wrap_line`); Latin words inside it still wrap at their spaces |
| Switch | `CONFIG_LANG_ZH` is off by default. Measure it like any switch and put the bytes in [the feature matrix](../feature-matrix.md) |

The table in this tree comes from Noto Sans SC VF, rendered with Pillow 13.0 (see the hash and options in
`core/ui/cjk_font_data.c`). Rebuild it with the same font and Pillow version:

```
python3 tools/cjkfont.py --font /path/to/NotoSansSC-VF.ttf --weight 500 \
    --font-name "Noto Sans SC (Windows variable font)" \
    --copyright "2014-2021 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'." \
    --license LICENSES/OFL-1.1.txt --spdx OFL-1.1 \
    --po core/ui/lang/zh.po --chars-file tools/cjk_names_zh.txt \
    --out-c core/ui/cjk_font_data.c --out-png /tmp/cjk.png
```

`--chars-file` adds the characters from the reviewed name list without pulling in GB2312 level 1. Lines beginning
with `#` are comments. The header records the font file's SHA-256: a different hash means a different font file, so
say which one you used in the pull request. `--weight` picks a variable font's axis. A different Pillow can render
slightly different bytes; commit the table it wrote.

### When the source changes

`python3 tools/i18n.py --sync` adds the new keys to every catalog (untranslated) and drops the ones the source no
longer has. A change that adds or edits a marked string runs it and commits the catalogs with the code.

| Case | What `--sync` does | CI |
|---|---|---|
| A new key | Adds it untranslated | Fails until it is translated in every catalog |
| A key's English edited | Keeps the translation, still in use | A warning on the pull request: the old and new English and each language's text |
| A translation marked `#, fuzzy` by a translator (in doubt) | Kept, not used | Fails until a translator checks it and removes `#, fuzzy` (and the `#\|` line under it) |
| New English under a new key | A new entry, untranslated; the old key dropped | Fails until it is translated |

An entry follows its key, so an edit of the English never loses a translation. A small edit often leaves the
translations right, so a key whose English changed while a translation stayed the same is a warning on the pull
request, not a failure (`tools/i18n_changes.py`): the old and new English and each language's text. Say in the pull
request whether each language still fits. When the meaning changes, use a new key: its entry starts untranslated.

#### Renaming a key

The tool matches a catalog entry by its key alone, so a key renamed only at the mark looks like a new key: `--sync`
drops the old entry with its translations and adds an empty one. Rename with the tool, which changes the marks and
the catalogs together:

```
python3 tools/i18n.py --rename OLD_KEY NEW_KEY     # one key
python3 tools/i18n.py --rename names.txt           # many: a line "OLD_KEY NEW_KEY" each
python3 tools/i18n.py --sync
```

It rewrites the key at every mark in `core/`, `tests/` and `sim/` and the `msgctxt` of its entry in every catalog,
and refuses a name that is taken. The pairs of a file are renamed together, so two keys may swap names. Afterwards
`git diff core/ui/lang` shows only `msgctxt` lines; no `msgstr` is lost.

The site's catalogs follow the same rule for edited English: a key whose English changed while a translation didn't
is a warning.

A pull request that adds or rewrites text and can't translate it lists it as an open task in its description
(`- [ ] translations: es, it`), and the strings are translated before the merge. To build or test with strings still
untranslated, `python3 tools/i18n.py --check --allow-pending` skips them; CI doesn't.

`python3 tools/i18n.py --stats` shows how much of each catalog is translated.

### Marking text in new code

| Text | Mark |
|---|---|
| A word or sentence the user reads | `TR(KEY, "...")`, or `TRN(KEY, "...")` in a table and `tr()` where it is shown |
| A screen's title, hint, empty text, a menu row | `TRN()` in the table: `list_draw` and the menus translate them |
| A hint built when the screen is drawn | `SF_HINT_FN` and `hint_fn` in the screen's definition, returning text already in the language in use |
| Names, numbers, units, standard abbreviations | Not marked; in a table of `tr_id_t` beside marked text, `TRX(KEY, "...")` |
| Debug link names and values, protocol text sent off the radio, log text | Not marked: tools and tests read them |
| Canned text messages | Marked: the radio sends them in the language in use, so keep them short and plain |
| Search words (the second string of a settings row) | Not marked: Find matches the English words and the shown name |

A mark's text is one C literal, or literals and the `font.h` glyph macros (`GLYPH_ELLIPSIS`, `TEXT_ALT_OPEN`, ...)
next to each other. Not a variable, no `\n` (write the line break with `sb_chr`). A macro whose value is a
`TRN(...)` is found like any other. Name a new key by [Key names](#key-names); a key in use is reused for the
same text where the meaning is the same. `TR_<KEY>` must not be a name the source already uses (the tool says so). `python3 tools/i18n.py --rewrite [FILE]`
gives a mark written without a key, `TR("...")`, one made from its English (the same English gets the key it already
has); rename it before the commit (`--rename`, below).
A text that travels between tasks (an event) travels as its `tr_id_t`, not as text. Build text from pieces
with `sbuf_t`, not `strcpy`/`strcat`: a translation can be longer than the English, and `str_copy` and `sb_str`
cut it safely. Don't compare shown text with `strcmp`; compare the state it came from. After marking, run
`--sync` and commit the catalogs.

### Looking at the result

`make -C tests/host ui-gallery` renders the screens in English. To see a language, build the simulator with that
language in (`UI_LANG` on the `make` line turns its `CONFIG_LANG_<CODE>` on, for a language that is off by default),
then render the gallery with `UI_LANG` set to its code, into a directory outside the repo, and look for text that is cut
or runs into something:

```
make -C tests/host -B uiSim UI_LANG=fr
cd tests/host && UI_LANG=fr ./uiSim --gallery /tmp/gallery-fr    # contact sheets in /tmp/gallery-fr/sheets
```

Commit only the English gallery.

### Flash cost

The first language costs the most: the lookup, the English of every key the build compiles in one table (the literals
it replaces, plus an offset per 8 keys) and the Latin-1 glyphs of every font. Each further language adds only its
text, about the size of the catalog's translations of those keys, and an offset per 8 keys. A string under a switch
that is off costs 1 byte per table. Measure a language like any switch
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
| `python3 tools/i18n.py --check` | Every mark has a key, one English per key; catalogs in step with the source, none untranslated or fuzzy, drawable characters, text alternates, spaces, the switch in every file |
| `python3 tools/i18n_test.py` | The tool: keys, tables, sync, migration, rewrite, rename |
| `make -C tests/host` | `i18nTest`: a key to its translation or its English, `tr_en`, switching, a language not built; `uiFlowTest`: the Language screen |
| `python3 site/build.py --strict` | The site catalogs: keys, placeholders, tags, `languages.json` |
| `python3 -m rfoundrysite.i18n` (from `site/`) | The same, and no key missing from a language |
| `tools/i18n_changes.py` | Warnings, not failures: English the branch changed while a translation didn't, radio and site |

`tools/ci.sh` runs all of them. In a pull request the warnings show as annotations on the changed catalog lines.
