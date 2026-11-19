<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Review instructions

When you review a pull request, check it against each rule in
[Contributing, Rules](../docs/dev/contributing.md#rules). Then look for the following problems, which the rules don't
spell out.

## Tests

- Flag new or changed behavior with no host test in `tests/host/` that fails without the change.
- Flag a bug fix without the test that would have caught it.
- Look for branches the tests skip: error paths, the other mode (DMR and FM), empty or full lists, a feature switch
  turned off, and keys or screens that only some radios have. Ask for a test for each, or for the reason the host
  tests can't reach it.
- Flag a change the host tests can't reach (a register write, a board's wiring) when the pull request doesn't say so,
  doesn't test the logic around it, or skips a HIL test that could be scripted.
- Flag a pull request description that claims coverage the diff doesn't show.

## Code

- Flag code, names, or comments that look copied from another radio firmware or from GPL source.
- Flag a definition guarded by a narrower condition than its callers.
- Flag a UI change that commits gallery PNGs it didn't mean to change.
