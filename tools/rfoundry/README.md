<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# rfoundry

rfoundry flashes RadioFoundry release images onto a radio. Release images never contain the AMBE codec, so rfoundry
either keeps the codec already in the radio (it writes only the firmware's own flash sectors) or writes the codec from
your radio's firmware file. It flashes the boards of the [board registry](../../docs/dev/board-registry.md): a USB DFU
radio over USB, the others over their programming cable (see [The serial route](#the-serial-route)). A radio installed
through the RAM stub (`[flash] family = "serial-iap"`) uses the web flasher or `stub/stubtool.py`.

## Install

- **Executable:** Download `rfoundry-<version>-<os>-<arch>` from the release page. It's one file with nothing else to
  install (libusb is inside). On macOS, run `xattr -d com.apple.quarantine rfoundry-*` once, then `chmod +x`. On
  Windows, see [USB access](#usb-access).
- **pipx:** Run `pipx install "git+https://github.com/radiofoundry/radiofoundry#subdirectory=tools/rfoundry"`, or
  `pipx install ./tools/rfoundry` from a checkout.
- **From a checkout without installing:** Run `cd tools/rfoundry && python3 -m rfoundry ...`. It needs
  `pip install pyusb libusb-package pyserial certifi`.

## Use

```
rfoundry flash --release latest                         newest release, downloaded and checked
rfoundry flash --release v26.11 --dry-run               show exactly which sectors would be erased and written
rfoundry flash rfoundry-dm1701-v26.11.bin               a downloaded image (its .json manifest next to it)
rfoundry flash rfoundry_dm1701_codec.bin                an image with the codec already merged in (see below)
rfoundry flash build/dm1701/rfoundry_dm1701.bin --board dm1701   a local build
rfoundry flash --release latest --stock "DM-1701(CSV)-V02.06.bin"
                                                        also write the codec from the firmware file
rfoundry probe                                          radio running RadioFoundry: can its codec be kept?
rfoundry detect                                         what's connected: RadioFoundry, the bootloader, or both
rfoundry key --stock "DM-1701(CSV)-V02.06.bin" --save   derive the bootloader key once and keep it
rfoundry download --release v26.11 -o DIR               fetch and verify the files only
rfoundry merge rfoundry-dm1701.bin --stock FILE -o merged.bin  a merged image for other flashing tools
rfoundry flash build/dm1801pro/rfoundry_dm1801pro.bin --board dm1801pro --port /dev/ttyUSB0
                                                        a radio over its programming cable (below)
```

`rfoundry flash` does the following, in order:

1. Loads the image and checks it against its manifest: SHA-256, the radio model, and the ranges.
2. Asks a running RadioFoundry radio for its codec over USB, decides the codec mode, and prints the plan.
3. On a board with `[flash.original_version]`, reads the version the radio's original firmware reports while it runs
   (`origversion.py`; nothing on the radio changes, and `--no-version-read` skips it).
4. Tells you to put the radio in its bootloader (DM-1701: off, hold PTT and SK1, on) and waits for it.
5. Erases and writes.
6. Waits for the radio to restart, checks what it reports, and stores the hardware option the version settled over
   the debug link (HWOPT), unless the radio already holds one (see
   [The original firmware version](../../docs/dev/web-flasher.md#the-original-firmware-version)).

### The codec

| Mode | Erases and writes | When |
|---|---|---|
| keep | The firmware's sectors only (DM-1701: within 3-9; MD-380: within 3-10). The codec sectors (DM-1701: 10, 11; MD-380: 11) are never touched | The radio runs RadioFoundry and reports a matching codec (`rfoundry probe`, or the start of `flash`), or `--codec keep` |
| merge | The firmware's sectors and the codec sectors, with the codec from the copy saved from the radio or from `--stock` | Keeping isn't known to be safe, and rfoundry has a saved copy or `--stock FILE`, or `--codec merge` |

For the rules behind the choice, the saved copy, and how an install moves a codec out of an earlier layout, see
[The AMBE codec](../../docs/dev/codec.md#keep-and-merge). In short:

- Manufacturer firmware never holds the codec where RadioFoundry keeps it, so `--stock-installed` doesn't make keep
  safe.
- When the radio reports a codec in an earlier layout, rfoundry reads it back over the debug link (every time, even
  with a copy saved before), checks it, and saves it as `codecs/<board>-<codec id>-<chip UID>.bin` in its settings
  folder, one file per radio. It then merges the codec into the new layout, on both routes. Where the radio can't be
  read, only a copy saved from the same radio is used, or else `--stock`. With neither, or with `--codec keep`, the
  install is refused and nothing is written. To finish an install that stopped halfway, run it again (through the
  bootloader route if the radio now starts in its bootloader); rfoundry says which copy or file finishes it.
- If rfoundry keeps a codec that turns out not to be there, nothing breaks: the firmware checks the codec before it
  uses it, so the radio runs FM only and shows "No codec: voice off". Flash again with `--stock`.

An image with the codec already merged in (`rfoundry merge`, or the `_codec.bin` a build writes when the firmware file
is in `codecs/`) needs no manifest or `--board`, and needs `--stock` only for the bootloader key. rfoundry checks every
codec window against the supported files by SHA-256, which also names the radio, requires 0xFF everywhere else past
the application, and flashes it in merge mode with the codec from the image. `--codec keep` writes only its
application part.

An image is one range from the board's application base. rfoundry refuses a release whose manifest lists two ranges.

The files rfoundry takes as a codec source are each board's `[codec] stock` (`board.toml`, with the profiles in
`rfoundry/stockfw.py`). `tools/codec_extract.py --help` lists them, and rfoundry names them when it needs one. It
checks each by SHA-256 and refuses other files. A board with several files (one codec, each build's tables at its own
address) merges from any of them, each with its own layout.

`--stock` also takes a read of the radio's internal flash made while it ran its manufacturer firmware
(`stub/stubtool.py PORT --progmem FILE`). The codec is plain there: rfoundry finds the build by its code window's
SHA-256, checks every window, and places the codec as a merge from the file would. A read gives no bootloader key; that
comes from a firmware file, a key file, or the radio.

The first time rfoundry sees a radio running RadioFoundry with its codec, it reads a copy and saves it. That read takes
a few minutes, in a `--dry-run` too: rfoundry tries each layout the codec can have in the radio and stops at the first
that matches. It says so first, then prints a dot for each segment it reads. There's no percentage, because the number
of layouts it needs isn't known in advance.

### The bootloader key

An `stm32f4` radio's bootloader XORs what it receives with a 1024-byte key for each model. The key isn't in this
repository or in a release. rfoundry and `tools/codec_extract.py` get it from the first of these that works:

1. `--key FILE`.
2. Derived from the user's firmware file (`--stock`, or once with `rfoundry key --stock FILE --save`), and checked
   against the key's published hash, so a wrong key can't pass (`stockfw.derive_key`).
3. A key saved earlier with `--save`, in rfoundry's settings directory.
4. Read from a radio that runs RadioFoundry, over the debug link: rfoundry reads the bootloader and takes the block
   with the key's hash (`rfoundry/bootkey.py`; the web flasher's `bootkey.js` does the same). `rfoundry key --save`
   keeps it.

A user needs one firmware file for the first install, which they need for the codec anyway, and never handles the
key. Updates of a radio that runs RadioFoundry need no firmware file.

### The serial route

Radios on the `hrc7000` platform are flashed over the programming cable, not DFU. rfoundry (`rfoundry/serialflash.py`)
doesn't speak either protocol itself. It calls the tools that do: the stub, `stub/stubtool.py`
([USER-GUIDE.md](../../stub/USER-GUIDE.md)), and the radio's update mode, `tools/serial_loader.py`. `--port` names the
USB-serial cable; rfoundry finds it by itself when it's the only one.

| Route | Tool | When (`--route auto`) | Writes | The codec slot |
|---|---|---|---|---|
| stub | `stub/stubtool.py` (`Radio.rfoundry_enter`, `write_image`, `reset_chip`) | RadioFoundry answers VERSION on the cable with this board, and the debug link's STUB info (op 0, [debuglink.md](../../docs/spec/debuglink.md) 3.18) carries its magic | Only the 4 KB sectors that differ, each checked by CRC; then the radio restarts | Kept when the image has none (keep); written when it has one or `--stock` is given (merge) |
| bootloader | `tools/serial_loader.py` `Flasher` | Anything else. rfoundry asks you to switch the radio on with its bootloader keys held, and waits `--wait` seconds for the loader | The loader erases the application area and the slot, then takes the whole image | Erased: it must be in the image (`rfoundry_<board>_codec.bin`) or built from `--stock` (merge). Keep, and a plain image without `--stock`, are refused |

`--stock` here is a program flash read of the radio or the manufacturer's firmware file, depending on the board.
`tools/hrc7000_codec.py` recognizes it by the code window's SHA-256 and builds the slot from it. A release never carries
the codec, so the first install of a release through the bootloader route needs it; later updates through the stub
keep the slot. `--route stub` or `--route bootloader` overrides the choice. A route the radio can't take is refused
before anything is written, and so is a radio that reports another board. After the restart, rfoundry reads VERSION
over the cable and compares the git hash and the codec with what it wrote. Neither route writes the bootloader.

The serial route loads `tools/serial_loader.py`, `tools/hrc7000_codec.py`, and `stub/stubtool.py` from the checkout;
the rfoundry executable carries them. A `pipx` install from the subdirectory doesn't, and says so.

### USB access


| System | Once |
|---|---|
| macOS | Nothing |
| Linux | A udev rule for the bootloader (0483:DF11), which rfoundry prints when access is denied; for the radio's serial port, membership of the `dialout` group (`sudo usermod -aG dialout $USER`, then log in again) |
| Windows | Bind the bootloader ("Digital Radio in USB mode", 0483:DF11) to WinUSB with Zadig, once ([steps](../../docs/user/02-installing.md#windows-the-bootloader-driver)); rfoundry prints them when it can't open the bootloader. The radio's serial port needs no driver |

## For developers

| Module | Does |
|---|---|
| `rfoundry/boards.py` | per radio, read from `registry.json` (generated by `tools/boards.py` from `boards/*/board.toml`): flash sectors, application base, codec windows and profile, bootloader IDs, key hash |
| `rfoundry/plan.py` | which sectors to erase and write for an image and a codec mode |
| `rfoundry/manifest.py` | release manifests: generate (used by `tools/release.py`) and verify |
| `rfoundry/stockfw.py` | the STM32 radios' stock files: container, decryption, codec extraction and merge (also from a flash readout), key derivation (also used by `tools/codec_extract.py`) |
| `rfoundry/dfu.py` | the AnyRoad DFU bootloader: find, open, run a plan |
| `rfoundry/debuglink.py` | VERSION / INFO_CHIP / READ_MEM over the debug link (read only) |
| `rfoundry/codecsave.py` | the codec read back from a radio that runs RadioFoundry (where stock keeps it, or where the merge placed it), checked by hash, saved in the config directory and written back where the radio's codec can't be kept or checked |
| `rfoundry/bootkey.py` | the bootloader key from a radio that runs RadioFoundry, found in its bootloader by the key's hash |
| `rfoundry/fakedfu.py` | a fake bootloader with a flash model, for the tests and `--simulate` |
| `rfoundry/serialflash.py` | the HR_C7000 radios over the programming cable: the route choice, the codec slot, and the calls into `stub/stubtool.py` and `tools/serial_loader.py` |
| `rfoundry/release.py`, `state.py`, `cli.py` | GitHub downloads, the remembered probe, the commands |

### Tests

The host tests cover the planning, the manifests, the codec library, key derivation, the codec decision, and complete
runs against a fake bootloader with a flash model. They need no USB or serial port, and `tools/ci.sh` runs them:

```
python3 -m unittest discover -s tools/rfoundry/tests -v
```

`test_serialflash.py` runs both serial routes against the wrapped tools' own simulated radios. To run a whole flash
against the fake bootloader, use `rfoundry flash IMAGE --simulate rfoundry-codec` (or `stock` with
`--stock-installed "V02.06 CSV"`, or `rfoundry-nocodec --stock synthetic`). To flash a local build without a manifest,
use `rfoundry flash build/<board>/rfoundry_<board>.bin --board <board>`.

### Testing rfoundry on a radio

Some answers only a radio can give. Check them in this order; the first flash of a new build is always a merge, so the
radio has a known codec:

1. Run `rfoundry detect` with the radio in its bootloader.
2. Merge: run `rfoundry flash <image> --stock <file> --dry-run`, then the same command without `--dry-run`. The radio
   restarts, and rfoundry reports the codec profile and "DMR voice is ready".
3. Keep, which proves the bootloader erases only the listed sectors: with RadioFoundry running, run `rfoundry probe`,
   then `rfoundry flash <image>` without `--stock`. The plan must erase only the image's sectors and keep the codec's.
   After the restart, rfoundry must report the codec still present. If it reports `absent`, the bootloader erased more
   than asked: turn keep off for that board (`keep_codec_supported=False`), and step 2 repairs the radio.
4. Make a DMR call both ways after step 3, which shows that the codec works, not only its CRC.

For the codec design and the sector maps, see [The AMBE codec](../../docs/dev/codec.md). For releases, see
[Releases](../../docs/dev/release.md).
