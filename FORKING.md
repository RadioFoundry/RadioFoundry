<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Using RadioFoundry in Your Project

RadioFoundry is MIT licensed ([LICENSE](LICENSE)). You may build your own firmware on it, copy parts of it into
another project, change it, sell it and ship it in a product, open or closed source. You don't need to ask.

The license asks one thing in return: **keep our copyright and license notices**. This page explains what that means
in practice. It also explains why we hope you'll build your feature here, where it carries your name.

This page describes the license in plain words. It isn't legal advice, and the [LICENSE](LICENSE) file is what
counts.

## Keep the notices

The license says: "The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software." Its permissions are granted subject to that condition. Code copied with the
notices removed isn't covered by the license, and copying it without a license infringes the authors' copyright.

That is the whole obligation, and it takes a minute to meet:

| You ship | What to keep |
|---|---|
| A fork, or source files copied from RadioFoundry | The `LICENSE` file, the `AUTHORS` and `CONTRIBUTORS` files that say who the authors are, and the SPDX and copyright lines at the top of each file. |
| Part of a file, moved into a file of your own | Our copyright line in that file, and `LICENSE` in your tree. The license speaks of "substantial portions"; when in doubt, keep the line. |
| Your own changes | Add your copyright line below ours. Please don't replace ours. |
| A firmware image | The `LICENSE` text with the download, in the release package or its documentation. On the radio, keep the `License:` line of the About page (**Settings > Advanced > About**): it is the copyright notice in the image, next to the third-party licenses. The maintainers listed below it are RadioFoundry's. Replace them with your own, so your users know whom to contact. |
| Code from `third_party/`, or the fonts | That component's own license and notice. They are in [LICENSES/](LICENSES/), in each submodule's tree and in `platform/<platform>/THIRD_PARTY`. Some of them (BSD-3-Clause, Apache-2.0) also require their license text to accompany a binary. |

Nothing else is required. You don't have to publish your changes, use the MIT license for your own code or tell us.

### Example: a file you changed

```c
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 Your Name or Project
```

### If your project uses another license

Your project can be under any license you choose, including the GPL or a proprietary one. MIT code may be combined
with either. Your license then applies to your own files and to the combined work. A file that came from RadioFoundry
still keeps our notice, with yours added below it:

```c
// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Copyright (c) 2026 The RadioFoundry Authors (MIT)
// Copyright (c) 2026 Your Name or Project (changes, GPL-3.0-or-later)
```

### If the notices are already gone

It happens, most often when files are moved or a project changes its license. Put the copyright lines back in the
files that came from RadioFoundry, and add `LICENSE`, `AUTHORS` and `CONTRIBUTORS` to your tree and your releases.
Once they're back, we consider the matter settled. If you aren't sure what a file needs, ask in a
[discussion](https://github.com/radiofoundry/radiofoundry/discussions) and we'll help.

The same applies to us. If you think RadioFoundry has missed someone's credit,
[open an issue](https://github.com/radiofoundry/radiofoundry/issues).

## What the license doesn't cover

| Topic | What applies |
|---|---|
| The name | The license covers the code, not the name. Please give your project its own name and describe it as "based on RadioFoundry", so nobody takes it for an official release. Change `Firmware: RadioFoundry` on the About page to your project's name, so your users' bug reports reach you. |
| The AMBE codec | It isn't in this repository and isn't ours to license ([codec](docs/dev/codec.md)). The same limits apply to your project. |
| Warranty | None, as the license says. Test before you depend on it. |
| Radio regulations | What your build transmits, and where, is up to you and your users' licenses. |

## Appreciated, not required

- Credit RadioFoundry in your README, for example:
  `Based on [RadioFoundry](https://github.com/radiofoundry/radiofoundry), MIT licensed.`
- Fork on GitHub instead of copying files. The history stays, and our fixes merge into your tree more easily.
- Tell us about your project in a [discussion](https://github.com/radiofoundry/radiofoundry/discussions). We like
  seeing what people build, and we're glad to link to it.

## Consider contributing instead

If you're adding a feature (a protocol such as P25, a mode, a screen, a radio), we would be glad to have it in
RadioFoundry, with you credited as its author. Open a
[discussion](https://github.com/radiofoundry/radiofoundry/discussions) about what you want to build, or send a pull
request, draft or finished ([contributing](docs/dev/contributing.md)). If you already have a fork, we'll help you
split it into pull requests.

What a feature gains by living in this tree:

| | In RadioFoundry | In a separate project |
|---|---|---|
| Radios | Available on every supported radio whose hardware can run it, and on radios ported later ([feature matrix](docs/feature-matrix.md)) | The radios you port and maintain |
| Upkeep | Changes to the core are made with your code in the tree, so it keeps building and working. Fixes from every contributor reach it. | You merge upstream changes yourself, and the merges get harder as the trees diverge |
| Testing | CI builds every radio with your switch on and off and runs the host tests under sanitizers. Maintainers run the HIL tests on real radios against the SDR bench ([testing](docs/dev/testing.md)). | Your own test setup |
| Users | One install, with the web flasher, the Codeplug Editor, the manual and the translations. Users get your feature together with everyone else's. | Users choose between firmwares |
| Review | Review by people who work on these chips every day ([gotchas](docs/spec/gotchas.md)) | Your own review |
| Credit | Your name on your commits, in [CONTRIBUTORS](CONTRIBUTORS), in the [contributors list](https://github.com/radiofoundry/radiofoundry/graphs/contributors) and in the release notes. You keep the copyright in the code you wrote and are credited among "The RadioFoundry Authors". | Your project's own credits |
| Direction | Changes go through review and the project's rules | You decide everything |

A feature that costs real flash goes behind a `CONFIG_` switch ([build configuration](docs/build-config.md)). Radios
with little flash can leave it out, so your feature doesn't push out anyone else's.

Code coming in must meet the clean-room rule: it is your own work or permissively licensed, written from specs,
datasheets and public standards, never from GPL firmware or firmware whose license forbids commercial use
([clean-room rules](docs/CLEANROOM.md)). If part of your project can't meet that, say so in the discussion. We can
often write a spec from your notes and implement that part fresh.

A separate project is sometimes the right choice. If it is for you, keep the notices, and we'd still like to hear
what you're building.
