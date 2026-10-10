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
3. The HIL tests pass on each radio the release claims, with the flags its features need.
4. odrflash has flashed the release candidate on each radio the release claims, in both codec modes (below, "Testing
   odrflash on a radio").
5. The user manual matches the release: the status of each setting in [Settings](../user/12-settings.md), the
   supported radios in the [Introduction](../user/01-introduction.md), the screenshots (`make -C tests/host ui-gallery`).
6. The README's radio status table is current.
7. Build the manual locally and read it through: `tools/build_manual.sh`.
8. Tag and push the tag:

   ```
   git tag -a v0.1.0 -m "OpenDigitalRadio 0.1.0"
   git push origin v0.1.0
   ```

9. The release workflow builds the firmware and odrflash and attaches them; the manual workflow attaches the PDF
   (below). Edit the release notes on GitHub.

## Release artifacts

| File | What |
|---|---|
| `odr-<board>-<version>.bin` | The raw application image, exactly as `make BOARD=<board>` built it, linked for the board's application base. No codec |
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
  "codec": {"included": false, "profile": "dm1701-v0206-csv", "sectors": [7, 8, 10, 11],
            "windows": [{"addr": "0x08074714", "size": 66076}, {"addr": "0x080D19F0", "size": 75856}],
            "keep_supported": true},
  "features": "FM,SUBTONE,DMR,...",
  "toolchain": "arm-none-eabi-gcc (Arm GNU Toolchain 13.3.Rel1 (Build arm-13.24)) 13.3.1 20240614"
}
```

`ranges` says where each part of the `.bin` goes (one contiguous range today; a later image that also uses sector 9
would list two). `sectors` are the flash sectors the image covers. `codec.profile` is the id the firmware reports for
a matching codec (`codec=` in the debug link's VERSION, [debuglink.md](../spec/debuglink.md) 3.1).

## Sector maps

A flash sector can only be erased whole, so each board's linker script keeps the application out of the sectors that
hold the codec. Whether an update can leave the codec alone (keep) depends on that map:

| Board | Application | Codec | Keep | Map |
|---|---|---|---|---|
| dm1701 | sectors 3-6 (336 KB) | sectors 7-8 (code) and 10-11 (tables), stock addresses | yes | [hardware-dm1701.md](../spec/hardware-dm1701.md) 1.4 |
| md380 | 0x0800C000..0x08050C43 (281,668 B), into sector 6 | from 0x08050C44: shares sector 6 with the application | no | [hardware-md380.md](../spec/hardware-md380.md) 1.3 |
| dm32 | 0x0300C000..0x030E8000 (880 KB) | slot 0x030E8000..0x03100000 | no: the updater erases the whole area | [hardware-dm32.md](../spec/hardware-dm32.md) 17 |

## Codec handling

odrflash picks one of two modes; `--dry-run` prints the exact sectors either would erase and write.

**Keep (the default when it is known to be safe).** Erase and write only the sectors the image covers (3-6); never
touch 7, 8, 10 and 11, so the codec already in the radio stays. Flash can't be read back over the bootloader (the
radio is at RDP level 1), so odrflash needs another way to know the codec is there. In order:

1. **ODR is running.** Before the user switches the radio into its bootloader, odrflash asks over the debug link
   (`odrflash probe`, or automatically at the start of `odrflash flash`). VERSION's `codec=` is the firmware's own check,
   the same one that gates DMR voice (on the DM-1701: the signature word and the CRC-32 of both windows against stock
   V02.06 CSV). If it reports the board's codec profile (`dm1701-v0206-csv`), keep is safe. The answer is remembered for 30 minutes (odrflash's settings directory),
   because the debug link is gone once the radio is in its bootloader.
2. **Stock firmware is running.** Stock has no debug link, so the user says which version it is (`--stock-installed
   "V02.06 CSV"` or the interactive question). Only V02.06 CSV holds this codec at these addresses; V02.01-V02.03 and
   the noCSV builds keep theirs elsewhere, so for those odrflash asks for the stock file instead.
3. Otherwise keep is not chosen automatically; `--codec keep` forces it.

Why a wrong guess is harmless: the firmware never runs the codec without checking it first (signature and both CRCs,
`boards/dm1701/vocoder_dm1701.c`). If the bytes in sectors 7-11 aren't the expected codec, `present()` is false: FM
works, DMR calls show "No codec: voice off", and the debug link reports `codec=absent`. The fix is to flash again with
`--stock`. odrflash checks this itself after flashing: it waits for the radio to restart and reads `codec=` again.

**Merge.** The user supplies the stock firmware file (`--stock`). odrflash checks it by SHA-256 against the codec
profile, decrypts the two windows, checks their SHA-256 and the signature (`odrflash/stockfw.py`, the same library
`tools/codec_extract.py` uses), and writes them at their stock addresses: sectors 3-6, 7, 8, 10 and 11 are erased and
written, sector 9 isn't touched. The merged codec exists only in memory on the user's machine and in the radio.

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
3. a key saved earlier with `--save` (odrflash's settings directory).

So a user needs one file from Baofeng's firmware, which they need anyway for the codec, and never handles the key.

## The release workflow

`.github/workflows/release.yml`:

| Job | Does |
|---|---|
| `firmware-dm1701` | Installs Arm GNU Toolchain 13.3.rel1 (checked by SHA-256) into `~/opt`, runs `tools/ci.sh --quick --board dm1701` as the gate, builds, runs `tools/release.py` |
| `firmware-dm32` | Only when the repository variable `ODR_RELEASE_DM32` is `true` (below) |
| `odrflash` | On Ubuntu 22.04, macOS 14 (arm64), macOS 13 (x86_64) and Windows 2022: installs the package, runs its tests, builds a single-file executable with PyInstaller 6.11.1 (libusb from the `libusb-package` wheel and the `certifi` CA bundle inside) and runs `--version` on it |
| `publish` | On a `v*` tag only: one `SHA256SUMS` over everything, checks every manifest against its image and that none includes the codec, creates the release if the manual workflow hasn't, uploads with `--clobber` |

Pull requests and pushes to main that touch `tools/odrflash/`, `tools/release.py` or the workflow build everything
as workflow artifacts and publish nothing. A manual run on a tag rebuilds and replaces the assets.

The executables are not code-signed. macOS marks downloads as quarantined: `xattr -d com.apple.quarantine
odrflash-*` (or right-click, Open). Windows SmartScreen asks once.

## The MD-380

Not released yet: `releasable=False` in `tools/odrflash/odrflash/boards.py` and no job in `release.yml` until
[docs/testing/md380-bringup.md](../testing/md380-bringup.md) passes. Its codec shares flash sector 6 with the
application, so keep is impossible (`keep_codec_supported=False`): every flash merges the codec from TYT's D002.032
file. `tools/md380_migrate.py` flashes it today and keeps the codeplug
([md380-migration.md](../spec/md380-migration.md)).

## Adding the DM-32UV

The DM-32UV builds and passes its host tests but hasn't run on a radio. The workflow job is there and off. To turn it
on once [docs/testing/dm32-bringup.md](../testing/dm32-bringup.md) passes:

1. Set the repository variable `ODR_RELEASE_DM32=true` (Settings, Secrets and variables, Actions, Variables). The job
   builds the csky-elf toolchain with `tools/build-csky-toolchain.sh` (binutils 2.43.1, GCC 14.2.0, newlib
   4.5.0.20241231, pinned in the script; about 45 minutes on four cores) and caches `~/toolchains/csky-elf` keyed by the
   script's hash, so later releases restore it in seconds. A prebuilt toolchain archive attached to a tools release
   would avoid the first build; that's a later choice.
2. Set `releasable=True` for `DM32` in `tools/odrflash/odrflash/boards.py`.
3. Flashing: the DM-32UV uses Baofeng's serial updater protocol ([hardware-dm32.md](../spec/hardware-dm32.md) 13.2), not
   DFU. odrflash would get a second backend speaking it over pyserial (the `bfuv32-serial` family). The updater erases
   the whole application area before writing (step 7), the codec slot 0x030E8000..0x03100000 included, so **keep is not
   possible on the DM-32UV**: every flash merges the codec from the stock file (`tools/dm32_codec.py` logic as a
   library, like `stockfw` for the DM-1701). The manifest already says so (`keep_supported: false`), and odrflash's
   codec decision refuses keep for it. Whether the updater erases only `size` bytes (which would make a keep mode
   possible with an image that ends before the slot) is a hardware question for the bring-up.

## Testing odrflash on a radio

Host tests cover the planning, manifests, the codec library, key derivation, the codec decision and complete runs
against a fake bootloader with a flash model (`python3 -m unittest discover -s tools/odrflash/tests`; `tools/ci.sh` runs
them). `odrflash flash IMAGE --simulate odr-codec` runs the whole command against that fake. What only a radio can
answer, in this order (the first flash of a new build is always a merge, so the radio has a known codec):

1. `odrflash detect` with the radio in its bootloader: record the strings (they become the model fingerprint table).
2. Merge: `odrflash flash odr-dm1701-<v>.bin --stock "DM-1701(CSV)-V02.06.bin" --dry-run`, then without `--dry-run`. The
   radio restarts; odrflash reports `codec dm1701-v0206-csv` and "DMR voice is ready".
3. Keep, which proves the bootloader erases only the listed sectors: with ODR running, `odrflash probe` (codec
   `dm1701-v0206-csv`), then `odrflash flash odr-dm1701-<v>.bin` (no `--stock`; the plan shows erase 3, 4, 5, 6 and keep
   7, 8, 10, 11). After the restart odrflash must report the codec still present. If it reports `absent`, the
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
