<!-- SPDX-License-Identifier: MIT -->

# Developer documentation

How OpenDigitalRadio is put together, built, tested and released. These pages summarize and link the detailed specs;
they don't repeat them. The user manual is in [docs/user](../user/README.md).

| Page | What it covers |
|---|---|
| [Architecture](architecture.md) | The layers (core, hal, drivers, platform, boards), the tasks, where each feature lives |
| [Building](building.md) | Toolchains, make targets, the `CONFIG_` switches, image size |
| [Adding a radio](adding-a-radio.md) | The files a new board needs and the order to bring it up in |
| [Testing](testing.md) | Host tests, the UI simulator and the screen gallery, HIL tests and the SDR bench |
| [Debug link](debug-link.md) | The USB debug protocol and `tools/odr_debug.py` |
| [CPS, codeplug and settings](cps-and-codeplug.md) | The legacy CPS shim qdmr uses, the codeplug layout, the settings store |
| [The codec](codec.md) | How the stock AMBE+2 codec is extracted and hosted per radio |
| [CI](ci.md) | What the CI workflows run per push, nightly and on the radio, how long, coverage, the runners, the csky image |
| [Contributing](contributing.md) | Clean-room rules, code style, commits, the release gate |
| [Releases](release.md) | The release artifacts and manifests, the DM-1701 sector map, odrflash's codec handling and key, the release and manual workflows |

## Where the detailed documents are

| Directory | Contents |
|---|---|
| [docs/spec](../spec/) | Prose specs: hardware, protocols, features, the UI's implementation, gotchas |
| [docs/ux](../ux/) | The UI design: principles, information architecture, task flows, usability tests, wireframes, screens |
| [docs/testing](../testing/README.md) | How to run every test tier; bench procedures; the MMDVM audit; DM-32 and MD-380 bring-up |
| [docs/build-config.md](../build-config.md) | Every build switch and value |
| [Tier III plan](../spec/dmr-tier3-plan.md) | ETSI DMR Tier III trunking: scope, architecture, risks, work packages (planning only) |
| [docs/CLEANROOM.md](../CLEANROOM.md) | The clean-room rules |
