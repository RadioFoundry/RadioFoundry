<!-- SPDX-License-Identifier: MIT -->

# Clean-room rules

Some of ODR's authors also worked on a popular alternative aftermarket firmware for these radios ("the aftermarket firmware" in these docs), whose license forbids commercial use. ODR is a separate project, not a fork or a successor. None of the aftermarket firmware's code, or code derived from it, may enter this repository or its history. These rules say how the work is split so that holds.

## Roles

**Spec writers (the "dirty" side)** may read the aftermarket firmware trees (its repository and branches, its upstream releases, other firmware sources and binaries). They write:

- prose specs in `docs/spec/`: hardware facts (addresses, register values, pin maps, timing, formats), behavior, protocols and known gotchas. No code, no pseudo-code that tracks the structure of an aftermarket firmware function, no copied comments or identifiers beyond the names of standard things (register names from datasheets, protocol field names from specs).
- `docs/spec/capabilities.md`: what the aftermarket firmware can do on each radio, as user-level capabilities. No menu trees, screen layouts or key mappings from it.
- the carry-over package (below).

**Implementers (the "clean" side)** work only in this repository plus a read-only reference directory kept outside it (`the external reference folder`, next to the checkout). They never open the aftermarket firmware trees, their upstream releases, or any source derived from them. Their inputs are `docs/`, the carry-over package, datasheets and manuals, public standards (ETSI TS 102 361, the MMDVM protocol) and permissively licensed third-party code.

The UI is designed in `docs/ux/` from first principles of good radio usability, not from the aftermarket firmware's menus.

## Carry-over package

The authors' own work moves over; the aftermarket firmware's doesn't.

- **(a) Whole files written entirely by us**, for example the DMR data service, LRRP, privacy, IP gateway, hotspot uplink/data/frame code, SFR, the Baofeng calibration reader and the host tests. They are copied into this repository with MIT SPDX headers. Their old commit is noted in a provenance record the maintainers keep outside this repository.
- **(b) Every change we made inside imported files, on every aftermarket firmware tree**, as diff hunks with 3 lines of context. Each hunk has a note with the commit, what the surrounding code did, the symptom, why the fix works, and what new code must do or avoid. The context and removed lines are aftermarket firmware text, so **(b) lives only in the reference directory and is never committed here**. Implementers read it to learn the lessons, then write new code.
- **(c) `docs/spec/gotchas.md`**: the same lessons in prose, grouped by subsystem. Committed here.

## Not allowed in this repository

- Aftermarket firmware code, headers, tables, fonts, strings or comments, or code derived from them, including the codeplug structs as C code (the layout is described in prose in `docs/spec/open-firmware-cps.md` and implemented fresh).
- Micrium uC/OS, C-SKY or Dahua SDK code, or any file without a clear permissive license.
- GPL code (MMDVMHost FEC, Linux vsprintf, …). FEC is written fresh from ETSI TS 102 361-1.
- The AMBE codec or any stock firmware image.

## Records and checks

- Every source file has an SPDX header, and `reuse lint` must pass. `docs/dev/ci.md` says what it still reports.
- Every file is accounted for in the maintainers' provenance record, kept outside this repository. Each entry is `ours` (with the original commit), `clean` (with the spec section) or `third-party` (with the license, upstream and version).
- Before each release, a similarity scan of this tree against the aftermarket firmware trees must have no unreviewed hits. Run it with `tools/similarity_scan.py REF...` or `ODR_SIMILARITY_REFS="REF..." tools/ci.sh` (docs/testing/README.md).
- `tools/license_grep.sh` finds no `commercial`, `Micrium`, `GPL`, `G4KLX`, `Roger Clark`, `Kai Ludwig` or similar markers outside `docs/` and `LICENSES/`.
