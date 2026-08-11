# Clean-room rules

OpenDigitalRadio replaces firmware that was built on a legacy open firmware, whose license forbids commercial use. None of that code, or code derived from it, may enter this repository or its history. These rules say how the work is split so that holds.

## Roles

**Spec writers (the "dirty" side)** may read the old trees (the legacy repository and its branches, legacy upstream releases, other firmware sources and binaries). They write:

- prose specs in `docs/spec/`: hardware facts (addresses, register values, pin maps, timing, formats), behavior, protocols and known gotchas. No code, no pseudo-code that tracks a legacy function's structure, no copied comments or identifiers beyond the names of standard things (register names from datasheets, protocol field names from specs).
- `docs/spec/capabilities.md`: what the radios can do, as user-level capabilities. No menu trees, screen layouts or key mappings from the legacy firmware.
- the carry-over package (below).

**Implementers (the "clean" side)** work only in this repository plus the read-only reference directory `the external reference folder`. They never open the old trees, legacy upstream, or any legacy-derived source. Their inputs are `docs/`, the carry-over package, datasheets and manuals, public standards (ETSI TS 102 361, the MMDVM protocol) and permissively licensed third-party code.

The UI is designed in `docs/ux/` from first principles of good radio usability, not from the legacy firmware's menus.

## Carry-over package

The author's own work moves over; the legacy firmware's doesn't.

- **(a) Whole files written entirely by us** (for example the DMR data service, LRRP, privacy, IP gateway, hotspot uplink/data/frame code, SFR, the DM-32 calibration reader, the host tests). They are copied into this repo with MIT SPDX headers and listed in `PROVENANCE.md` with their old commit.
- **(b) Every change we made inside imported files, on every old tree**, as diff hunks with 3 lines of context, each with a note: the commit, what the surrounding code did, the symptom, why the fix works, and what new code must do or avoid. The context and removed lines are legacy firmware text, so **(b) lives only in `the external reference folder` and is never committed here**. Implementers read it to learn the lessons, then write new code.
- **(c) `docs/spec/gotchas.md`**: the same lessons in prose, grouped by subsystem. Committed here.

## Not allowed in this repository

- Legacy firmware code, headers, tables, fonts, strings or comments, or code derived from them, including the codeplug structs as C code (the layout is described in prose in `docs/spec/legacy-cps-interop.md` and implemented fresh).
- Micrium uC/OS, C-SKY or Dahua SDK code, or any file without a clear permissive license.
- GPL code (MMDVMHost FEC, Linux vsprintf, …). FEC is written fresh from ETSI TS 102 361-1.
- The AMBE codec or any stock firmware image.

## Records and checks

- Every source file has an SPDX header. `reuse lint` passes.
- `PROVENANCE.md` lists every file: `ours` (old commit), `clean` (spec section), or `third-party` (license, upstream, version).
- Before each release, a similarity scan of this tree against the legacy-derived trees has no unreviewed hits (`tools/similarity_scan.py REF...`, or `ODR_SIMILARITY_REFS="REF..." tools/ci.sh`; docs/testing/README.md).
- `tools/license_grep.sh` finds no `commercial`, `Micrium`, `GPL`, `G4KLX`, `Roger Clark`, `Kai Ludwig` or similar markers outside `docs/` and `LICENSES/`.
