<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Clean-room rules

Some of RadioFoundry's authors also worked on a popular alternative aftermarket firmware for these radios ("the
aftermarket firmware" in these docs), whose license forbids commercial use. RadioFoundry is a separate project, not a
fork or a successor. No code from the aftermarket firmware, and no code derived from it, can enter this repository or
its history. This page describes how the work is split so that this holds. For the rules every change follows, see
[Contributing](dev/contributing.md#rules).

## Roles

*Spec writers* (the "dirty" side) can read the aftermarket firmware trees: its repository and branches, its upstream
releases, and other firmware sources and binaries. They write the following:

- Prose specs in `docs/spec/`: hardware facts (addresses, register values, pin maps, timing, formats), behavior,
  protocols, and known gotchas. Specs contain no code, no pseudo-code that follows the structure of an aftermarket
  firmware function, and no copied comments or identifiers beyond the names of standard things (register names from
  datasheets, protocol field names from specs).
- `docs/spec/capabilities.md`: what the aftermarket firmware can do on each radio, as user-level capabilities. It
  contains no menu trees, screen layouts, or key mappings from that firmware.
- The carry-over package (see [Carry-over package](#carry-over-package)).

*Implementers* (the "clean" side) work only in this repository and a read-only external reference folder kept outside
it. They never open the aftermarket firmware trees, their upstream releases, or any source derived from them. Their
inputs are `docs/`, the carry-over package, datasheets and manuals, public standards (ETSI TS 102 361, the MMDVM
protocol), and permissively licensed third-party code.

The UI is designed in `docs/ux/` from first principles of radio usability, not from the aftermarket firmware's menus.

## Carry-over package

The authors' own work moves over; the aftermarket firmware's work doesn't. The package has three parts:

- **(a) Whole files written entirely by the authors**, for example the DMR data service, LRRP, privacy, the IP gateway,
  the hotspot uplink, data, and frame code, SFR, the Baofeng calibration reader, and the host tests. They're copied into
  this repository with MIT SPDX headers. A provenance record that the maintainers keep outside this repository notes
  each file's old commit.
- **(b) Every change the authors made inside imported files, on every aftermarket firmware tree**, as diff hunks with
  3 lines of context. Each hunk has a note with the commit, what the surrounding code did, the symptom, why the fix
  works, and what new code must do or avoid. The context and removed lines are aftermarket firmware text, so **(b)
  lives only in the reference folder and is never committed here**. Implementers read it to learn the lessons, then
  write new code.
- **(c) `docs/spec/gotchas.md`**: the same lessons in prose, grouped by subsystem. It's committed here.

## Not allowed in this repository

- Aftermarket firmware code, headers, tables, fonts, strings, or comments, or code derived from them. This includes
  the codeplug structs as C code: `docs/spec/open-firmware-cps.md` describes the layout in prose, and the code
  implements it fresh.
- Micrium uC/OS, C-SKY, or Dahua SDK code, or any file without a clear permissive license.
- GPL code, such as MMDVMHost FEC or Linux `vsprintf`. FEC is written fresh from ETSI TS 102 361-1.
- The AMBE codec or any manufacturer firmware image.

## Records and checks

- Every source file has an SPDX header, and `reuse lint` must pass. For what it reports on the tree, see
  [Continuous integration](dev/ci.md).
- The maintainers' provenance record, kept outside this repository, accounts for every file. Each entry is `ours`
  (with the original commit), `clean` (with the spec section), or `third-party` (with the license, upstream, and
  version).
- Before each release, a similarity scan of this tree against the aftermarket firmware trees must have no hits that
  nobody reviewed. To run it, use `tools/similarity_scan.py REF...` or
  `RFOUNDRY_SIMILARITY_REFS="REF..." tools/ci.sh`. The project lead runs it; public CI never does.
- `tools/license_grep.sh` finds no `commercial`, `Micrium`, `GPL`, `G4KLX`, `Roger Clark`, `Kai Ludwig`, or similar
  markers outside `docs/` and `LICENSES/`.

`tools/ci.sh` runs `tools/license_grep.sh`, `reuse lint` when it's installed, and the similarity scan when
`RFOUNDRY_SIMILARITY_REFS` is set.
