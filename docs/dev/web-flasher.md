<!-- SPDX-License-Identifier: MIT -->

# The Web Flasher

The browser tool that backs up, installs, updates and restores a radio, from `site/static/js/flasher/`. For users it is the
recommended way to install RadioFoundry ([Installing](../user/02-installing.md)); this page is for people who change it.
The site that hosts it is described in [website.md](website.md).

The flasher is plain ES modules. The logic ports rfoundry, and the tests check the two agree:

| Module | Does | Ported from |
|---|---|---|
| `stockfw.js` | Stock file container, decryption, codec extraction, key derivation (column vote and runner-up search; the byte-pair refinement some keys need isn't ported) | `rfoundry/stockfw.py` |
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
| `fakec7000.js` | A simulated HR_C7000 radio: boot ROM window, RadioFoundry's debug link (VERSION, 0x1A), the stub and both flash chips | |
| `usmart.js`, `iap.js`, `iapui.js` | The stock side of an HR_C7000 radio, by protocol: the stub loaded through a stock debug console (USMART, `flash.stock_console`), the vendor update mode write (IAP, `flash.update_mode`) and their steps | |
| `c7000ui.js`, `radiotop.js` | The HR_C7000 backend: its steps (connect, back up and check, write, check) and the radio picture with its LED | |
| `dom.js` | DOM helpers and strings shared by `app.js` and `c7000ui.js` | |
| `app.js` | The page: the tasks, the shared steps and the `anyroad-dfu` backend | |

Add `#sim=<radio>-stock` to the flasher's address to run every step against a simulated radio on its stock firmware,
`#sim=<radio>` against one that runs RadioFoundry (an STM32F4 radio updates through the STM32F4 loader, an HR_C7000 radio
through the loader hand-over). `#sim` alone is the first radio on stock firmware. The fake comes from the radio's data
(`fakeC7000Options()` and `c7000Simulation()` in `fakec7000.js`, `simulation()` in `fake.js`): a radio with a stock
console gets one, with its first stock version.

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
| Stock (STM32F4) | Nothing: the bootloader can't read flash (read protection level 1) and the stock firmware's own USB protocol has no spec here. The install therefore backs up right after writing RadioFoundry: flashing never touches the SPI flash, and RadioFoundry doesn't write to a foreign settings region until it is claimed, so the stock codeplug, calibration and resources are still intact |

## Restore

| Target | Writes | Limits |
|---|---|---|
| Exactly as backed up (backup taken on RadioFoundry) | The internal flash image through the bootloader (every sector erased and rewritten), then the codeplug areas that differ through CPS writes | The CPS refuses RadioFoundry's own areas and the calibration window |
| Codeplug only (any backup that holds the codeplug span) | The codeplug areas that differ, through CPS writes; the firmware is not touched | As above |
| Back to stock | The stock application from the user's stock file, codec included, through the bootloader | The SPI flash stays as it is: writing it back whole needs a firmware command that doesn't exist yet (a gated STORAGE write, like CLAIM). Until then the flasher says so, and the stock codeplug goes back with the vendor CPS |

The stock file's resource segment (SPI flash 0x60000) is not written back: how the vendor updater writes it is not
documented, and RadioFoundry doesn't overwrite it unless an ID database is uploaded.

## HR_C7000 radios (the boot stub)

Radios whose backend is `c7000-stub` (the `serial-loader` and `serial-iap` families; a board with a first stage adds the section
below) are flashed over their
programming cable with Web Serial, through the boot stub ([stub/README.md](../../stub/README.md),
[debuglink.md](../spec/debuglink.md) 3.18). The Baofeng bootloader is never used and never written. The install has
its own steps:

| Step | What happens |
|---|---|
| Connect | The page looks for a stub that still runs, then for RadioFoundry. RadioFoundry with `CONFIG_STUB_ENTRY` hands over to the stub (command 0x1A); RadioFoundry without it is restarted with its CPS reboot, which passes the boot ROM's serial window (not yet tried on a radio). Otherwise the page asks the user to switch the radio off and on and catches the boot ROM. The radio model is then checked by the bootloader's ID string, and the page sends the stub the board's profile (CONFIG: clocks, UART, pins, board id) and checks the echo; the stub refuses writes until then |
| Back up and check | A first install reads the whole program memory (required) and, if asked, the data memory, keeps the program memory in IndexedDB and downloads a backup bundle. An update reads only the codec slot. The page then decides the codec |
| Write | Only sectors whose CRC differs are written. The first application sector goes last, each sector is read back right away (three tries), and a final pass checks every sector |
| Check | The stub resets the chip; the page asks RadioFoundry's VERSION for the build and the codec |

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
| Factory update mode (recovery) | `iap.js`: the loader's update mode takes `Erase` and 2 KB pages with additive checksums. Install: the first stage, RadioFoundry and the codec slot in one file; it must start with the first stage's signature (`flash.first_stage`). Restore: a backup's application area and slot (`restoreTarget()`). Offered when nothing answers on the cable, for a stock build the page doesn't know, and in Troubleshooting (`recovery.extra`, "Reinstall in Factory Update Mode") |

The first stage is the only way into the stub from a radio that runs RadioFoundry without answering.
`site/tests/iap.test.mjs` runs the console stub load and the IAP write against simulated radios (`fakec7000.js`) and
compares the codec carve with `tools/hrc7000_codec.py`.

### A radio with a second chip

A radio with a second receiver on another chip has that chip as a board of its own, `part_of` the radio in its
`board.toml` with `[flash.stub] bridge`; the site's radio entry carries it as `flash.second`. The cable doesn't reach
that chip. Once RadioFoundry runs on the first chip, the page's done step offers to install the second one: the same
steps, as a radio with the second board's id (its releases and builds), reached through the first chip. RadioFoundry
on the first chip hands the cable over to the second chip's first stage (`DL_CMD_RX2` BRIDGE,
[rx2-link.md](../spec/rx2-link.md) 4), the stub loads and writes as usual, and the first chip restarts by itself
afterwards; the done step asks it whether the second chip answers. A second chip that still runs its original firmware
has no first stage and no link: the first chip reports it down (`DL_CMD_RX2` flags), so the page offers the factory
update mode instead (`IAP_B`, steps from the second board's `[flash.bootloader]`), with the same write as any other
factory update mode install. The page's `#sim=` simulation has a case for that second chip on its original firmware.
The update-mode texts that describe the screen take the radio's form where `site/i18n/en.json` has a `<key>_<board id>`
text (`iapui.js` `own`): the second chip's board id first, then its radio's. A board is listed with
`[flash.stub] tested = false` until the stub has run on a radio.

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

## A failed install (the help task)

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
