<!-- SPDX-License-Identifier: MIT -->

# Developer Documentation

The contributor guide: how RadioFoundry is built, tested, released and extended. These pages describe process and
tooling and link the specs for behavior; they don't repeat them. Users read [docs/user](../user/README.md).

## Reading order

| Step | Page | What it covers |
|---|---|---|
| 1. Rules | [Contributing](contributing.md) | Branches, pull requests, clean-room rules, code, tests and docs a change needs |
| 2. Layout | [Architecture](architecture.md) | The layers (core, hal, drivers, platform, boards), the tasks, where each feature lives |
| 3. Build | [Building](building.md) | Toolchains, make targets, feature switches, build options, image size, flashing your own build |
| 4. Test | [Testing](testing.md) | Host tests, the UI simulator and gallery, sanitizers, fuzzing, HIL and the SDR bench |
| 5. CI | [CI](ci.md) | The workflows per push, per pull request and nightly; running each piece locally; coverage |
| | [CI runners and images](ci-runners.md) | The Gitea and GitHub runners, the csky-elf image, the HIL runner |
| | [Stack check](stack-check.md) | The static worst-case stack depth of every task |
| 6. Add a radio | [Adding a radio](adding-a-radio.md) | The files a board needs, `board_t`, the key map, bring-up order and records |
| | [The board registry](board-registry.md) | `board.toml` and `platform.toml` field by field, what is derived, `tools/boards.py` |
| 7. Codec | [The codec](codec.md) | How a radio's codec is merged and hosted, and what a merge tool must do |
| 8. Release | [Releases](release.md) | The checklist, artifacts and manifests, sector maps, codec handling, the release workflow |
| | [The user manual PDF](manual-pdf.md) | Building the manual and its workflow |
| 9. Web site | [The web site](website.md) | The generated site, data sources, templates, translating, radio images, error logging |
| | [The web flasher](web-flasher.md) | The browser installer: modules, backends, backups, restore, version links |
| | [The Codeplug Editor](codeplug-editor.md) | The browser codeplug editor |
| | [Translating](translating.md) | Adding a language to the radio and the site: catalogs, build switches, checks |

## Reference

| Page | What it covers |
|---|---|
| [Debug link](debug-link.md) | The host-port debug protocol and `tools/rfoundry_debug.py` |
| [CPS, codeplug and settings](cps-and-codeplug.md) | Where the CPS shim, the codeplug layout and the settings store are specified |
| [Developer glossary](glossary.md) | Terms of the specs, generated from `docs/glossary.json` |

## Common tasks

| To | See |
|---|---|
| Build a radio | [Building](building.md) |
| Run everything a pull request must pass | `JOBS=8 tools/ci.sh`, which ends with "all checks passed" ([CI](ci.md#running-the-pieces-locally)) |
| Add a feature switch | [build-config.md, "Adding a feature"](../build-config.md#adding-a-feature), then the matrix ([feature-matrix.md](../feature-matrix.md)) |
| Add a host test | [Testing, "Host tests"](testing.md#host-tests) |
| Add a HIL test | [tests/hil/README.md](../../tests/hil/README.md), "Writing a test" |
| Change the UI | [docs/ux](../ux/), [ui.md](../spec/ui.md), then `make -C tests/host ui-gallery` ([Testing](testing.md#the-ui-simulator-and-the-gallery)) |
| Add a radio | [Adding a radio](adding-a-radio.md) |
| Move a radio from stock firmware | The migration tools and specs are listed in the [spec index](../spec/README.md#migrating-from-stock) |
| Cut a release | [Releases, "Checklist"](release.md#checklist) |
| Preview the web site | `python3 site/build.py --serve` ([The web site](website.md)) |
| Install RadioFoundry or edit a codeplug (as a user) | [Installing](../user/02-installing.md) |

## Elsewhere

| Directory | Contents |
|---|---|
| [docs/spec](../spec/README.md) | Behavior, protocols, the DMR modems, each radio's hardware |
| [docs/ux](../ux/) | The UI design: principles, information architecture, task flows, wireframes, screens |
| [docs/testing](../testing/README.md) | How to run every test tier, on-air coverage, bench procedures, bring-up records |
| [docs/build-config.md](../build-config.md) | Every build switch and value |
| [docs/feature-matrix.md](../feature-matrix.md) | Every feature's flash cost and which radios build it |
| [docs/CLEANROOM.md](../CLEANROOM.md) | The clean-room rules |
| [AGENTS.md](../../AGENTS.md) | The short version of the rules for coding agents |
