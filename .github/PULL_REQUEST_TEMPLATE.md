<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->
<!-- Read docs/dev/contributing.md and docs/CLEANROOM.md first. Delete what doesn't apply. -->

## What and why

<!-- What changes, and the reason. Link the issue: "Fixes #123". -->

## Radios

<!-- Which radios this touches, from this list, or all (core only), or none (tools, docs):
generated: tools/boards.py docs radios; edit boards/*/board.toml
DM-32UV, DM-1701, DM-1801 Pro, DB25-D PRO, MD-380, MD-UV380, MD-9600, RT10
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

<!-- The numbers are the rules in docs/dev/contributing.md#rules. -->

- [ ] Branch `<type>/<area>-<short-description>`, title `<Area>: <what changed>`
      ([branch names](https://github.com/radiofoundry/radiofoundry/blob/main/docs/dev/contributing.md#branch-names)).
- [ ] I wrote this, or have the right to submit it, and license it under the project's MIT license
      ([license of contributions](https://github.com/radiofoundry/radiofoundry/blob/main/docs/dev/contributing.md#license-of-contributions)).
- [ ] 1. Clean room ([CLEANROOM.md](https://github.com/radiofoundry/radiofoundry/blob/main/docs/CLEANROOM.md)).
- [ ] 2. No codec, firmware images, keys, dumps, or recordings in the diff.
- [ ] 3. SPDX header on every new file.
- [ ] 4. `core/` stays radio independent.
- [ ] 5. Flash costs behind a `CONFIG_` switch; the build is clean with it off.
- [ ] 6. Zero warnings in every configuration `tools/ci.sh` builds.
- [ ] 7. US English.
- [ ] 8. Host tests cover the change, named in Testing above; a UI change commits only the PNGs it meant to change.
- [ ] 9. Specs in `docs/spec/` and, if users see it, `docs/user/` updated.
- [ ] 10. New text marked for translation; translations done or listed as open tasks.
