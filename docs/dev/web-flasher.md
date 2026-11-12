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
| `dfu.js` | The AnyRoad DFU bootloader over WebUSB. `WebUsbTransport.open` refuses the radio switched on normally (`normalMode`: the board's `dfu_layout`, else a layout of only the bootloader's sectors) before it claims the interface | `rfoundry/dfu.py` |
| `codecsave.js` | The codec read back from a radio that runs RadioFoundry (where stock keeps it, or where the merge placed it), checked against the profile's hashes, and a copy kept in IndexedDB: before every update that erases it, elsewhere once, so an install that can't keep or check the codec (a radio back on stock firmware, in its bootloader, or after an interrupted install) needs no stock file | `rfoundry/codecsave.py` |
| `bootkey.js` | The bootloader key from a radio that runs RadioFoundry: the bootloader read over READ_MEM, the block with the key's SHA-256 | `rfoundry/bootkey.py` |
| `origbackup.js` | The backup before the install through the original firmware's or the bootloader's DFU device, behind an allowlist (below) | |
| `origversion.js` | The original firmware's version read before an install, and the hardware option it settles (below) | `rfoundry/origversion.py` |
| `link.js` | The debug link and open-firmware CPS reads and writes over Web Serial | `rfoundry/debuglink.py`, [open-firmware-cps.md](../spec/open-firmware-cps.md) |
| `backends.js` | The radio access backend registry: which steps and views a radio's family uses (below) | |
| `readers.js` | Bulk memory readers for backups: the debug link and the STM32F4 loader (below) | |
| `backup.js`, `zip.js` | Backup bundles | |
| `restore.js` | What a bundle can put back, and the plans for it | |
| `select.js` | À la carte feature selection with the dependencies, matching prebuilt variants | |
| `update.js` | The Update task's build: which build a radio runs (below) and the build its update installs | |
| `codeplug.js` | Codeplug formats and the converter registry | |
| `migrate.js` | The original firmware's channels after an install: a stock codeplug found, converted, written and read back (below) | `tools/codeplug_port.py` |
| `fake.js` | A simulated radio: bootloader with a flash model, RadioFoundry's serial port, a synthetic stock file | `rfoundry/fakedfu.py` |
| `stub.js`, `lz4.js` | The HR_C7000 boot stub: loaded through the boot ROM or handed over by RadioFoundry, then reads, sector writes and the LED | `stub/stubtool.py` |
| `f4stub.js` | The Cortex-M loaders (the STM32F4, GD32F30x and AT32F43x stubs, one target table): handed over by RadioFoundry, the board record (CONFIG) and its echo, then the install in the interlock's order and SPI flash writes. The target comes from the board's `f4stub.boards.<id>.target`; `data/f4-stub.json` carries each target's image under `targets`. Only the `anyroad-dfu` backend offers a loader route; none offers the GD32F30x or AT32F43x one yet | `stub/f4stub.py` |
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
ROM refuses the load, to practice the factory update mode; `#sim=<radio>-slowcable` one on a cable too slow for the
boot ROM's 48 ms window, to check that the page's answer sent ahead gets through; a radio with the Baofeng updater enters it when switched on
with "Hold the Keys and Turn On".

## The beta notice

The page opens a dialog (`templates/partials/beta_notice.html`, opened by `beta.js`) before anything else: what can
go wrong, that the radio can always be recovered, where to get help, and common questions folded into accordions
(testing, damage, channels and settings, fixes, feedback). It shows once per browser session and again once 12 hours
have passed since it was closed, so a tab left open still sees it (`sessionStorage` key `rfoundry.betaNoticeSeen`
holds the time of the last close, set on any close); where storage is blocked it shows on every visit. The installation count
in `flash.beta.lead` is written by hand: total successful installations from the backend's `flashes` table plus about
50 development installs, rounded down. Raise it now and then.

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
| 5 | A C-SKY radio's whole program flash dump whose bootloader ID string names the chosen radio and whose application area holds code (`c7000.js` `isProgmemDump`), with no codec step 3 knows | kind `progmem`: a restore source, never a codec source. The restore puts it back as a backup's program memory is (application and codec slot, never the bootloader) |

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
| `dump` | A program memory dump of this radio (kind `progmem`) given to the codec step: a firmware version nobody has mapped yet. It restores, but its codec can't be used in an install | send it from the notice's button so it can be mapped (`progmem-dump`), or select a different firmware file: the notice links the board's `stock_download` when it has one and folds the supported files into an accordion |
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
picked holds a vendor firmware container: a file with the `OutSecurityBin` or the C-SKY stock header, 4 MB at most.
For `dump` it offers **Send This Firmware to the RadioFoundry Team to Be Mapped** with the program memory dump itself
(reason `progmem-dump`): the backend takes a dump by the bootloader ID in its first 48 KB. It
comes both when the codec can't be taken from the file and when a later step fails on a file identified for this radio
(placing the codec, restoring). Readouts, other files and a file identified as another radio's are never offered. The
text asks for the manufacturer's original file only. The file goes to the backend only on the user's click, with:

- its name;
- the radio being flashed;
- what the page found (`codec-unmapped`, `key`, `key-unknown`, `unknown-container`, or the problem);
- the site version;
- an optional note (it asks where the file came from). For `internal`, the page's message is added to it.

The backend (`POST /v1/samples`, raw body, the rest as query parameters) checks the header (or a dump's bootloader ID)
and the size itself. It rate-limits per address and per day, caps the total storage, keeps one copy per SHA-256 and never serves a file back.
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

The start page's privacy text says that a report names the stock firmware file, never its contents, and that the
flash log includes the browser's language setting.

### The flash log and the vendor file

A use reports its milestones to the flash log, each once: the build request (custom builds), the start, the first
data on the radio (`first_byte`), the last (`last_byte`), the end of the write (`result`) and the check on the
"Done" step (`verify`, with its outcome). `flashMark()` sends the three data events to
`POST /v1/flashes/{id}/events` from each install route's progress callback and from `doneView()`. A row with
`first_byte` and no `last_byte` is a write that stopped midway. A row with no `started` is a build nobody wrote.

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

The POST and the build request (`POST /v1/builds`, when it carries the use's id) also carry `languages`; the PATCH
and the events don't. Both requests carry `adapter`, and the PATCH carries `trail`:

| Field | Content |
|---|---|
| `languages` | The browser's language preferences (`navigator.languages`, the first 5; else `navigator.language`), such as `["en-US", "en"]`. The backend keeps at most 10 tags of 35 characters and an older one ignores the field |
| `adapter` | `{vid, pid, chip, product}` of the USB device in use (`adapter.js`). `chip` from the vendor id: `FTDI` (0403), `CH340` (1a86), `CP210x` (10c4), `PL2303` (067b), `radio USB` (the radio's own port), `DFU bootloader`, else `other`. `product` only from WebUSB. Never a serial number |
| `trail` | The newest 200 breadcrumbs (`diag.trail()`, scrubbed, 300 characters each), the oldest dropped to keep the keepalive request under 64 KB |

### Breadcrumbs

`diag.note()` lines record what the page decided, built in `trail.js` (names, sizes, hashes and reasons; never bytes):

| Line | When |
|---|---|
| `file picked` | A vendor file: name, size, SHA-256 prefix, `logId()`, what it matched, its problem and why (`whyNot()`: also for a pick nothing in which serves, and for the restore's file) |
| `restore bundle` | The restore task's input: whether it holds program memory, whether it is a vendor file, the part names |
| `write range` | A cable radio's install or restore write: how many sectors, the first and last address written, and whether the bootloader area (read before and after the write) is `unchanged`, `changed` (the write then fails) or `not checked` (also when the write failed part way) |
| `codec search at known addresses`, `moved codec search` | The codec search in an open-firmware read: each stage, the profile and address found, or how many code starts were checked and why the nearest failed |
| `codec decision` | `decideCodec()`: mode, source, why; or why none |
| `key` | Where the bootloader key came from |
| `write plan` | Mode, sectors erased, written (with content), kept, blocks, the codec profile |
| `open firmware backup` | The key and codec found in the open-firmware read |
| `verify`, `verify failed` | What the radio reported after the install |
| `cable radio check`, `cable radio codec decision` | The cable radio's read choices and codec decision |
| `no codec confirm` | The answer to the dialog Install opens before an install without the codec: `go`, `back` or `stay` |
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

Every failure says what to do next: the message itself, or `errorNotice(text, { next })` beside it. Where the step can
run again in place, `{ retry }` puts Try Again in the notice. Pressing it removes the notice first, so a second
failure replaces the first instead of stacking under it. A step that shows its result in a status area clears that
area at the start of each run.

## The Update task

**Update** reaches a radio that runs RadioFoundry with one port chooser (its USB port and the programming cables),
probes it, and lets the radio's backend take the connection: the STM32 radios keep the debug link (`S.link`), the
HR_C7000 radios hand the open cable to their connect step (`c7000ui.js` `adoptCable`), which enters the stub without
asking for the port again. The steps are `update`, then the backend's: `write`, `done` on the STM32 radios (the loader
route when it can, the bootloader otherwise), `connect`, `check`, `write`, `done` on the HR_C7000 radios, whose check
reads only the codec slot unless the user asked for a backup. **Edit Build** (or a build the radio can't name) puts
`build` after `update`. On the HR_C7000 radios an update that keeps the codec runs from the check to the write without
another click, once per cable session; a failed write, and the step reached by **Back**, wait for the button.

Which build runs (`update.js` `currentBuild`), from VERSION ([debuglink.md](../spec/debuglink.md) 3.1):

| Radio says | Build | The update |
|---|---|---|
| `preset=` a variant, `off=` | that variant | the same variant; features it gained since are listed |
| `preset=custom`, `off=` equal to a variant's | that variant | as above |
| `preset=custom`, `off=` | custom | every switch as it was; each feature added since is asked, On or Off, before Update |
| neither (older firmware) | its `features=` against the variants of its release, by the names `features=` can carry (`features.json` `reported`) | one exact match: that variant; else the build step, from the closest |

The release a radio's build came from is the one with its commit, else the newest one older than its `git_time`. What
that release knew is its variants' features and its manifests' `off` lists (`tools/release.py`): a switch in neither
is new. A release from before manifests carried `off` names only its variants' features, so a switch a custom build had
on in no variant reads as new, once.

The Codeplug Editor's **Update Firmware** opens `flash/index.html#update` and closes its own connection.

## The original firmware version

A board with `[flash.original_version]` ([board-registry.md](board-registry.md#original-firmware-version)) has a
hardware option RadioFoundry can't detect but its original firmware's version settles. The install learns it before
the write and stores it after the first start, so nobody picks it. The web flasher (`origversion.js`) and rfoundry
(`origversion.py`) do the same steps:

| Step | Web flasher | rfoundry `flash` |
|---|---|---|
| 1. Read | Current firmware step, under **Original firmware** and **Other / Not sure**: **Read Firmware Version** opens the WebUSB chooser on the bootloader's USB IDs; a device showing `interface` gets the read, any other (the bootloader) gets nothing. Optional | Before it asks for the bootloader, when no RadioFoundry answered the probe: reads a device with the bootloader's IDs that is connected and shows `interface`, or asks to connect the radio switched on normally (`--yes`: only one already there). `--no-version-read` skips it |
| 2. Map | `major` gives the choice. The bootloader (by its strings, or its 0x54 answer) is told as "in update mode already"; a version not listed or no answer (a stall) is unknown. None of them settles anything, and none is an alarm | The same, as a `version` line |
| 3. Bootloader | **Connect** refuses a device showing `interface` (and the very WebUSB device that answered): the original firmware has the bootloader's USB IDs | `check_bootloader` refuses a device showing `interface`; `wait_for_dfu` passes over the device that answered (its bus and address) and waits for the bootloader |
| 4. Store | After the install, once the check reads the radio: HWOPT LIST, then SET when no choice is stored. A stored choice that differs is shown and kept: a person set it | The same, after `verify_after`'s probe. `--no-verify` (or no debug link) prints how to set it on the radio instead |

Only the requests of the read are sent: GETSTATUS, CLRSTATUS or ABORT to reach dfuIDLE, DNLOAD block 0 `A2 02`, two
GETSTATUS, ABORT, UPLOAD block 0 of 1024 bytes. The fakes (`fake.js` with `originalVersion`, rfoundry's
`fakedfu.FakeOriginalFirmware`) fail on any other request; `site/tests/origversion.test.mjs` and
`tools/rfoundry/tests/test_origversion.py` check the sequence, the parse, the table and the store.

## Backup before the install

A board with `[flash.original_backup]` ([board-registry.md](board-registry.md#backup-before-the-install)) is backed up
before anything is written, through the DFU device its original firmware or its bootloader shows (`origbackup.js`).
The Current firmware step offers it under **Original firmware** and **Other / Not sure**, above the version read:
recommended, never required.

1. **Back Up Now** opens the WebUSB chooser on the bootloader's USB IDs. The interface strings say which device it is
   (`deviceKind`): the original firmware (`original_version.interface`), the bootloader (`loader_interface`), or
   neither (refused, nothing sent).
2. The info blocks (`info`), each a block 0 command `A2 ss` and an UPLOAD of block 0.
3. Before program mode, one snapshot of the internal flash from its start: 65535 bytes, then 32768, 16384, 8192 and
   4096 until the host passes one (Linux stops at 4096). It holds the radio's bootloader.
4. **Everything** (`spi`, about 6 minutes) or **Channels and settings only** (`quick`): program mode (`91 01`), the
   address set to 0, then 1 KB blocks with a progress bar. **Stop** ends the read early; the part read is kept. The
   bootloader gives `loader_spi` at most, and the page says a full backup needs the radio switched on normally.
5. The original firmware is left with `91 05`: it restarts and runs normally again. The bootloader stays in program
   mode: the page remembers that USB device, and a write to it starts at `91 31` (`Dfu.run` `programMode`), since the
   bootloader takes `91 01` once. A power cycle makes a new device and the whole sequence.
6. The bundle is a backup bundle (`rfoundry-backup/1`, below): `info/info-<ss>.bin` (`info-<ss>`),
   `bootloader-<bytes>.bin` (`internal-flash-mirror`, at 0x08000000), `spi-flash.bin` or `spi-flash-0-<n>MB.bin`
   (`spi-flash`, complete only when it is all of `spi`). `backup.json` carries `original_backup` (the device, the
   version, the bootloader's version, the choice, whether the radio restarted, the browser) and `firmware_now` /
   `firmware_before` (`stock` with the version when the original firmware answered). Named
   `<model>_Original_<last 8 UID digits>_<date>.zip`. The Codeplug Editor's import and the channel conversion read
   `spi-flash` from it as from any bundle; Restore offers only putting the original firmware back from a file.

`Guard` is an allowlist in front of the transport: GETSTATUS, CLRSTATUS, ABORT, UPLOAD (1024 bytes, or the one
snapshot of block 2 before program mode), and DNLOAD block 0 with exactly `A2 ss`, `21` + address, `91 01`, or `91 05`
(original firmware only). Anything else throws before it is sent. The fake radio (`fake.js`) models both devices: the
mode byte and its gates, the ignored block number of the snapshot, the bootloader's 1 MB, the restart on `91 05`, a
host's transfer cap (`usbCap`), and a list of every request outside the allowlist that reached it (`forbidden`).
`site/tests/origbackup.test.mjs` checks the allowlist, the order, both devices, the bundle, and a write after a
bootloader backup.

The bundle holds the radio's calibration and its bootloader. It is saved as a download only: it is not kept for the
report attachments (`S.lastBackup`), and the flash log and breadcrumbs get only its size and part count.

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
| `buildGate(redraw)` | Optional: the hardware variant asked in the build step: `{view(), need(), refused()}`. Build Firmware stays off until a variant is picked or detected, and for a variant with no firmware, which shows a refusal |

| Backend | Radios | Module |
|---|---|---|
| `anyroad-dfu` | `usb-dfu` family (STM32F4 radios) | `app.js` (`dfu.js`, `f4stub.js`) |
| `c7000-stub` | `serial-loader` and `serial-iap` families (HR_C7000 radios) | `c7000ui.js` (`stub.js`, `c7000.js`) |
| `anytone-update` | `anytone-update` family (the vendor update mode on the radio's USB serial port) | `anytoneui.js` (`anytone.js`; `fakeanytone.js` for tests) |
| `none` | any other | none: the radio is listed but grayed out |

A radio without a backend the page knows gets `none`, so a family can come before its module. The
contract tests are in `site/tests/backends.test.mjs`.

Bulk reads for a backup go through readers (`readers.js`). `backup.js` `readRadio` reads the small parts only the
debug link has (radio info, security registers) first. It then tries the readers it is given, in order, and the debug
link reads the memories when none opens. A reader may also have `security()`, the security registers or null: the
STM32F4 loader's SREAD (stub-stm32f4.md 6.2), each register checked against the CRC the loader sends. A reader has `open(progress)` (false: not possible now, nothing changed),
`used`, `read(mem, addr, n)` giving `{data, complete}`, `close()` and `abort()`. A new read-back protocol is a reader,
and the bundle format and the backup step stay as they are.

## Variant detection

A radio with `[hardware] variants` (several builds sold under one model, each needing its own image or build options)
is asked which one it is before anything is written. `detect.js` holds the engine and the probes every radio shares.
`detectui.js` is the Auto-detect button a backend puts beside its variant chooser: the user connects the radio in
whatever state it is in, and the probes run in turn on that one port until one names the variant.
The port chooser lists the USB devices the variants name (`usb_ids`) and RadioFoundry's (`portFilters()`). A backend that
can't name every state's device passes `showAllPorts`, which adds a link to a chooser with every port.

A probe is `{id, baud, run(stream, ctx), confirm(session, ctx)}`, registered with `registerProbe()`. `run` returns null
when the radio didn't answer that way, else `{code, version, via, keep}`. `keep` means the radio stays in a session the
backend takes over, such as an open update mode, so the user needn't connect again. A backend lists its probes in the
order that is safe for its radios: probes that send nothing first, then words or frames the other firmwares ignore.

| Probe | Module | Reads | Sends |
|---|---|---|---|
| `usb-id` | `detect.js` | The port's USB ids against the variants' `usb_ids` (board.toml) | nothing |
| `rfoundry` | `detect.js` | The board RadioFoundry runs (debug link `VERSION`), when one variant takes that image | debug link frames |
| `anytone-update` | `anytonedetect.js` | The update mode's model answer (`anytone.js` `variantOf`) | `UPDATE`, `02` |

A probe never puts the radio in a state it can't leave without a restart; one that can't tell the variants apart
without that is left out. For example, the AnyTone PC mode answers the same model name on both MCUs, and leaving it
restarts the radio.

**The check before a write is mandatory.** Just before the first byte, the backend calls `confirmVariant()` on the
session it writes through. The probe that opened the session asks again with a read-only question (`confirm`). The
write stops with nothing written when the answer is another variant than the one judged at connect, or a variant the
image isn't for. A probe without `confirm` refuses the write. Tests: `site/tests/detect.test.mjs`, and the backend's
own (`site/tests/anytoneui.test.mjs`).

To support another radio's variants:
1. List them in its board.toml, with `usb_ids` where the original firmware's USB device differs per variant.
2. Write a probe for any other protocol that names the variant.
3. Add the Auto-detect control and the `confirmVariant()` call to the radio's backend.

A variant with `blocked` in board.toml (the reason, as text) can be identified and given its original firmware back, but
no installer writes RadioFoundry on it: the page's check at connect, the check before the write and the `rfoundry` tools
all refuse it. It is for an image that exists but has never run on that hardware.

## Backup bundles (`rfoundry-backup/1`)

A zip the user keeps: `backup.json` (the radio, its chip UID, the firmware when the backup was taken and before RadioFoundry,
each part's address, size, SHA-256 and whether the read completed), `internal-flash.bin` (from the application
base; the bootloader is never read or written), `spi-flash.bin`, `security-registers.bin` (the factory calibration),
`radio-info.bin`, and `stock-firmware.bin` when the user gave one.

`spi-flash.bin` is the radio's own memory chip: `backup.spi_size`, or less when the radio reports a smaller chip
(`spiBytes`: the radio info's flash part id, or the loader's SPI flash size from HELLO). Some radios of a model came
with a smaller chip, and a read past its end is refused or wraps around. A backup over another open firmware sizes its
read the same way (`newState`). Restoring the codeplug and opening the backup in the Codeplug Editor need only the
codeplug's regions (`TFTP_SIZE`), not the whole open-firmware span.

`openBundle()` also reads a backup whose files sit in one folder (`bundleFolder()`): what macOS's Compress makes of a
backup it unpacked, `__MACOSX/` ignored. The restore picker and the codeplug editor use the same test.

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
| RadioFoundry with stub entry (STM32F4) | The same. The radio info comes over the debug link; the internal flash, the SPI flash and the security registers come through the STM32F4 loader, once it has taken the radio's board record, every 4 KB block checked against the radio's CRC, and the radio then restarts into RadioFoundry. The codeplug choice stays on the debug link (the hand-over would cost more than it saves). A loader that doesn't start leaves RadioFoundry running, and the debug link reads everything. After a first install the hand-over is the one the build just written offers: Verify asks it (`afterOffer`), since the probe before the install found the original firmware. A build whose codeplug protocol has no area 10 still gets the security registers, through the loader's SREAD (protocol 5). When both read them and the copies differ, the loader's checked copy is `security-registers.bin` and the other is kept as `security-registers-codeplug-protocol.bin`, marked incomplete |
| Another open firmware (OpenGD77 family) | Everything but the security registers, over that firmware's CPS (`openfw.js`, below) |
| Stock (STM32F4) | Nothing: the bootloader can't read flash (read protection level 1) and the stock firmware's own USB protocol has no spec here. The install therefore backs up right after writing RadioFoundry: flashing never touches the SPI flash, and RadioFoundry doesn't write to a foreign settings region until it is claimed, so the stock codeplug, calibration and resources are still intact |

A CPS read the radio doesn't answer is tried up to five times, 0.3, 1, 2 and 3 s apart (`link.js` `cpsRead`): a radio
quiet for a few seconds must not end a backup halfway. A read answered after a retry goes on the trail. Reads a build
may not answer at all (the radio info and security registers in `backup.js`, the `openfw.js` probe) keep 3 tries, and a
probe asks once.

## Channels after an install (`migrate.js`)

A radio installed from its stock firmware still holds the stock codeplug, in a layout RadioFoundry does not read. The
last step of the install brings it over by default, on every radio whose stock codeplug the browser converts. The
backup panel before it says so (`flash.backup.after_channels`). With a backup that holds the stock codeplug the step
writes without asking; without one it asks first:

| Step | What happens |
|---|---|
| 0. Skip | An update, a move from another open firmware, a board without a memory chip (`backup.spi_size` 0), or a backup from this visit that read every stock family's tables and found none (`channelsWanted`): nothing is read. A radio without a settings region (STORAGE INFO unavailable: a chip too small for the board's flash map) stores no codeplug: the step says so and ends |
| 1. Read | The codeplug span over the CPS, no further than the radio's chip. The stock codeplug comes from this visit's backup when it holds one (`stockSource`), else from the live read, further on a family whose stock tables reach past the span (`readSpan`) |
| 2. Recognize | `classify` ([codeplug-stock-tyt.md](../spec/codeplug-stock-tyt.md) 2). A RadioFoundry codeplug, a blank chip or an unknown layout ends the step: nothing is written |
| 3. Convert | `importStock`, the Codeplug Editor's conversion, with its counts and notes. No channels ends the step |
| 4. Prepare | The settings area is claimed (`prepareStorage`), as before a codeplug restore |
| 5. Write | The radio's bytes with the converted image over the codeplug's regions (`writeRanges`, as a Codeplug Editor write), in the 4 KB sectors that differ (`codeplugSectors`). The radio's own settings block, the VFO pair, the ID database area, RadioFoundry's own areas and protected data keep the radio's bytes |
| 6. Check | The span read back: every written sector holds its bytes (the VFO pair aside: a read shows the live VFOs), and the decoded codeplug has the converted counts. A mismatch is reported as a problem and the step stops |

The backup's copy comes first because the backup ends by claiming the settings area. On a family whose stock tables
lie inside that area (zones on the 256 KB layout), the live read no longer holds them.

It runs at once when the backup taken in this visit holds the stock codeplug (the SPI flash read covers `readSpan`).
Without one it waits for the user and says that the original bytes may be lost: on a board whose stock codeplug shares
sectors with RadioFoundry's codeplug, the write replaces them. A failure at any step leaves RadioFoundry running without
channels and says so. With the backup, the Codeplug Editor converts the original codeplug from there; without one, the
message says whether the original is still on the radio (a read or conversion failure writes nothing).

The boards whose stock codeplug the browser converts are those in the TYT family layouts of `codeplug/stock_tyt.js`,
and those whose paged stock codeplug `codeplug/stock_baofeng.js` reads ([feature-matrix.md](../feature-matrix.md)).

### HR_C7000 radios (`c7000ui.js` `bringChannels`)

The stock codeplug sits in its own pages of the data flash, below RadioFoundry's codeplug, and no step of the install
writes there. So the step needs no backup and loses nothing:

| Step | What happens |
|---|---|
| 0. Skip | Not an install, a radio that ran RadioFoundry before, or a board without a paged stock area (`calibration.area`) or a codeplug base |
| 1. Read | The stock area as this visit's check step read it (`c.calArea`). Without one (the factory update mode route reads no data memory), RadioFoundry reads it over the CPS, area 1 at its physical addresses |
| 2. Convert | `convertPagedStock`. No stock codeplug, or one without channels, ends the step |
| 3. Write | `writeConverted` through the Codeplug Editor's transport (`CpsSerial`), which adds the board's codeplug base: the settings area claimed when foreign, the radio's codeplug read, the bytes that differ written and read back. A codeplug with channels already on the radio is replaced only when the user says so |
| 4. Check | The radio's codeplug read again and counted. A mismatch is reported as a problem |
The HR_C7000 backend has no conversion: its stock codeplug stays in the data flash and in the backup
(`original-channels.bin`), and `tools/backup_codeplug.py` ports it.

The AnyTone update mode backend (`anytoneui.js`) reads before the install instead: RadioFoundry keeps its codeplug in
its own NAND blocks and can't read the stock tables once it runs. Its optional `at_channels` step, before Connect,
asks for the radio on in normal mode, reads what the zones reach through the radio's PC mode
(`AnytoneProgram`, `readAnytone`; [codeplug-stock-anytone.md](../spec/codeplug-stock-anytone.md)), converts it
(`convertAnytone`) and ends the PC mode (the radio restarts). The done page then offers **Write Channels**, which runs
steps 1 and 4-6 above with that converted image (`bringChannels` with `converted`): the original stays in the radio's
NAND, so it runs without asking.

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

### Protected regions

No write touches a board's protected regions ([board-registry.md](board-registry.md#protected-regions):
`[[flash.protect]]`, the factory calibration first). `protect.js` is the one check, with the data from
`flash-boards.json`.

| Writer | Over a protected region |
|---|---|
| `Stub` (`stub.js`) | Every erase and program command is checked before it is sent; a chip erase is refused. `writeProgram` refuses an install that overlaps one. The restore task passes `carve`: those sectors keep what the radio has and show as never written |
| `F4Stub` (`f4stub.js`) | `erase`, `program` and `writeImage` refuse. `spiWrite` carves |
| `makePlan` (`plan.js`), `Dfu.run` | Keep and merge plans refuse; a restore plan keeps the sector (`keep`, "protected"). `Dfu.run` checks the plan again before the first request |
| `Iap`, `BfLoader` | Refused when the area the loader erases holds one: the vendor loaders erase a fixed span, which can't be carved |
| Codeplug restore (`codeplugSectors`) | Protected data sectors are left out |

The error is a `ProtectedError` (`code` "protected") naming the region and the reason from `board.toml`. Nothing is
erased before the check.

## HR_C7000 radios (the boot stub)

Radios whose backend is `c7000-stub` (the `serial-loader` and `serial-iap` families; a board with a first stage adds the section
below) are flashed over their
programming cable with Web Serial, through the boot stub ([stub/README.md](../../stub/README.md),
[debuglink.md](../spec/debuglink.md) 3.18), or through the radio's factory update mode when the stub can't be loaded
(below). No loader in the radio is ever written. The install has its own steps:

| Step | What happens |
|---|---|
| Connect | The page looks for a stub that still runs, then for RadioFoundry. RadioFoundry with `CONFIG_STUB_ENTRY` hands over to the stub (command 0x1A); RadioFoundry without it is restarted with its CPS reboot, which passes the boot ROM's serial window (not yet tried on a radio). Otherwise the page asks the user to switch the radio off and on and catches the boot ROM. The radio model is then checked by the bootloader's ID string, and the page sends the stub the board's profile (CONFIG: clocks, UART, pins, board id) and checks the echo; the stub refuses writes until then |
| Back up and check | A first install reads the whole program memory (required) and the calibration area; the whole data memory only if asked (off by default: many minutes at 115200 baud). Each choice shows its time: the stub's sector sums count the blank sectors first (no data moved, `c7000.js` `blankCount`, at most 8 s per memory, windows spread over the range), and `readSeconds` turns the used share into wire bytes, plus the radio's own time per sector where it is slower (a NAND), pessimistically: every used sector whole (no credit for LZ4), 13 bytes a record, at 9000 B/s or the rate measured on this radio, never above the 11,520 B/s of the line; without a count every sector is taken as used ("up to"). The page keeps the program memory in IndexedDB and downloads a backup bundle. A first install can skip the program memory read, before or during it, and take the codec from the stock file instead: only the codec slot is read, and the firmware is reported as not read. An update reads only the codec slot. The page then decides the codec |
| Write | Only sectors whose CRC differs are written. The first application sector goes last, each sector is read back right away (three tries), and a final pass checks every sector |
| Check | The stub resets the chip; the page asks RadioFoundry's VERSION for the build and the codec |

A cable whose USB vendor id is a CH340's (1a86) gets a warning on the connect, check, write, backup and restore steps:
some of these cables fail the backup or the install. The boot ROM load drops an exact echo of each frame it sent (some
cables loop TX back to RX). A frame not acknowledged logs the first 16 bytes received, and the error names the cable's
chip.

The boot ROM waits about 48 ms for the answer to its greeting (measured; the radio pages have the timing), and
the greeting in and the answer out both cross the cable and the browser's read loop. When the load fails, the ROM says
it stopped waiting (`00 10 00 03`, error code `rom-late`, or `rom-booted` when no greeting was seen). Answering only
after the greeting arrived failed often on the first try, so every wait for the ROM uses the **slow cable mode**
(`powerCycle()` in `c7000ui.js`; it was once a choice offered after a failure, and a collapsed note on these panels
says where it went): `waitForRom({ repeat: ROM_REPEAT })` sends the answer every 30 ms, and on for 150 ms after the
greeting, so one leaves within 30 ms of it whatever the cable's latency. The page then waits for the line to go quiet
(the ROM NAKs the extra answers) before block 0. The firmware of a radio left on gets the answers too: RadioFoundry
ignores them, stock firmware is untested. So on a radio not known to run RadioFoundry the sending starts only after the
user selects "Radio Is Off"; RadioFoundry's soft reset and a power cycle of a radio it found running RadioFoundry start
at once. When the load fails, the factory update mode is the main action. `stubtool.py --slow-cable` does the same.

The session holds a stub only once it answered HELLO. A cable whose reading stopped (the device lost) is dropped
before a step uses it, and after a failed backup, restore or check read the page asks the stub for HELLO: one that
no longer answers (the radio switched off or reset) is dropped too, and Try Again goes to the connect step, which says
why. Breaks and framing errors never stop the reading: a radio switched off with the cable in gives one after another
for as long as it is off.

A page of an SPI NAND whose bit errors its ECC can't correct reads the same every time: the stub answers status 0x0D
for its sector, in a read (op 09) or a checksum (op 07). The page then reads that sector's two pages itself with SPI
requests (page read, status, read from cache), twice, and keeps them as the chip holds them. The final checksum pass
sums a window that holds one sector by sector and leaves it out, and `backup.json` lists those sectors as `nand_ecc`.
`stub/stubtool.py` reads a NAND the same way (`Radio.nand_raw`) and prints the list after a `--nand` read.

### The factory update mode

Every HR_C7000 board has one: `update_mode` in its `data/radios.json` `flash` entry, from the family in
`tools/boards.py` (`serial-iap`: `iap`; `serial-loader`: `baofeng`), entered by the keys in `flash.bootloader` (the
steps) and `bootloader_hold` (marked on the radio's picture). The registry check refuses a board on such a family with
no steps.

| Protocol | Module | What it takes |
|---|---|---|
| `iap` | `iap.js` | Erase, then 2 KB pages with an additive checksum the loader reports back. The install image starts with the first stage. A radio without a complete application stays in this mode at every power-on, so the page probes for it when nothing else answers |
| `baofeng` | `bfloader.js` | The Baofeng updater (the protocol `tools/serial_loader.py` names, on the radio pages of the boards that use it): steps 1-3 identify the loader by the board's `bootloader_id`, step 7 erases the whole application area (the codec slot included), then 1 KB blocks, each acknowledged by its CRC-16/XMODEM. A port of `tools/serial_loader.py`'s `Flasher`; `site/tests/bfloader.test.mjs` checks the bytes it sends against the Python tool's (`fixtures.py`) and runs it against `FakeBfLoader` |

The page offers it when the boot ROM load fails (above), in Troubleshooting
(`recovery.extra`, "Reinstall in Factory Update Mode"), and where the stock console and the first stage routes offer it.
Nothing is read from the radio this way, so the check step takes the codec from a file (`factoryRoute()` in
`c7000ui.js`): the stock firmware file where a profile of the board names one, else a program memory backup (or a slot
this browser kept). The decision is never "keep": the image is `c7.target()` with the slot, without the 0xFF tail
(`factoryInstallImage()` in `iapui.js`). Restore writes a backup's application area and slot, or the original firmware
from its file, the same way. A step the user leaves (Back, Start over) while the page listens for update mode stops
listening, so a radio switched on with its keys later is not written (`site/tests/iapui.test.mjs`). The Baofeng updater
answers steps 1-3 once per power-on, so the session keeps the `BfLoader` that found it (`c.loader`, `ready`) for the
write and the next try; after a write started, a try waits for update mode again. After the end of transfer the radio
starts RadioFoundry by itself, so the done step's first check waits 20 s before it asks for a power cycle.

`bfloader.js` and `c7000.js` have installed a radio this way from Node, over an FTDI cable (the radio's hardware page
has the figures). The page around them is checked only in practice mode, and the route is untested on the other
radios on this loader.

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

An install without the codec asks once more when Install is pressed (`nocodec.js` `confirmNoCodec()`), on every route:
the cable write, the update mode write (decision `none`) and the bootloader write (a decision with `expectCodec`
false). The dialog says DMR voice calls won't work and offers **Go Back and Add the Codec** (the step before, where a
file is selected) and **Install Anyway**. Escape stays on the step. A bootloader route without a codec step before
the write (an update) offers only **Install Anyway**. A retry after a failed write doesn't ask again.

The page never sends a STUB command to RadioFoundry except right before it enters the stub: any STUB command parks RadioFoundry's codec
until the next restart.

The build writes the layout and the codec profiles to `data/flash-boards.json` (`c7000`, from `stub/stubtool.py`,
`tools/hrc7000_codec.py` and `tools/serial_loader.py`) and the universal stub image (with its load address, run address and RAM end) to `data/c7000-stub.json`. Bundle parts:

Each board's `[flash.stub]` in `board.toml` says what there is to back up: the program memory's size (`progmem`), whether
the data memory is SPI NOR flash or an SPI NAND (from the board's NAND driver: the stub's `nand` flag), and where the
factory calibration is (`calibration`, required). The page lists the same three choices for every radio from that data:
the program memory, the calibration and the whole data memory, each with its size and time.

| `calibration` | Where | What the calibration choice reads |
|---|---|---|
| `{area, page_id}` | A data flash: the 4 KB page of the stock codeplug area [0, `area`) whose last byte is `page_id` | The whole area (about 10 seconds), kept as `original-channels.bin` |
| `{progmem, nand, magic}` | A NAND radio: a copy in a 4 KB sector of the program memory and one in the NAND, each starting with `magic` | Both sectors. The NAND copy is looked for at `nand`, then a block later each time (`findNandCalibration`): the stock firmware skips its bad blocks, so a bad block at or below the copy moves it up. The bundle keeps the copies starting with `magic` and names a blank one (`calibrationCopies`). The program memory choice is then "Program Memory & Calibration" |

The NAND is read through the stub's NAND path (`stub.js` `MEM.nand`: flags bit 2 on op 07 and 09, the stub configured
for a NAND board): its 2 KB pages without the spare bytes, two to a 4 KB sector, never written. The whole chip (256 MB)
reads in minutes because the stub sends a blank sector as its record alone. `blankCount` samples windows spread over the
whole range, so the estimate holds for a chip whose data sits in a few areas.

The count gives how much of a memory is used, not how well it packs. `readSeconds` takes a used sector whole for its
time and packed to `BEST_PACK` (0.3) for `least`. Where the two differ by a minute or more, the choice shows a range:
a full data memory packs well with a codeplug and not with an encrypted image.

| File | Part | Content |
|---|---|---|
| `program-memory.bin` | `c7000-progmem` | The whole program memory |
| `data-memory.bin` | `c7000-data` | The whole data flash |
| `nand.bin` | `c7000-nand` | The whole NAND, 2 KB pages without the spare bytes |
| `original-channels.bin` | `c7000-data-start` | The data flash's stock codeplug area, when the whole data memory isn't read |
| `calibration.bin` | `c7000-calibration` | A data flash's calibration page, at its own address |
| `calibration.bin` | `c7000-calibration-nand` | A NAND radio's NAND copy (4 KB sector), at its NAND address |
| `calibration-program-memory.bin` | `c7000-calibration-progmem` | A NAND radio's program memory copy (4 KB sector), at its address |

The calibration choice is on by default. Choosing the whole data memory includes it. Restore writes the program memory
back from the application start up; the data memory is not written.

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

The firmware's language packs are feature switches named `LANG_<CODE>` (`CONFIG_LANG_ES`, the code upper-cased). When
the browser's languages (`navigator.languages`, primary subtag) include one the radio can have a pack for and the
selection leaves it out, the build step offers it under the build cards: one button turns the switch on, another
dismisses the offer until the page reloads. Only the first language the browser asks for that has a pack counts, and
English (the firmware's own) never does. The code comes from the switch name alone (`flasher/langsuggest.js`), so a new
pack needs no flasher change. Strings: `flash.build.pack_*`.

**Radio language** (`LANG_DEFAULT_<CODE>`) lists English and every pack the radio can have; one the build leaves out
reads "(adds it to the build)". Picking it turns its pack on, and the pick holds across the build cards until the pack
is turned off by hand (the radio then starts in English). Picking another language takes out a pack the pick added. On
an installation it is preset to the page's language only when the build has that pack.

A build too big for the radio fails at the build service with `too big: <region> overflowed by <N> bytes` (the
linker's region, flash first). The build step shows it as a notice, not an error to report: how many KB too big for
the program memory (or the RAM), to turn features off in Custom or pick a smaller build, and, when the Radio language
added a pack, that English leaves it out again (`flasher/buildfit.js`; strings `flash.build.too_big*`).

## Version links

To have someone install a version that isn't a release (a commit to try out), send them the flasher with
`?version=`:

```
https://<site>/flash/?version=<full 40-character commit hash>
https://<site>/flash/?version=<full commit hash>#radio=<id>
https://<site>/flash/?version=v26.11
https://<site>/flash/?version=feature/my_radio
```

| Behavior | Detail |
|---|---|
| What it builds | The build request sends that ref in place of the newest release tag. The build service takes a release tag, a branch of the main repository (`main` included) or a full commit hash, and the commit has to be pushed. A branch name passes the service's rule: up to four `/`-separated parts of letters, digits, `_`, `.` and `-`, no `..`, no part ending in `.lock` (`version.js`). A short hash shows a "can't find" banner and builds nothing. |
| How long it lasts | The tab keeps it in `sessionStorage` for the session: reloads and visits to other pages keep it, a new browser session doesn't. `?version=` (empty) drops it. |
| On every step | A banner names the version and has "Install the latest release instead", which drops it. |
| Before the build | The build step's button says "Get version <hash>", and a dialog asks for confirmation. The dialog can also switch to the latest release and continue with it. |
| `#radio=` | Picks the radio, as on any flasher link. |
| A beta | When the site offers a newer pre-release (`site.json` `beta`), the start page and the first install step link to `?version=<beta tag>` ([release.md](release.md#release-names)). The beta is never installed without that link. |

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

## The manual install kit

Where the page can't install, `manual.js` builds a zip for installing from the command line with rfoundry (user view:
[installing](../user/02-installing.md#install-from-the-command-line-with-a-kit)). No release carries an rfoundry
executable yet, and the tool must come from the site's own commit: `manifest.verify` refuses a codec profile or board
it doesn't know. So the site build publishes `downloads/rfoundry-tool.zip` (`site/rfoundrysite/toolzip.py`): the
`rfoundry` package, `tools/serial_loader.py`, `tools/hrc7000_codec.py`, `stub/stubtool.py` and `stub/f4stub.py` in the
repository's layout (`serialflash.py` loads the tools by path from the package's grandparent), a root `rfoundry.py`
launcher, `requirements.txt` and the license. The zip is reproducible: sorted entries, the commit's time.
`data/site.json` `tool` is `{zip, sha256, size}`; the page refuses a download that doesn't match it.

| Function | Does |
|---|---|
| `kitRoute(radio)` | `dfu` (`usb-dfu` family), `serial` (`serial-loader`), or null: rfoundry refuses `serial-iap`, so that radio gets no kit |
| `missingApi(radio, task, step, apis)` | The API an install step needs and the browser lacks: the `anyroad-dfu` write (USB, or serial on the loader route), the `c7000-stub` connect (serial) |
| `kitReadme(o)` | `README.txt`, a pure function: per-OS setup, the radio's `[flash.bootloader] steps` (a docs link when there are none), the `--dry-run` and install commands with the kit's file names, where to get help. English, like rfoundry's output. A `serial` radio whose codec has no stock file (only a program flash read) gets the `stubtool.py --progmem` command first |
| `makeKit(o)` | Fetches and checks the tool zip, adds `README.txt` (CRLF), `firmware/<base>.bin` and `.json` (`<base>`: the manifest's `image.file`, else `rfoundry_<board>`), `stock/<file>` |
| `kitPanel`, `kitLine` | The panel `render()` draws in place of a step `missingApi` names, with the stock file picker when the kit has none; the line under a failure |

The stock file is `S.stock` (the inner file of a vendor zip), else the file the C7000 codec step took
(`S.vendorPick`); a file with a problem is left out. The line sits under a failed bootloader connect (not a cancelled
chooser) or write, and under the C7000 connect's failed boot ROM wait or load (`ctx.manualKitLine` in `c7000ui.js`). Practice
mode never offers a kit: its image is random bytes. Tests: `site/tests/manual.test.mjs`. Untested with a radio.

The README's setup (`kitSetup()`) names Python `PYTHON_MIN` and each package of `PACKAGES`; the test checks both
against `tools/rfoundry/pyproject.toml` and the zip's `requirements.txt`. The packages are not bundled: pip picks the
`libusb-package` build for the user's system, which a kit made in the browser can't know.

## Browser support

WebUSB and Web Serial: Chrome, Edge and other Chromium browsers on a computer, over https or localhost. Windows needs
the WinUSB driver for the bootloader (Zadig, once), as rfoundry does. On Windows the bootloader step shows the Zadig
steps ([installing](../user/02-installing.md#windows-the-bootloader-driver)), open until this computer has reached a
bootloader once, and again with any failed connect: without the driver Chrome either doesn't list the bootloader or
can't open it. The device name comes from `[flash.dfu] product` in the board's `board.toml`.

Other browsers (Safari, Firefox) can still choose and build the firmware; the step that needs the radio then offers
the [manual install kit](#the-manual-install-kit).
