<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The OpenDigitalRadio Authors -->

# odrflash

Flashes OpenDigitalRadio release images onto a radio. Release images never contain the AMBE codec; odrflash either
keeps the codec already in the radio (it writes only the firmware's own flash sectors) or writes the codec from your
radio's stock firmware file. Radios: Baofeng DM-1701 today; the DM-32UV is designed for but not released yet.

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
odrflash flash --release latest --stock "DM-1701(CSV)-V02.06.bin"
                                                        also write the codec from the stock firmware file
odrflash probe                                          radio running ODR: can its codec be kept?
odrflash detect                                         what's connected: ODR, and/or the bootloader
odrflash key --stock "DM-1701(CSV)-V02.06.bin" --save   derive the bootloader key once and keep it
odrflash download --release v0.1.0 -o DIR               just fetch and verify the files
odrflash merge odr-dm1701.bin --stock FILE -o merged.bin  a merged image for other flashing tools
```

`odrflash flash` does, in order: load the image and check it against its manifest (SHA-256, the radio model, the
ranges); ask a running ODR radio for its codec over USB; decide the codec mode; tell you to put the radio in its
bootloader (DM-1701: off, hold PTT and SK1, on) and wait for it; print the plan; erase and write; wait for the radio to
restart and check what it reports.

### The codec

| Mode | Erases and writes | When |
|---|---|---|
| keep | the firmware's sectors only (DM-1701: 3-6); the codec sectors 7, 8, 10, 11 are never touched | the radio runs ODR and reports a matching codec (`odrflash probe`, or the start of `flash`); or you say it runs stock V02.06 CSV (`--stock-installed "V02.06 CSV"`, or answer the question); or `--codec keep` |
| merge | the firmware's sectors and the codec sectors, with the codec from `--stock` | `--stock FILE` is given and keeping isn't known to be safe, or `--codec merge` |

If odrflash keeps a codec that turns out not to be there, nothing breaks: the firmware checks the codec before it
uses it, so the radio runs FM only and shows "No codec: voice off". Flash again with `--stock`.

An image with the codec already merged in (`odrflash merge`, or the `_codec.bin` a build writes when the stock file is
in `codecs/`) needs no manifest or `--board`, and `--stock` only for the bootloader key: odrflash checks every codec
window against the supported stock files by SHA-256, which also names the radio, requires 0xFF everywhere else past the
application, and flashes it in merge mode with the codec from the image. `--codec keep` writes only its application
part.

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

### USB access

| System | Once |
|---|---|
| macOS | nothing |
| Linux | a udev rule for the bootloader (0483:DF11), which odrflash prints when access is denied; for the radio's serial port, membership of the `dialout` group (`sudo usermod -aG dialout $USER`, then log in again) |
| Windows | bind the bootloader (0483:DF11, "STM32 BOOTLOADER") to WinUSB with Zadig (zadig.akeo.ie). The radio's serial port needs no driver |

## For developers

| Module | Does |
|---|---|
| `odrflash/boards.py` | per radio: flash sectors, application base, codec windows and profile, bootloader IDs, key hash |
| `odrflash/plan.py` | which sectors to erase and write for an image and a codec mode |
| `odrflash/manifest.py` | release manifests: generate (used by `tools/release.py`) and verify |
| `odrflash/stockfw.py` | the DM-1701 and MD-380 stock files: container, decryption, codec extraction and merge, key derivation (also used by `tools/codec_extract.py`) |
| `odrflash/dfu.py` | the AnyRoad DFU bootloader: find, open, run a plan |
| `odrflash/debuglink.py` | VERSION / INFO_CHIP / READ_MEM over the debug link (read only) |
| `odrflash/bootkey.py` | the bootloader key from a radio that runs ODR, found in its bootloader by the key's hash |
| `odrflash/fakedfu.py` | a fake bootloader with a flash model, for the tests and `--simulate` |
| `odrflash/release.py`, `state.py`, `cli.py` | GitHub downloads, the remembered probe, the commands |

Tests: `python3 -m unittest discover -s tools/odrflash/tests -v` (no USB; `tools/ci.sh` runs them). A whole flash
against the fake bootloader: `odrflash flash IMAGE --simulate odr-codec` (or `stock` with `--stock-installed "V02.06
CSV"`, or `odr-nocodec --stock synthetic`). A local build without a manifest: `odrflash flash
build/dm1701/odr_dm1701.bin --board dm1701`. Design and the sector map: [docs/dev/release.md](../../docs/dev/release.md).
