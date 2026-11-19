// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
import { grow, hex, unhex } from "./modebytes.js";

export const AES_KIND = 5, AES_ALG = 0x25;
export const trafficRecord = (v) => v?.length === 37 && v[0] !== 255 && v[1] === AES_ALG && v[3] === 0;
export const readTraffic = (v) => ({ id: v[2], key: hex(v.subarray(5)) });
export function writeTraffic(system, item, prev) {
	const bytes = unhex(item.key);
	if (bytes?.length !== 32 || !Number.isInteger(item.id) || item.id < 0 || item.id > 255)
		throw new Error("Invalid AES traffic key");
	const v = grow(prev, 37);
	v.set([system, AES_ALG, item.id, 0]);
	v.set(bytes, 5);
	return v;
}

export const aesKind = {
	id: AES_KIND, feature: "DMR_AES", systemType: AES_KIND, contacts: true, ownFreqs: true, defaultTx: true,
	recordFields: ["contact", "rxList", "dmrId", "colorCode", "timeslot", "dmo", "noBeep", "aliasTs1", "aliasTs2"],
	flags: {}, newParams: () => ({ id: null }),
	decode: (v) => ({ id: v?.length >= 2 && v[0] === AES_ALG ? v[1] : null }),
	encode: (p, prev) => {
		const v = grow(prev, 2);
		v.set([p.id == null ? 0 : AES_ALG, p.id ?? 0]);
		return v;
	},
	fields: () => [{ key: "id", label: "editor.aes.id", type: "select", options: (r) => {
		const ids = new Set((r?.system?.trafficKeys ?? []).map((k) => k.id));
		if (r?.params?.id != null) ids.add(r.params.id);
		return [[null, "editor.none"], ...[...ids].sort((a, b) => a - b).map((id) => [id, String(id)])];
	} }],
	validate(c, p, problem) {
		if (!c.system?.trafficKeys?.some((k) => k.id === p.id)) problem.warn("system", "mode_no_secret");
	},
};

export const aesSystem = {
	id: AES_KIND, feature: "DMR_AES", idLen: 1, defaultTx: true,
	newFields: () => ({ network: 1 }), decode: (v) => ({ network: v?.[0] ?? 1 }),
	encode: (p, prev) => { const v = grow(prev, 1); v[0] = p.network; return v; },
	identity: (p) => String(p.network), classes: [], ownedKeys: () => [], validate() {},
	fields: () => [{ key: "network", label: "editor.aes.network", type: "int", min: 0, max: 255 }],
};
