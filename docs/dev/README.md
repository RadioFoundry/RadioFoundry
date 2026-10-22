<!-- SPDX-License-Identifier: MIT -->

# Developer documentation

How OpenDigitalRadio is put together, built, tested and released. These pages summarize and link the detailed specs;
they don't repeat them. The user manual is in [docs/user](../user/README.md).

| Page | What it covers |
|---|---|
| [Contributing](contributing.md) | Start here: clean-room rules, code style, tests and docs a change needs, the release gate |
| [Architecture](architecture.md) | The layers (core, hal, drivers, platform, boards), the tasks, where each feature lives |
| [Building](building.md) | Toolchains (Arm, csky-elf, the Docker image), make targets, image size, flashing your own build |
| [Testing](testing.md) | Host tests, the UI simulator and gallery, coverage, sanitizers, fuzzing, HIL and the SDR bench; where a new test goes |
| [Stack check](stack-check.md) | The static worst-case stack depth of every task, how pointer calls are resolved, the table of hand-kept targets |
| [CI](ci.md) | The workflows per push, nightly and on the radio; running each piece locally; coverage figures; the runners; the csky-elf image |
| [Adding a radio](adding-a-radio.md) | The files a new board needs, `board_t`, the key map, the order to bring it up in |
| [The board registry](board-registry.md) | `board.toml` and `platform.toml` field by field, what is derived, `tools/boards.py`, the build view for tools that build from a checkout |
| [Debug link](debug-link.md) | The host-port debug protocol and `tools/odr_debug.py` |
| [CPS, codeplug and settings](cps-and-codeplug.md) | The open-firmware CPS shim qdmr uses, the codeplug layout, the settings store |
| [The codec](codec.md) | How each radio's stock AMBE+2 codec is extracted, merged and hosted |
| [Web site and web flasher](website.md) | The generated site, where each page's data comes from, templates and design tokens, translating, radio images, the browser flasher, backups and restore |
| [Developer glossary](glossary.md) | Protocol, hardware and firmware terms of the specs; generated from `docs/glossary.json` |
| [Releases](release.md) | The checklist, artifacts and manifests, the sector maps, odrflash's codec handling and key, the release and manual workflows |

## Common tasks

| To | See |
|---|---|
| Build a radio | [Building](building.md); `make BOARD=dm1701` (or `md380`, `dm32`) |
| Run the tests before a pull request | `JOBS=8 tools/ci.sh`, which must end with "all checks passed" ([docs/testing/README.md](../testing/README.md) section 1) |
| Add a feature switch | [build-config.md, "Adding a feature"](../build-config.md#adding-a-feature), then a row in [feature-matrix.md](../feature-matrix.md) |
| Add a host test | [Testing, "Host tests"](testing.md#host-tests) |
| Add a HIL test | [tests/hil/README.md](../../tests/hil/README.md), "Writing a test" |
| Change the UI | [docs/ux](../ux/), [ui.md](../spec/ui.md), then `make -C tests/host ui-gallery` ([Testing](testing.md#the-ui-simulator-and-the-gallery)) |
| Add a radio | [Adding a radio](adding-a-radio.md), its bring-up checklist ([DM-32UV](../testing/dm32-bringup.md), [MD-380](../testing/md380-bringup.md) as models) |
| Move an MD-380 from stock firmware | `tools/md380_migrate.py` ([md380-migration.md](../spec/md380-migration.md); users: [the manual](../user/18-moving-from-stock-md380.md)) |
| Flash a DM-32UV, or move it from stock firmware | `tools/serial_loader.py`, `tools/dm32_migrate.py` ([dm32-migration.md](../spec/dm32-migration.md); users: [the manual](../user/19-moving-from-stock-dm32.md)) |
| Cut a release | [Releases, "Checklist"](release.md#checklist) |
| Build or preview the web site | `python3 site/build.py --serve` ([Web site](website.md)) |

## Where the detailed documents are

| Directory | Contents |
|---|---|
| [docs/spec](../spec/README.md) | Behavior, protocols, the DMR modems, each radio's hardware; [gotchas.md](../spec/gotchas.md) lists past bugs by subsystem |
| [docs/ux](../ux/) | The UI design: principles, information architecture, task flows, usability tests, wireframes, screens |
| [docs/testing](../testing/README.md) | How to run every test tier; on-air coverage; bench procedures; the MMDVM audit; DM-32 and MD-380 bring-up |
| [docs/build-config.md](../build-config.md) | Every build switch and value |
| [docs/feature-matrix.md](../feature-matrix.md) | Every feature's flash cost and per-radio default |
| [Tier III plan](../spec/dmr-tier3-plan.md) | ETSI DMR Tier III trunking: scope, architecture, risks, work packages (planning only) |
| [docs/CLEANROOM.md](../CLEANROOM.md) | The clean-room rules |
| [AGENTS.md](../../AGENTS.md) | The short version of these rules for coding agents |
