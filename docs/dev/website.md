<!-- SPDX-License-Identifier: MIT -->

# The Web Site

The site is generated from the repository, like the PDF manual. It lists the radios, their screens
and features, holds the documentation, and carries two tools that run in the browser with nothing sent to a server:
the web flasher (back up, install, restore; [web-flasher.md](web-flasher.md)) and the Codeplug Editor
([codeplug-editor.md](codeplug-editor.md)). The site is the recommended way for users to install RadioFoundry and edit
a codeplug ([Installing](../user/02-installing.md)); this page is for people who change it.

```
python3 site/build.py                       # build/site/
python3 site/build.py --serve               # build, then http://localhost:8000 (WebUSB needs localhost or https)
python3 site/build.py --firmware build/dist # with release images for the flasher (tools/release.py --out)
python3 site/build.py --strict              # any warning fails (CI)
site/tests/run.sh                           # the flasher's and the editor's tests (Node 20+)
```

The generator needs one Python package, markdown-it-py (`pip install -r site/requirements.txt`, or prefix the
command with `uv run --no-project --with-requirements site/requirements.txt`). The pages need no build step and no
npm packages: plain HTML, CSS and ES modules. `tools/ci.sh` builds the site and runs the tests (Python step).

`.github/workflows/site.yml` builds it and deploys it to radiofoundry.org (see [Hosting](#hosting)):

| Run | Does |
|---|---|
| Nightly (07:17 UTC) | Builds and deploys only if something changed since the last deployment: a file the site is built from (`SITE_INPUTS` in the workflow), or a release published after it. Otherwise stops after the check |
| By hand (Actions > Web site > Run workflow) | Always builds and deploys |
| Pull request | Not run: the pull request's unit tests job builds the site (`--strict`) and runs the flasher's tests (`tools/ci.sh`'s Python step) |

A merge or a release therefore shows on the site the next morning, or right away when the workflow is run by hand.

## Hosting

The site is static files. nginx serves them from `/var/www/radiofoundry/current`, a symlink to one directory under
`releases/`. The workflow's `build` job uploads the finished site as an artifact. Its `deploy` job runs on a
self-hosted runner labeled `rfoundry-deploy`, which sits on the host that serves the site. The job copies the build
into a new `releases/<sha>-<run>` directory, swaps the symlink in one step and keeps the last three builds.
Only the nightly and manual runs reach that runner, never a pull request.

| Part | Setup |
|---|---|
| nginx | One server block on port 80 with `root /var/www/radiofoundry/current`. TLS and the public name are the reverse proxy's and the CDN's |
| Runner | A user without sudo that owns `/var/www/radiofoundry`, with a runner registered for the repository with the label `rfoundry-deploy` |
| Roll back | Point `current` at an earlier directory in `releases/` (`ln -sfn releases/<id> current.new && mv -T current.new current`) |

## Sources

Nothing on the site is copied by hand. Each page reads its source at build time:

| Shows | Comes from |
|---|---|
| Radios, their order and status | The board registry, `boards/<radio>/board.toml` (`tools/boards.py json --for site`). `order` sorts them; the lowest is the home page's radio |
| Planned radios | The rows of the [radio survey](../spec/radios.md) "Summary table" that name no board, by their Port cell (`tools/registry_docs.py`): `Planned`, `Planned, hardware needed`, `In progress, #N` (badge "In progress", linked to the pull request) or `Not planned` (not listed), each with an optional `. note` |
| A radio's one-line note for visitors | board.toml `[status] summary`, the same line the README and the user manual show; a catalog may translate it as `radio.note.<radio>`. A planned radio's: the note of its Port cell, which a catalog may translate as `planned.note.<slug of the survey name>` |
| A radio's name, bands, processor, screen, bootloader steps, flash backend, codeplug location | board.toml, the CPU from `platform/<p>/platform.toml`, the backend from the `[flash] family`, the codeplug location from the board's `hal_flash_map_t` |
| A radio's status badge (Supported, In development) | board.toml `[status] level`: `supported` reads Supported, `testing` and `bring-up` read In development |
| Which features a radio has, per variant | `make BOARD=<radio> [VARIANT=<v>] print-config` and `print-required`; what it can't have is config.mk `BOARD_UNSUPPORTED` (shown ✕) and what isn't written for it yet `BOARD_NOT_WRITTEN` (shown …), with their reasons, through the registry |
| Feature names, help, dependencies | `config/features.mk` (the comment above each switch, the `needs` checks) |
| Feature groups, titles, flash cost, notes | The tables in [feature-matrix.md](../feature-matrix.md) |
| Variants and why each leaves a feature out | `config/variants/<id>.mk` ([build-config.md](../build-config.md)) |
| Screenshots | `docs/ux/screens/<gallery>/` (`make -C tests/host ui-gallery`), six per radio page in two rows of three: the first of `featured` in `site/data/screens.json` the radio has |
| Flash layout, codec profiles, stock files, key hashes | The board registry (`tools/rfoundry/rfoundry/registry.json`, read through `boards.py`) and `stockfw.py` (the USB HID update route: `k22fcodec.py`, `updatefile.py`, `hidboot.py`), exported to `data/flash-boards.json` |
| Documentation | Every Markdown file under `docs/` |
| Firmware images | Release manifests and images (`--firmware`; the workflow downloads the newest release) |

The build fails when a source doesn't parse, and warns (an error with `--strict`) when a feature has no row in the
feature matrix, a screen isn't in `screens.json`, a string is missing from the English catalog, or a page links to a
file the build didn't write.

`data/*.json` is what the pages read and doubles as a public API for other tools: `radios.json`, `features.json`,
`variants.json`, `flash-boards.json`, `screens.json`, `releases.json`, `site.json`.

## Layout

| Path | What |
|---|---|
| `site/build.py` | Entry point |
| `site/rfoundrysite/repo.py` | Reads the sources above |
| `site/rfoundrysite/build.py` | Assembles the pages and data files |
| `site/rfoundrysite/docs.py` | `docs/` to HTML: GitHub heading ids, `.md` links to `.html`, links out of `docs/` to the repository |
| `site/rfoundrysite/device.py` | Radio images: frame, overlay geometry, boot screen composite |
| `site/rfoundrysite/tmpl.py`, `site/templates/` | Page markup (below) |
| `site/i18n/` | String catalogs (below) |
| `site/static/css/tokens.css`, `site.css` | Design tokens, then layout and components |
| `site/static/js/site.js` | Theme toggle, menu, features filter, screen overlay on radio pages |
| `site/static/js/flasher/` | The web flasher (below) |
| `site/static/js/codeplug/` | Codeplug decoding and encoding, CSV, the radio transport, contacts sources ([codeplug-editor.md](codeplug-editor.md)) |
| `site/static/js/editor/` | The Codeplug Editor page |
| `site/rfoundrysite/radioid_regions.py` | Writes `codeplug/sources/regions.js` from a RadioID.net `user.csv` (country and state names only) |
| `site/static/js/tools.js` | The Tools page (codeplug conversion) |
| `site/static/img/radios/src/<radio>.svg` | Radio illustrations (below) |
| `site/tests/` | Flasher and editor tests: `fixtures.py` makes inputs with rfoundry, the HR_C7000 tools and `tests/host/cpsSim`; the `*.test.mjs` files check the JavaScript |

## Templates and design

Markup lives in `site/templates/*.html` and `partials/`, with four kinds of placeholder: `{{ value }}` (escaped),
`{{{ html }}}` (as built), `{{t key}}` (a translated string) and `{{> partial }}`. `{{root}}` is the path to the site
root (static files, data) and `{{home}}` the path to the root of the page's language (links between pages). Loops
stay in Python, which renders a partial per item.

Every page's footer ends with a faint build stamp: the commit the site was built from (`-dirty` with uncommitted
changes, linked to the commit) and the build time in UTC (`data/site.json` has the same commit and the date).

All visual decisions are variables in `tokens.css`: colors for light and dark, type sizes, spacing, radii.
`site.css` only uses them. A redesign replaces `tokens.css`, then `site.css` and the templates; the generator doesn't
change. Readability comes first ([principles](../ux/principles.md)): 18 px base type, high contrast, nothing
squeezed.

## Glossary

`docs/glossary.json` is the one source. `python3 tools/glossary.py --write` generates the two glossary pages from it:
[user/22-glossary.md](../user/22-glossary.md) (entries with scope `user`) and [glossary.md](glossary.md) (scope `dev`).
Never edit those pages by hand. `tools/ci.sh` fails when they are stale.

| Field | Meaning |
|---|---|
| `term` | The name as the page shows it. A plain word (lowercase, or one capital) also matches in any case and with a plural `s`. An acronym or mixed-case name matches exactly. |
| `aliases` | Other spellings and plurals. Each follows the same matching rule. |
| `category` | One of `categories` in the file. It is the page's section. |
| `scope` | `user`: listed in the user glossary, previewed in the whole site. `dev`: listed in the developer glossary, previewed outside the user guide. |
| `definition` | One or two short sentences. Say what the standard calls it when a vendor uses another name. |
| `link` | Optional path under `docs/` of the section that explains it. The preview's "More" link goes there, else to the glossary row. |
| `preview` | `false` keeps a common word (Monitor, Zone) in the glossary but never marks it in the text. |

The site build (`docs.py`) wraps a term in `<abbr class="gl" title="...">` at build time. The Markdown stays unmarked, so GitHub
shows plain text. Rules:

- Whole words only. Not inside code, headings, link text, a table's first column, hyphenated words or paths.
- The first use in each section (a section ends at the next `#` or `##` heading).
- `<!-- glossary: off -->` in a page turns marking off for that page. `<!-- glossary: off TG, SFR -->` turns off those terms.

`site.js` turns each `<abbr>` into a popover on hover, keyboard focus and tap, with a "More" link (Enter follows it from the
keyboard). Without JavaScript the `title` shows the definition. `site/tests/glossary_test.py` tests the rules.

To add a term, add an entry to the file, run `python3 tools/glossary.py --write` and `python3 tools/glossary.py --check`.
`python3 tools/glossary.py --unexplained` lists all-capital words the docs use that have no entry (pin names and
identifiers with digits or underscores are skipped). Add an entry for each one that matters to a reader, or its name to `excluded`.

## Translating

`site/i18n/languages.json` lists the languages. The default (English) is at the site root, others under `/<code>/`.
Each language has a flat catalog `site/i18n/<code>.json`; a missing key falls back to English. The flasher and the
Tools page get their strings from the same catalog (embedded in the page). Text that comes from the repository
(feature help, variant reasons) is English at its source; a catalog can override it with `feature.<SWITCH>.title`,
`feature.<SWITCH>.help`, `variant.<id>.title`, `variant.<id>.summary` or `group.<slug>`.

A translated documentation page goes in `docs/i18n/<code>/<path under docs>`. Pages without one are shown in
English with a notice.

To add a language: copy `en.json` to `<code>.json`, translate, add the language to `languages.json`.

## Radio images

Each radio has an original illustration, `site/static/img/radios/src/<radio>.svg`, drawn from straight-on reference
photos (the photos stay outside the repository: product photos are not ours to publish). Rules:

- viewBox `0 0 W H`, transparent background, no external references or embedded images;
- exactly one `<rect id="screen" x y width height/>`, black, transform-free and not covered by anything, on the
  display's visible area, with the aspect ratio of the radio's gallery (5:4 for 160x128 and 160x120i5, 22:31 for
  240x320i10x5);
- for the radio's web control page (`docs/spec/tcp-services.md` 5.1): each key it shows carries `id="key-<name>"`, the
  name being the board's `KEY_*` in lower case without `KEY_` (`key-fn_left`); the channel knob `id="encoder"` and a
  drawn LED `id="led"`. Keys whose shape doesn't match a key (a d-pad drawn as one piece) get transparent hit rects
  (`fill="none" pointer-events="all"`) at the end. A key off the front face (side keys, top keys) also carries
  `data-edge="left|right|top|bottom"`, the drawing's edge it is on: the page puts a labeled button for it outside
  that edge. Every key of the radio is tagged, or named in `data-undrawn="name ..."` on the `<svg>` element when the
  drawing doesn't show it (a front view without the side keys); the page makes those buttons;
- `tools/svg_keys.py check` checks the tags: each `key-` id names one of the radio's keys, once, and no key is
  missing. The site build runs the same check, so a drawing that lost its tags fails CI. After drawing or redrawing a
  radio, `tools/svg_keys.py tag <radio>` adds the tags: by the labels in the drawing ("MENU", "1", "*"), then by the
  last tagged version in git, mapped onto the new drawing (edges, the LED, the knob and hit rects come along). It
  prints each tag and what it couldn't place. Check the result on the web control page before committing;
- a radio with a keyed hand microphone may have `<radio>-mic.svg` beside its drawing, for the web control page only:
  the same `key-` rule, the ids counted once across both files, and gradient ids of its own (the page puts both
  drawings in one document).

The build writes `static/img/radios/<radio>-frame.svg` (screen black), `<radio>.svg` (with the boot screen) and the
screen's position in `data/radios.json` (`frame_geometry`: viewBox units and percentages), so any screenshot can be
laid over the frame; the radio pages do that when a screenshot is clicked. A radio without an illustration gets a
generic drawing from board.toml `[frame]`.

## Error logging and bug reports

The flasher and the Codeplug Editor log what they do in memory (`static/js/diag.js`). Neither keeps the log after
the page closes.

| Entry | Holds |
|---|---|
| Note | A step: the radio found and its firmware, a write plan's size, a flasher step, each line of the flasher's log box |
| Wire | One line per debug link or CPS frame (`link.js`): command, address, length, result. Never the data |
| Error | Every error shown to the user, once, with the stage it happened in, its code and params, stack and causes |

Every error also goes to the browser console (`[rfoundry] <stage> failed:`). Failure messages carry **Report a Problem**
(`static/js/report.js`). It downloads a zip with `report.json` (the log, the browser, the tool's state) and, if the
user ticks them, the edited codeplug and the one last read from the radio.

Automatic error reports go to Sentry when the build has `RFOUNDRY_SENTRY_DSN` set (`site.yml` takes it from the
repository variable of the same name; `site.json` `reports.sentry_dsn`). Without it nothing is sent and the privacy
note is left off the pages. With it:

| Sent | When |
|---|---|
| The error, its stage and code, the tool's state (radio model, firmware version, task; no codeplug contents) | Every error except ones the user caused (port chooser closed, Stop) |
| The whole report and any codeplug the user ticked | Only when the user clicks Send Report |

Automatic reports go through `diag.scrub()`, which removes callsigns, DMR IDs and email addresses. The Sentry bundle
is pinned with an SRI hash in `report.js`. It loads without breadcrumbs or other default integrations, so it sends only
what `diag.error` hands it. `?reporting=0` turns sending off, as it does for the flash log. Ad blockers often block
Sentry; the page then works the same and offers the download only.
