<!-- SPDX-License-Identifier: MIT -->

# The web site and the web flasher

The site is generated from the repository, like the PDF manual. It lists the radios, their screens
and features, holds the documentation, and carries the web flasher: back up, install and restore in the browser,
with nothing sent to a server.

```
python3 site/build.py                       # build/site/
python3 site/build.py --serve               # build, then http://localhost:8000 (WebUSB needs localhost or https)
python3 site/build.py --firmware build/dist # with release images for the flasher (tools/release.py --out)
python3 site/build.py --strict              # any warning fails (CI)
site/tests/run.sh                           # the flasher's tests, against odrflash (Node 20+)
```

The generator needs one Python package, markdown-it-py (`pip install -r site/requirements.txt`, or prefix the
command with `uv run --no-project --with-requirements site/requirements.txt`). The pages need no build step and no
npm packages: plain HTML, CSS and ES modules. `tools/ci.sh` builds the site and runs the tests (Python step).

`.github/workflows/site.yml` deploys it to GitHub Pages:

| Run | Does |
|---|---|
| Nightly (07:17 UTC) | Builds and deploys only if something changed since the last deployment: a file the site is built from (`SITE_INPUTS` in the workflow), or a release published after it. Otherwise stops after the check |
| By hand (Actions > Web site > Run workflow) | Always builds and deploys |
| Pull request touching the site's inputs | Builds and runs the tests, never deploys |

A merge or a release therefore shows on the site the next morning, or right away when the workflow is run by hand.

## Sources

Nothing on the site is copied by hand. Each page reads its source at build time:

| Shows | Comes from |
|---|---|
| Radios, their status, planned radios | The README's "Radios and status" table |
| A radio's one-line note for visitors | `radio.note.<radio>` and `planned.note.<slug of the README name>` in the string catalog (the README's notes are for developers; the build warns when one is missing) |
| A radio's name, bands, processor, screen, bootloader steps, flash backend | `boards/<radio>/radio.json` |
| Which features a radio has, per variant | `make BOARD=<radio> [VARIANT=<v>] print-config`, `print-unsupported`, `print-required` |
| Feature names, help, dependencies | `config/features.mk` (the comment above each switch, the `needs` checks) |
| Feature groups, titles, flash cost, notes | The tables in [feature-matrix.md](../feature-matrix.md) |
| Variants and why each leaves a feature out | `config/variants/<id>.mk` ([build-config.md](../build-config.md)) |
| Screenshots | `docs/ux/screens/<gallery>/` (`make -C tests/host ui-gallery`), grouped by `site/data/screens.json` |
| Flash layout, codec profiles, stock files, key hashes | `tools/odrflash/odrflash/boards.py` and `stockfw.py`, exported to `data/flash-boards.json` |
| Documentation | Every Markdown file under `docs/` |
| Firmware images | Release manifests and images (`--firmware`; the workflow downloads the newest release) |

The build fails when a source doesn't parse, and warns (an error with `--strict`) when a feature has no row in the
feature matrix, a screen isn't in `screens.json` or a string is missing from the English catalog.

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
| `site/static/js/tools.js` | The Tools page (codeplug conversion) |
| `site/static/img/radios/src/<radio>.svg` | Radio illustrations (below) |
| `site/tests/` | Flasher tests: `fixtures.py` makes inputs with odrflash, `flasher.test.mjs` checks the JavaScript |

## Templates and design

Markup lives in `site/templates/*.html` and `partials/`, with four kinds of placeholder: `{{ value }}` (escaped),
`{{{ html }}}` (as built), `{{t key}}` (a translated string) and `{{> partial }}`. `{{root}}` is the path to the site
root (static files, data) and `{{home}}` the path to the root of the page's language (links between pages). Loops
stay in Python, which renders a partial per item.

All visual decisions are variables in `tokens.css`: colors for light and dark, type sizes, spacing, radii.
`site.css` only uses them. A redesign replaces `tokens.css`, then `site.css` and the templates; the generator doesn't
change. Readability comes first ([principles](../ux/principles.md)): 18 px base type, high contrast, nothing
squeezed.

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
  display's visible area, with the aspect ratio of the radio's gallery (5:4 for 160x128 and 160x120i5, 3:4 for
  120x160).

The build writes `static/img/radios/<radio>-frame.svg` (screen black), `<radio>.svg` (with the boot screen) and the
screen's position in `data/radios.json` (`frame_geometry`: viewBox units and percentages), so any screenshot can be
laid over the frame; the radio pages do that when a screenshot is clicked. A radio without an illustration gets a
generic drawing from `radio.json` `frame`.

## The web flasher

`site/static/js/flasher/`, plain ES modules. The logic ports odrflash, and the tests check the two agree:

| Module | Does | Ported from |
|---|---|---|
| `stockfw.js` | Stock file container, decryption, codec extraction, key derivation (column vote and runner-up search; the MD-380's pair refinement isn't ported) | `odrflash/stockfw.py` |
| `plan.js` | Sector plans: keep, merge, and restore (a whole image put back exactly) | `odrflash/plan.py` |
| `manifest.js` | Release manifest checks | `odrflash/manifest.py` |
| `decide.js` | Keep or merge the codec | `odrflash/cli.py` `decide_codec` |
| `dfu.js` | The AnyRoad DFU bootloader over WebUSB | `odrflash/dfu.py` |
| `bootkey.js` | The bootloader key from a radio that runs ODR: the bootloader read over READ_MEM, the block with the key's SHA-256 | `odrflash/bootkey.py` |
| `link.js` | The debug link and open-firmware CPS reads and writes over Web Serial | `odrflash/debuglink.py`, [open-firmware-cps.md](../spec/open-firmware-cps.md) |
| `backup.js`, `zip.js` | Backup bundles | |
| `restore.js` | What a bundle can put back, and the plans for it | |
| `select.js` | À la carte feature selection with the dependencies, matching prebuilt variants | |
| `codeplug.js` | Codeplug formats and the converter registry | |
| `fake.js` | A simulated radio: bootloader with a flash model, ODR's serial port, a synthetic stock file | `odrflash/fakedfu.py` |
| `app.js` | The page | |

Add `#sim` to the flasher's address to run every step against the simulated radio.

### Backup bundles (`odr-backup/1`)

A zip the user keeps: `backup.json` (the radio, its chip UID, the firmware when the backup was taken and before ODR,
each part's address, size, SHA-256 and whether the read completed), `internal-flash.bin` (from the application
base; the bootloader is never read or written), `spi-flash.bin`, `security-registers.bin` (the factory calibration),
`radio-info.bin`, and `stock-firmware.bin` when the user gave one.

The user picks what to read, each with a time estimate from a short timed read of this radio:

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
| Stock (DM-1701, MD-380) | Nothing: the bootloader can't read flash (read protection level 1) and the stock firmware's own USB protocol has no spec here. The install therefore backs up right after writing ODR: flashing never touches the SPI flash, and ODR doesn't write to a foreign settings region until it is claimed, so the stock codeplug, calibration and resources are still intact |

### Restore

| Target | Writes | Limits |
|---|---|---|
| Exactly as backed up (backup taken on ODR) | The internal flash image through the bootloader (every sector erased and rewritten), then the codeplug areas that differ through CPS writes | The CPS refuses ODR's own areas and the calibration window |
| Codeplug only (any backup that holds the codeplug span) | The codeplug areas that differ, through CPS writes; the firmware is not touched | As above |
| Back to stock | The stock application from the user's stock file, codec included, through the bootloader | The SPI flash stays as it is: writing it back whole needs a firmware command that doesn't exist yet (a gated STORAGE write, like CLAIM). Until then the flasher says so, and the stock codeplug goes back with the vendor CPS |

The stock file's resource segment (SPI flash 0x60000) is not written back: how the vendor updater writes it is not
documented, and ODR doesn't overwrite it unless an ID database is uploaded.

### Firmware images and custom builds

The flasher gets every image, a prebuilt variant's included, by building it on request (`requestBuild` in `app.js`)
when the build step continues. The page sends the radio, the newest release tag (`main` before the first
release) and the feature list, waits for the build, and checks the image against its manifest. Release images on the
site (`firmware/<radio>/`) are not used by the flasher. The `make` command that builds the same selection, and a picker
for the image it produces, stay under "Build it yourself".

A radio whose image depends on more than the feature switches lists the choices in `radio.json` `flash.build_options`
(the MD-380's band: `MD380_BAND=uhf|vhf`). The build step asks for each one before it builds, or takes it from a
connected radio's debug link name (`board_name`), sends it with the request, and refuses an image whose manifest
doesn't carry it (`build_options`). The current-firmware step refuses a radio whose name says another value.

### USB network mode

A radio in USB network mode (1FC9:0096, [usb-network.md](../spec/usb-network.md)) has no serial port, and the
flasher can't reach it another way: pages can't send UDP (port 3334 carries the debug link and CPS in that mode), an
https page can't fetch from the radio's http status server, and the OS network driver owns the adapter's interfaces,
so WebUSB can't claim them. WebUSB can still list the device, so the flasher detects it (a radio this page has seen
before is found without a prompt; otherwise "Check for USB network mode" under every connect button) and tells the user
to switch USB back to serial on the radio. The bootloader is unaffected: it is a separate USB device.

### Browser support

WebUSB and Web Serial: Chrome, Edge and other Chromium browsers on a computer, over https or localhost. Windows needs
the WinUSB driver for the bootloader (Zadig, once), as odrflash does.
