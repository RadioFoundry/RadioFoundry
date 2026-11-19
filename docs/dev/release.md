<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Releases

A release is a git tag on `main` named for its year and month (`v26.11`) and a GitHub release. The release carries,
for each releasable radio, the firmware image **without the AMBE codec**, its manifest, `SHA256SUMS`, the `rfoundry`
flashing tool as single-file executables for Linux, macOS, and Windows, and the user manual as a PDF. Users never
build firmware: `rfoundry` downloads the image, checks it, and either keeps the codec already in the radio or writes
the codec from the user's own firmware file (see [tools/rfoundry](../../tools/rfoundry/README.md) and
[The AMBE codec](codec.md)).

## Release names

A release is named for the year and month it comes out, so a user sees at a glance how old the firmware is.
`tools/release_version.py` holds the rule, and the Makefile, `tools/release_build.sh`, the release workflow, and the
site all ask it.

| Name | When |
|---|---|
| `vYY.M` | The first release of a month: `v26.11` |
| `vYY.M.N` | Another release that month: `v26.11.1`, `v26.11.2` |
| `vYY.MbN` | A pre-release (beta) of that name: `v26.12b1`. The final keeps the name even if it ships the next month |

The year has two digits (`26` is 2026) and a beta is a bare `b` and its count, to save room on the radios' small
displays. The month has no leading zero and the count starts at 1, so `v26.11.3` never reads as March. `main` is
the alpha. `tools/release_build.sh` and the release workflow refuse any other tag.

**On the radio.** A clean build of a commit that carries a release tag gets `RFOUNDRY_RELEASE` (the Makefile reads the
tag; `make RELEASE=<tag>` sets it, as `tools/release_build.sh` does, and `RELEASE=` leaves it out). The splash screen
shows the release name in place of the commit, About shows both on their own lines (Release, then Build), and the debug
link's VERSION adds `release=` ([debuglink.md](../spec/debuglink.md) 3.1). `RFOUNDRY_GIT` stays the commit everywhere
else: the crash record's build id, the manifests and the CPS id need it. The build service fetches tags, so a custom
build of a tagged commit shows the release name too.

**Betas.** A tag ending `bN` becomes a GitHub pre-release. GitHub's latest release skips pre-releases, and so does
the site: the flasher installs the newest release that is not a pre-release by default, and offers a newer beta only as
an opt-in version link (`?version=<tag>`, [website.md](website.md#flasher-notices)). `?version=main` still builds main.

**On the site.** The footer shows the firmware release the flasher installs, linked to its GitHub release, and a
Changelog link to the GitHub releases. The release notes start from GitHub's generated notes (`--generate-notes`):
edit them into a short list of what changed for users.

## Checklist

1. `main` is green: `JOBS=8 tools/ci.sh` ends with "all checks passed" (see [Continuous integration](ci.md)), on a
   machine with both compilers and `reuse`.
2. The project lead runs the similarity scan against the reference trees and reviews every hit (see
   [Records and checks](../CLEANROOM.md#records-and-checks)).
3. A comprehensive legal scan of what the release ships, reviewed by the project lead: the license and notices of
   every file and dependency (`reuse lint`, `third_party/`, the rfoundry executables' bundled packages), the names
   and marks used in the docs, the site and the images, and nothing in the tree, the release or the site that the
   clean-room rules ([CLEANROOM.md](../CLEANROOM.md)) or [Contributing](contributing.md#rules) rule 2 forbid.
4. The HIL tests pass on each radio the release claims, with the flags its features need.
5. rfoundry has flashed the release's build on each radio the release claims, in both codec modes (see
   [Testing rfoundry on a radio](../../tools/rfoundry/README.md#testing-rfoundry-on-a-radio)).
6. The user manual matches the release: each setting in [Settings](../user/17-settings.md), the supported radios in
   the [Introduction](../user/01-introduction.md), and the screenshots (`make -C tests/host ui-gallery`).
7. The README's radio status table is current.
8. Build the manual locally and read it through ([manual-pdf.md](manual-pdf.md)).
9. Update the contributors list and commit it if it changed: `python3 tools/gen_contributors.py` writes
   `CONTRIBUTORS` from the git history. CI never checks that the file is current, so a new contributor's pull
   request can't fail over it.
10. Tag and push the tag:

   ```
   git tag -a v26.11 -m "RadioFoundry 26.11"
   git push origin v26.11
   ```

11. The release workflow builds the firmware and rfoundry and attaches them; the manual workflow attaches the PDF
    ([manual-pdf.md](manual-pdf.md)). Edit the release notes on GitHub (they are the changelog the site links).
12. The next push to main redeploys the site, which then installs the new release (or offers the new beta).

## Release artifacts

| File | What |
|---|---|
| `rfoundry-<board>-<version>.bin` | The raw image `make BOARD=<board>` built, linked for the board's application base. No codec. A board with a first stage releases `rfoundry_<board>_install.bin`, the first stage then the application; `tools/release.py` refuses the application alone |
| `rfoundry-<board>-<version>.json` | Its manifest (below) |
| `rfoundry-<version>-<os>-<arch>[.exe]` | rfoundry, one file per system: `linux-x86_64`, `macos-arm64`, `macos-x86_64`, `windows-x86_64` |
| `SHA256SUMS` | SHA-256 of every file above |
| `RadioFoundry-manual-<version>.pdf` | The user manual (the manual workflow) |

`tools/release.py --board <id> --version v26.11` writes the first two and the sums for a built image into `dist/` (or
`--out DIR`). With `--variant amateur`, it writes the image of `make VARIANT=amateur` as
`rfoundry-<board>-amateur-<version>.bin`, with a manifest that names the variant (see
[Variants](../build-config.md#variants)). It refuses an image that reaches into a codec sector or carries the codec's
signature word, so a release can never contain the codec, even by accident.

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
offer the release to a build that isn't one. When a manifest has no `git_time`, the site's build looks the commit up
in its clone. `ranges` says where the `.bin` goes: one range, the application at the board's base. `sectors` are the
flash sectors the image covers. `codec.windows` are where the merge puts the codec: on an `stm32f4` radio, the code,
the tables, and the descriptor (see [The AMBE codec](codec.md)). `codec.profile` is the id the firmware reports for a
matching codec (`codec=` in the debug link's VERSION, [debuglink.md](../spec/debuglink.md) 3.1).

## The release workflow

`.github/workflows/release.yml` has the following jobs.

| Job | Does |
|---|---|
| `firmware (<toolchain>)` | One job per toolchain: `firmware (Arm)` and `firmware (C-SKY)`. Sets up the pinned compiler with `.github/actions/toolchain` ([ci.md](ci.md#toolchains)): the self-hosted image's, the Arm GNU Toolchain checked by SHA-256, or the csky-elf image, never a source build. Without csky-elf GCC on the runner and without the image, the C-SKY job fails. On a tag runs the gate: `tools/ci.sh --quick --steps registry,host,python,licenses` on the runner, then `tools/ci.sh --quick --steps firmware --toolchain <id>` with the toolchain's compiler (through `tools/docker/csky-toolchain/run.sh` for the image). Then `tools/release_build.sh --toolchain <id>`, the same way: for every board on the toolchain with `[status] release = "stable"` (`tools/boards.py list --toolchain <id> --release stable`), it builds the default image and each build option value's image (`tools/boards.py builds`) and runs `tools/release.py` on each. Uploads them as `firmware-arm` or `firmware-csky` |
| `rfoundry` | On Ubuntu 22.04, macOS 14 (arm64), macOS 13 (x86_64) and Windows 2022: installs the package, runs its tests, builds a single-file executable with PyInstaller 6.11.1 (libusb from the `libusb-package` wheel, the `certifi` CA bundle, and `serial_loader`, `hrc7000_codec` and `stubtool` for the serial route inside) and runs `--version` on it |
| `publish` | On a `v*` tag only, after every toolchain's job and every rfoundry build passed: one `SHA256SUMS` over everything, checks every manifest against its image and that none includes the codec, creates the release if the manual workflow hasn't, uploads with `--clobber` |

On a pull request, nothing is published and the release gate doesn't run (the pull request's own checks run the same
tests). What builds depends on what the pull request changes:

| What the pull request changes | What builds |
|---|---|
| `tools/rfoundry/` (with the `registry.json` a `board.toml` change regenerates), the serial flashing tools, `stub/stubtool.py` | `release.yml`: `version` and `rfoundry`, as artifacts |
| `release.yml` or the toolchain action | `release.yml`: everything, as artifacts |
| A `board.toml`, `board.mk` or `platform.toml`, `tools/release.py`, `tools/release_build.sh`, `tools/boards.py`, the toolchain scripts | `pr-tests.yml`: its `firmware (<toolchain>)` jobs run `tools/release_build.sh` after their builds, so every stable board is packaged as a tag would package it |

A manual run of the workflow on a tag rebuilds and replaces the assets.

The executables aren't code-signed. macOS marks downloads as quarantined: `xattr -d com.apple.quarantine
rfoundry-*` (or right-click, Open). Windows SmartScreen asks once.
