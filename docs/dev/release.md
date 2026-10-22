<!-- SPDX-License-Identifier: MIT -->

# Releases

A release is a git tag `vMAJOR.MINOR.PATCH` on main and a GitHub release that carries, for each releasable radio, the
firmware image **without the AMBE codec**, its manifest, `SHA256SUMS`, the `odrflash` flashing tool as single-file
executables for Linux, macOS and Windows, and the user manual as a PDF. Users never build firmware: `odrflash`
downloads the image, checks it, and either keeps the codec already in the radio or writes the codec from the user's
own stock firmware file ([tools/odrflash/README.md](../../tools/odrflash/README.md)).

## Checklist

1. Main is green: `JOBS=8 tools/ci.sh` ends with "all checks passed", on a machine with both compilers and `reuse`.
2. The project lead runs the similarity scan against the reference trees and reviews every hit:
   `ODR_SIMILARITY_REFS="..." tools/ci.sh` ([docs/testing/README.md](../testing/README.md) 1).
3. A comprehensive legal scan of what the release ships, reviewed by the project lead: the license and notices of
   every file and dependency (`reuse lint`, `third_party/`, the odrflash executables' bundled packages), the names
   and marks used in the docs, the site and the images, and nothing in the tree, the release or the site that the
   clean-room rules ([CLEANROOM.md](../CLEANROOM.md)) or "Never commit" ([AGENTS.md](../../AGENTS.md)) forbid.
4. The HIL tests pass on each radio the release claims, with the flags its features need.
5. odrflash has flashed the release candidate on each radio the release claims, in both codec modes (below, "Testing
   odrflash on a radio").
6. The user manual matches the release: the status of each setting in [Settings](../user/12-settings.md), the
   supported radios in the [Introduction](../user/01-introduction.md), the screenshots (`make -C tests/host ui-gallery`).
7. The README's radio status table is current.
8. Build the manual locally and read it through: `tools/build_manual.sh`.
9. Tag and push the tag:

   ```
   git tag -a v0.1.0 -m "OpenDigitalRadio 0.1.0"
   git push origin v0.1.0
   ```

10. The release workflow builds the firmware and odrflash and attaches them; the manual workflow attaches the PDF
    (below). Edit the release notes on GitHub.

## Future improvements

Not needed for a release; noted so they aren't lost.

- **Flash size per selection in the web flasher.** The feature picker should estimate the image size of the chosen
  feature combination on the chosen radio and warn when it may not fit. CI's full variant and feature-combination
  builds can write the sizes to a JSON file (per radio: a base size and each switch's cost, or the measured
  combinations) that the site publishes next to `features.json`.

## Release artifacts

| File | What |
|---|---|
| `odr-<board>-<version>.bin` | The raw image `make BOARD=<board>` built, linked for the board's application base. No codec. A board with a first stage (the DB25-D) releases `odr_<board>_install.bin`, the stage then the application; `tools/release.py` refuses the application alone |
| `odr-<board>-<version>.json` | Its manifest (below) |
| `odrflash-<version>-<os>-<arch>[.exe]` | odrflash, one file per system: `linux-x86_64`, `macos-arm64`, `macos-x86_64`, `windows-x86_64` |
| `SHA256SUMS` | SHA-256 of every file above |
| `OpenDigitalRadio-manual-<version>.pdf` | The user manual (the manual workflow) |

`tools/release.py --board dm1701 --version v0.1.0` writes the first two (with `--variant amateur`, the image of `make
VARIANT=amateur` as `odr-dm1701-amateur-<version>.bin`, its manifest naming the variant: [build-config.md](../build-config.md#variants)) and the sums for a built image into `dist/`
(or `--out DIR`). It refuses an image that reaches into a codec sector or carries the codec's signature word, so a
release can never contain the codec, even by accident.

The manifest (`format` `odr-release/1`, written and checked by `tools/odrflash/odrflash/manifest.py`):

```json
{
  "format": "odr-release/1",
  "board": "dm1701", "board_name": "Baofeng DM-1701",
  "version": "v0.1.0", "git": "5f034440", "git_full": "5f03444001b77a00ebaef28d3358dd902c03474b",
  "image": {"file": "odr-dm1701-v0.1.0.bin", "size": 220780, "sha256": "dc8b8ed7..."},
  "ranges": [{"addr": "0x0800C000", "size": 220780}],
  "sectors": [3, 4, 5, 6],
  "codec": {"included": false, "profile": "dm1701-v0206-csv-s11", "sectors": [10, 11],
            "windows": [{"addr": "0x080E3A74", "size": 66076}, {"addr": "0x080D19F0", "size": 73858},
                        {"addr": "0x080FFF80", "size": 128}],
            "keep_supported": true},
  "features": "FM,SUBTONE,DMR,...",
  "toolchain": "arm-none-eabi-gcc (Arm GNU Toolchain 13.3.Rel1 (Build arm-13.24)) 13.3.1 20240614"
}
```

`ranges` says where the `.bin` goes: one range, the application at the board's base. `sectors` are the flash sectors
the image covers. `codec.windows` are where the merge puts the codec: on the DM-1701 the code, the tables and the
descriptor ([codec-placement.md](../spec/codec-placement.md), "STM32 radios"). `codec.profile` is the id the firmware
reports for a matching codec (`codec=` in the debug link's VERSION, [debuglink.md](../spec/debuglink.md) 3.1).

## Sector maps

Each board's map comes from the board registry: the application region and the codec symbols from its linker script,
the sectors from `[flash] sectors` in its `board.toml` (a platform geometry, or `app+slot`), and the STM32 codec
windows from the stock file's placement (`stockfw.PROFILES`). `tools/boards.py json --for odrflash` writes them to
`tools/odrflash/odrflash/registry.json`, which odrflash, `tools/release.py` and the stub tools read; `tools/ci.sh`
fails when it is out of date.

A flash sector can only be erased whole, so each board's linker script keeps the application out of the sectors that
hold the codec. Whether an update can leave the codec alone (keep) depends on that map:

| Board | Application | Codec | Keep | Map |
|---|---|---|---|---|
| dm1701 | sectors 3-9 (720 KB) | sectors 10 and 11: tables at their stock address, code moved to 0x080E3A74, descriptor | yes | [hardware-dm1701.md](../spec/hardware-dm1701.md) 1.4 |
| md380 | sectors 3-7 (464 KB) | sectors 8, 10 and 11: main tables at their stock address in sector 8, code moved to 0x080E0000, descriptor; sector 9 unused | yes | [hardware-md380.md](../spec/hardware-md380.md) 1.3 |
| dm32 | 0x0300C000..0x030E8000 (880 KB) | slot 0x030E8000..0x03100000 | no: the updater erases the whole area | [hardware-dm32.md](../spec/hardware-dm32.md) 17 |
| dm1801pro | 0x0300C000..0x030E8000 (880 KB) | slot 0x030E8000..0x03100000 | through the stub yes (it writes only the image's sectors); through update mode no (the updater erases the whole area) | [hardware-dm1801pro.md](../spec/hardware-dm1801pro.md) 6.2, [codec-dm1801pro.md](../spec/codec-dm1801pro.md) |

**What an interrupted update leaves.** All erases come first, then the writes in address order, so sector 3 (the vector
table) is the first one written back: an update cut off before that leaves the bootloader in DFU. Cut off later, the
radio may boot half an application: a crash, never a brick. Enter the bootloader by hand (PTT and SK1 at power on)
and flash again.

## Releases from before the codec moved

On the DM-1701, sector 9 is part of the application area, like sectors 3-8. Releases made before the codec moved
linked the read-only data into sector 9 and carried it as a second range, with the profile `dm1701-v0206-csv`
(their codec code sat in sectors 7 and 8). odrflash and the web flasher refuse a manifest with two ranges or with
that profile, and `tools/stm32_dfu.py` refuses a release image whose manifest lists two ranges.

## Codec handling

odrflash picks one of two modes; `--dry-run` prints the exact sectors either would erase and write.

**Keep (the default when it is known to be safe).** Erase and write only the sectors the image covers (DM-1701: within
3-9); never touch the codec sectors (10 and 11), so the codec already in the radio stays. Flash can't be read back
over the bootloader (the radio is at RDP level 1), so odrflash needs another way to know the codec is there:

1. **ODR is running.** Before the user switches the radio into its bootloader, odrflash asks over the debug link
   (`odrflash probe`, or automatically at the start of `odrflash flash`). VERSION's `codec=` is the firmware's own check,
   the same one that gates DMR voice (on the DM-1701: the descriptor, the signature word and the CRC-32 of both
   windows at their placed addresses). If it reports the board's codec profile (`dm1701-v0206-csv-s11`), keep is safe.
   The answer is remembered for 30 minutes (odrflash's settings directory), because the debug link is gone once the
   radio is in its bootloader. A build from before the codec moved reports `dm1701-v0206-csv`: its codec code is in
   sectors 7 and 8, which the update overwrites, so keep is not safe.
2. **Otherwise the codec is written (merge),** from the copy odrflash saved from this radio if it has one, else from
   the stock file. No stock firmware holds the codec where ODR keeps it, so `--stock-installed` never makes keep safe.
3. `--codec keep` forces keep in every case.

**The saved copy.** Whenever odrflash or the web flasher sees a radio running ODR, it reads the codec over the debug
link (READ_MEM, in the stock or the placed layout), checks it against the profile's hashes and saves it (odrflash's
config directory, or the browser). It uses that copy instead of the stock file whenever the radio's codec can't be
kept or checked: no answer over the debug link, the radio back on stock firmware, or no codec reported
(`odrflash/codecsave.py`, `site/static/js/flasher/codecsave.js`). There is no read-back during the update itself, so
a radio on a build from before the move needs the stock file once unless a copy was saved earlier.

Why a wrong guess is harmless: the firmware never runs the codec without checking it first (descriptor, signature and both CRCs,
`boards/dm1701/vocoder_dm1701.c`). If the bytes in sectors 10 and 11 aren't the expected codec, `present()` is false: FM
works, DMR calls show "No codec: voice off", and the debug link reports `codec=absent`. The fix is to flash again with
`--stock`. odrflash checks this itself after flashing: it waits for the radio to restart and reads `codec=` again.

**Merge.** The user supplies the stock firmware file (`--stock`). odrflash checks it by SHA-256 against the codec
profile, decrypts the two windows, checks their SHA-256 and the signature (`odrflash/stockfw.py`, the same library
`tools/codec_extract.py` uses), places them as the board's codec map says and writes the descriptor
([codec-placement.md](../spec/codec-placement.md), "STM32 radios"). The image's sectors and the codec sectors (DM-1701:
10 and 11) are erased and written. The merged codec exists only in memory on the user's machine and in the radio.

**Downloading the stock file for the user (not implemented).** The supported V02.06 CSV file is not on a Baofeng or
dealer URL; the known copy is a third-party archive (BG7QVB's GitHub repository). Radioddity's public bucket has
V02.01-V02.03, whose codec sits at other addresses (a second codec profile would be needed), and neither source states
a license for redistribution or automated download. odrflash therefore only reads a file the user downloaded;
changing that is an owner decision.

## The bootloader key

The AnyRoad bootloader XORs what it receives with a 1024-byte per-model key ([hardware-dm1701.md](../spec/hardware-dm1701.md)
15.3). The key is not in this repository and not in a release. odrflash and `tools/codec_extract.py` get it, in order:

1. `--key FILE`;
2. derived from the user's stock firmware file (`--stock`, or once with `odrflash key --stock FILE --save`). The stock
   file's payload is plain XOR key[i mod 1024], and in a firmware image the most common byte in every key column is
   0x00, so the most common cipher byte of each column is the key byte; the few columns where that vote is close are
   settled by trying the runner-up bytes until the result's SHA-256 matches the published key hash. Nothing but the
   hash is needed, and a wrong key can't pass. It works on every DM-1701 stock file listed in codec-dm1701.md
   (V02.01-V02.06, CSV and noCSV) in well under a second (`stockfw.derive_key`);
3. a key saved earlier with `--save` (odrflash's settings directory);
4. read from a radio that runs ODR: the stock bootloader holds the key as one plain block, and odrflash reads the
   bootloader over the debug link and takes the block with the key's hash (`odrflash/bootkey.py`; the web flasher's
   `bootkey.js` does the same).

So a user needs one file from Baofeng's firmware for the first install, which they need anyway for the codec, and
never handles the key. Updates of a radio that runs ODR need no stock file.

## The release workflow

`.github/workflows/release.yml`:

| Job | Does |
|---|---|
| `firmware-dm1701` | Installs Arm GNU Toolchain 13.3.rel1 (checked by SHA-256) into `~/opt`, runs `tools/ci.sh --quick --board dm1701` as the gate, builds, runs `tools/release.py` |
| `firmware-dm32` | Only when the repository variable `ODR_RELEASE_DM32` is `true` (below) |
| `firmware-dm1801pro` | Only when the repository variable `ODR_RELEASE_DM1801PRO` is `true` (below, "The DM-1801 Pro"): the DM-32UV job's toolchain and cache, then `make BOARD=dm1801pro` and `tools/release.py --board dm1801pro` |
| `odrflash` | On Ubuntu 22.04, macOS 14 (arm64), macOS 13 (x86_64) and Windows 2022: installs the package, runs its tests, builds a single-file executable with PyInstaller 6.11.1 (libusb from the `libusb-package` wheel, the `certifi` CA bundle, and `serial_loader`, `hrc7000_codec` and `stubtool` for the serial route inside) and runs `--version` on it |
| `publish` | On a `v*` tag only: one `SHA256SUMS` over everything, checks every manifest against its image and that none includes the codec, creates the release if the manual workflow hasn't, uploads with `--clobber` |

Pull requests that touch `tools/odrflash/`, `tools/release.py`, the DM-32 flashing tools or the workflow build
everything as workflow artifacts and publish nothing, without the release gate (the pull request's own checks run the
same tests), and odrflash for Linux only when the self-hosted runner is set up. A manual run on a tag rebuilds and
replaces the assets.

The executables are not code-signed. macOS marks downloads as quarantined: `xattr -d com.apple.quarantine
odrflash-*` (or right-click, Open). Windows SmartScreen asks once.

## The MD-380

Not released yet: `[status] release = "testing"` in `boards/md380/board.toml` and no job in `release.yml` until
[docs/testing/md380-bringup.md](../testing/md380-bringup.md) passes. The application has sectors 3-7 and the codec
sectors 8, 10 and 11, so an update can keep the codec (`keep_codec_supported=True`), as on the DM-1701. The codec
profile is `md380-d2032-s11` (a build from before the move reports `md380-d2032`). The moved code has run only in an
emulator, not on a radio. `tools/md380_migrate.py` flashes it today and keeps the codeplug
([md380-migration.md](../spec/md380-migration.md)).

## Adding the DM-32UV

The DM-32UV runs on a radio but is still in bring-up. The workflow job is there and off. To turn it
on once [docs/testing/dm32-bringup.md](../testing/dm32-bringup.md) passes:

1. Set the repository variable `ODR_RELEASE_DM32=true` (Settings, Secrets and variables, Actions, Variables). The job
   builds the csky-elf toolchain with `tools/build-csky-toolchain.sh` (binutils 2.43.1, GCC 14.2.0, newlib
   4.5.0.20241231, pinned in the script; about 45 minutes on four cores) and caches `~/toolchains/csky-elf` keyed by the
   script's hash, so later releases restore it in seconds. A prebuilt toolchain archive attached to a tools release
   would avoid the first build; that's a later choice.
2. Set `[status] release = "stable"` in `boards/dm32/board.toml` and regenerate `tools/odrflash/odrflash/registry.json`.
3. Flashing: the DM-32UV uses Baofeng's serial updater protocol ([hardware-dm32.md](../spec/hardware-dm32.md) 13.2), not
   DFU. odrflash would get a second backend speaking it over pyserial (the `bfuv32-serial` family). The updater erases
   the whole application area before writing (step 7), the codec slot 0x030E8000..0x03100000 included, so **keep is not
   possible on the DM-32UV**: every flash merges the codec from the stock file (`tools/hrc7000_codec.py` logic as a
   library, like `stockfw` for the DM-1701). The manifest already says so (`keep_supported: false`), and odrflash's
   codec decision refuses keep for it. Whether the updater erases only `size` bytes (which would make a keep mode
   possible with an image that ends before the slot) is a hardware question for the bring-up.

## The DM-1801 Pro

In bring-up ([docs/testing/dm1801pro-bringup.md](../testing/dm1801pro-bringup.md)): `[status] release = "testing"` in its `board.toml`,
and the workflow job is off until the repository variable `ODR_RELEASE_DM1801PRO` is `true`.

**Package.** `tools/release.py --board dm1801pro --version v0.1.0` packs `build/dm1801pro/odr_dm1801pro.bin` like any
board: one range from 0x0300C000, `sectors` `[0]`, codec profile `dm1801pro-049` in "sector" 1 (the slot), not
included. The board has no second flash region. It refuses the build's `odr_dm1801pro_codec.bin` (the slot's `DDRC`
word at 0x030E8000) and any image that reaches the slot.

**The serial route.** odrflash flashes the package, a local build or a `_codec.bin` over the programming cable
(`odrflash/serialflash.py`, [tools/odrflash/README.md](../../tools/odrflash/README.md#the-dm-1801-pro)). It doesn't
speak either protocol itself; it calls the tools that do:

| Route | Tool | Chosen by `--route auto` when | Codec |
|---|---|---|---|
| stub | `stub/stubtool.py`: `Radio.odr_enter`, `write_image`, `reset_chip` (the `--odr-enter --write-app IMAGE --boot reset` run) | VERSION answers on the cable with this board and the debug link's STUB info (op 0, [debuglink.md](../spec/debuglink.md) 3.18) carries its magic | keep: only the image's sectors are written, so the slot stays; merge when the image carries a slot or `--stock` builds one |
| bootloader | `tools/serial_loader.py`: `Flasher` with model `dm1801pro` (ID `BF1801-V1`) | otherwise | merge only: the loader erases the slot ([hardware-dm1801pro.md](../spec/hardware-dm1801pro.md) 6.2), so a plain image without `--stock` is refused |

`--stock` is a program flash read of this radio; `tools/hrc7000_codec.py` recognizes it by the code window's SHA-256 and
builds the slot. The release package never carries the codec, so the first install of a release through update mode
needs that read (`stub/stubtool.py PORT --progmem FILE`, radio off). Later updates go through the stub and keep the
slot. The DM-32UV shares the family (`bfuv32-serial`) and so the route; its `--stock` is the stock firmware file.

Host tests (`tools/odrflash/tests/test_serialflash.py`) run `write_image` against stubtool's simulated radio and the
update mode protocol against serial_loader's simulated loader, and check the route and codec choices, the package and the
refusals. Not yet done on a radio: a whole odrflash run on either route.

## Testing odrflash on a radio

Host tests cover the planning, manifests, the codec library, key derivation, the codec decision and complete runs
against a fake bootloader with a flash model (`python3 -m unittest discover -s tools/odrflash/tests`; `tools/ci.sh` runs
them). `odrflash flash IMAGE --simulate odr-codec` runs the whole command against that fake. What only a radio can
answer, in this order (the first flash of a new build is always a merge, so the radio has a known codec):

1. `odrflash detect` with the radio in its bootloader: record the strings (they become the model fingerprint table).
2. Merge: `odrflash flash odr-dm1701-<v>.bin --stock "DM-1701(CSV)-V02.06.bin" --dry-run`, then without `--dry-run`. The
   radio restarts; odrflash reports `codec dm1701-v0206-csv-s11` and "DMR voice is ready".
3. Keep, which proves the bootloader erases only the listed sectors: with ODR running, `odrflash probe` (codec
   `dm1701-v0206-csv-s11`), then `odrflash flash odr-dm1701-<v>.bin` (no `--stock`; the plan shows the image's sectors
   from 3 to at most 9 erased and 10, 11 kept). After the restart odrflash must report the codec still present. If it reports `absent`, the
   bootloader erased more than asked: keep must be disabled for the DM-1701 (`keep_codec_supported=False`) and the
   radio is fixed by step 2.
4. A DMR call both ways after step 3 (the codec really works, not only its CRC).

## The user manual PDF

The manual is the Markdown in [docs/user/](../user/README.md). `tools/build_manual.sh` turns it into one PDF:

```
tools/build_manual.sh                         # build/manual/OpenDigitalRadio-manual-<git describe>.pdf
tools/build_manual.sh --version v0.1.0        # the version on the title page and in the file name
tools/build_manual.sh --out manual.pdf
tools/build_manual.sh --typ-only              # stop after the typst source, for debugging the template
```

| Part | Does |
|---|---|
| `tools/build_manual.sh` | Collects `docs/user/[0-9][0-9]-*.md` in order, runs pandoc, then typst |
| `tools/manual/manual.lua` | pandoc filter: turns `<img width>` into images, maps links between chapters to PDF links, turns links into the repository into web links (with `ODR_REPO_URL`) or plain text |
| `tools/manual/manual.typ` | typst template: title page, contents, numbered chapters, page footer, tables |

Chapter files are numbered (`01-introduction.md`, ...); a new chapter is a new numbered file plus a line in
[docs/user/README.md](../user/README.md). Images are `<img src="../ux/screens/160x120i5/NAME.png" width="200" alt="...">`,
which GitHub and the PDF both show. A missing image is left out of the PDF with a warning. The PDF goes to `build/`,
which git ignores: never commit it.

Set `ODR_REPO_URL` (for example `https://github.com/OWNER/OpenDigitalRadio/blob/v0.1.0`) to make links to files
outside the manual into web links. `$PANDOC` and `$TYPST` choose the binaries.

### Toolchain: pandoc and typst

| Option | Why not |
|---|---|
| pandoc with LaTeX | A TeX distribution is gigabytes to install in CI, slow, and its errors are hard to read |
| A headless browser (wkhtmltopdf, Chromium) | Heavy, poor page control (headers, page breaks, contents) |
| mdBook, MkDocs | Built for web sites; their PDF output goes through a browser anyway |
| pandoc with typst | Two single static binaries (about 30 MB each), seconds per build, page layout in one readable template, fonts bundled with typst |

pandoc reads GitHub Markdown, so the files stay readable on GitHub. typst lays out the pages. Versions in use:
pandoc 3.6, typst 0.13. Install them:

| System | Command |
|---|---|
| macOS | `brew install pandoc typst` |
| Debian, Ubuntu | `pandoc` from the [release page](https://github.com/jgm/pandoc/releases) (the distribution's is often too old), `typst` from its [release page](https://github.com/typst/typst/releases) |
| Any | Download both release archives, unpack, and point `$PANDOC` and `$TYPST` at the binaries |

## The manual workflow

`.github/workflows/manual.yml`, independent of the firmware build:

| Trigger | Does |
|---|---|
| Push of a `v*` tag | Checks the doc links, builds the PDF with the tag as its version, uploads it as a workflow artifact, creates the GitHub release if it doesn't exist and attaches the PDF |
| Pull request or push to main touching `docs/`, `README.md` or the manual tools | Checks the doc links and builds the PDF as an artifact; nothing is published |
| Manual run (`workflow_dispatch`) | On a `v*` tag, the same as a tag push; on a branch, the same as a pull request |

It downloads fixed versions of pandoc and typst, so the PDF doesn't change with the runner image. To rebuild the PDF
for an existing tag, run the workflow by hand on the tag (`gh workflow run manual.yml --ref v0.1.0`); the upload
replaces the asset.
