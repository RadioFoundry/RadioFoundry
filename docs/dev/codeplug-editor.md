<!-- SPDX-License-Identifier: MIT -->

# The Codeplug Editor

The browser tool for editing a codeplug, hosted on the site ([website.md](website.md)). For users it is the recommended
way to edit a codeplug ([Programming the radio](../user/04-programming.md)); this page is for people who change it.

`site/templates/codeplug.html` and `site/static/js/editor/` make the page (`codeplug/index.html`).
`site/static/js/codeplug/` holds what doesn't depend on the page, so the Tools page, the flasher and the tests can
use it too. Both are plain ES modules that also run in Node. The user's view is in
[Programming the radio](../user/04-programming.md#the-codeplug-editor).

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
| `codeplug/detect.js`, `codeplug/stock_tyt.js` | Telling the open-firmware layout from a vendor codeplug, and converting that to the editing model ([codeplug-stock-tyt.md](../spec/codeplug-stock-tyt.md)) |
| `codeplug/transport.js` | How the page reaches a radio (below) |
| `codeplug/fakecps.js` | A simulated RadioFoundry radio: VERSION, CPS reads, sector writes and `C 5`, writes refused outside the codeplug as the radio refuses them |
| `codeplug/sources/` | Contacts sources: `radioid.js` (RadioID.net's `user.csv` and API), `regions.js` (generated region names), `output.js` (users to the ID database or contacts) |
| `editor/app.js` | The page: tabs, the file menu, radio read and write, the draft |
| `editor/grid.js`, `views.js` | The table (in-place edit, sort, filter, selection, keyboard) and each record kind's columns, form and membership editor |
| `editor/undo.js` | Snapshots of every table's order and every record's fields |
| `editor/files.js` | What an opened file holds (codeplug image, backup bundle, `codeplug.bin`), and the draft in IndexedDB |
| `editor/extensions.js`, `parts.js` | Optional parts register their Save items, table actions, text file openers and DMR ID Database views. `parts.js` lists them, so a part is added without touching `app.js` |
| `editor/csvpart.js`, `qdmrpart.js`, `contactspart.js` | The CSV, qdmr YAML and RadioID parts |

Add `#sim` to the page's address to read and write a simulated radio (`fakecps.js`).

## Transports

A transport has `identify(radios)`, `read({iddb, progress, signal})`, `write(image, current, {progress, signal})` and
`close()`. Addresses are image addresses; the transport adds the radio's base on the wire. `cps-serial` is the only one
today: the open-firmware CPS ([cps-protocol.md](../spec/cps-protocol.md) 2) over the radio's USB serial port or the
programming cable (Web Serial). It identifies the radio by the board name the debug link reports, matched against
`codeplug.names` in `data/radios.json`. The registry (`tools/boards.py` `Board.codeplug()`) fills that entry from the
board's flash map: the base (the CPS address offset, nonzero on some radios), the end of the room for the codeplug and ID database, and whether
the radio has a TFTP staging area. A read takes the codeplug's regions and the ID database. A write compares with the
radio and sends only the changed bytes inside the codeplug's regions, never whole sectors, since a sector can hold the
radio's own settings (0x6000). It verifies, then ends with `C 5` so the radio reloads the codeplug without a restart.

A browser can't reach the radio's UDP port or its http server (see USB network mode above), so for a radio in USB
network mode the editor saves `codeplug.bin` for `tftp put` instead. A faster transport (the loader's CRC-compared
sector writes) can register beside `cps-serial` later.

## RadioID.net

RadioID.net's API sends no CORS headers. `radioid.js` tries a request directly, then through a public CORS proxy
(`PROXIES`), until RadioID allows the site's origin. The user's own API key (optional) is sent only on direct requests
(`X-API-Token`), never through a proxy, and is kept in the browser's `localStorage` with the user's choices. Pages
come 200 users at a time, one request after another. Larger selections use a `user.csv` the user downloads. No user
data is in the repository or on the site: `regions.js` holds only country and state names
(`site/rfoundrysite/radioid_regions.py`).

## Tests

`site/tests/codeplug.test.mjs` checks the decoder and encoder against the radio's own codeplug code: `fixtures.py`
builds `tests/host/cpsSim`, stores a populated codeplug, and records what the radio reads back (`--detail`). The JS
decode must match it, and the encode must store what the radio then reads. `csv.test.mjs` covers the CSV round trips
and import problems; `sources.test.mjs` covers the RadioID parser, the API client against a fake `fetch` (the key
never sent through the proxy), templates and outputs. `qdmr.test.mjs` covers the YAML parser, the mapping, round
trips and the converters; when `dmrconf` (qdmr's command line tool) is installed it also encodes an exported file.
Nothing reaches the network.
