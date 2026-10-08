// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Byte helpers shared by the flasher modules. Runs in browsers and in Node (site/tests).

export const hex = (n, width = 8) => "0x" + (n >>> 0).toString(16).toUpperCase().padStart(width, "0");

export function toHex(bytes) {
	let s = "";
	for (const b of bytes) s += b.toString(16).padStart(2, "0");
	return s;
}

export function fromHex(s) {
	const out = new Uint8Array(s.length / 2);
	for (let i = 0; i < out.length; i++) out[i] = parseInt(s.substr(2 * i, 2), 16);
	return out;
}

export async function sha256(bytes) {
	return toHex(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)));
}

export const u32le = (b, off) => (b[off] | (b[off + 1] << 8) | (b[off + 2] << 16) | (b[off + 3] << 24)) >>> 0;
export const u16le = (b, off) => b[off] | (b[off + 1] << 8);
export const u16be = (b, off) => (b[off] << 8) | b[off + 1];

export function le32(n) {
	return new Uint8Array([n & 0xFF, (n >>> 8) & 0xFF, (n >>> 16) & 0xFF, (n >>> 24) & 0xFF]);
}

export function be32(n) {
	return new Uint8Array([(n >>> 24) & 0xFF, (n >>> 16) & 0xFF, (n >>> 8) & 0xFF, n & 0xFF]);
}

export function concat(...parts) {
	const n = parts.reduce((a, p) => a + p.length, 0);
	const out = new Uint8Array(n);
	let o = 0;
	for (const p of parts) {
		out.set(p, o);
		o += p.length;
	}
	return out;
}

export function filled(n, value = 0xFF) {
	return new Uint8Array(n).fill(value);
}

export function startsWith(bytes, ascii) {
	for (let i = 0; i < ascii.length; i++) if (bytes[i] !== ascii.charCodeAt(i)) return false;
	return true;
}

export function equal(a, b) {
	if (a.length !== b.length) return false;
	for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
	return true;
}

let crcTable = null;
export function crc32(bytes, crc = 0) {
	if (!crcTable) {
		crcTable = new Uint32Array(256);
		for (let n = 0; n < 256; n++) {
			let c = n;
			for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
			crcTable[n] = c >>> 0;
		}
	}
	crc = ~crc >>> 0;
	for (let i = 0; i < bytes.length; i++) crc = crcTable[(crc ^ bytes[i]) & 0xFF] ^ (crc >>> 8);
	return ~crc >>> 0;
}

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export const ascii = (bytes) => new TextDecoder("latin1").decode(bytes);
