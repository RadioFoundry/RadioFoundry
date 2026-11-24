// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Byte helpers the channel-mode layer and its mode modules share (docs/spec/channel-modes.md): little-endian fields
// that read 0 past a short record's end, the name check, and record comparison.

import { putName } from "./fields.js";

export const u8 = (b, o) => (b && o < b.length ? b[o] : 0);
export const u16 = (b, o) => (b && o + 2 <= b.length ? b[o] | (b[o + 1] << 8) : 0);
export const u24 = (b, o) => (b && o + 3 <= b.length ? b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) : 0);
export const u32 = (b, o) => (b && o + 4 <= b.length ? (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0 : 0);
export const s8 = (b, o) => (u8(b, o) >= 0x80 ? u8(b, o) - 0x100 : u8(b, o));
export const put16 = (b, o, v) => {
	b[o] = v & 0xFF;
	b[o + 1] = (v >>> 8) & 0xFF;
};
export const put24 = (b, o, v) => {
	put16(b, o, v);
	b[o + 2] = (v >>> 16) & 0xFF;
};
export const put32 = (b, o, v) => {
	for (let i = 0; i < 4; i++) b[o + i] = Math.floor(v / 2 ** (8 * i)) & 0xFF;
};

// prev's bytes, at least len long (zeros added), or len zeros: a record written over the one stored keeps the bytes
// no field covers
export function grow(prev, len) {
	const v = new Uint8Array(Math.max(len, prev?.length ?? 0));
	if (prev) v.set(prev);
	return v;
}

export const sameBytes = (a, b) => a === b || (a != null && b != null && a.length === b.length && a.every((x, i) => x === b[i]));

// Two decoded records hold the same values: plain values, arrays, objects and byte arrays compared by content
export function sameValue(a, b) {
	if (a === b) return true;
	if (a instanceof Uint8Array || b instanceof Uint8Array) return a instanceof Uint8Array && b instanceof Uint8Array && sameBytes(a, b);
	if (Array.isArray(a)) return Array.isArray(b) && a.length === b.length && a.every((x, i) => sameValue(x, b[i]));
	if (a && b && typeof a === "object" && typeof b === "object") {
		const ka = Object.keys(a), kb = Object.keys(b);
		return ka.length === kb.length && ka.every((k) => sameValue(a[k], b[k]));
	}
	return false;
}

// The name check (2.2): FNV-1a 32 over the name as a channel record stores it (16 octets; from the first 0x00 or 0xFF
// on, 0x00), the halves XORed
export function nameCheck(name) {
	const r = new Uint8Array(16);
	putName(r, 0, 16, name);
	let h = 0x811C9DC5, end = false;
	for (let i = 0; i < 16; i++) {
		end = end || r[i] === 0x00 || r[i] === 0xFF;
		h = Math.imul(h ^ (end ? 0 : r[i]), 0x01000193) >>> 0;
	}
	return (h ^ (h >>> 16)) & 0xFFFF;
}

export const hex = (b) => [...(b ?? [])].map((x) => x.toString(16).padStart(2, "0")).join("").toUpperCase();
export function unhex(s) {
	s = String(s ?? "").replace(/[\s:-]/g, "");
	if (!/^([0-9A-Fa-f]{2})*$/.test(s)) return null;
	return Uint8Array.from({ length: s.length / 2 }, (_, i) => parseInt(s.slice(2 * i, 2 * i + 2), 16));
}
