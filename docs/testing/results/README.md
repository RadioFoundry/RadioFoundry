<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The OpenDigitalRadio Authors -->

# Bring-up results

The records of bring-up runs, one directory per board (`<board>/`, the id in `boards/`). Every file here is written
by `tools/bringup.py run --radio <board>` ([tests/hil/README.md](../../../tests/hil/README.md#bring-up-mode)). Don't
write or edit one by hand: a radio's support claims are generated from these records. Commit a record with the
change it checks. A record with failures is still a record: the failures are the to-do list.

## File

`<board>/<YYYYMMDD-HHMMSS>-<commit>.json`, the date in UTC and the first 8 hex digits of the commit. A run of one
feature (`--feature F`) adds `-<feature>`.

| Key | Content |
|---|---|
| `format` | The record format, 1 |
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

## Status

A feature's status on a board comes from the newest record that ran it. The record goes stale when files under the
feature's `spec` or `source` (`tests/hil/features.toml`) changed after its commit: the feature needs a re-run.
`tools/bringup.py status` shows every board and feature.
