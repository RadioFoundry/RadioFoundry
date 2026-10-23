<!-- SPDX-License-Identifier: MIT -->

# The web site and the web flasher

The site is generated from the repository, like the PDF manual. It lists the radios, their screens
and features, holds the documentation, and carries two tools that run in the browser with nothing sent to a server:
the web flasher (back up, install, restore) and the Codeplug Editor.

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

`.github/workflows/site.yml` deploys it to GitHub Pages:

| Run | Does |
|---|---|
| Nightly (07:17 UTC) | Builds and deploys only if something changed since the last deployment: a file the site is built from (`SITE_INPUTS` in the workflow), or a release published after it. Otherwise stops after the check |
| By hand (Actions > Web site > Run workflow) | Always builds and deploys |
| Pull request | Not run: the pull request's unit tests job builds the site (`--strict`) and runs the flasher's tests (`tools/ci.sh`'s Python step) |

A merge or a release therefore shows on the site the next morning, or right away when the workflow is run by hand.

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
| Screenshots | `docs/ux/screens/<gallery>/` (`make -C tests/host ui-gallery`), grouped by `site/data/screens.json` |
| Flash layout, codec profiles, stock files, key hashes | The board registry (`tools/odrflash/odrflash/registry.json`, read through `boards.py`) and `stockfw.py`, exported to `data/flash-boards.json` |
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
| `site/odrsite/repo.py` | Reads the sources above |
| `site/odrsite/build.py` | Assembles the pages and data files |
| `site/odrsite/docs.py` | `docs/` to HTML: GitHub heading ids, `.md` links to `.html`, links out of `docs/` to the repository |
| `site/odrsite/device.py` | Radio images: frame, overlay geometry, boot screen composite |
| `site/odrsite/tmpl.py`, `site/templates/` | Page markup (below) |
| `site/i18n/` | String catalogs (below) |
| `site/static/css/tokens.css`, `site.css` | Design tokens, then layout and components |
| `site/static/js/site.js` | Theme toggle, menu, features filter, screen overlay on radio pages |
| `site/static/js/flasher/` | The web flasher (below) |
| `site/static/js/codeplug/` | Codeplug decoding and encoding, CSV, the radio transport, contacts sources ([below](#the-codeplug-editor)) |
| `site/static/js/editor/` | The Codeplug Editor page |
| `site/odrsite/radioid_regions.py` | Writes `codeplug/sources/regions.js` from a RadioID.net `user.csv` (country and state names only) |
| `site/static/js/tools.js` | The Tools page (codeplug conversion) |
| `site/static/img/radios/src/<radio>.svg` | Radio illustrations (below) |
| `site/tests/` | Flasher and editor tests: `fixtures.py` makes inputs with odrflash, the HR_C7000 tools and `tests/host/cpsSim`; the `*.test.mjs` files check the JavaScript |

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
  240x320i10x5, 4:3 for 320x240).

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

Every error also goes to the browser console (`[odr] <stage> failed:`). Failure messages carry **Report a Problem**
(`static/js/report.js`). It downloads a zip with `report.json` (the log, the browser, the tool's state) and, if the
user ticks them, the edited codeplug and the one last read from the radio.

Automatic error reports go to Sentry when the build has `ODR_SENTRY_DSN` set (`site.yml` takes it from the
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

## The web flasher

`site/static/js/flasher/`, plain ES modules. The logic ports odrflash, and the tests check the two agree:

| Module | Does | Ported from |
|---|---|---|
| `stockfw.js` | Stock file container, decryption, codec extraction, key derivation (column vote and runner-up search; the MD-380's pair refinement isn't ported) | `odrflash/stockfw.py` |
| `plan.js` | Sector plans: keep, merge, and restore (a whole image put back exactly) | `odrflash/plan.py` |
| `manifest.js` | Release manifest checks | `odrflash/manifest.py` |
| `decide.js` | Keep or merge the codec | `odrflash/cli.py` `decide_codec` |
| `dfu.js` | The AnyRoad DFU bootloader over WebUSB | `odrflash/dfu.py` |
| `codecsave.js` | The codec read back from a radio that runs ODR (where stock keeps it, or where the merge placed it), checked against the profile's hashes, and a copy kept in IndexedDB: on the MD-380 before every update erases it, elsewhere once, so an install that can't keep or check the codec (a radio back on stock firmware, in its bootloader, or after an interrupted install) needs no stock file | `odrflash/codecsave.py` |
| `bootkey.js` | The bootloader key from a radio that runs ODR: the bootloader read over READ_MEM, the block with the key's SHA-256 | `odrflash/bootkey.py` |
| `link.js` | The debug link and open-firmware CPS reads and writes over Web Serial | `odrflash/debuglink.py`, [open-firmware-cps.md](../spec/open-firmware-cps.md) |
| `backends.js` | The radio access backend registry: which steps and views a radio's family uses (below) | |
| `readers.js` | Bulk memory readers for backups: the debug link and the STM32F4 loader (below) | |
| `backup.js`, `zip.js` | Backup bundles | |
| `restore.js` | What a bundle can put back, and the plans for it | |
| `select.js` | À la carte feature selection with the dependencies, matching prebuilt variants | |
| `codeplug.js` | Codeplug formats and the converter registry | |
| `fake.js` | A simulated radio: bootloader with a flash model, ODR's serial port, a synthetic stock file | `odrflash/fakedfu.py` |
| `stub.js`, `lz4.js` | The HR_C7000 boot stub: loaded through the boot ROM or handed over by ODR, then reads, sector writes and the LED | `stub/stubtool.py` |
| `f4stub.js` | The STM32F4 loader: handed over by ODR, the board record (CONFIG) and its echo, then the install in the interlock's order and SPI flash writes | `stub/f4stub.py` |
| `c7000.js` | HR_C7000 program flash: what a read holds, the codec found by content, the codec slot, what an install writes | `tools/hrc7000_codec.py` |
| `fakec7000.js` | A simulated HR_C7000 radio: boot ROM window, ODR's debug link (VERSION, 0x1A), the stub and both flash chips | |
| `usmart.js`, `iap.js`, `iapui.js` | The stock side of an HR_C7000 radio, by protocol: the stub loaded through a stock debug console (USMART, `flash.stock_console`), the vendor update mode write (IAP, `flash.update_mode`) and their steps | `docs/spec/hardware-db25d.md` 7 |
| `c7000ui.js`, `radiotop.js` | The HR_C7000 backend: its steps (connect, back up and check, write, check) and the radio picture with its LED | |
| `dom.js` | DOM helpers and strings shared by `app.js` and `c7000ui.js` | |
| `app.js` | The page: the tasks, the shared steps and the `anyroad-dfu` backend | |

Add `#sim=<radio>-stock` to the flasher's address to run every step against a simulated radio on its stock firmware,
`#sim=<radio>` against one that runs ODR (an STM32F4 radio updates through the STM32F4 loader, an HR_C7000 radio
through the loader hand-over). `#sim` alone is the first radio on stock firmware. The fake comes from the radio's data
(`fakeC7000Options()` and `c7000Simulation()` in `fakec7000.js`, `simulation()` in `fake.js`): a radio with a stock
console gets one, with its first stock version.

### Radio access backends

A radio's `flash.backend` in `data/radios.json` names how the page reaches it (from the board.toml `[flash] family`,
`tools/boards.py` FAMILIES). `app.js` runs the tasks and the steps every
radio shares (radio, build, bundle, symptom, recover); the backend supplies the rest. A new way to install or back up
a radio, such as another bootloader protocol or a stock firmware's own read-back protocol, is a new module that calls
`register()` from `backends.js` and is imported by `app.js`. The task flow doesn't change.

| Field | What it gives |
|---|---|
| `id` | The `flash.backend` value |
| `available(radio, fb, task)` | Whether the page can run `install` or `backup` on the radio (grayed out in the radio list otherwise) |
| `steps(task, {recovering})` | The task's step ids, or null for the shared default |
| `views` | A view function per step the backend owns |
| `stepLabel(step, task)` | Optional: the `flash.step.*` key of a step's name |
| `recovery` | The help task: `first` (`updatemode` checks the bootloader first, `fix` goes to the choices), `stock` (`file`: stock firmware from the user's stock file; `restore`: from a backup), `reach` and `stockText` (string keys), and optionally `extra(radio, fb)`: more reinstall choices, each `{id, title, text, prepare()}` (string keys; `prepare` runs once the install task is set up) |
| `ownsBundle(bundle)` | Optional: a backup bundle this backend restores with its own view |
| `ownImage(fb, board, data)` | Optional: a user's own image file, split into the image and anything the backend keeps (a merged codec slot) |
| `release()` | Optional: let go of a port or loader still held (Start Over, another radio) |

| Backend | Radios | Module |
|---|---|---|
| `anyroad-dfu` | DM-1701, MD-380 | `app.js` (`dfu.js`, `f4stub.js`) |
| `c7000-stub` | DM-1801 Pro, DM-32UV, DB25-D PRO | `c7000ui.js` (`stub.js`, `c7000.js`) |
| `none` | any other | none: the radio is listed but grayed out |

A radio without a backend the page knows gets `none`, so a family can come before its module. The
contract tests are in `site/tests/backends.test.mjs`.

Bulk reads for a backup go through readers (`readers.js`). `backup.js` `readRadio` reads the small parts only the
debug link has (radio info, security registers) first. It then tries the readers it is given, in order, and the debug
link reads the memories when none opens. A reader has `open(progress)` (false: not possible now, nothing changed),
`used`, `read(mem, addr, n)` giving `{data, complete}`, `close()` and `abort()`. A new read-back protocol is a reader,
and the bundle format and the backup step stay as they are.

### Backup bundles (`odr-backup/1`)

A zip the user keeps: `backup.json` (the radio, its chip UID, the firmware when the backup was taken and before ODR,
each part's address, size, SHA-256 and whether the read completed), `internal-flash.bin` (from the application
base; the bootloader is never read or written), `spi-flash.bin`, `security-registers.bin` (the factory calibration),
`radio-info.bin`, and `stock-firmware.bin` when the user gave one.

The user picks what to read, each with a time estimate: from a short timed read of this radio, or through the loader
from its measured speeds (stub-stm32f4.md 11) plus the hand-over and the restart:

| Choice | Reads |
|---|---|
| Everything | The internal flash, the whole SPI flash, the security registers |
| Firmware and calibration | The internal flash and the security registers |
| Codeplug and calibration | The codeplug span of the SPI flash (emulated EEPROM to the end of the open-firmware image) and the security registers |

Reads ask for 32 KB at a time. ODR answers that much (open-firmware-cps.md 2.1); older builds answer 2045 bytes, and
the reader takes what comes back.

What can be read depends on the firmware running:

| Radio runs | Read |
|---|---|
| ODR | Everything, through the CPS reads (areas 1, 5, 9 and 10): the whole SPI flash, the internal flash, the security registers |
| ODR with stub entry (STM32F4) | The same. The radio info and security registers come over the debug link; the internal flash and the SPI flash come through the STM32F4 loader, once it has taken the radio's board record, every 4 KB block checked against the radio's CRC, and the radio then restarts into ODR. The codeplug choice stays on the debug link (the hand-over would cost more than it saves). A loader that doesn't start leaves ODR running, and the debug link reads everything |
| Stock (STM32F4) | Nothing: the bootloader can't read flash (read protection level 1) and the stock firmware's own USB protocol has no spec here. The install therefore backs up right after writing ODR: flashing never touches the SPI flash, and ODR doesn't write to a foreign settings region until it is claimed, so the stock codeplug, calibration and resources are still intact |

### Restore

| Target | Writes | Limits |
|---|---|---|
| Exactly as backed up (backup taken on ODR) | The internal flash image through the bootloader (every sector erased and rewritten), then the codeplug areas that differ through CPS writes | The CPS refuses ODR's own areas and the calibration window |
| Codeplug only (any backup that holds the codeplug span) | The codeplug areas that differ, through CPS writes; the firmware is not touched | As above |
| Back to stock | The stock application from the user's stock file, codec included, through the bootloader | The SPI flash stays as it is: writing it back whole needs a firmware command that doesn't exist yet (a gated STORAGE write, like CLAIM). Until then the flasher says so, and the stock codeplug goes back with the vendor CPS |

The stock file's resource segment (SPI flash 0x60000) is not written back: how the vendor updater writes it is not
documented, and ODR doesn't overwrite it unless an ID database is uploaded.

### HR_C7000 radios (the boot stub)

Radios whose backend is `c7000-stub` (the `serial-loader` and `serial-iap` families; the DB25-D PRO adds the section
below) are flashed over their
programming cable with Web Serial, through the boot stub ([stub/README.md](../../stub/README.md),
[debuglink.md](../spec/debuglink.md) 3.18). The Baofeng bootloader is never used and never written. The install has
its own steps:

| Step | What happens |
|---|---|
| Connect | The page looks for a stub that still runs, then for ODR. ODR with `CONFIG_STUB_ENTRY` hands over to the stub (command 0x1A); ODR without it is restarted with its CPS reboot, which passes the boot ROM's serial window (not yet tried on a radio). Otherwise the page asks the user to switch the radio off and on and catches the boot ROM. The radio model is then checked by the bootloader's ID string, and the page sends the stub the board's profile (CONFIG: clocks, UART, pins, board id) and checks the echo; the stub refuses writes until then |
| Back up and check | A first install reads the whole program memory (required) and, if asked, the data memory, keeps the program memory in IndexedDB and downloads a backup bundle. An update reads only the codec slot. The page then decides the codec |
| Write | Only sectors whose CRC differs are written. The first application sector goes last, each sector is read back right away (three tries), and a final pass checks every sector |
| Check | The stub resets the chip; the page asks ODR's VERSION for the build and the codec |

A radio with a second receiver on another chip (the GD-88: SoC B) has that chip as a board of its own, `part_of` the
radio in its `board.toml` with `[flash.stub] bridge`; the site's radio entry carries it as `flash.second`. The cable doesn't reach that chip. Once ODR runs on the first chip, the page's
done step offers to install the second one: the same steps, as a radio with the second board's id (its releases and
builds), reached through the first chip. ODR on the first chip hands the cable over to the second chip's first stage
(`DL_CMD_RX2` BRIDGE, [rx2-link.md](../spec/rx2-link.md) 4), the stub loads and writes as usual, and the first chip
restarts by itself afterwards; the done step asks it whether the second chip answers. A second chip that still runs
its original firmware has no first stage and no link: the first chip reports it down (`DL_CMD_RX2` flags), so the page
offers the factory update mode instead (`IAP_B`, steps from the second board's `[flash.bootloader]`), with the same write as the
DB25-D's. `#sim=gd88-bstock` simulates that second chip. The update-mode texts that describe the screen take the radio's form
where `site/i18n/en.json` has a `<key>_<board id>` text (`iapui.js` `own`): the second chip's board id first, then its
radio's (`_gd88b`: IAP_B on the top line; `_gd88`: IAP_A). The GD-88 is listed with `[flash.stub] tested = false` until the
stub has run on a radio.

#### DB25-D PRO

The DB25-D has no boot ROM window on its cable until ODR's first stage is installed, and no SPI data flash (its NAND is
not read). The board's table entry in `data/flash-boards.json` (`c7000.boards.db25d`, from `tools/hrc7000_codec.py`
BOARDS) carries the 2 MB program flash size, `data_flash: false` and `stub_board`, the profile the page sends in CONFIG
(`stub/stubtool.py` `C7_BOARDS`). Its `data/radios.json` `flash` entry, from board.toml and board.mk, names the rest:
`first_stage` (`BOOT_SIZE`, `BOOT_SIGNATURE`), `stock_console: "usmart"` with the `stock_versions` the console route
knows, `update_mode: "iap"` (the `serial-iap` family), `read_rate` (the program memory read's time estimate) and
`codec_version` (the stock file the page asks for). The steps are the ones above, with these changes:

| Situation | What the page does |
|---|---|
| ODR runs (debug link answers) | The universal stub (`data/c7000-stub.json`, `stubImages()` in `stub.js`) goes into the RAM span the firmware reports in STUB INFO (0x30000 for a build without DMR, 0x1D000 with it) and copies itself to 0x30000. `univFits()` refuses a span whose copy would overlap or leave SRAM, as `stubtool.py` does. After HELLO the page sends CONFIG with the board's profile and checks the echo before any write. The backup is the 2 MB program memory |
| ODR runs but does not answer | "Recover at Power-On": the first stage offers the boot ROM handshake for about half a second at power-on. The page answers it and loads the universal stub at 0x10000, as the boot ROM route does (the first stage takes blocks below 0x4E000) |
| The stock firmware runs | `usmart.js` `Usmart`: the stock firmware's debug console takes one framed command per write (`@AT` + a filler byte + the text + CR LF) and answers `read_addr` and `write_addr`. The page reads the build from its version string (018 or 020; another build is refused), writes the universal stub word by word into 0x54000-0x58000, reads every word back, then points the console's `cmd_rec` entry (0x18000848) at it and sends one more frame, which starts it. Only those addresses can be written, never flash. The stub then copies itself to 0x30000 and the radio continues as from ODR (`c.how = "usmart"`): the 2 MB backup, the codec, the install and a restore all go through the stub. About 4 minutes for the load at 115200 baud |
| Factory update mode (recovery) | `iap.js` `Iap`: the loader's update mode (P1 and power) takes `Erase` and 2 KB pages with additive checksums (`docs/spec/hardware-db25d.md` 7). Install: the first stage, ODR and the codec slot (`odr_db25d_install.bin` with the slot merged in, or the build service's equivalent); it must start with the first stage's signature (`flash.first_stage`). Restore: a backup's application area and slot (`restoreTarget()`), which must start with the same entry. Offered when nothing answers on the cable, for a stock build the page doesn't know, and in Troubleshooting (`recovery.extra`, "Reinstall in Factory Update Mode", which skips the search). `Iap.probe()` sends a one-page `Flash Read ` (never Erase) when the console is silent, and again when Troubleshooting forces update mode: a loader that answers (a radio with no valid application, white screen and red LED at power-on) is reported as already in update mode and the install starts without waiting for a banner (`c.iapReady`). Troubleshooting lists the symptom "White screen and a red LED at every power-on" for radios with a `recovery.extra` route; it leads to the reinstall or Restore from Backup |

The first stage is the only way into the stub from a radio that runs ODR without answering. `site/tests/iap.test.mjs`
runs the console stub load and the IAP write against simulated radios (`fakec7000.js` with `usmart: "018"` models the
console; `#sim=db25d-stock` runs it in the page) and compares the codec carve with `tools/hrc7000_codec.py`
(`ODR_DB25D_STOCK=<a real 018 file> python3 site/tests/fixtures.py` adds the real file to the comparison).

The codec decision (`decide()` in `c7000.js`), first match wins:

| Source | When |
|---|---|
| Keep | The codec slot in flash checks out (magic, CRC, signature, the board's RAM windows). Only the image is written |
| The radio | The stock firmware in the read holds a known codec (found by its signature and SHA-256, wherever that stock version put it). It is copied into the slot |
| This browser | A slot an earlier run kept in IndexedDB |
| A file | A program memory backup, a known stock firmware file, or an image merged by `tools/hrc7000_codec.py` |
| None | Only when the user asks for it: FM works, DMR voice stays off |

The page never sends a STUB command to ODR except right before it enters the stub: any STUB command parks ODR's codec
until the next restart.

The build writes the layout and the codec profiles to `data/flash-boards.json` (`c7000`, from `stub/stubtool.py`,
`tools/hrc7000_codec.py` and `tools/serial_loader.py`) and the universal stub image (with its load address, run address and RAM end) to `data/c7000-stub.json`. Bundle parts:

| File | Part | Content |
|---|---|---|
| `program-memory.bin` | `c7000-progmem` | The whole 1 MB program memory |
| `data-memory.bin` | `c7000-data` | The whole 16 MB data memory |
| `original-channels.bin` | `c7000-data-start` | Data memory 0 to 0xC9000, the stock codeplug area, when the whole data memory isn't read |
| `calibration.bin` | `c7000-calibration` | The 4 KB calibration page (logical ID 2, [hardware-dm32.md](../spec/hardware-dm32.md) 11.1), at its own address |

The calibration choice reads the stock codeplug area, which is about 10 seconds, and is on by default. Choosing the whole
data memory includes it. Restore writes the program memory back from 0x0300C000 up; the data memory is not written.

### STM32F4 radios (the loader route)

An STM32F4 radio on stock firmware always installs through its bootloader (`dfu.js`): that is the only way into a
stock radio. Once it runs ODR, an update can skip the bootloader. When the "current firmware" step finds ODR, the page
asks for the STUB offer (command 0x1A op 0; on the STM32 it only reports the staging buffer and parks nothing). The
loader route is used when all of these hold:

| Condition | Why |
|---|---|
| ODR offers stub entry (`CONFIG_STUB_ENTRY`) | the hand-over needs it |
| ODR reports the radio's codec | the loader writes only the firmware's own flash, so the codec must already be in place |
| the image has no codec in it, and no mode was forced under Advanced | a merge or a forced mode goes through the bootloader |

The codec step then needs no stock file or key. The write step sends the embedded stub (`data/f4-stub.json`, from
`stub/f4stub.py`), finds it on the same port or, if it enumerated again, on the granted port with the same serial
number (one click picks it when the browser didn't keep the grant), and sends it the radio's board record
([stub-stm32f4.md](../spec/stub-stm32f4.md) 5.2: SPI flash bus, mode and pins, chip select, chip selects held high,
crystal, the bootloader's end). It reads HELLO again and goes on only when the echo is the record byte for byte, the
configured flag is set and the application base is 0x0800C000. Then it installs in the order of
[stub-stm32f4.md](../spec/stub-stm32f4.md) 7: sector 3 erased first and its first word written last. A sector that
also holds bytes the firmware doesn't own (no radio has one since the codec moved) is downloaded as a file before
anything is erased, and those bytes are written back unchanged. The bootloader route stays on the same step, folded,
and opens by itself when the loader can't finish. An install cut short leaves the radio in its bootloader at the next
power-on, which that route finishes.

| Loader answer | What the page does |
|---|---|
| HELLO before CONFIG: application base 0, no SPI flash, configured flag clear | Expected for protocol 4: it sends CONFIG |
| CONFIG refused (status `0D`) | Stops, nothing written; the bootloader route opens |
| HELLO after CONFIG echoes other bytes, or another application base | Stops, nothing written |
| A write answered `11` (not configured) | Stops; the page checks the echo before every write, so this means the loader lost its record |
| Protocol 2 (an older loader still running from an earlier session) | No CONFIG: accepted only for a board whose record matches the wiring built into it (`v2_wiring`) |
| A backup through the loader whose record is refused | The page restarts the radio into ODR before it reports the error |

`data/flash-boards.json` carries per board (`f4stub`, from the board registry's `[flash.stub]` through `stub/f4stub.py` `BOARDS` and `config_record()`): the
owned flash, the stub's board id, the 24-byte record as hex, and `v2_wiring`, with the protocol version and record
length at the top. `data/f4-stub.json` gives the image and its protocol. The page sends the record as it is and never
builds one, so the page and `stubtool.py` can't drift; `site/tests/f4stub.test.mjs` checks the records and the HELLO
layout against `f4stub.py`'s simulated stub (`f4-vectors.json`, written by `site/tests/fixtures.py`), and a board
with other SPI wiring against f4stub's test profile.

### Firmware images and custom builds

The flasher gets every image, a prebuilt variant's included, by building it on request (`requestBuild` in `app.js`)
when the build step continues. The page sends the radio, the newest release tag (`main` before the first
release) and the feature list, waits for the build, and checks the image against its manifest. Release images on the
site (`firmware/<radio>/`) are not used by the flasher. The `make` command that builds the same selection, and a picker
for the image it produces, stay under "Build it yourself".

A radio whose image depends on more than the feature switches lists the choices in board.toml `[[build_options]]`
(`flash.build_options` in `data/radios.json`; the MD-380's band: `MD380_BAND=uhf|vhf`). The build step asks for each one
before it builds, or takes it from a connected radio's debug link name (`board_name`), sends it with the request, and
refuses an image whose manifest doesn't carry it (`build_options`). The current-firmware step refuses a radio whose name
says another value.

### Version links

To have someone install a version that isn't a release (a commit to try out), send them the flasher with
`?version=`:

```
https://<site>/flash/?version=<full 40-character commit hash>
https://<site>/flash/?version=<full commit hash>#radio=dm1701
https://<site>/flash/?version=v0.3.0
```

| Behavior | Detail |
|---|---|
| What it builds | The build request sends that ref in place of the newest release tag. The build service takes a release tag, `main` or a full commit hash, and the commit has to be pushed. A short hash shows a "can't find" banner and builds nothing. |
| How long it lasts | The tab keeps it in `sessionStorage` for the session: reloads and visits to other pages keep it, a new browser session doesn't. `?version=` (empty) drops it. |
| On every step | A banner names the version and has "Install the latest release instead", which drops it. |
| Before the build | The build step's button says "Get version <hash>", and a dialog asks for confirmation. The dialog can also switch to the latest release and continue with it. |
| `#radio=` | Picks the radio, as on any flasher link. |

The feature list comes from the site's own release data. A commit with other feature names makes the build fail with
the service's error.

### A failed install (the help task)

"My radio isn't working" on the start page asks for the radio and the symptom, then walks to a fix. Every screen
keeps the reassurance that the page never writes the part of the radio that starts update mode.

| Symptom | Path |
|---|---|
| Won't switch on | Battery check, then (STM32) a practice of update mode with a connection check, then the fix chooser. |
| Stuck on a screen, or the install stopped | Update mode check (STM32) or straight to the fix chooser (HR_C7000, which the page reaches over the cable at power-on). |
| Works but something is wrong | The fix chooser. |

The fix chooser offers installing again (the install steps without the current-firmware question), putting back a
backup (the restore task with the target preselected) or the original firmware (stock file, then update mode). Each
failure message in the install steps links here (`helpLink`).

### USB network mode

A radio in USB network mode (1FC9:0096, [usb-network.md](../spec/usb-network.md)) has no serial port, and the
flasher can't reach it another way: pages can't send UDP (port 3334 carries the debug link and CPS in that mode), an
https page can't fetch from the radio's http status server, and the OS network driver owns the adapter's interfaces,
so WebUSB can't claim them. WebUSB can still list the device, so the flasher detects it (a radio this page has seen
before is found without a prompt; otherwise "Check for USB network mode" under every connect button) and tells the user
to switch USB back to serial on the radio. The bootloader is unaffected: it is a separate USB device.

### Browser support

WebUSB and Web Serial: Chrome, Edge and other Chromium browsers on a computer, over https or localhost. Windows needs
the WinUSB driver for the bootloader (Zadig, once), as odrflash does. On Windows the bootloader step shows the Zadig
steps ([installing](../user/02-installing.md#windows-the-bootloader-driver)), open until this computer has reached a
bootloader once, and again with any failed connect: without the driver Chrome either doesn't list the bootloader or
can't open it. The device name comes from `[flash.dfu] product` in the board's `board.toml`.

## The Codeplug Editor

`site/templates/codeplug.html` and `site/static/js/editor/` make the page (`codeplug/index.html`).
`site/static/js/codeplug/` holds what doesn't depend on the page, so the Tools page, the flasher and the tests can
use it too. Both are plain ES modules that also run in Node. The user's view is in
[Programming the radio](../user/15-programming-qdmr.md#the-codeplug-editor).

| Module | Does |
|---|---|
| `codeplug/image.js` | The codeplug as one flat image, file offset = the STM32 wire address ([open-firmware-cps.md](../spec/open-firmware-cps.md) 7.1), as `tools/cps_client.py` and backups store it. Keeps every byte the editor doesn't decode |
| `codeplug/fields.js` | Field encodings: BCD frequencies, IDs and tones, 0xFF-padded names, 24-bit locations |
| `codeplug/records.js` | Each record decoded into a plain object and encoded back. An encoder writes only the fields it owns, so vendor bytes survive an edit |
| `codeplug/model.js` | The editing model. References are the records themselves, so a delete or renumber can't leave one pointing at the wrong record. `encode()` rewrites only changed records: an unedited codeplug round-trips byte for byte. `validate()` returns the Problems list |
| `codeplug/iddb.js` | The DMR ID database, format N (7.5): area 1, then area 2 up to the radio's end |
| `codeplug/csv.js` | Tables as CSV (RFC 4180): fixed English headers, references by name, import matched by number then name |
| `codeplug/yaml.js` | The YAML subset qdmr writes: block and flow maps and lists, quoted values, `!default` tags, comments. Anchors are refused |
| `codeplug/qdmr.js` | qdmr YAML to the model and back ([qdmr-yaml.md](../spec/qdmr-yaml.md)), with notes on what each side can't hold |
| `codeplug/transport.js` | How the page reaches a radio (below) |
| `codeplug/fakecps.js` | A simulated ODR radio: VERSION, CPS reads, sector writes and `C 5`, writes refused outside the codeplug as the radio refuses them |
| `codeplug/sources/` | Contacts sources: `radioid.js` (RadioID.net's `user.csv` and API), `regions.js` (generated region names), `output.js` (users to the ID database or contacts) |
| `editor/app.js` | The page: tabs, the file menu, radio read and write, the draft |
| `editor/grid.js`, `views.js` | The table (in-place edit, sort, filter, selection, keyboard) and each record kind's columns, form and membership editor |
| `editor/undo.js` | Snapshots of every table's order and every record's fields |
| `editor/files.js` | What an opened file holds (codeplug image, backup bundle, `codeplug.bin`), and the draft in IndexedDB |
| `editor/extensions.js`, `parts.js` | Optional parts register their Save items, table actions, text file openers and DMR ID Database views. `parts.js` lists them, so a part is added without touching `app.js` |
| `editor/csvpart.js`, `qdmrpart.js`, `contactspart.js` | The CSV, qdmr YAML and RadioID parts |

Add `#sim` to the page's address to read and write a simulated DM-1701 (`fakecps.js`).

### Transports

A transport has `identify(radios)`, `read({iddb, progress, signal})`, `write(image, current, {progress, signal})` and
`close()`. Addresses are image addresses; the transport adds the radio's base on the wire. `cps-serial` is the only one
today: the open-firmware CPS ([cps-protocol.md](../spec/cps-protocol.md) 2) over the radio's USB serial port or the
programming cable (Web Serial). It identifies the radio by the board name the debug link reports, matched against
`codeplug.names` in `data/radios.json`. The registry (`tools/boards.py` `Board.codeplug()`) fills that entry from the
board's flash map: the base (0x700000 on the DM-32UV), the end of the room for the codeplug and ID database, and whether
the radio has a TFTP staging area. A read takes the codeplug's regions and the ID database. A write compares with the
radio and sends only the changed bytes inside the codeplug's regions, never whole sectors, since a sector can hold the
radio's own settings (0x6000). It verifies, then ends with `C 5` so the radio reloads the codeplug without a restart.

A browser can't reach the radio's UDP port or its http server (see USB network mode above), so for a radio in USB
network mode the editor saves `codeplug.bin` for `tftp put` instead. A faster transport (the loader's CRC-compared
sector writes) can register beside `cps-serial` later.

### RadioID.net

RadioID.net's API sends no CORS headers. `radioid.js` tries a request directly, then through a public CORS proxy
(`PROXIES`), until RadioID allows the site's origin. The user's own API key (optional) is sent only on direct requests
(`X-API-Token`), never through a proxy, and is kept in the browser's `localStorage` with the user's choices. Pages
come 200 users at a time, one request after another. Larger selections use a `user.csv` the user downloads. No user
data is in the repository or on the site: `regions.js` holds only country and state names
(`site/odrsite/radioid_regions.py`).

### Tests

`site/tests/codeplug.test.mjs` checks the decoder and encoder against the radio's own codeplug code: `fixtures.py`
builds `tests/host/cpsSim`, stores a populated codeplug, and records what the radio reads back (`--detail`). The JS
decode must match it, and the encode must store what the radio then reads. `csv.test.mjs` covers the CSV round trips
and import problems; `sources.test.mjs` covers the RadioID parser, the API client against a fake `fetch` (the key
never sent through the proxy), templates and outputs. `qdmr.test.mjs` covers the YAML parser, the mapping, round
trips and the converters; when `dmrconf` (qdmr's command line tool) is installed it also encodes an exported file.
Nothing reaches the network.
