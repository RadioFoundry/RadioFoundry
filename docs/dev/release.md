<!-- SPDX-License-Identifier: MIT -->

# Releases

A release is a git tag on main named for its year and month (`v26.11`, below) and a GitHub release that carries,
for each releasable radio, the firmware image **without the AMBE codec**, its manifest, `SHA256SUMS`, the `rfoundry`
flashing tool as single-file executables for Linux, macOS and Windows, and the user manual as a PDF. Users never
build firmware: `rfoundry` downloads the image, checks it, and either keeps the codec already in the radio or writes
the codec from the user's own stock firmware file ([tools/rfoundry/README.md](../../tools/rfoundry/README.md)).

## Release names

A release is named for the year and month it comes out, so a user sees at a glance how old the firmware is.
`tools/release_version.py` holds the rule; the Makefile, `tools/release_build.sh`, the release workflow and the site
all ask it.

| Name | When |
|---|---|
| `vYY.M` | The first release of a month: `v26.11` |
| `vYY.M.N` | Another release that month: `v26.11.1`, `v26.11.2` |
| `vYY.MbN` | A pre-release (beta) of that name: `v26.12b1`. The final keeps the name even if it ships the next month |

The year has two digits (`26` is 2026) and a beta is a bare `b` and its count, to save room on the radios' small
displays. The month has no leading zero and the count starts at 1, so `v26.11.3` never reads as March. Main is the
alpha; there is no release candidate yet. `tools/release_build.sh` and the release workflow refuse any other tag.

**On the radio.** A clean build of a commit that carries a release tag gets `RFOUNDRY_RELEASE` (the Makefile reads the
tag; `make RELEASE=<tag>` sets it, as `tools/release_build.sh` does, and `RELEASE=` leaves it out). The splash screen
shows the release name in place of the commit, About shows both, and the debug link's VERSION adds `release=`
([debuglink.md](../spec/debuglink.md) 3.1). `RFOUNDRY_GIT` stays the commit everywhere else: the crash record's build
id, the manifests and the CPS id need it. The build service fetches tags, so a custom build of a tagged commit shows
the release name too.

**Betas.** A tag ending `bN` becomes a GitHub pre-release. GitHub's latest release skips pre-releases, and so does
the site: the flasher installs the newest release that is not a pre-release by default, and offers a newer beta only as
an opt-in version link (`?version=<tag>`, [website.md](website.md#flasher-notices)). `?version=main` still builds main.

**On the site.** The footer shows the firmware release the flasher installs, linked to its GitHub release, and a
Changelog link to the GitHub releases. The release notes start from GitHub's generated notes (`--generate-notes`):
edit them into a short list of what changed for users.

## Checklist

1. Main is green: `JOBS=8 tools/ci.sh` ends with "all checks passed" ([ci.md](ci.md)), on a machine with both compilers and `reuse`.
2. The project lead runs the similarity scan against the reference trees and reviews every hit
   ([docs/testing/README.md](../testing/README.md) 1).
3. A comprehensive legal scan of what the release ships, reviewed by the project lead: the license and notices of
   every file and dependency (`reuse lint`, `third_party/`, the rfoundry executables' bundled packages), the names
   and marks used in the docs, the site and the images, and nothing in the tree, the release or the site that the
   clean-room rules ([CLEANROOM.md](../CLEANROOM.md)) or "Never commit" ([AGENTS.md](../../AGENTS.md)) forbid.
4. The HIL tests pass on each radio the release claims, with the flags its features need.
5. rfoundry has flashed the release candidate on each radio the release claims, in both codec modes (below, "Testing
   rfoundry on a radio").
6. The user manual matches the release: the status of each setting in [Settings](../user/17-settings.md), the
   supported radios in the [Introduction](../user/01-introduction.md), the screenshots (`make -C tests/host ui-gallery`).
7. The README's radio status table is current.
8. Build the manual locally and read it through ([manual-pdf.md](manual-pdf.md)).
9. Tag and push the tag:

   ```
   git tag -a v26.11 -m "RadioFoundry 26.11"
   git push origin v26.11
   ```

10. The release workflow builds the firmware and rfoundry and attaches them; the manual workflow attaches the PDF
    ([manual-pdf.md](manual-pdf.md)). Edit the release notes on GitHub (they are the changelog the site links).
11. The next push to main redeploys the site, which then installs the new release (or offers the new beta).

## Release artifacts

| File | What |
|---|---|
| `rfoundry-<board>-<version>.bin` | The raw image `make BOARD=<board>` built, linked for the board's application base. No codec. A board with a first stage releases `rfoundry_<board>_install.bin`, the first stage then the application; `tools/release.py` refuses the application alone |
| `rfoundry-<board>-<version>.json` | Its manifest (below) |
| `rfoundry-<version>-<os>-<arch>[.exe]` | rfoundry, one file per system: `linux-x86_64`, `macos-arm64`, `macos-x86_64`, `windows-x86_64` |
| `SHA256SUMS` | SHA-256 of every file above |
| `RadioFoundry-manual-<version>.pdf` | The user manual (the manual workflow) |

`tools/release.py --board <id> --version v26.11` writes the first two (with `--variant amateur`, the image of `make
VARIANT=amateur` as `rfoundry-<board>-amateur-<version>.bin`, its manifest naming the variant: [build-config.md](../build-config.md#variants)) and the sums for a built image into `dist/`
(or `--out DIR`). It refuses an image that reaches into a codec sector or carries the codec's signature word, so a
release can never contain the codec, even by accident.

The manifest (`format` `rfoundry-release/1`, written and checked by `tools/rfoundry/rfoundry/manifest.py`):

```json
{
  "format": "rfoundry-release/1",
  "board": "<id>", "board_name": "<name>",
  "version": "v26.11", "git": "5f034440", "git_full": "5f03444001b77a00ebaef28d3358dd902c03474b",
  "image": {"file": "rfoundry-<id>-v26.11.bin", "size": 220780, "sha256": "dc8b8ed7..."},
  "ranges": [{"addr": "0x0800C000", "size": 220780}],
  "sectors": [3, 4, 5, 6],
  "codec": {"included": false, "profile": "<id>-<codec>-s11", "sectors": [10, 11],
            "windows": [{"addr": "0x080E3A74", "size": 66076}, {"addr": "0x080D19F0", "size": 73858},
                        {"addr": "0x080FFF80", "size": 128}],
            "keep_supported": true},
  "features": "FM,SUBTONE,DMR,...",
  "toolchain": "arm-none-eabi-gcc (Arm GNU Toolchain 13.3.Rel1 (Build arm-13.24)) 13.3.1 20240614",
  "variant": "full", "git_time": 1761840000
}
```

`git_time` is the commit's time, which the codeplug editor compares with a radio's `git_time=` (debug link VERSION) to
offer the release to a build that isn't one. Manifests from before it have none; the site's build then looks the
commit up in its clone. `ranges` says where the `.bin` goes: one range, the application at the board's base. `sectors` are the flash sectors
the image covers. `codec.windows` are where the merge puts the codec: on an STM32F4 radio the code, the tables and the
descriptor ([codec-placement.md](../spec/codec-placement.md), "STM32 radios"). `codec.profile` is the id the firmware
reports for a matching codec (`codec=` in the debug link's VERSION, [debuglink.md](../spec/debuglink.md) 3.1).

## Sector maps

Each board's map comes from the board registry. The application region and the codec symbols come from its linker
script. The sectors are the platform's default geometry (`default_sectors` in `platform.toml`) from the application's
start up, or `app+slot` on the HR_C7000; a board's `[flash] sectors` overrides that. The protected range is the flash
from the platform's `flash_base` to the application. `tools/boards.py json --for rfoundry` writes them to
`tools/rfoundry/rfoundry/registry.json`, which rfoundry, `tools/release.py` and the stub tools read; `tools/ci.sh`
fails when it is out of date. The STM32 codec windows come from the codec profile (`stockfw.PROFILES`), which rfoundry reads when it loads the
registry.

A flash sector can only be erased whole, so each board's linker script keeps the application out of the sectors that
hold the codec. Whether an update can leave the codec alone (keep) depends on that map:

| Family | Application | Codec | Keep |
|---|---|---|---|
| STM32F4 | The sectors from the application's start to the codec sectors | The tables stay at their stock address, the code moves into the last sectors, a descriptor says where ([codec-placement.md](../spec/codec-placement.md) "STM32 radios") | Yes: the update writes only the application's sectors |
| HR_C7000 | From the application base to the slot | A slot at the end of the region ([codec-placement.md](../spec/codec-placement.md) "HR_C7000 radios") | Per route: through the stub yes (it writes only the image's sectors), through an updater that erases the whole area no |

The exact map of a radio is in its hardware spec (`hardware-<id>.md`) and in `tools/rfoundry/rfoundry/registry.json`.

**What an interrupted update leaves.** All erases come first, then the writes in address order, so sector 3 (the vector
table) is the first one written back: an update cut off before that leaves the bootloader in DFU. Cut off later, the
radio may boot half an application: a crash, never a brick. Enter the bootloader by hand (PTT and SK1 at power on)
and flash again.

## Releases from before the codec moved

Releases made before the codec moved on an STM32F4 radio carried a second range for read-only data and named the old
codec profile (without the `-s11` suffix). rfoundry and the web flasher refuse a manifest with two ranges or with an
old profile, and `tools/stm32_dfu.py` refuses a release image whose manifest lists two ranges.

## Codec handling

rfoundry picks one of two modes; `--dry-run` prints the exact sectors either would erase and write.

**Keep (the default when it is known to be safe).** Erase and write only the sectors the image covers; never touch the codec sectors, so the codec already in the radio stays. Flash can't be read back
over the bootloader (the radio is at RDP level 1), so rfoundry needs another way to know the codec is there:

1. **RadioFoundry is running.** Before the user switches the radio into its bootloader, rfoundry asks over the debug link
   (`rfoundry probe`, or automatically at the start of `rfoundry flash`). VERSION's `codec=` is the firmware's own check,
   the same one that gates DMR voice (on an STM32F4 radio: the descriptor, the signature word and the CRC-32 of both
   windows at their placed addresses). If it reports the board's codec profile (ending in `-s11`), keep is safe.
   The answer is remembered for 30 minutes (rfoundry's settings directory), because the debug link is gone once the
   radio is in its bootloader. A build from before the codec moved reports the profile without `-s11`: its codec code is in
   sectors the update overwrites, so keep is not safe.
2. **Otherwise the codec is written (merge),** from the copy rfoundry saved from this radio if it has one, else from
   the stock file. A vendor firmware doesn't hold the codec where RadioFoundry keeps it, so `--stock-installed` never makes keep safe.
3. `--codec keep` forces keep in every case. Where the radio is known to have no codec, the keep expects none, so the
   check after flashing doesn't count `codec=absent` as a failure: the radio reported no codec, or the user says it runs
   its stock firmware (`--stock-installed`, or the web flasher's "Original firmware" with or without a version) and no
   stock version of the board holds the codec where RadioFoundry keeps it (the board's `codec.installed` is empty).
   rfoundry and the web flasher (`site/static/js/flasher/decide.js`) apply the same rules.

**The saved copy.** Whenever rfoundry or the web flasher sees a radio running RadioFoundry, it reads the codec over the debug
link (READ_MEM, in the stock or the placed layout), checks it against the profile's hashes and saves it (rfoundry's
config directory, or the browser). It uses that copy instead of the stock file whenever the radio's codec can't be
kept or checked: no answer over the debug link, the radio back on stock firmware, or no codec reported
(`rfoundry/codecsave.py`, `site/static/js/flasher/codecsave.js`). There is no read-back during the update itself, so
a radio on a build from before the move needs the stock file once unless a copy was saved earlier.

Why a wrong guess is harmless: the firmware never runs the codec without checking it first (descriptor, signature and both CRCs, `boards/<id>/vocoder_<id>.c`). If the bytes in sectors 10 and 11 aren't the expected codec, `present()` is false: FM
works, DMR calls show "No codec: voice off", and the debug link reports `codec=absent`. The fix is to flash again with
`--stock`. rfoundry checks this itself after flashing: it waits for the radio to restart and reads `codec=` again.

**Merge.** The user supplies the stock firmware file (`--stock`). rfoundry checks it by SHA-256 against the codec
profile, decrypts the two windows, checks their SHA-256 and the signature (`rfoundry/stockfw.py`, the same library
`tools/codec_extract.py` uses), places them as the board's codec map says and writes the descriptor
([codec-placement.md](../spec/codec-placement.md), "STM32 radios"). The image's sectors and the codec sectors are erased and written. The merged codec exists only in memory on the user's machine and in the radio.

**Getting the stock file.** rfoundry only reads a file the user downloaded; it doesn't fetch vendor firmware.

## The bootloader key

An STM32F4 radio's vendor bootloader XORs what it receives with a 1024-byte per-model key. The key is not in this
repository and not in a release. rfoundry and `tools/codec_extract.py` get it, in order:

1. `--key FILE`;
2. derived from the user's stock firmware file (`--stock`, or once with `rfoundry key --stock FILE --save`) and checked
   against the published key hash, so a wrong key can't pass (`stockfw.derive_key`);
3. a key saved earlier with `--save` (rfoundry's settings directory);
4. read from a radio that runs RadioFoundry, over the debug link (`rfoundry/bootkey.py`; the web flasher's `bootkey.js`
   does the same).

So a user needs one file from the vendor's firmware for the first install, which they need anyway for the codec, and
never handles the key. Updates of a radio that runs RadioFoundry need no stock file.

## The release workflow

`.github/workflows/release.yml`:

| Job | Does |
|---|---|
| `firmware (<toolchain>)` | One job per toolchain: `firmware (Arm)` and `firmware (C-SKY)`. Sets up the pinned compiler with `.github/actions/toolchain` ([ci.md](ci.md#toolchains)): the self-hosted image's, the Arm GNU Toolchain checked by SHA-256, or the csky-elf image, never a source build. Without csky-elf GCC on the runner and without the image, the C-SKY job fails. On a tag runs the gate: `tools/ci.sh --quick --steps registry,host,python,licenses` on the runner, then `tools/ci.sh --quick --steps firmware --toolchain <id>` with the toolchain's compiler (through `tools/docker/csky-toolchain/run.sh` for the image). Then `tools/release_build.sh --toolchain <id>`, the same way: for every board on the toolchain with `[status] release = "stable"` (`tools/boards.py list --toolchain <id> --release stable`), it builds the default image and each build option value's image (`tools/boards.py builds`) and runs `tools/release.py` on each. Uploads them as `firmware-arm` or `firmware-csky` |
| `rfoundry` | On Ubuntu 22.04, macOS 14 (arm64), macOS 13 (x86_64) and Windows 2022: installs the package, runs its tests, builds a single-file executable with PyInstaller 6.11.1 (libusb from the `libusb-package` wheel, the `certifi` CA bundle, and `serial_loader`, `hrc7000_codec` and `stubtool` for the serial route inside) and runs `--version` on it |
| `publish` | On a `v*` tag only, after every toolchain's job and every rfoundry build passed: one `SHA256SUMS` over everything, checks every manifest against its image and that none includes the codec, creates the release if the manual workflow hasn't, uploads with `--clobber` |

On a pull request nothing is published and the release gate doesn't run (the pull request's own checks run the same
tests):

| What the pull request changes | What builds |
|---|---|
| `tools/rfoundry/` (with the `registry.json` a `board.toml` change regenerates), the serial flashing tools, `stub/stubtool.py` | `release.yml`: `version` and `rfoundry`, as artifacts |
| `release.yml` or the toolchain action | `release.yml`: everything, as artifacts |
| A `board.toml`, `board.mk` or `platform.toml`, `tools/release.py`, `tools/release_build.sh`, `tools/boards.py`, the toolchain scripts | `pr-tests.yml`: its `firmware (<toolchain>)` jobs run `tools/release_build.sh` after their builds, so every stable board is packaged as a tag would package it |
 A manual run on a tag rebuilds and
replaces the assets.

The executables are not code-signed. macOS marks downloads as quarantined: `xattr -d com.apple.quarantine
rfoundry-*` (or right-click, Open). Windows SmartScreen asks once.

## The serial route (HR_C7000 radios)

Radios on the serial-loader and serial-iap families are flashed over the programming cable, not DFU. rfoundry
(`rfoundry/serialflash.py`, [tools/rfoundry/README.md](../../tools/rfoundry/README.md)) doesn't speak either protocol
itself; it calls the tools that do:

| Route | Tool | Chosen by `--route auto` when | Codec |
|---|---|---|---|
| stub | `stub/stubtool.py` (`Radio.rfoundry_enter`, `write_image`, `reset_chip`) | VERSION answers on the cable with this board and the debug link's STUB info (op 0, [debuglink.md](../spec/debuglink.md) 3.18) carries its magic | keep: only the image's sectors are written, so the slot stays; merge when the image carries a slot or `--stock` builds one |
| bootloader | `tools/serial_loader.py` `Flasher` | otherwise | merge only where the loader erases the slot, so a plain image without `--stock` is refused |

`--stock` is a program flash read of the radio or the vendor's firmware file, depending on the board;
`tools/hrc7000_codec.py` recognizes it by the code window's SHA-256 and builds the slot. The release package never
carries the codec, so the first install of a release through update mode needs it. Later updates through the stub keep
the slot. Each radio's route, slot and keep rule are in its hardware spec (`hardware-<id>.md`,
[codec-placement.md](../spec/codec-placement.md) "HR_C7000 radios"), and the open items in its bring-up record under
[docs/testing/](../testing/README.md). Host tests (`tools/rfoundry/tests/test_serialflash.py`) run `write_image` against
stubtool's simulated radio and the update mode protocol against serial_loader's simulated loader.

## Testing rfoundry on a radio

Host tests cover the planning, manifests, the codec library, key derivation, the codec decision and complete runs
against a fake bootloader with a flash model (`python3 -m unittest discover -s tools/rfoundry/tests`; `tools/ci.sh` runs
them). `rfoundry flash IMAGE --simulate rfoundry-codec` runs the whole command against that fake. What only a radio can
answer, in this order (the first flash of a new build is always a merge, so the radio has a known codec):

1. `rfoundry detect` with the radio in its bootloader.
2. Merge: `rfoundry flash <image> --stock <stock file> --dry-run`, then without `--dry-run`. The radio restarts;
   rfoundry reports the codec profile and "DMR voice is ready".
3. Keep, which proves the bootloader erases only the listed sectors: with RadioFoundry running, `rfoundry probe`, then
   `rfoundry flash <image>` without `--stock`. The plan must erase only the image's sectors and keep the codec's. After
   the restart rfoundry must report the codec still present. If it reports `absent`, the bootloader erased more than
   asked: keep must be disabled for that board (`keep_codec_supported=False`) and step 2 repairs the radio.
4. A DMR call both ways after step 3 (the codec works, not only its CRC).
