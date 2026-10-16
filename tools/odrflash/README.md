<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The OpenDigitalRadio Authors -->

# odrflash

Flashes OpenDigitalRadio release images onto a radio. Release images never contain the AMBE codec; odrflash either
keeps the codec already in the radio (it writes only the firmware's own flash sectors) or writes the codec from your
radio's stock firmware file. Radios: Baofeng DM-1701 today; the DM-1801 Pro flashes over its programming cable (below,
not released yet); the DM-32UV is designed for but not released yet.

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
odrflash flash build/dm1701/odr_dm1701.bin --board dm1701   a local build; its odr_dm1701_rodata2.bin (sector 9) is
                                                        written with it
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
| keep | the firmware's sectors only (DM-1701: 3-6); the codec sectors 7, 8, 10, 11 are never touched | the radio runs ODR and reports a matching codec (`odrflash probe`, or the start of `flash`); or you say it runs stock V02.06 CSV (`--stock-installed "V02.06 CSV"`, or answer the question); or `--codec keep` |
| merge | the firmware's sectors and the codec sectors, with the codec from `--stock` | `--stock FILE` is given and keeping isn't known to be safe, or `--codec merge` |

If odrflash keeps a codec that turns out not to be there, nothing breaks: the firmware checks the codec before it
uses it, so the radio runs FM only and shows "No codec: voice off". Flash again with `--stock`.

An image with the codec already merged in (`odrflash merge`, or the `_codec.bin` a build writes when the stock file is
in `codecs/`) needs no manifest or `--board`, and `--stock` only for the bootloader key: odrflash checks every codec
window against the supported stock files by SHA-256, which also names the radio, requires 0xFF everywhere else past the
application, and flashes it in merge mode with the codec from the image. `--codec keep` writes only its application
and sector 9 parts.

**Sector 9 (DM-1701).** The firmware's read-only data lives in flash sector 9, so a DM-1701 image has two ranges: the
application (sectors 3-6) and the data (sector 9). A release's manifest lists both, and `flash` erases and writes
sectors 3-6 and 9 and never 7, 8, 10, 11 or the bootloader. For a local build it reads `odr_dm1701_rodata2.bin` next to
the image. [release.md](../../docs/dev/release.md#sector-9) has the details.

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

### The codec on the MD-380

On the MD-380 every update erases part of the codec (ODR's image shares a flash sector with it), so it is written
again each time. A radio that runs ODR with the codec hands it over: odrflash reads it back over the debug link,
checks it against the stock codec's hashes, saves a copy in its config directory and writes that back, so an update
needs no `--stock`. The saved copy also finishes an update that stopped halfway, when the radio can no longer start
ODR.

### The DM-1801 Pro

The DM-1801 Pro has no USB data port: odrflash talks to it over the programming cable (a USB-serial cable, found by
itself when it is the only one; `--port` names it). There are two ways in, and odrflash wraps the tool that already does
each: the stub, `stub/stubtool.py` ([USER-GUIDE.md](../../stub/USER-GUIDE.md)), and Baofeng's update mode,
`tools/dm32_flash.py --model dm1801pro`.

| Route | When (`--route auto`) | Writes | The codec slot (0x030E8000..0x030FFFFF) |
|---|---|---|---|
| stub | ODR answers on the cable and offers stub entry (built with `CONFIG_STUB_ENTRY=y`) | only the 4 KB sectors that differ, each checked by CRC; then the radio restarts | kept when the image has none (keep); written when it has one or `--stock` is given (merge) |
| bootloader | anything else: odrflash asks you to switch the radio on with PTT and SK1 held, and waits `--wait` seconds for the loader | the loader erases 0x0300C000..0x030FFFFF, then takes the whole image | erased: it must be in the image (`odr_dm1801pro_codec.bin`) or built from `--stock` (merge); keep is refused |

`--stock` here is a program flash read of the radio (`stubtool.py PORT --progmem FILE`). `tools/dm32_codec.py` checks
it by SHA-256 and builds the slot from it. `--route stub` or `--route bootloader` overrides the choice. A route the
radio can't take is refused before anything is written, and so is a radio that reports another board. After the restart
odrflash reads VERSION over the cable and compares the git hash and the codec with what it wrote. Neither route writes
the bootloader below 0x0300C000.

The serial route loads `tools/dm32_flash.py`, `tools/dm32_codec.py` and `stub/stubtool.py` from the checkout; the
odrflash executable carries them. A `pipx` install from the subdirectory doesn't, and says so. Neither route of odrflash
has flashed a radio yet; the tools it wraps have.

### USB access

| System | Once |
|---|---|
| macOS | nothing |
| Linux | a udev rule for the bootloader (0483:DF11), which odrflash prints when access is denied; for the radio's serial port, membership of the `dialout` group (`sudo usermod -aG dialout $USER`, then log in again) |
| Windows | bind the bootloader ("Digital Radio in USB mode", 0483:DF11) to WinUSB with Zadig, once ([steps](../../docs/user/02-installing.md#windows-the-bootloader-driver)); odrflash prints them when it can't open the bootloader. The radio's serial port needs no driver |

## For developers

| Module | Does |
|---|---|
| `odrflash/boards.py` | per radio: flash sectors, application base, codec windows and profile, bootloader IDs, key hash |
| `odrflash/plan.py` | which sectors to erase and write for an image and a codec mode |
| `odrflash/manifest.py` | release manifests: generate (used by `tools/release.py`) and verify |
| `odrflash/stockfw.py` | the DM-1701 and MD-380 stock files: container, decryption, codec extraction and merge, key derivation (also used by `tools/codec_extract.py`) |
| `odrflash/dfu.py` | the AnyRoad DFU bootloader: find, open, run a plan |
| `odrflash/debuglink.py` | VERSION / INFO_CHIP / READ_MEM over the debug link (read only) |
| `odrflash/codecsave.py` | where an update erases the codec (the MD-380): the codec read back from a radio that runs ODR, checked by hash, saved in the config directory and written back |
| `odrflash/bootkey.py` | the bootloader key from a radio that runs ODR, found in its bootloader by the key's hash |
| `odrflash/fakedfu.py` | a fake bootloader with a flash model, for the tests and `--simulate` |
| `odrflash/serialflash.py` | the HR_C7000 radios over the programming cable: the route choice, the codec slot, and the calls into `stub/stubtool.py` and `tools/dm32_flash.py` |
| `odrflash/release.py`, `state.py`, `cli.py` | GitHub downloads, the remembered probe, the commands |

Tests: `python3 -m unittest discover -s tools/odrflash/tests -v` (no USB, no serial port; `tools/ci.sh` runs them).
`test_serialflash.py` runs both serial routes against the wrapped tools' own simulated radios. A whole flash
against the fake bootloader: `odrflash flash IMAGE --simulate odr-codec` (or `stock` with `--stock-installed "V02.06
CSV"`, or `odr-nocodec --stock synthetic`). A local build without a manifest: `odrflash flash
build/dm1701/odr_dm1701.bin --board dm1701`. Design and the sector map: [docs/dev/release.md](../../docs/dev/release.md).
