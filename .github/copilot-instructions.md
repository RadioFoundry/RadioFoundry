<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Review instructions

The full rules are in [AGENTS.md](../AGENTS.md) and [docs/dev/contributing.md](../docs/dev/contributing.md). When you
review a pull request, check these first.

## Tests

- Code that adds or changes behavior comes with host tests in `tests/host/` that fail without the change. Flag
  new code with no test.
- A bug fix comes with the test that would have caught it. Flag a fix without one.
- Look for branches the tests skip: error paths, the other mode (DMR and FM), empty or full lists, a feature switch
  turned off, and keys or screens that exist on one radio only. Ask for a test for each, or for the reason it can't
  be reached from the host.
- A change the host tests can't reach (a register write, a board's wiring) must say so, test the logic around it, and
  add a HIL test when one can be scripted.
- A UI change regenerates the gallery and commits only the PNGs it meant to change.
- A parser of outside input has a fuzz loop.
- The pull request names the tests it adds. Flag a description that claims coverage the diff doesn't show.

## Other checks

- Clean room: no code, names or comments that came from another radio firmware or GPL source.
- Every new file has the SPDX header in its neighbors' style.
- `core/` includes no board or platform header and tests no radio define.
- A feature that adds real code size sits behind a `CONFIG_` switch, and the build stays clean with it off.
- Zero warnings in every configuration `tools/ci.sh` builds.
- A behavior change updates `docs/spec/`, and `docs/user/` when users see it.
- US English in code, comments, docs and commit messages.
