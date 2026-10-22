<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The OpenDigitalRadio Authors -->

# Bring-up results

The records of bring-up runs, one directory per board (`<board>/`, the id in `boards/`). Every run record is written
by `tools/bringup.py run --radio <board>` ([tests/hil/README.md](../../../tests/hil/README.md#bring-up-mode)). Don't
write or edit one by hand: a radio's support claims are generated from these records. The one hand-written file is a
board's seed record (below). Commit a record with the
change it checks. A record with failures is still a record: the failures are the to-do list.

## File

`<board>/<YYYYMMDD-HHMMSS>-<commit>.json`, the date in UTC and the first 8 hex digits of the commit. A run of one
feature (`--feature F`) adds `-<feature>`.

| Key | Content |
|---|---|
| `format` | The record format, 1 |
| `kind` | `run` (a record without `kind` is a run) |
| `board` | The board id, the same as the directory |
| `commit` | The full commit hash the firmware and the tests were run from |
| `dirty` | `true` when the tree had uncommitted changes |
| `date` | The end of the run, ISO 8601 in UTC (`2026-10-22T19:01:33Z`) |
| `features` | The checklist ids the run covered |
| `granted` | The needs the run allowed (`radio`, `rf`, `bench` and so on) |
| `missing` | Listed tests pytest didn't collect (a failure of the run) |
| `tests` | One object per test case: `id` (node id, with parameters), `features`, `required`, `outcome` (`passed`, `failed`, `skipped`), and for a skip `skip_kind`, `excused` and `reason` |
| `firmware` | The radio's VERSION keys `proto`, `view`, `git`, `board`, `features`, `codec`, `display` |
| `options` | The run's flags and frequencies (`allow-rf`, `dmr-freq` and so on) |
| `tools` | The Python and pytest versions |

A record holds no serial port, host name or serial number: the reasons are scrubbed before they are written, and
`tools/bringup.py check` (in `tools/ci.sh`) fails on a record that names one. It also fails on a record that doesn't
parse, sits in the wrong board's directory or names a commit the repository doesn't have.

## Seed record

`<board>/seed.json` holds the evidence for the board from before its first recorded run: what the spec, the
hardware page or an earlier test log says works on the radio. It keeps the generated claims from understating a
board that works but hasn't been through `tools/bringup.py run`. A board has at most one, and it is the only file here
written by hand.

| Key | Content |
|---|---|
| `format` | 1 |
| `kind` | `seed` |
| `board` | The board id, the same as the directory |
| `source` | The doc the evidence came from, a path in the repository |
| `date` | When the seed was written, ISO 8601 |
| `features` | Feature id to row: `result` (`pass` or `fail`), and optionally `commit` (the commit the check ran on, where known), `source` (a doc for this row, when it isn't the record's) and `note` |

`tools/bringup.py check` fails on a seed that isn't named `seed.json`, names a feature the checklist doesn't have, has
a row without a `pass` or `fail` result, or names a commit or doc the repository doesn't have.

## Status

A feature's status never understates the board. For a feature the board builds:

| Status | When |
|---|---|
| `pass`, `fail` | The newest run record that ran the feature passed or failed it |
| `partial` | That run had excused skips or didn't run every test, and no seed row covers the feature |
| `needs re-run` | Files under the feature's `spec` or `source` (`tests/hil/features.toml`) changed after the record's commit. The mark isn't downgraded; the feature needs a run |
| `pass (seed)`, `fail (seed)` | No run passed or failed the feature, and the seed record has a row for it |
| `built, not yet recorded` | No record covers the feature yet, or it has no HIL test |

A run's row replaces the seed's row for each feature the run passed or failed. A seed row with a `commit` needs a
re-run when the feature's files changed after it; a row without one stands until a run replaces it. Only a failing
record shows a failure. `tools/bringup.py list --radio <board>` shows one board with details; `tools/bringup.py status`
shows every board and feature and marks with `*` each status that rests on a seed record.
