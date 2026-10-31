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
| `codeplug/fastlink.js` | The fast codeplug protocol ([cps-fast.md](../spec/cps-fast.md)): block CRCs, LZ4 reads and pipelined writes |
| `codeplug/iddb.js` | The DMR ID database in format N and format C ([iddb-compact.md](../spec/iddb-compact.md)): write, read, convert, size |
| `codeplug/sources/` | Contacts sources: `radioid.js` (RadioID.net's `user.csv` and API), `regions.js` (generated region names), `output.js` (users to the ID database or contacts) |
| `editor/app.js` | The page: tabs, the file menu, radio read and write, the draft |
| `editor/grid.js`, `views.js` | The table (in-place edit, sort, filter, selection, keyboard) and each record kind's columns, form and membership editor |
| `editor/undo.js` | Snapshots of every table's order and every record's fields |
| `editor/files.js` | What an opened file holds (codeplug image, backup bundle, `codeplug.bin`), and the draft in IndexedDB |
| `editor/extensions.js`, `parts.js` | Optional parts register their Save items, table actions, text file openers and DMR ID Database views. `parts.js` lists them, so a part is added without touching `app.js` |
| `editor/csvpart.js`, `qdmrpart.js`, `contactspart.js` | The CSV, qdmr YAML and RadioID parts |

Add `#sim` to the page's address to read and write a simulated radio (`fakecps.js`), `#sim-compact` for one that
reads format C ID databases. Append `&release=vYY.M` for a radio that runs that release, or `&git_time=N` for a build
of a commit from that time, which brings up the update notice when `releases.json` lists a newer one.

After a read, the radio is offered the newest release for its board from `releases.json` when that is newer
(`updateFor` in `flasher/version.js`). A radio that runs a release (`release=` in VERSION) is compared by release name,
and is offered pre-releases only when it runs one. Any other build (a developer's, one from a version link) is
compared by commit time (`git_time=` against the release's `git_time`) and is offered only final releases. Firmware
from before `git_time=` that isn't a release gets no offer. The dialog lists what the editor gains (`CPS_FAST`,
`IDDB_COMPACT`, when the release has them and the radio doesn't) and opens the installer for the radio. It comes once
per tab for each radio, running release and new release, after the compact database offer when both apply.

## Transports

A transport has `identify(radios)`, `read({iddb, progress, signal})`, `write(image, current, {progress, signal})` and
`close()`. Addresses are image addresses; the transport adds the radio's base on the wire. `cps-serial` is the only one
today: the open-firmware CPS ([cps-protocol.md](../spec/cps-protocol.md) 2) over the radio's USB serial port or the
programming cable (Web Serial). It identifies the radio by the board name the debug link reports, matched against
`codeplug.names` in `data/radios.json`. The registry (`tools/boards.py` `Board.codeplug()`) fills that entry from the
board's flash map: the base (the CPS address offset, nonzero on some radios), the end of the room for the codeplug and ID database, and whether
the radio has a TFTP staging area. A read takes the ID database's header first
(`readIddbHeader`), then the codeplug's regions and the database in one pass, so one progress bar covers it. The
database's size and the last read's speed (a guess on a radio's first read) give its time: at `IDDB_ASK` (20 s) or
more the editor asks before reading. A skipped database stays out of the model (its header erased there, kept in `S.conn.image`), so no write
touches it; the DMR ID Database tab reads it later into the model. A write compares with the
radio and sends only the changed bytes inside the codeplug's regions, never whole sectors, since a sector can hold the
radio's own settings (0x6000). It verifies, then ends with `C 5` so the radio reloads the codeplug without a restart.

The radio's bytes a write compares with (`S.conn.image`) belong to one radio. A programming cable stays open while the
radio on it is swapped, so before every read and write the page asks `who(radios)`: the board again and the chip UID
(INFO_CHIP). A different UID, or none, drops the image and the write reads the radio first. `keepIdentity(want, have)`
copies the radio's name, DMR ID and boot text (`IDENTITY`) into the image it writes, for one codeplug written to
several radios.

A browser can't reach the radio's UDP port or its http server (see USB network mode above), so for a radio in USB
network mode the editor saves `codeplug.bin` for `tftp put` instead.

A radio that lists `CPS_FAST` in its features is read and written with the fast protocol (`fastlink.js`) on the
same port: SUMS compares 4 KB blocks by CRC, so a read with a cached image moves only blocks that differ, and a
write sends only changed blocks, LZ4 coded. The codeplug goes in one staged session when it fits the staging area.
The ID database goes in its own direct session, its header block first with the header erased and again last.
Blocks the radio refuses as out of range fall back to CPS sector writes; `tr.noFast` turns the protocol off.

The ID database is kept as records and written in the format the radio reads: format C when it lists
`IDDB_COMPACT`, else format N. A format N database read from a radio that reads C is offered for conversion; a format
C database written to a radio that doesn't is converted to N, with a warning when records don't fit.

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
