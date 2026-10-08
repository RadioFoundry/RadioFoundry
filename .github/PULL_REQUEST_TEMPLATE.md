<!-- SPDX-License-Identifier: MIT -->
<!-- Read docs/dev/contributing.md and docs/CLEANROOM.md first. Delete what doesn't apply. -->

## What and why

<!-- What changes, and the reason. Link the issue: "Fixes #123". -->

## Radios

<!-- Which radios this touches: DM-1701, DM-32UV, MD-380, DB25-D, all (core only), none (tools, docs) -->

## Testing

| Check | Result |
|---|---|
| `JOBS=8 tools/ci.sh` ends with "all checks passed" | <!-- yes; list any skips --> |
| Host test added or changed | <!-- test name, or why none --> |
| HIL (`tests/hil`) | <!-- tests run and radio, or "not run" --> |
| On the radio, on air | <!-- radio, firmware commit, what you tried; or "not tried" --> |

<!-- A UI change: attach the gallery PNGs it changed (make -C tests/host ui-gallery), before and after. -->

## Checklist

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
