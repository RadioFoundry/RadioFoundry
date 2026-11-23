// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The registry of channel kinds and system types (docs/spec/channel-modes.md 4) in the Codeplug Editor: the same rows
// as core/codeplug/codeplug_modes.h and the spec's tables (tools/check_modes.py checks all three). Each row names its
// mode module, which knows the kind's or the type's bytes and fields; the generic layer (channelmodes.js) and the
// pages know none. A row's feature is the firmware feature (the radio's probe) that runs it: with a radio connected, a
// mode is offered for a new channel only when the radio reports it or the codeplug already holds it; with none, every
// mode is offered (validation warns once a radio that lacks one connects).

import { t3Kind, t3System } from "./mode_t3.js";
import { p25Kind, p25TrunkKind, p25System } from "./mode_p25.js";
import { aesKind, aesSystem } from "./mode_aes.js";
import { nxdnKind, nxdnSystem } from "./mode_nxdn.js";

// id, the C token, the short name the radio shows, the feature that runs it, its module
export const KINDS = [
	{ id: 1, token: "DMR_TRUNK", short: "T3", feature: "DMR_TRUNK", module: t3Kind },
	{ id: 2, token: "P25", short: "P25", feature: "P25", module: p25Kind },
	{ id: 3, token: "P25_TRUNK", short: "P25T", feature: "P25_TRUNK", module: p25TrunkKind },
	{ id: 4, token: "NXDN", short: "NXDN", feature: "NXDN", module: nxdnKind },
	{ id: 5, token: "DMR_AES", short: "AES", feature: "DMR_AES", module: aesKind },
];

export const SYSTEM_TYPES = [
	{ id: 1, token: "DMR_TRUNK", short: "T3", feature: "DMR_TRUNK", module: t3System },
	{ id: 3, token: "P25_TRUNK", short: "P25T", feature: "P25_TRUNK", module: p25System },
	{ id: 4, token: "NXDN", short: "NXDN", feature: "NXDN", module: nxdnSystem },
	{ id: 5, token: "DMR_AES", short: "AES", feature: "DMR_AES", module: aesSystem },
];

export const kindRow = (id) => KINDS.find((k) => k.id === id) ?? null;
export const typeRow = (id) => SYSTEM_TYPES.find((k) => k.id === id) ?? null;
// The short name shown for a kind or a type: "?" for an id this editor doesn't know
export const kindName = (id) => kindRow(id)?.short ?? "?";
export const typeName = (id) => typeRow(id)?.short ?? "?";

const featureList = (features) => (features == null ? [] : Array.isArray(features) ? features : String(features).split(","));

// The kinds a new channel may take: those the radio's features name, and those the codeplug already has. Null features
// (no radio): all of them
export function offeredKinds(m, features) {
	if (features == null) return KINDS.slice();
	const have = featureList(features);
	return KINDS.filter((k) => have.includes(k.feature) || m.channels.some((c) => c.kind === k.id));
}

// The system types a new system may take, the same way
export function offeredTypes(m, features) {
	if (features == null) return SYSTEM_TYPES.slice();
	const have = featureList(features);
	const kinds = offeredKinds(m, features);
	return SYSTEM_TYPES.filter((s) => have.includes(s.feature) || kinds.some((k) => k.module.systemType === s.id) ||
		(m.systems ?? []).some((x) => x.type === s.id));
}

// Whether a radio with these features runs a kind (null features: no radio to ask)
export const radioRuns = (features, id) => features == null || featureList(features).includes(kindRow(id)?.feature);
