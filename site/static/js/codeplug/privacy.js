// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AES provisioning and per-channel TX policy in RFKV records, independent of vendor channel bytes.
import { decodeArea, putArea, KV_ADDR } from "./kv.js";
export const AES_KEYS_KEY = 0x0007;
export const CHANNEL_PRIVACY_KEY = 0x0008;
export const AES_SLOTS = 2;
const KEY_BYTES = 68, MAP_BYTES = 2049;
export const validAesKey = (k) => k == null || (Number.isInteger(k.id) && k.id >= 0 && k.id <= 255 && /^[0-9a-f]{64}$/i.test(k.key));

export function decodePrivacy(bytes) {
	const area = decodeArea(bytes);
	const get = (key) => area.entries.find((e) => e.key === key)?.value;
	const keys = get(AES_KEYS_KEY), map = get(CHANNEL_PRIVACY_KEY);
	const keyInvalid = !!keys && (keys.length < KEY_BYTES || keys[0] !== 1);
	const aesKeys = !keys || keyInvalid ? null : Array.from({ length: AES_SLOTS }, (_, i) => (keys[1] & (1 << i)) ? {
		id: keys[2 + 33 * i], key: [...keys.subarray(3 + 33 * i, 35 + 33 * i)].map((x) => x.toString(16).padStart(2, "0")).join(""),
	} : null);
	const magic = [0x52, 0x46, 0x4B, 0x56].every((x, i) => bytes[KV_ADDR + i] === x);
	const mapInvalid = area.state === "newer" || (area.state === "bad" && magic) || !!map && (map.length < MAP_BYTES || map[0] !== 1);
	return { aesKeys, keyInvalid, mapInvalid, channel(number) {
		if (mapInvalid) return { privacyMode: 3, privacyId: 0 };
		const at = 1 + 2 * (number - 1);
		return { privacyMode: map?.[at] ?? 0, privacyId: map?.[at + 1] ?? 0 };
	} };
}

const normalized = (keys) => keys == null ? null : Array.from({ length: AES_SLOTS }, (_, i) => keys[i] ? { id: keys[i].id, key: keys[i].key.toLowerCase() } : null);

export function encodePrivacy(model, bytes) {
	const area = decodeArea(bytes), was = decodePrivacy(bytes);
	let entries = area.entries.slice(), changed = false;
	const put = (key, value) => {
		const previous = entries.find((e) => e.key === key)?.value;
		if (value && previous?.length > value.length) {
			const extended = previous.slice(); extended.set(value); value = extended;
		}
		entries = entries.filter((e) => e.key !== key);
		if (value) entries.push({ key, value });
		changed = true;
	};
	const keys = model.general.aesKeys ?? null;
	if (keys && (!Array.isArray(keys) || keys.length > AES_SLOTS || keys.some((k) => !validAesKey(k)))) throw new RangeError("invalid AES key ring");
	if (JSON.stringify(normalized(keys)) !== JSON.stringify(normalized(was.aesKeys)) || was.keyInvalid && !model.general.aesInvalid) {
		if (keys && (keys.length > AES_SLOTS || keys.some((k) => !validAesKey(k)) || keys[0] && keys[1] && keys[0].id === keys[1].id))
			throw new RangeError("invalid AES key ring");
		let record = null;
		if (keys) {
			record = new Uint8Array(KEY_BYTES); record[0] = 1;
			record[1] = (area.entries.find((e) => e.key === AES_KEYS_KEY)?.value[1] || 0) & ~3;
			keys.forEach((k, i) => {
				if (!k) return;
				record[1] |= 1 << i; record[2 + 33 * i] = k.id;
				for (let j = 0; j < 32; j++) record[3 + 33 * i + j] = parseInt(k.key.slice(2 * j, 2 * j + 2), 16);
			});
		}
		put(AES_KEYS_KEY, record);
	}
	const record = new Uint8Array(MAP_BYTES); record[0] = 1;
	let mapChanged = false, needed = false;
	const channels = new Map(model.channels.filter((c) => c.number).map((c) => [c.number, c]));
	for (let n = 1; n <= 1024; n++) {
		const c = channels.get(n), mode = c?.privacyMode ?? 0, id = mode === 2 ? c.privacyId : 0;
		const old = was.channel(n);
		if (c || !was.mapInvalid) mapChanged ||= mode !== old.privacyMode || id !== (old.privacyMode === 2 ? old.privacyId : 0);
		needed ||= mode !== 0;
		record[2 * n - 1] = mode; record[2 * n] = id;
	}
	if (mapChanged) {
		for (const c of channels.values()) if (!Number.isInteger(c.privacyMode ?? 0) || (c.privacyMode ?? 0) < 0 || (c.privacyMode ?? 0) > 2 ||
			c.privacyMode === 2 && (!Number.isInteger(c.privacyId) || c.privacyId < 0 || c.privacyId > 255)) throw new RangeError("invalid channel privacy");
		put(CHANNEL_PRIVACY_KEY, needed ? record : null);
	}
	if (changed && !putArea(bytes, entries)) throw new RangeError("newer codeplug settings area");
}

export const hasPrivacy = (model) => model.general.aesKeys != null || model.general.aesInvalid || model.channels.some((c) => c.privacyMode);
