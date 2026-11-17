<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Adding a Mode

How a digital mode beyond FM and DMR gets into the codeplug, the firmware, the Codeplug Editor and zone sharing. The
format is [channel-modes.md](../spec/channel-modes.md); this page is the order of work. A mode is a **kind** (what a
channel is) and, when its channels share a network or an identity, a **system type**. The generic layer never reads a
mode's bytes: the mode's own module does, in the firmware and in the editor.

## 1. Plan the records

Write the mode's spec first, `docs/spec/<mode>-codeplug.md`, linked from the spec index. Answer each row from
the table at the top of [channel-modes.md](../spec/channel-modes.md), "What a mode needs":

| Question | Home |
|---|---|
| What a channel holds beyond its base record (access code, options, a per-channel target) | The tag's parameter block (2.1): the layout, its default when zeroed, grown only by appending |
| Which base record fields the kind keeps using (contact, RX group list, color code...) | The spec says; the editor shows only those (`recordFields`) |
| Call targets | The contact list when they fit a 24-bit ID with a call type; else targets (5.2), with the address format |
| Radio-wide identity or options | The kind's radio-wide record, key 0x0A00 + kind (5.5) |
| A network, a system ID, sites, control channels, a channel plan | A system type: its block (identity first), its entry classes and their bodies (7) |
| An authentication secret, traffic keys | The system secret (7.4), traffic keys (5.4) with algorithm ids |
| Whether a new channel or system may transmit by default | The tag's flag bit 0, the system's flag bit 1 (listen only) |

Every value must read safely when zeroed or short, and unverified standard values stay out of the spec.

## 2. Claim the ids

Append one row to each of the three registries in the same change: the tables in
[channel-modes.md](../spec/channel-modes.md) 4, `CODEPLUG_KINDS` / `CODEPLUG_SYSTEM_TYPES` in
`core/codeplug/codeplug_modes.h`, and `KINDS` / `SYSTEM_TYPES` in `site/static/js/codeplug/modes.js`. A kind and its
system type share the id. `tools/check_modes.py` (in `tools/ci.sh`) fails when the three differ. Ids are never reused.

## 3. Firmware

| Step | Where |
|---|---|
| A switch | `CONFIG_<MODE>` in `config/features.mk` and `core/config.h`, off until the mode runs ([build-config.md](../build-config.md)) |
| The codec | `core/<mode>/<mode>_codeplug.c`: decodes the parameter block (`codeplug_tag_params`), the type's block and entries (`codeplug_records.h`) into the mode's own structs. Add the switch to `CODEPLUG_MODE_RECORDS` when it reads those families |
| On air | A mode whose frames the core builds describes its physical layer as an `air_profile_t` (symbol rate, levels, deviation, sync patterns; [air-interface.md](../spec/air-interface.md) 2) and runs its engine on an air session (7). Every board's provider then reports how well it fits (4), with no board change; the mode's radio core checks that fit before it offers the mode. Its RAM comes from memory an idle part lends (as P25 does, [p25.md](../spec/p25.md) 7) where it can |
| Built | The registry row's `built` column becomes the switch once an engine runs the kind. Until then a channel of the kind is parked (6) on every build |
| Host test | The codec against the shared vectors (`tests/vectors/channel-modes.txt`), with the switch on and off |

Nothing outside the mode's module tests a kind id: the radio core asks the registry (`codeplug_kind_built`) and the
mode module.

## 4. Codeplug Editor

Add `site/static/js/codeplug/mode_<mode>.js` and name it in the registry row, with the feature the radio's probe
reports for the mode. A kind module gives:

| Member | Meaning |
|---|---|
| `id`, `systemType` | The kind, and the system type its channels need (null: none) |
| `contacts`, `recordFields` | Whether its targets are contacts; the base record fields it uses |
| `ownFreqs` | Whether the channel keeps its own frequencies (false: the system gives them) |
| `defaultTx`, `fallback`, `flags` | Transmit on a new channel; whether it has a fallback channel; its flag bits 4-7 by name |
| `newParams`, `decode`, `encode` | The parameter block. `encode(params, prev)` keeps the bytes past what it knows |
| `fields()` | The form's fields, declared (`editor/modefields.js`: int, hex, freq, bool, select, list, hexbytes, text) |
| `validate`, `settings` | Problems of a channel; the radio-wide record's fields, when it has one |

A system type module gives `id`, `idLen`, `defaultTx`, `newFields`, `decode`, `encode`, `identity` (the text the
Systems tab shows), `fields()`, `classes` (each entry class: its fields, `decode` and `encode` of the body, whether
its entries are control channels), `secret` (null or its fields and check), `ownedKeys` (records of the type's own
family deleted with the system) and `validate`.

Text a user reads goes in `site/i18n/*.json` under `editor.<mode>.*`. Add the module to `site/tests/modes.test.mjs`:
the vectors, a round trip byte for byte, and its validation.

## 5. Sharing

Zone sharing carries a kind's tag and its system's bytes as they are ([zone-share.md](../spec/zone-share.md) 4.2),
within its limits (a parameter block of 64 octets, a type's block of 200, entry bodies of 64), so a mode that keeps
within them needs nothing there. A mode whose channels refer to targets needs the target part of zone sharing first.

## 6. Docs

Add the switch to [feature-matrix.md](../feature-matrix.md), and the mode to the user manual's
[Programming the radio](../user/04-programming.md#digital-modes-and-systems) when users see something new.
