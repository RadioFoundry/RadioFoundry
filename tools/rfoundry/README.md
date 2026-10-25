<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# rfoundry

Flashes RadioFoundry release images onto a radio. Release images never contain the AMBE codec; rfoundry either
keeps the codec already in the radio (it writes only the firmware's own flash sectors) or writes the codec from your
radio's stock firmware file. Radios: the boards of the [board registry](../../docs/dev/board-registry.md); a USB DFU
radio flashes over USB, the others over their programming cable (below). A radio installed through the RAM stub
(`[flash] family = "serial-iap"`) uses the web flasher or `stub/stubtool.py`.

## Install

- **Executable:** download `rfoundry-<version>-<os>-<arch>` from the release page. One file, nothing else to install
  (libusb is inside). macOS: `xattr -d com.apple.quarantine rfoundry-*` once, then `chmod +x`. Windows: see "USB
  access" below.
- **pipx:** `pipx install "git+https://github.com/radiofoundry/radiofoundry#subdirectory=tools/rfoundry"`, or
  `pipx install ./tools/rfoundry` from a checkout.
- **From a checkout without installing:** `cd tools/rfoundry && python3 -m rfoundry ...` (needs `pip install pyusb
  libusb-package pyserial certifi`).

## Use

```
rfoundry flash --release latest                         newest release, downloaded and checked
rfoundry flash --release v0.1.0 --dry-run               show exactly which sectors would be erased and written
rfoundry flash rfoundry-dm1701-v0.1.0.bin                    a downloaded image (its .json manifest next to it)
rfoundry flash rfoundry_dm1701_codec.bin                     an image with the codec already merged in (see below)
rfoundry flash build/dm1701/rfoundry_dm1701.bin --board dm1701   a local build
rfoundry flash --release latest --stock "DM-1701(CSV)-V02.06.bin"
                                                        also write the codec from the stock firmware file
rfoundry probe                                          radio running RadioFoundry: can its codec be kept?
rfoundry detect                                         what's connected: RadioFoundry, and/or the bootloader
rfoundry key --stock "DM-1701(CSV)-V02.06.bin" --save   derive the bootloader key once and keep it
rfoundry download --release v0.1.0 -o DIR               just fetch and verify the files
rfoundry merge rfoundry-dm1701.bin --stock FILE -o merged.bin  a merged image for other flashing tools
rfoundry flash build/dm1801pro/rfoundry_dm1801pro.bin --board dm1801pro --port /dev/ttyUSB0
                                                        a DM-1801 Pro over its programming cable (below)
```

`rfoundry flash` does, in order: load the image and check it against its manifest (SHA-256, the radio model, the
ranges); ask a running RadioFoundry radio for its codec over USB; decide the codec mode; tell you to put the radio in its
bootloader (DM-1701: off, hold PTT and SK1, on) and wait for it; print the plan; erase and write; wait for the radio to
restart and check what it reports.

### The codec

| Mode | Erases and writes | When |
|---|---|---|
| keep | the firmware's sectors only (DM-1701: within 3-9; MD-380: within 3-7); the codec sectors (DM-1701: 10, 11; MD-380: 8, 10, 11) are never touched | the radio runs RadioFoundry and reports a matching codec (`rfoundry probe`, or the start of `flash`); or `--codec keep` |
| merge | the firmware's sectors and the codec sectors, with the codec from the copy saved from the radio or from `--stock` | keeping isn't known to be safe and rfoundry has a saved copy or `--stock FILE`; or `--codec merge` |

A radio on a RadioFoundry build from before the codec moved reports `dm1701-v0206-csv` (the new layout's profile is
`dm1701-v0206-csv-s11`). Its codec code sits in sectors the update overwrites, so rfoundry writes the codec: from a
copy saved earlier, else from `--stock`. Stock firmware never holds the codec where RadioFoundry keeps it, so `--stock-installed`
does not make keep safe.

If rfoundry keeps a codec that turns out not to be there, nothing breaks: the firmware checks the codec before it
uses it, so the radio runs FM only and shows "No codec: voice off". Flash again with `--stock`.

An image with the codec already merged in (`rfoundry merge`, or the `_codec.bin` a build writes when the stock file is
in `codecs/`) needs no manifest or `--board`, and `--stock` only for the bootloader key: rfoundry checks every codec
window against the supported stock files by SHA-256, which also names the radio, requires 0xFF everywhere else past the
application, and flashes it in merge mode with the codec from the image. `--codec keep` writes only its application
part.

An image is one range from 0x0800C000. A release whose manifest lists two ranges (an application and a sector 9 data
part) is from before the codec moved, and rfoundry refuses it.

The stock file is Baofeng's `DM-1701(CSV)-V02.06.bin` (sha256 `0314efd4...`); an archive is at
github.com/daniel5600/BAOFENG-DM1701-Original-Firmware. rfoundry checks it by SHA-256 and refuses other files as a
codec source.

### The bootloader key

The radio's bootloader needs a 1024-byte key. rfoundry derives it from the stock firmware file (any DM-1701 version)
and checks it against the key's published SHA-256, so `--stock` is enough; `rfoundry key --stock FILE --save` keeps it
for later runs without `--stock`; `--key FILE` takes a key file you already have. A radio that runs RadioFoundry gives the key
itself: rfoundry reads the stock bootloader over the debug link and takes the block with that hash
(`rfoundry/bootkey.py`), so an update needs none of these (`rfoundry key --save` keeps it from the radio). The key is
never part of RadioFoundry.

### The saved codec

The codec stays in place across an update. rfoundry still reads a copy, the first time it sees the radio running RadioFoundry
with its codec: over the debug link, where stock keeps it or where the merge placed it, checked against the stock
codec's hashes, saved in its config directory. That copy is written instead of asking for `--stock` whenever the
radio's codec can't be kept or checked: the radio doesn't answer over the debug link, is back on stock firmware, or
reports no codec. `--codec keep` still wins. The copy also finishes an update that stopped halfway, when the radio can
no longer start RadioFoundry.

### The DM-1801 Pro

The DM-1801 Pro has no USB data port: rfoundry talks to it over the programming cable (a USB-serial cable, found by
itself when it is the only one; `--port` names it). There are two ways in, and rfoundry wraps the tool that already does
each: the stub, `stub/stubtool.py` ([USER-GUIDE.md](../../stub/USER-GUIDE.md)), and Baofeng's update mode,
`tools/serial_loader.py --model dm1801pro`.

| Route | When (`--route auto`) | Writes | The codec slot (0x030E8000..0x030FFFFF) |
|---|---|---|---|
| stub | RadioFoundry answers on the cable and offers stub entry (built with `CONFIG_STUB_ENTRY=y`) | only the 4 KB sectors that differ, each checked by CRC; then the radio restarts | kept when the image has none (keep); written when it has one or `--stock` is given (merge) |
| bootloader | anything else: rfoundry asks you to switch the radio on with PTT and SK1 held, and waits `--wait` seconds for the loader | the loader erases 0x0300C000..0x030FFFFF, then takes the whole image | erased: it must be in the image (`rfoundry_dm1801pro_codec.bin`) or built from `--stock` (merge); keep is refused |

`--stock` here is a program flash read of the radio (`stubtool.py PORT --progmem FILE`). `tools/hrc7000_codec.py` checks
it by SHA-256 and builds the slot from it. `--route stub` or `--route bootloader` overrides the choice. A route the
radio can't take is refused before anything is written, and so is a radio that reports another board. After the restart
rfoundry reads VERSION over the cable and compares the git hash and the codec with what it wrote. Neither route writes
the bootloader below 0x0300C000.

The serial route loads `tools/serial_loader.py`, `tools/hrc7000_codec.py` and `stub/stubtool.py` from the checkout; the
rfoundry executable carries them. A `pipx` install from the subdirectory doesn't, and says so. Neither route of rfoundry
has flashed a radio yet; the tools it wraps have.

### USB access

| System | Once |
|---|---|
| macOS | nothing |
| Linux | a udev rule for the bootloader (0483:DF11), which rfoundry prints when access is denied; for the radio's serial port, membership of the `dialout` group (`sudo usermod -aG dialout $USER`, then log in again) |
| Windows | bind the bootloader ("Digital Radio in USB mode", 0483:DF11) to WinUSB with Zadig, once ([steps](../../docs/user/02-installing.md#windows-the-bootloader-driver)); rfoundry prints them when it can't open the bootloader. The radio's serial port needs no driver |

## For developers

| Module | Does |
|---|---|
| `rfoundry/boards.py` | per radio, read from `registry.json` (generated by `tools/boards.py` from `boards/*/board.toml`): flash sectors, application base, codec windows and profile, bootloader IDs, key hash |
| `rfoundry/plan.py` | which sectors to erase and write for an image and a codec mode |
| `rfoundry/manifest.py` | release manifests: generate (used by `tools/release.py`) and verify |
| `rfoundry/stockfw.py` | the DM-1701 and MD-380 stock files: container, decryption, codec extraction and merge, key derivation (also used by `tools/codec_extract.py`) |
| `rfoundry/dfu.py` | the AnyRoad DFU bootloader: find, open, run a plan |
| `rfoundry/debuglink.py` | VERSION / INFO_CHIP / READ_MEM over the debug link (read only) |
| `rfoundry/codecsave.py` | the codec read back from a radio that runs RadioFoundry (where stock keeps it, or where the merge placed it), checked by hash, saved in the config directory and written back where the radio's codec can't be kept or checked |
| `rfoundry/bootkey.py` | the bootloader key from a radio that runs RadioFoundry, found in its bootloader by the key's hash |
| `rfoundry/fakedfu.py` | a fake bootloader with a flash model, for the tests and `--simulate` |
| `rfoundry/serialflash.py` | the HR_C7000 radios over the programming cable: the route choice, the codec slot, and the calls into `stub/stubtool.py` and `tools/serial_loader.py` |
| `rfoundry/release.py`, `state.py`, `cli.py` | GitHub downloads, the remembered probe, the commands |

Tests: `python3 -m unittest discover -s tools/rfoundry/tests -v` (no USB, no serial port; `tools/ci.sh` runs them).
`test_serialflash.py` runs both serial routes against the wrapped tools' own simulated radios. A whole flash
against the fake bootloader: `rfoundry flash IMAGE --simulate rfoundry-codec` (or `stock` with `--stock-installed "V02.06
CSV"`, or `rfoundry-nocodec --stock synthetic`). A local build without a manifest: `rfoundry flash
build/dm1701/rfoundry_dm1701.bin --board dm1701`. Design and the sector map: [docs/dev/release.md](../../docs/dev/release.md).
