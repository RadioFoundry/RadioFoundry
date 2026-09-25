<!-- SPDX-License-Identifier: MIT -->

# Contributing

Issues and pull requests are welcome. Read this page and [docs/CLEANROOM.md](../CLEANROOM.md) first.

## Clean room

ODR is MIT licensed and must stay free of code it may not use. The rules are in [docs/CLEANROOM.md](../CLEANROOM.md).
The short version:

- Write code from the prose specs in `docs/spec/`, datasheets, public standards (ETSI TS 102 361, the MMDVM protocol)
  and permissively licensed libraries. Never from another radio firmware's source.
- Don't open, copy or paraphrase GPL or non-commercial firmware, vendor SDKs or disassembly while writing code
  for this repository. If you have read them, write specs (facts, not code) and let someone else implement.
- Never add the AMBE codec, a stock firmware image, a bootloader key or a memory dump to the repository.
- Every file carries an SPDX header (`SPDX-License-Identifier: MIT` for our files). `reuse lint` must pass.
- Test vectors come from standards, from your own captures, or from the stock codec's output on your own radio.

The checks: `tools/license_grep.sh` (markers of foreign code), `reuse lint`, and before a release a similarity scan
against the reference trees (run by the project lead, never in public CI). All three are part of `tools/ci.sh`.

## Code

- `core/` stays radio independent. Radio differences go through `board_t` and the `hal_*` interfaces
  ([Architecture](architecture.md)).
- Anything that costs flash goes behind a `CONFIG_` switch ([Building](building.md)).
- C11 (gnu11) with `-Wall -Wextra -Wshadow -Wdouble-promotion`. Zero warnings in every configuration.
- Follow the style of the file you edit. Comments say why, not what.
- Keep RTOS calls and blocking waits out of the UI's behavior code and the protocol engines, so the host tests can run them.
- Read the section of [gotchas.md](../spec/gotchas.md) for the subsystem you change.

## Tests

- New behavior gets a host test. A bug fix gets the test that would have caught it.
- A UI change updates the gallery (`make -C tests/host ui-gallery`) and commits the PNGs it meant to change.
- A change that touches hardware gets a HIL test when it can be scripted ([Testing](testing.md)).

## Documentation

- A behavior change updates its spec in `docs/spec/` and, if users see it, the user manual in `docs/user/`.
- A new setting goes into [Settings](../user/12-settings.md) with its default and status.
- Docs use US English, short sentences and tables. Links are relative so they work on GitHub and in the PDF.
- `python3 tools/check_doc_links.py` checks every relative link, image and anchor in the docs (ci.sh runs it).

## Before you open a pull request

```
JOBS=8 tools/ci.sh
```

It must end with "all checks passed". Skips (a missing compiler, `reuse` not installed) are reported; install what you
can. CI runs the quick part on every push and the feature matrix nightly ([CI](ci.md)); run the whole gate before you
open the pull request. Commit in small steps with messages that say what changed and why.
