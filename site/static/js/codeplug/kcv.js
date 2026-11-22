// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Key check values (docs/spec/key-check.md), as the radio computes them (core/support/kcv.c): six hex digits that say
// two radios, or a radio and this editor, hold the same key without showing it. WebCrypto, so every function is async.

const LABEL = [0x52, 0x46, 0x4b, 0x43, 0x56, 0x31];	// "RFKCV1"
export const KCV_CLASS = { privacy: 1, traffic: 2 };

const hex3 = (b) => [...b.subarray(0, 3)].map((x) => x.toString(16).padStart(2, "0")).join("").toUpperCase();

// The hashed check value (2): SHA-256 of the label, the class, the two label octets, the key's length (2, big endian)
// and the key, its first three octets
export async function kcvHashed(cls, a, b, key) {
	const msg = Uint8Array.of(...LABEL, cls, a & 0xff, b & 0xff, (key.length >> 8) & 0xff, key.length & 0xff, ...key);
	return hex3(new Uint8Array(await crypto.subtle.digest("SHA-256", msg)));
}

// The block cipher check value (3): AES of a zero block under the key, the first three octets. One CBC block from a zero
// IV is that block's ECB encryption. null for a key that isn't 32 octets
export async function kcvAes(key) {
	if (key?.length !== 32) return null;
	const k = await crypto.subtle.importKey("raw", key, { name: "AES-CBC" }, false, ["encrypt"]);
	return hex3(new Uint8Array(await crypto.subtle.encrypt({ name: "AES-CBC", iv: new Uint8Array(16) }, k, new Uint8Array(16))));
}

// A privacy key slot: its type and value (four octets, big endian)
export const kcvPrivacy = (type, value) =>
	kcvHashed(KCV_CLASS.privacy, type, 0, Uint8Array.of(value >>> 24, value >>> 16, value >>> 8, value).map((x) => x & 0xff));

// A traffic key record's value (channel-modes.md 5.4: system, algorithm, key ID (2), flags, the key's octets) on a
// system of sysType. aes: the mode's keys are AES (its traffic spec says kcv: "aes")
export function kcvTrafficRecord(sysType, record, aes = false) {
	const key = record.subarray(5);
	return aes ? kcvAes(key) : kcvHashed(KCV_CLASS.traffic, sysType, record[1], key);
}
