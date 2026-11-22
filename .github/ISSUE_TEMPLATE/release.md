---
name: Release (maintainers)
about: The month's release tracking issue, opened by the release manager
title: "Release vYY.M"
labels: ["release"]
---

<!-- SPDX-License-Identifier: MIT -->
<!-- docs/dev/release.md: the schedule, branches, backports and the checklist. -->

Release manager: @

| Step | Date |
|---|---|
| Cut `release/vYY.M`, tag `vYY.Mb1` (about the 20th) | |
| Beta week ends | |
| Final `vYY.M` (about the 1st) | |

## Cut

- [ ] `tools/release_version.py next beta` gives the name; branch `release/vYY.M` from main, pushed
- [ ] `vYY.Mb1` tagged on the branch; the release workflow published the pre-release
- [ ] String freeze announced to translators

## Beta week

- [ ] Every `backport` pull request merged to the branch or deferred (list them below)
- [ ] Further betas tagged when the fixes matter

## Final (docs/dev/release.md "Checklist")

- [ ] 1. The release branch is green: `JOBS=8 tools/ci.sh`
- [ ] 2. Similarity scan reviewed
- [ ] 3. Legal scan reviewed
- [ ] 4. HIL passes on each radio the release claims (`--tier standard`)
- [ ] 5. rfoundry flashed the candidate on each radio, both codec modes
- [ ] 6. User manual matches the release
- [ ] 7. README radio table current
- [ ] 8. Manual PDF read through
- [ ] 9. CONTRIBUTORS updated
- [ ] 10. `vYY.M` tagged on the branch and pushed
- [ ] 11. Release notes edited from the `changelog:` groups
- [ ] 12. Site installs the new release
- [ ] 13. The previous release branch retired

## Backports
