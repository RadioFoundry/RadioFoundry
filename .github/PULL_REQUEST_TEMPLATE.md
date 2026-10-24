<!-- SPDX-License-Identifier: MIT -->
<!-- Read docs/dev/contributing.md and docs/CLEANROOM.md first. Delete what doesn't apply. -->

## What and why

<!-- What changes, and the reason. Link the issue: "Fixes #123". -->

## Radios

<!-- Which radios this touches, from this list, or all (core only), or none (tools, docs):
generated: tools/boards.py docs radios; edit boards/*/board.toml
DM-1701, DM-1801 Pro, DM-32UV, DB25-D PRO, MD-380, GD-88
end generated
-->

## Testing

| Check | Result |
|---|---|
| `JOBS=8 tools/ci.sh` ends with "all checks passed" | <!-- yes; list any skips --> |
| Host unit tests added or changed (required for code changes) | <!-- test names; docs-only PRs: "docs only" --> |
| HIL (`tests/hil`) | <!-- tests run and radio, or "not run" --> |
| On the radio, on air | <!-- radio, firmware commit, what you tried; or "not tried" --> |

<!-- A UI change: attach the gallery PNGs it changed (make -C tests/host ui-gallery), before and after. -->

## Checklist

- [ ] Branch named `<type>/<area>-<short-description>`, title `<Area>: <what changed>`
      ([contributing.md](https://github.com/sfowlr/OpenDigitalRadio/blob/main/docs/dev/contributing.md#branches-and-pull-requests)).
- [ ] Host unit tests cover the change (code changes), named in Testing above.
- [ ] Clean room: written from `docs/spec/`, datasheets, public standards or permissively licensed code. Nothing
      copied, translated or paraphrased from other firmware, GPL code, vendor SDKs or disassembly
      ([CLEANROOM.md](https://github.com/sfowlr/OpenDigitalRadio/blob/main/docs/CLEANROOM.md)).
- [ ] No AMBE codec, stock firmware, bootloader keys, flash or memory dumps or audio recordings in the diff.
- [ ] SPDX header on every new file.
- [ ] `core/` stays radio independent: no board headers, no radio defines.
- [ ] Code that costs flash is behind a `CONFIG_` switch, and the build is clean with it off.
- [ ] Zero warnings in every configuration `tools/ci.sh` builds.
- [ ] A behavior change updates its spec in `docs/spec/`; if users see it, `docs/user/` too.
- [ ] A UI change regenerates the gallery and commits only the PNGs it meant to change.
- [ ] US English in code, comments, docs and commit messages.
