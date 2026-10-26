<!-- SPDX-License-Identifier: MIT -->

# The Web Flasher

The browser tool that backs up, installs, updates and restores a radio, from `site/static/js/flasher/`. For users it is the
recommended way to install RadioFoundry ([Installing](../user/02-installing.md)); this page is for people who change it.
The site that hosts it is described in [website.md](website.md).

The flasher is plain ES modules. The logic ports rfoundry, and the tests check the two agree:

| Module | Does | Ported from |
|---|---|---|
| `stockfw.js` | Stock file container, decryption, codec extraction, key derivation (column vote and runner-up search; the byte-pair refinement some keys need isn't ported) | `rfoundry/stockfw.py` |
| `vendorfile.js` | Every vendor file a user picks, on every radio's page (below) | |
| `explain.js` | Why a vendor file can't be used, as one of six problems, and what an error report says about the file (below) | |
| `bytes.js` | Byte helpers (hex, CRC, concatenation) shared by the modules | |
| `codecmap.js` | Codec maps: which words a relocated codec rewrites ([codec-placement.md](../spec/codec-placement.md)) | `rfoundry/codecmap.py` |
| `plan.js` | Sector plans: keep, merge, and restore (a whole image put back exactly) | `rfoundry/plan.py` |
| `manifest.js` | Release manifest checks | `rfoundry/manifest.py` |
| `decide.js` | Keep or merge the codec | `rfoundry/cli.py` `decide_codec` |
| `dfu.js` | The AnyRoad DFU bootloader over WebUSB | `rfoundry/dfu.py` |
| `codecsave.js` | The codec read back from a radio that runs RadioFoundry (where stock keeps it, or where the merge placed it), checked against the profile's hashes, and a copy kept in IndexedDB: before every update that erases it, elsewhere once, so an install that can't keep or check the codec (a radio back on stock firmware, in its bootloader, or after an interrupted install) needs no stock file | `rfoundry/codecsave.py` |
| `bootkey.js` | The bootloader key from a radio that runs RadioFoundry: the bootloader read over READ_MEM, the block with the key's SHA-256 | `rfoundry/bootkey.py` |
| `link.js` | The debug link and open-firmware CPS reads and writes over Web Serial | `rfoundry/debuglink.py`, [open-firmware-cps.md](../spec/open-firmware-cps.md) |
| `backends.js` | The radio access backend registry: which steps and views a radio's family uses (below) | |
| `readers.js` | Bulk memory readers for backups: the debug link and the STM32F4 loader (below) | |
| `backup.js`, `zip.js` | Backup bundles | |
| `restore.js` | What a bundle can put back, and the plans for it | |
| `select.js` | À la carte feature selection with the dependencies, matching prebuilt variants | |
| `codeplug.js` | Codeplug formats and the converter registry | |
| `fake.js` | A simulated radio: bootloader with a flash model, RadioFoundry's serial port, a synthetic stock file | `rfoundry/fakedfu.py` |
| `stub.js`, `lz4.js` | The HR_C7000 boot stub: loaded through the boot ROM or handed over by RadioFoundry, then reads, sector writes and the LED | `stub/stubtool.py` |
| `f4stub.js` | The STM32F4 loader: handed over by RadioFoundry, the board record (CONFIG) and its echo, then the install in the interlock's order and SPI flash writes | `stub/f4stub.py` |
| `c7000v1.js` | Rebuilds a version 1 codec slot (from before the codec moved) as version 2 | |
| `c7000.js` | HR_C7000 program flash: what a read holds, the codec found by content, the codec slot, what an install writes | `tools/hrc7000_codec.py` |
| `fakec7000.js` | A simulated HR_C7000 radio: boot ROM window, RadioFoundry's debug link (VERSION, 0x1A), the stub, both flash chips and the Baofeng updater (`FakeBfLoader`) | `tools/serial_loader.py` `FakeBootloader` |
| `usmart.js`, `iap.js`, `bfloader.js`, `iapui.js` | The stock side of an HR_C7000 radio, by protocol: the stub loaded through a stock debug console (USMART, `flash.stock_console`), the factory update mode write (IAP or the Baofeng updater, `flash.update_mode`) and their steps | `tools/serial_loader.py` (`bfloader.js`) |
| `c7000ui.js`, `radiotop.js` | The HR_C7000 backend: its steps (connect, back up and check, write, check) and the radio picture with its LED | |
| `dom.js` | DOM helpers and strings shared by `app.js` and `c7000ui.js` | |
| `app.js` | The page: the tasks, the shared steps and the `anyroad-dfu` backend | |

Add `#sim=<radio>-stock` to the flasher's address to run every step against a simulated radio on its stock firmware,
`#sim=<radio>` against one that runs RadioFoundry (an STM32F4 radio updates through the STM32F4 loader, an HR_C7000 radio
through the loader hand-over). `#sim` alone is the first radio on stock firmware. The fake comes from the radio's data
(`fakeC7000Options()` and `c7000Simulation()` in `fakec7000.js`, `simulation()` in `fake.js`): a radio with a stock
console gets one, with its first stock version. `#sim=<radio>-romfail` is an HR_C7000 radio on stock firmware whose boot
ROM refuses the load, to practice the factory update mode; a radio with the Baofeng updater enters it when switched on
with "Hold the Keys and Turn On".

## Vendor files

`vendorfile.js` opens whatever the user picks, the same way on every radio's page. The input can be:

- a firmware `.bin`;
- a program flash readout;
- a vendor's zip with updaters, manuals and several builds inside, or a zip inside a zip (three levels at most).

Files are set aside by their content, never their extension: Windows programs, Office documents, PDFs, text and anything
under 64 KB. Each remaining file is identified against every radio the flasher knows, the most certain way first:

| Step | How | Result |
|---|---|---|
| 1 | SHA-256 in a codec profile or a C-SKY profile. A file in the known files without a profile goes on to step 2. A file known to hold no codec (`stockfw.py` `NO_CODEC_FILES`, such as another firmware project's build) is named and stops here | the codec (and the key); the key only for a known file without a profile; `not-vendor` for a file without a codec, with rfoundry's message |
| 2 | A TYT/Baofeng container (`OutSecurityBin`) no profile knows: every known bootloader key is tried, the chosen radio's first. A key that decrypts it names the radio, and the decrypted application is searched for the codec's signature word | the key; "the codec, no recipe for this build" |
| 3 | The C-SKY codec search: the codec's own hashes wherever the file or readout holds it (`c7000.js`) | the codec; "no recipe" when only the signature matches. The signature is the same on every C-SKY radio, so such a file is credited to no radio and never written back as original firmware |
| 4 | An STM32 readout, by the codec windows' hashes (`stockfw.fromFlash`) | named, not used: the page asks for the manufacturer's file instead |

The best file for the chosen radio is used:

1. one with the codec;
2. then one that gives the key;
3. then the first by name.

The page lists what each file is: used, for another radio, set aside and why. A codec found by its signature alone is
never taken. Moving the codec needs that build's map, because its calls into the rest of the vendor firmware differ
per build ([codec-placement.md](../spec/codec-placement.md)).

### When a file can't be used

`explain.js` sorts every failure with a vendor file into one problem. `stockfw.js` `StockError` and `c7000.js`
`CodecError` carry its code; `vendorfile.js` marks each file it opened (`problem` on a file no radio claims, on
anything set aside).

| Problem | When | The page says to |
|---|---|---|
| `other-radio` | The file is identified for another radio the flasher knows (its SHA-256, a key that opens it, the codec's hashes). `extract()` refuses it by name before decrypting it, here and in rfoundry | pick this radio's file, or open the other radio's page |
| `wrong-key` | A vendor container no known bootloader key opens: another model, or a modified build | pick the manufacturer's file for this radio |
| `not-vendor` | No vendor container: a plain image, another firmware project's build, a zip without a firmware file | pick the manufacturer's file, not another project's |
| `unmapped` | This radio's build, but no codec map for it yet: a known build without a profile (kind `key`), an unknown one whose codec was found (`codec-unmapped`), or a C-SKY codec no profile describes | pick one of the supported files, which the notice lists in full (`supportedFiles()`: a C-SKY radio's profiles with a stock file first, then the files its codec sources name; a source that isn't a file, such as a program flash read, in a sentence of its own), or send the file |
| `damaged` | A cut-short file or a damaged zip | download it again |
| `internal` | A check that fails after the file was identified: a fault in the page or its data, not the file | nothing: it has been reported (or report it, without error reports) |

For `unmapped` the notice names the version when the file is known, says when the file's bootloader key still works,
and lists every supported file (folded above 12). When the install goes on without the file's codec (a kept codec),
`keepNote()` picks a second notice on the codec step that says what DMR voice then depends on: the radio's verified
codec, an unverified one, or none. None also covers a radio on its original firmware when no stock version of that
radio keeps the codec where RadioFoundry uses it (`codec.installed` empty).

Each notice gives what happened, why and what to do. The exact message is under **Technical details** and in the error
report. The same notices come on the HR_C7000 pages (`c7000ui.js` gets `problemNotice` and `failureNotice`).

A radio that doesn't answer the debug link probe is told apart by what its port gave (`link.js` `silence()`,
`whyNoAnswer()`): nothing (`silent`), debug link frames that weren't the reply (`link`), or other bytes (`other`). When
the first byte is `-`, the open-firmware CPS's refusal ([open-firmware-cps.md](../spec/open-firmware-cps.md) 2), the
page reads the radio info (area 9, section 4) and checks its structure, with a revision or a build date. Only that read
is sent, never a `C` control command. A match is `openfw`: a radio on another open firmware (the OpenGD77 family), or a
RadioFoundry build with the CPS shim but without the debug link, which answers the same way and can't be told apart in
general. Right after an install, the done step passes the written build's manifest: when the radio's revision is that
build's commit, or its feature list leaves out `DEBUGLINK`, the kind is `rfoundry-nodebug` and the page says the build
has no debug link.

Where the radio answered (`openfw`, `rfoundry-nodebug`), the connection worked, so the page gives the next step instead
of the cable and driver tips: on the "current firmware" step a button that chooses **Another open firmware** (on a
board `openfw.js` backs up from, an `openfw` answer goes straight to that choice with its backup); on the radio, done
and backup steps one line (`connectHelp()`).

The "current firmware" step has four choices (`explain.js` `NOW_CHOICES`): RadioFoundry, original firmware, **Another
open firmware** and **Other / Not sure**. The last two are the same to the install (`S.now.kind` `unknown`: the codec
decision and the steps don't change); `S.now.openfw` marks the first, for the pressed button and the error report.
On a board `openfw.js` supports, **Another open firmware** also offers **Check the Radio** and, once the probe
recognized the radio (`S.now.radioType`, `git`), the backup below. **Other / Not sure** doesn't: its route is the stock
file.

The browser's own USB and serial errors (`NetworkError`, `InvalidStateError`, a failed `fetch`) say little. `errorText()`
puts what to do in front of them: close the program that holds the port, check the cable, the Windows driver or the
Linux device permission, reload the page.

### Sending a file

The page offers **Send this file for further analysis** for `unmapped`, `wrong-key` and `internal`, when what was
picked holds a vendor firmware container: a file with the `OutSecurityBin` or the C-SKY stock header, 4 MB at most. It
comes both when the codec can't be taken from the file and when a later step fails on a file identified for this radio
(placing the codec, restoring). Readouts, other files and a file identified as another radio's are never offered. The
text asks for the manufacturer's original file only. The file goes to the backend only on the user's click, with:

- its name;
- the radio being flashed;
- what the page found (`codec-unmapped`, `key`, `key-unknown`, `unknown-container`, or the problem);
- the site version;
- an optional note (it asks where the file came from). For `internal`, the page's message is added to it.

The backend (`POST /v1/samples`, raw body, the rest as query parameters) checks the header and the size itself. It
rate-limits per address and per day, caps the total storage, keeps one copy per SHA-256 and never serves a file back.
Simulations don't offer it.

### What an error report says about the file

Every error report carries `vendor_file` (`vendorContext()`) once a vendor file was picked. It never holds the file's
bytes:

| Field | Content |
|---|---|
| `board` | The radio being flashed |
| `picked` | What the user chose: name (80 characters at most), size, SHA-256 |
| `files`, `skipped` | Each file opened: path, size, SHA-256, the radio it is for, how it was identified, the known name, its problem; each file set aside and why |
| `used`, `profile` | The file the page took, and its codec profile with that profile's radio |
| `key` | Where the bootloader key came from: `sim`, `saved`, `file` or `radio` |
| `decision` | The codec decision (mode, source) or the task (`restore`) |
| `problem` | The problem, the other radio, the exact message (300 characters at most) |

The start page's privacy text says that a report names the stock firmware file, never its contents.

### The flash log and the vendor file

The flash log (`POST /v1/flashes`, `PATCH /v1/flashes/{id}`) also records which vendor firmware a use involved, so a
failed install can be traced to a file. Only lengths, SHA-256 hashes and a short id; never bytes (those go only through
[Sending a file](#sending-a-file), on the user's click):

| Fields | What | Set |
|---|---|---|
| `stock_len`, `stock_sha256`, `stock_id` | The vendor file in use (`S.stock`: the inner file `vendorfile.js` chose for a zip) and what the page took it for (`logId()`: `codec:<known build>`, `no-recipe:<board>`, `key:<board>`, `problem:<problem>`, `known:<name>`, `unknown`) | with the start of the use, when a file was picked by then |
| `readout_len`, `readout_sha256`, `readout_id` | The internal flash (program memory on the C-SKY radios) read from the radio, and `known:<name>` or the kind of firmware that was running (`noteReadout()`) | with the start when an earlier readout belongs to the use (the backups around an install); with the end for the backup task, which reads after it starts |

`flashStarted()` adds the fields known then to the POST and `flashFinished()` adds them again to the PATCH; the
backend keeps a field the PATCH leaves out. Opting out (`?reporting=0`) and simulations send nothing, as for the rest of
the log. A backend without these columns ignores the extra fields.

Both requests also carry `adapter`, and the PATCH carries `trail`:

| Field | Content |
|---|---|
| `adapter` | `{vid, pid, chip, product}` of the USB device in use (`adapter.js`). `chip` from the vendor id: `FTDI` (0403), `CH340` (1a86), `CP210x` (10c4), `PL2303` (067b), `radio USB` (the radio's own port), `DFU bootloader`, else `other`. `product` only from WebUSB. Never a serial number |
| `trail` | The newest 200 breadcrumbs (`diag.trail()`, scrubbed, 300 characters each), the oldest dropped to keep the keepalive request under 64 KB |

### Breadcrumbs

`diag.note()` lines record what the page decided, built in `trail.js` (names, sizes, hashes and reasons; never bytes):

| Line | When |
|---|---|
| `file picked` | A vendor file: name, size, SHA-256 prefix, `logId()`, what it matched, its problem |
| `codec search at known addresses`, `moved codec search` | The codec search in an open-firmware read: each stage, the profile and address found, or how many code starts were checked and why the nearest failed |
| `codec decision` | `decideCodec()`: mode, source, why; or why none |
| `key` | Where the bootloader key came from |
| `write plan` | Mode, sectors erased, written (with content), kept, blocks, the codec profile |
| `open firmware backup` | The key and codec found in the open-firmware read |
| `verify`, `verify failed` | What the radio reported after the install |
| `cable radio check`, `cable radio codec decision` | The cable radio's read choices and codec decision |
| `adapter` | The USB device, once per change |

The cable log (`logBox()`) also goes to the trail, so a boot ROM load that fails leaves the bytes received.

Every Sentry event carries the newest 40 lines in `contexts.rfoundry.trail`, the adapter in `contexts.rfoundry.adapter`
and the tag `adapter_chip`.

### Report a Problem

`report.js` `reportDialog()`. With the backend configured (the flasher), Send posts to the project's backend
(`problemreport.js`), not Sentry:

1. `POST /v1/reports` with `{result, flash_id, tool, tool_version, board, task, step, decisions, trail, error, message,
   contact, adapter}`. `result` is `problem` when the install verified and `failed` otherwise. `decisions` holds
   `codec_mode`, `codec_source`, `key_source`, `plan` and `files` (role, name, length, SHA-256, id). The answer is 201
   with `id` and an upload `token`.
2. Each checked file: `POST /v1/reports/{id}/files?kind=stock|backup|readout&name=&token=`, `application/octet-stream`.
   A refused file (413, 409, 410, 429, ...) is listed under the result. The report still counts.

The files offered are the vendor file picked, the internal flash read and the last backup the page saved. All are
checked by default. The message and the email address are optional. The dialog says what is sent and that files and
the address are deleted within 90 days. It works with `?reporting=0`, since the user sends it. `#sim` answers
locally (`practiceFetch`) and sends nothing.

Where it is offered: the button under every failure notice (`errorNotice()`, `problemNotice()`), the failed checks of
the install, cable radio and loader routes, and the quiet "Something wrong? Report a Problem" link on Verify.

## Radio access backends

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
| `vendorRestore(radio, file, fb)` | Optional: a restore bundle from the radio's original firmware file, so the restore task takes that file too |
| `ownImage(fb, board, data)` | Optional: a user's own image file, split into the image and anything the backend keeps (a merged codec slot) |
| `release()` | Optional: let go of a port or loader still held (Start Over, another radio) |

| Backend | Radios | Module |
|---|---|---|
| `anyroad-dfu` | `usb-dfu` family (STM32F4 radios) | `app.js` (`dfu.js`, `f4stub.js`) |
| `c7000-stub` | `serial-loader` and `serial-iap` families (HR_C7000 radios) | `c7000ui.js` (`stub.js`, `c7000.js`) |
| `none` | any other | none: the radio is listed but grayed out |

A radio without a backend the page knows gets `none`, so a family can come before its module. The
contract tests are in `site/tests/backends.test.mjs`.

Bulk reads for a backup go through readers (`readers.js`). `backup.js` `readRadio` reads the small parts only the
debug link has (radio info, security registers) first. It then tries the readers it is given, in order, and the debug
link reads the memories when none opens. A reader has `open(progress)` (false: not possible now, nothing changed),
`used`, `read(mem, addr, n)` giving `{data, complete}`, `close()` and `abort()`. A new read-back protocol is a reader,
and the bundle format and the backup step stay as they are.

## Backup bundles (`rfoundry-backup/1`)

A zip the user keeps: `backup.json` (the radio, its chip UID, the firmware when the backup was taken and before RadioFoundry,
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

Reads ask for 32 KB at a time. RadioFoundry answers that much (open-firmware-cps.md 2.1); older builds answer 2045 bytes, and
the reader takes what comes back.

What can be read depends on the firmware running:

| Radio runs | Read |
|---|---|
| RadioFoundry | Everything, through the CPS reads (areas 1, 5, 9 and 10): the whole SPI flash, the internal flash, the security registers |
| RadioFoundry with stub entry (STM32F4) | The same. The radio info and security registers come over the debug link; the internal flash and the SPI flash come through the STM32F4 loader, once it has taken the radio's board record, every 4 KB block checked against the radio's CRC, and the radio then restarts into RadioFoundry. The codeplug choice stays on the debug link (the hand-over would cost more than it saves). A loader that doesn't start leaves RadioFoundry running, and the debug link reads everything |
| Another open firmware (OpenGD77 family) | Everything but the security registers, over that firmware's CPS (`openfw.js`, below) |
| Stock (STM32F4) | Nothing: the bootloader can't read flash (read protection level 1) and the stock firmware's own USB protocol has no spec here. The install therefore backs up right after writing RadioFoundry: flashing never touches the SPI flash, and RadioFoundry doesn't write to a foreign settings region until it is claimed, so the stock codeplug, calibration and resources are still intact |

## Another open firmware (`openfw.js`)

A radio whose port answers the debug link probe with `-` and gives a valid radio info is on another open firmware
(`link.js` `whyNoAnswer()`, kind `openfw`). Where the board supports it, the **Another open firmware** choice of the
current-firmware step offers a full backup before the install, and the install then needs no stock file.

| | |
|---|---|
| Which boards | `openfw.supported()`: CPS radio types (`board.toml` `[cps]`, `cps_radio_types` in `flash-boards.json`), the `usb-dfu` family, a `key_sha256`, an integer `rom_base` and a `protected` list (where the key is searched). All from board data; HR_C7000 boards are left out (other install route, area 1 offset) |
| What the probe leads to | `openfw.recognized()`: `backup` (offered), `wrong-type` (the radio info names another radio: a wrong pick, with the stock-file route, **Other / Not sure**, one click away), `unsupported` (the stock-file route, as for any other firmware) |
| Radio type | The radio info's type must be one of the board's, or nothing is read |
| Requests | `R` only: area 9, then area 5 over exactly [`rom_base`, `flash_end`) (bootloader included, for the key), then area 1 over the whole SPI flash. Never `C` (a `C` 6 2 saves settings and once hung a radio's USB), never area 10 (some builds don't answer), never more than 2045 bytes per request. 0x7590-0x75FF of area 1 is read in 32-byte requests, so the VFO overlay never applies |
| A radio that stops answering | Each request is asked three times (a read changes nothing). Then the backup stops and keeps what it read (`state.stopped`, why `no answer`); after a power cycle the same state continues (`readBackup({state})`) where it stopped. A refused read (`refused`) stops the same way. **Stop Reading**, or leaving the step (`render()` aborts it), stops it as `canceled`. What was read can be saved as an incomplete bundle |
| Fallback | Whatever the backup didn't give comes from the stock file, as from any other firmware: key or codec not found, a read that stopped before them, a skipped backup, a wrong radio type. The codec step says why (`openfwCodecNote()`). `decideCodec()` never keeps a codec it can't verify unless the user forces it |
| Speed | About 190 KB/s (2045-byte requests): 1 MB of MCU flash and 16 MB of SPI flash take about 1.5 minutes |
| The key | `keyFromImage()`: the 1024-byte block of the bootloader region whose SHA-256 is the board's `key_sha256`, searched (`bootkey.js`), no offset assumed. Stored as a key read from a RadioFoundry radio (`S.key`, local storage) |
| The codec | `codecFromImage()`: first the forms `stockfw.fromFlash()` knows (stock addresses, RadioFoundry's placement); then a copy moved anywhere: the codec's first word at each word boundary, the move undone with the codec map (`codecmap.js` `unmoveCode()`, `unmoveBranches()` for tables whose islands call the moved copy or still the stock address), the stock SHA-256s checked (for a profile whose merge cuts the tables, the codec's own data at their start), then placed and checked as a merge from the stock file. Kept as the saved codec (`codecsave.js`), so `decideCodec()` merges from the saved copy. Not found: the codec step asks for the stock file as before |
| Bundle | `firmware_now` `{kind: "openfw", radio_type, git, built, flash_id}`; `radio-info.bin`, `internal-flash.bin` from the application base, `spi-flash.bin`. No security registers; the bootloader (and the key in it) is read but not stored |
| After the install | The settings area is claimed (`prepareStorage()`), as after any full backup |
| Practice mode | `#sim=<board>-openfw`: `fake.js` state `openfw` (2045-byte clip, VFO overlay, unbounded area 5, silent area 10, hangs); `#sim=<board>-openfw-nocodec`: the same without the codec, for the stock-file fallback |

Not yet tested on a real radio.

## Restore

| Target | Writes | Limits |
|---|---|---|
| Exactly as backed up (backup taken on RadioFoundry) | The internal flash image through the bootloader (every sector erased and rewritten), then the codeplug areas that differ through CPS writes | The CPS refuses RadioFoundry's own areas and the calibration window |
| Codeplug only (any backup that holds the codeplug span) | The codeplug areas that differ, through CPS writes; the firmware is not touched | As above |
| Previous open firmware (backup taken on another open firmware) | That firmware's internal flash image through the bootloader (`planInternal`) | The SPI flash is not written back, as for stock: that firmware's own CPS writes its codeplug from `spi-flash.bin` |
| Back to stock | The stock application from the user's stock file, codec included, through the bootloader. Any file for this radio that its key opens can be used, mapped or not: no codec is taken from it | The SPI flash stays as it is: writing it back whole needs a firmware command that doesn't exist yet (a gated STORAGE write, like CLAIM). Until then the flasher says so, and the stock codeplug goes back with the vendor CPS |

The stock file's resource segment (SPI flash 0x60000) is not written back: how the vendor updater writes it is not
documented, and RadioFoundry doesn't overwrite it unless an ID database is uploaded.

## HR_C7000 radios (the boot stub)

Radios whose backend is `c7000-stub` (the `serial-loader` and `serial-iap` families; a board with a first stage adds the section
below) are flashed over their
programming cable with Web Serial, through the boot stub ([stub/README.md](../../stub/README.md),
[debuglink.md](../spec/debuglink.md) 3.18), or through the radio's factory update mode when the stub can't be loaded
(below). No loader in the radio is ever written. The install has its own steps:

| Step | What happens |
|---|---|
| Connect | The page looks for a stub that still runs, then for RadioFoundry. RadioFoundry with `CONFIG_STUB_ENTRY` hands over to the stub (command 0x1A); RadioFoundry without it is restarted with its CPS reboot, which passes the boot ROM's serial window (not yet tried on a radio). Otherwise the page asks the user to switch the radio off and on and catches the boot ROM. The radio model is then checked by the bootloader's ID string, and the page sends the stub the board's profile (CONFIG: clocks, UART, pins, board id) and checks the echo; the stub refuses writes until then |
| Back up and check | A first install reads the whole program memory (required) and the calibration area; the whole data memory only if asked (off by default: 16 MB at 115200 baud). Each choice shows its time: the stub's sector sums count the blank sectors first (no data moved, `c7000.js` `blankCount`, at most 8 s per memory), and `readSeconds` turns the used share into wire bytes, pessimistically: every used sector whole (no credit for LZ4), 13 bytes a record, at 9000 B/s or the rate measured on this radio, never above the 11,520 B/s of the line; without a count every sector is taken as used ("up to"). The page keeps the program memory in IndexedDB and downloads a backup bundle. A first install can skip the program memory read, before or during it, and take the codec from the stock file instead: only the codec slot is read, and the firmware is reported as not read. An update reads only the codec slot. The page then decides the codec |
| Write | Only sectors whose CRC differs are written. The first application sector goes last, each sector is read back right away (three tries), and a final pass checks every sector |
| Check | The stub resets the chip; the page asks RadioFoundry's VERSION for the build and the codec |

A cable whose USB vendor id is a CH340's (1a86) gets a warning on the connect, check, write, backup and restore steps:
some of these cables fail the backup or the install. The boot ROM load drops an exact echo of each frame it sent (some
cables loop TX back to RX). A frame not acknowledged logs the first 16 bytes received, and the error names the cable's
chip and points to the factory update mode.

### The factory update mode

Every HR_C7000 board has one: `update_mode` in its `data/radios.json` `flash` entry, from the family in
`tools/boards.py` (`serial-iap`: `iap`; `serial-loader`: `baofeng`), entered by the keys in `flash.bootloader` (the
steps) and `bootloader_hold` (marked on the radio's picture). The registry check refuses a board on such a family with
no steps.

| Protocol | Module | What it takes |
|---|---|---|
| `iap` | `iap.js` | Erase, then 2 KB pages with an additive checksum the loader reports back. The install image starts with the first stage. A radio without a complete application stays in this mode at every power-on, so the page probes for it when nothing else answers |
| `baofeng` | `bfloader.js` | The Baofeng updater (the protocol `tools/serial_loader.py` names, on the radio pages of the boards that use it): steps 1-3 identify the loader by the board's `bootloader_id`, step 7 erases the whole application area (the codec slot included), then 1 KB blocks, each acknowledged by its CRC-16/XMODEM. A port of `tools/serial_loader.py`'s `Flasher`; `site/tests/bfloader.test.mjs` checks the bytes it sends against the Python tool's (`fixtures.py`) and runs it against `FakeBfLoader` |

The page offers it next to Try Again when the boot ROM load fails or the ROM wait times out, in Troubleshooting
(`recovery.extra`, "Reinstall in Factory Update Mode"), and where the stock console and the first stage routes offer it.
Nothing is read from the radio this way, so the check step takes the codec from a file (`factoryRoute()` in
`c7000ui.js`): the stock firmware file where a profile of the board names one, else a program memory backup (or a slot
this browser kept). The decision is never "keep": the image is `c7.target()` with the slot, without the 0xFF tail
(`factoryInstallImage()` in `iapui.js`). Restore writes a backup's application area and slot, or the original firmware
from its file, the same way. The Baofeng route has not been run from the web page on a radio.

### A board with a first stage

A board with a first stage has no boot ROM window on its cable until the first stage is installed, and may have no
SPI data flash the page can read. The board's table entry in `data/flash-boards.json` (`c7000.boards.<id>`, from
`tools/hrc7000_codec.py` `BOARDS`) carries the program flash size, `data_flash` and `stub_board`, the profile the page
sends in CONFIG (`stub/stubtool.py` `C7_BOARDS`). Its `data/radios.json` `flash` entry, from board.toml and board.mk,
names the rest: `first_stage` (`BOOT_SIZE`, `BOOT_SIGNATURE`), `stock_console` with the `stock_versions` the console
route knows, `update_mode` (the `serial-iap` family), `read_rate` (the program memory read's time estimate) and
`codec_version` (the stock file the page asks for). The steps are the ones above, with these changes:

| Situation | What the page does |
|---|---|
| RadioFoundry runs (debug link answers) | The universal stub (`data/c7000-stub.json`, `stubImages()` in `stub.js`) goes into the RAM span the firmware reports in STUB INFO and copies itself to 0x30000. `univFits()` refuses a span whose copy would overlap or leave SRAM, as `stubtool.py` does. After HELLO the page sends CONFIG with the board's profile and checks the echo before any write |
| RadioFoundry runs but does not answer | "Recover at Power-On": the first stage offers the boot ROM handshake for a short time at power-on. The page answers it and loads the universal stub, as the boot ROM route does |
| The stock firmware runs | `usmart.js`: the stock firmware's debug console takes one framed command per write and answers `read_addr` and `write_addr`. The page identifies the build from its version string (another build is refused), writes the universal stub word by word into RAM, reads every word back, and points the console's command entry at it. Only RAM addresses can be written, never flash. The stub then copies itself to 0x30000 and the radio continues as from RadioFoundry |
| Factory update mode (recovery) | `iap.js`: the loader's update mode takes `Erase` and 2 KB pages with additive checksums. Install: the first stage, RadioFoundry and the codec slot in one file; it must start with the first stage's signature (`flash.first_stage`). Restore: a backup's application area and slot (`restoreTarget()`), or the original firmware from its file. Offered when nothing answers on the cable, for a stock build the page doesn't know, and as on every HR_C7000 board ([above](#the-factory-update-mode)) |

The first stage is the only way into the stub from a radio that runs RadioFoundry without answering.
`site/tests/iap.test.mjs` runs the console stub load and the IAP write against simulated radios (`fakec7000.js`) and
compares the codec carve with `tools/hrc7000_codec.py`.

The codec decision (`decide()` in `c7000.js`), first match wins:

| Source | When |
|---|---|
| Keep | The codec slot in flash checks out (magic, CRC, signature, the board's RAM windows). Only the image is written |
| The radio | The stock firmware in the read holds a known codec (found by its signature and SHA-256, wherever that stock version put it). It is copied into the slot |
| This browser | A slot an earlier run kept in IndexedDB |
| A file | A program memory backup, a known stock firmware file, or an image merged by `tools/hrc7000_codec.py` |
| None | Only when the user asks for it: FM works, DMR voice stays off |

The page never sends a STUB command to RadioFoundry except right before it enters the stub: any STUB command parks RadioFoundry's codec
until the next restart.

The build writes the layout and the codec profiles to `data/flash-boards.json` (`c7000`, from `stub/stubtool.py`,
`tools/hrc7000_codec.py` and `tools/serial_loader.py`) and the universal stub image (with its load address, run address and RAM end) to `data/c7000-stub.json`. Bundle parts:

| File | Part | Content |
|---|---|---|
| `program-memory.bin` | `c7000-progmem` | The whole 1 MB program memory |
| `data-memory.bin` | `c7000-data` | The whole 16 MB data memory |
| `original-channels.bin` | `c7000-data-start` | Data memory 0 to 0xC9000, the stock codeplug area, when the whole data memory isn't read |
| `calibration.bin` | `c7000-calibration` | The 4 KB calibration page (logical ID 2), at its own address |

The calibration choice reads the stock codeplug area, which is about 10 seconds, and is on by default. Choosing the whole
data memory includes it. Restore writes the program memory back from 0x0300C000 up; the data memory is not written.

Without a backup, the original firmware comes back from its vendor file. The restore task takes the file in place of a
bundle (`vendorRestore`) once `vendorfile.js` identifies it for this radio, mapped or not. `c7.progmemFromFile()` takes a
whole program memory readout as it is. A firmware file it lays out as a program memory read holds it: the application
after the file's header (256 bytes for a `BFUV32-V2` file, none for a plain one) at the application start, everything
else erased. The stub or update mode then writes it as it writes a backup. A file larger than the application area is refused as `internal`.

## STM32F4 radios (the loader route)

An STM32F4 radio on stock firmware always installs through its bootloader (`dfu.js`): that is the only way into a
stock radio. Once it runs RadioFoundry, an update can skip the bootloader. When the "current firmware" step finds RadioFoundry, the page
asks for the STUB offer (command 0x1A op 0; on the STM32 it only reports the staging buffer and parks nothing). The
loader route is used when all of these hold:

| Condition | Why |
|---|---|
| RadioFoundry offers stub entry (`CONFIG_STUB_ENTRY`) | the hand-over needs it |
| RadioFoundry reports the radio's codec | the loader writes only the firmware's own flash, so the codec must already be in place |
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
| A backup through the loader whose record is refused | The page restarts the radio into RadioFoundry before it reports the error |

`data/flash-boards.json` carries per board (`f4stub`, from the board registry's `[flash.stub]` through `stub/f4stub.py` `BOARDS` and `config_record()`): the
owned flash, the stub's board id, the 24-byte record as hex, and `v2_wiring`, with the protocol version and record
length at the top. `data/f4-stub.json` gives the image and its protocol. The page sends the record as it is and never
builds one, so the page and `stubtool.py` can't drift; `site/tests/f4stub.test.mjs` checks the records and the HELLO
layout against `f4stub.py`'s simulated stub (`f4-vectors.json`, written by `site/tests/fixtures.py`), and a board
with other SPI wiring against f4stub's test profile.

## Firmware images and custom builds

The flasher gets every image, a prebuilt variant's included, by building it on request (`requestBuild` in `app.js`)
when the build step continues. The page sends the radio, the newest release tag (`main` before the first
release) and the feature list, waits for the build, and checks the image against its manifest. Release images on the
site (`firmware/<radio>/`) are not used by the flasher. The `make` command that builds the same selection, and a picker
for the image it produces, stay under "Build it yourself".

A radio whose image depends on more than the feature switches lists the choices in board.toml `[[build_options]]`
(`flash.build_options` in `data/radios.json`, such as a band: `BAND=uhf|vhf`). The build step asks for each one
before it builds, or takes it from a connected radio's debug link name (`board_name`), sends it with the request, and
refuses an image whose manifest doesn't carry it (`build_options`). The current-firmware step refuses a radio whose name
says another value.

## Version links

To have someone install a version that isn't a release (a commit to try out), send them the flasher with
`?version=`:

```
https://<site>/flash/?version=<full 40-character commit hash>
https://<site>/flash/?version=<full commit hash>#radio=<id>
https://<site>/flash/?version=v0.3.0
https://<site>/flash/?version=feature/my_radio
```

| Behavior | Detail |
|---|---|
| What it builds | The build request sends that ref in place of the newest release tag. The build service takes a release tag, a branch of the main repository (`main` included) or a full commit hash, and the commit has to be pushed. A branch name passes the service's rule: up to four `/`-separated parts of letters, digits, `_`, `.` and `-`, no `..`, no part ending in `.lock` (`version.js`). A short hash shows a "can't find" banner and builds nothing. |
| How long it lasts | The tab keeps it in `sessionStorage` for the session: reloads and visits to other pages keep it, a new browser session doesn't. `?version=` (empty) drops it. |
| On every step | A banner names the version and has "Install the latest release instead", which drops it. |
| Before the build | The build step's button says "Get version <hash>", and a dialog asks for confirmation. The dialog can also switch to the latest release and continue with it. |
| `#radio=` | Picks the radio, as on any flasher link. |

Without a link, a radio with no release is built from `main`. On a preview of another branch (`data/site.json` `branch`, from `CF_PAGES_BRANCH` or the checked-out branch), it is built from that branch instead, so a radio that exists only there can be tried.

The feature list comes from the site's own release data. A commit with other feature names makes the build fail with
the service's error.

## A failed install (the help task)

"My radio isn't working" on the start page asks for the radio and the symptom, then walks to a fix. Every screen
keeps the reassurance that the page never writes the part of the radio that starts update mode.

| Symptom | Path |
|---|---|
| Won't switch on | Battery check, then (STM32) a practice of update mode with a connection check, then the fix chooser. |
| Stuck on a screen, or the install stopped | Update mode check (STM32) or straight to the fix chooser (HR_C7000, which the page reaches over the cable at power-on). |
| Works but something is wrong | The fix chooser. |

The fix chooser offers installing again (the install steps without the current-firmware question), putting back a
backup (the restore task with the target preselected) or the original firmware (STM32: the stock file, then update
mode; HR_C7000: the restore task, with the backup or the original firmware file). Each
failure message in the install steps links here (`helpLink`).

## USB network mode

A radio in USB network mode (1FC9:0096, [usb-network.md](../spec/usb-network.md)) has no serial port, and the
flasher can't reach it another way: pages can't send UDP (port 3334 carries the debug link and CPS in that mode), an
https page can't fetch from the radio's http status server, and the OS network driver owns the adapter's interfaces,
so WebUSB can't claim them. WebUSB can still list the device, so the flasher detects it (a radio this page has seen
before is found without a prompt; otherwise "Check for USB network mode" under every connect button) and tells the user
to switch USB back to serial on the radio. The bootloader is unaffected: it is a separate USB device.

## Browser support

WebUSB and Web Serial: Chrome, Edge and other Chromium browsers on a computer, over https or localhost. Windows needs
the WinUSB driver for the bootloader (Zadig, once), as rfoundry does. On Windows the bootloader step shows the Zadig
steps ([installing](../user/02-installing.md#windows-the-bootloader-driver)), open until this computer has reached a
bootloader once, and again with any failed connect: without the driver Chrome either doesn't list the bootloader or
can't open it. The device name comes from `[flash.dfu] product` in the board's `board.toml`.
