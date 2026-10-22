<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The OpenDigitalRadio Authors -->

# odrflash

Flashes OpenDigitalRadio release images onto a radio. Release images never contain the AMBE codec; odrflash either
keeps the codec already in the radio (it writes only the firmware's own flash sectors) or writes the codec from your
radio's stock firmware file. Radios: the boards of the [board registry](../../docs/dev/board-registry.md); a USB DFU
radio flashes over USB, the others over their programming cable (below). A radio installed through the RAM stub
(`[flash] family = "serial-iap"`) uses the web flasher or `stub/stubtool.py`.

## Install

- **Executable:** download `odrflash-<version>-<os>-<arch>` from the release page. One file, nothing else to install
  (libusb is inside). macOS: `xattr -d com.apple.quarantine odrflash-*` once, then `chmod +x`. Windows: see "USB
  access" below.
- **pipx:** `pipx install "git+https://github.com/sfowlr/OpenDigitalRadio#subdirectory=tools/odrflash"`, or
  `pipx install ./tools/odrflash` from a checkout.
- **From a checkout without installing:** `cd tools/odrflash && python3 -m odrflash ...` (needs `pip install pyusb
  libusb-package pyserial certifi`).

## Use

```
odrflash flash --release latest                         newest release, downloaded and checked
odrflash flash --release v0.1.0 --dry-run               show exactly which sectors would be erased and written
odrflash flash odr-dm1701-v0.1.0.bin                    a downloaded image (its .json manifest next to it)
odrflash flash odr_dm1701_codec.bin                     an image with the codec already merged in (see below)
odrflash flash build/dm1701/odr_dm1701.bin --board dm1701   a local build
odrflash flash --release latest --stock "DM-1701(CSV)-V02.06.bin"
                                                        also write the codec from the stock firmware file
odrflash probe                                          radio running ODR: can its codec be kept?
odrflash detect                                         what's connected: ODR, and/or the bootloader
odrflash key --stock "DM-1701(CSV)-V02.06.bin" --save   derive the bootloader key once and keep it
odrflash download --release v0.1.0 -o DIR               just fetch and verify the files
odrflash merge odr-dm1701.bin --stock FILE -o merged.bin  a merged image for other flashing tools
odrflash flash build/dm1801pro/odr_dm1801pro.bin --board dm1801pro --port /dev/ttyUSB0
                                                        a DM-1801 Pro over its programming cable (below)
```

`odrflash flash` does, in order: load the image and check it against its manifest (SHA-256, the radio model, the
ranges); ask a running ODR radio for its codec over USB; decide the codec mode; tell you to put the radio in its
bootloader (DM-1701: off, hold PTT and SK1, on) and wait for it; print the plan; erase and write; wait for the radio to
restart and check what it reports.

### The codec

| Mode | Erases and writes | When |
|---|---|---|
| keep | the firmware's sectors only (DM-1701: within 3-9; MD-380: within 3-7); the codec sectors (DM-1701: 10, 11; MD-380: 8, 10, 11) are never touched | the radio runs ODR and reports a matching codec (`odrflash probe`, or the start of `flash`); or `--codec keep` |
| merge | the firmware's sectors and the codec sectors, with the codec from the copy saved from the radio or from `--stock` | keeping isn't known to be safe and odrflash has a saved copy or `--stock FILE`; or `--codec merge` |

A radio on an ODR build from before the codec moved reports `dm1701-v0206-csv` (the new layout's profile is
`dm1701-v0206-csv-s11`). Its codec code sits in sectors the update overwrites, so odrflash writes the codec: from a
copy saved earlier, else from `--stock`. Stock firmware never holds the codec where ODR keeps it, so `--stock-installed`
does not make keep safe.

If odrflash keeps a codec that turns out not to be there, nothing breaks: the firmware checks the codec before it
uses it, so the radio runs FM only and shows "No codec: voice off". Flash again with `--stock`.

An image with the codec already merged in (`odrflash merge`, or the `_codec.bin` a build writes when the stock file is
in `codecs/`) needs no manifest or `--board`, and `--stock` only for the bootloader key: odrflash checks every codec
window against the supported stock files by SHA-256, which also names the radio, requires 0xFF everywhere else past the
application, and flashes it in merge mode with the codec from the image. `--codec keep` writes only its application
part.

An image is one range from 0x0800C000. A release whose manifest lists two ranges (an application and a sector 9 data
part) is from before the codec moved, and odrflash refuses it.

The stock file is Baofeng's `DM-1701(CSV)-V02.06.bin` (sha256 `0314efd4...`); an archive is at
github.com/daniel5600/BAOFENG-DM1701-Original-Firmware. odrflash checks it by SHA-256 and refuses other files as a
codec source.

### The bootloader key

The radio's bootloader needs a 1024-byte key. odrflash derives it from the stock firmware file (any DM-1701 version)
and checks it against the key's published SHA-256, so `--stock` is enough; `odrflash key --stock FILE --save` keeps it
for later runs without `--stock`; `--key FILE` takes a key file you already have. A radio that runs ODR gives the key
itself: odrflash reads the stock bootloader over the debug link and takes the block with that hash
(`odrflash/bootkey.py`), so an update needs none of these (`odrflash key --save` keeps it from the radio). The key is
never part of ODR.

### The saved codec

The codec stays in place across an update. odrflash still reads a copy, the first time it sees the radio running ODR
with its codec: over the debug link, where stock keeps it or where the merge placed it, checked against the stock
codec's hashes, saved in its config directory. That copy is written instead of asking for `--stock` whenever the
radio's codec can't be kept or checked: the radio doesn't answer over the debug link, is back on stock firmware, or
reports no codec. `--codec keep` still wins. The copy also finishes an update that stopped halfway, when the radio can
no longer start ODR.

### The DM-1801 Pro

The DM-1801 Pro has no USB data port: odrflash talks to it over the programming cable (a USB-serial cable, found by
itself when it is the only one; `--port` names it). There are two ways in, and odrflash wraps the tool that already does
each: the stub, `stub/stubtool.py` ([USER-GUIDE.md](../../stub/USER-GUIDE.md)), and Baofeng's update mode,
`tools/serial_loader.py --model dm1801pro`.

| Route | When (`--route auto`) | Writes | The codec slot (0x030E8000..0x030FFFFF) |
|---|---|---|---|
| stub | ODR answers on the cable and offers stub entry (built with `CONFIG_STUB_ENTRY=y`) | only the 4 KB sectors that differ, each checked by CRC; then the radio restarts | kept when the image has none (keep); written when it has one or `--stock` is given (merge) |
| bootloader | anything else: odrflash asks you to switch the radio on with PTT and SK1 held, and waits `--wait` seconds for the loader | the loader erases 0x0300C000..0x030FFFFF, then takes the whole image | erased: it must be in the image (`odr_dm1801pro_codec.bin`) or built from `--stock` (merge); keep is refused |

`--stock` here is a program flash read of the radio (`stubtool.py PORT --progmem FILE`). `tools/hrc7000_codec.py` checks
it by SHA-256 and builds the slot from it. `--route stub` or `--route bootloader` overrides the choice. A route the
radio can't take is refused before anything is written, and so is a radio that reports another board. After the restart
odrflash reads VERSION over the cable and compares the git hash and the codec with what it wrote. Neither route writes
the bootloader below 0x0300C000.

The serial route loads `tools/serial_loader.py`, `tools/hrc7000_codec.py` and `stub/stubtool.py` from the checkout; the
odrflash executable carries them. A `pipx` install from the subdirectory doesn't, and says so. Neither route of odrflash
has flashed a radio yet; the tools it wraps have.

### The flash stub route (DM-1701, MD-380, MD-9600, GD-77)

A radio that runs ODR built with `CONFIG_STUB_ENTRY=y` can be updated without the bootloader key press:

    odrflash flash build/dm1701/odr_dm1701.bin --board dm1701 --route stub
    odrflash flash build/gd77/odr_gd77.bin --board gd77 --route stub --stock GD-77-update-file.sgl

odrflash hands the image to `stub/stubtool.py`, which loads the flash stub into the radio's RAM over the USB debug link,
writes the sectors that differ and restarts the radio. The radio keeps its codec. On the GD-77, `--stock` merges the codec
into the image first; without it the radio keeps the one it has. `--port` names the debug link's serial port (found
when there is only one). Not tried on a radio ([stub-stm32f4.md](../../docs/spec/stub-stm32f4.md),
[stub-k22f.md](../../docs/spec/stub-k22f.md)).

### USB access

| System | Once |
|---|---|
| macOS | nothing |
| Linux | a udev rule for the bootloader (0483:DF11), which odrflash prints when access is denied; for the radio's serial port, membership of the `dialout` group (`sudo usermod -aG dialout $USER`, then log in again) |
| Windows | bind the bootloader ("Digital Radio in USB mode", 0483:DF11) to WinUSB with Zadig, once ([steps](../../docs/user/02-installing.md#windows-the-bootloader-driver)); odrflash prints them when it can't open the bootloader. The radio's serial port needs no driver |

## For developers

| Module | Does |
|---|---|
| `odrflash/boards.py` | per radio, read from `registry.json` (generated by `tools/boards.py` from `boards/*/board.toml`): flash sectors, application base, codec windows and profile, bootloader IDs, key hash |
| `odrflash/plan.py` | which sectors to erase and write for an image and a codec mode |
| `odrflash/manifest.py` | release manifests: generate (used by `tools/release.py`) and verify |
| `odrflash/stockfw.py` | the DM-1701 and MD-380 stock files: container, decryption, codec extraction and merge, key derivation (also used by `tools/codec_extract.py`) |
| `odrflash/dfu.py` | the AnyRoad DFU bootloader: find, open, run a plan |
| `odrflash/debuglink.py` | VERSION / INFO_CHIP / READ_MEM over the debug link (read only) |
| `odrflash/codecsave.py` | the codec read back from a radio that runs ODR (where stock keeps it, or where the merge placed it), checked by hash, saved in the config directory and written back where the radio's codec can't be kept or checked |
| `odrflash/bootkey.py` | the bootloader key from a radio that runs ODR, found in its bootloader by the key's hash |
| `odrflash/fakedfu.py` | a fake bootloader with a flash model, for the tests and `--simulate` |
| `odrflash/serialflash.py` | the HR_C7000 radios over the programming cable: the route choice, the codec slot, and the calls into `stub/stubtool.py` and `tools/serial_loader.py` |
| `odrflash/release.py`, `state.py`, `cli.py` | GitHub downloads, the remembered probe, the commands |

Tests: `python3 -m unittest discover -s tools/odrflash/tests -v` (no USB, no serial port; `tools/ci.sh` runs them).
`test_serialflash.py` runs both serial routes against the wrapped tools' own simulated radios. A whole flash
against the fake bootloader: `odrflash flash IMAGE --simulate odr-codec` (or `stock` with `--stock-installed "V02.06
CSV"`, or `odr-nocodec --stock synthetic`). A local build without a manifest: `odrflash flash
build/dm1701/odr_dm1701.bin --board dm1701`. Design and the sector map: [docs/dev/release.md](../../docs/dev/release.md).
