// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DMR AES-256 mode module of the Codeplug Editor (docs/spec/channel-modes.md): kind 5 and system type 5. The keys
// are the system's traffic keys (algorithm 0x25), on the shared traffic-key model in channelmodes.js.
import { grow, hex, unhex } from "./modebytes.js";

export const AES_KIND = 5, AES_ALG = 0x25, AES_KEY_BYTES = 32;
const RECORD_LEN = 5 + AES_KEY_BYTES;

// A fresh key from the browser's CSPRNG: typed keys tend to be weak
export function randomAesKey(rng = globalThis.crypto) {
	return hex(rng.getRandomValues(new Uint8Array(AES_KEY_BYTES)));
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
	id: AES_KIND, feature: "DMR_AES", idLen: 1, conventional: true, defaultTx: true,
	sharingHelp: "editor.aes.sharing_help",
	newFields: () => ({ network: 1 }), decode: (v) => ({ network: v?.[0] ?? 1 }),
	encode: (p, prev) => { const v = grow(prev, 1); v[0] = p.network; return v; },
	identity: (p) => String(p.network), classes: [], ownedKeys: () => [], validate() {},
	fields: () => [{ key: "network", label: "editor.aes.network", type: "int", min: 0, max: 255 }],
	// Record: system, algorithm, key ID (u16, the PI header's 8 bits), flags, the 32 key bytes
	traffic: {
		max: 256,
		labels: { title: "editor.aes.keys", add: "editor.aes.add", show: "editor.aes.show_keys" },
		columns: [
			{ key: "id", label: "editor.aes.id", type: "int", min: 0, max: 255 },
			{ key: "key", label: "editor.aes.key", type: "hexbytes", bytes: AES_KEY_BYTES, secret: true,
				generate: () => randomAesKey() },
		],
		decode(v) {
			if (v.length !== RECORD_LEN || v[1] !== AES_ALG || v[3] !== 0) return null;
			return { id: v[2], key: hex(v.subarray(5)), flags: v[4] };
		},
		encode(system, { id, key, flags = 0 }) {
			const bytes = unhex(key);
			if (bytes?.length !== AES_KEY_BYTES || !Number.isInteger(id) || id < 0 || id > 255)
				throw new Error("Invalid AES traffic key");
			const v = new Uint8Array(RECORD_LEN);
			v.set([system, AES_ALG, id, 0, flags]);
			v.set(bytes, 5);
			return v;
		},
	},
};
