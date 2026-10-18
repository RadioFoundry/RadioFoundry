// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Field encodings of the open-firmware codeplug (docs/spec/open-firmware-cps.md 7.3): BCD frequencies, DMR IDs and
// tones, 0xFF-padded names, 24-bit locations. The C reference is core/codeplug/codeplug.c. Runs in browsers and Node.

// Packed BCD, most significant digit first in the word; null if a nibble is not 0-9
export function bcd(word, digits) {
	let v = 0;
	for (let i = digits - 1; i >= 0; i--) {
		const d = Math.floor(word / 16 ** i) % 16;
		if (d > 9) return null;
		v = v * 10 + d;
	}
	return v;
}

export function toBcd(v, digits) {
	let w = 0;
	for (let i = 0; i < digits; i++) {
		w += (v % 10) * 16 ** i;
		v = Math.floor(v / 10);
	}
	return w;
}

// Frequency: BCD of 10 Hz units as a LE word (438.500 MHz = bytes 00 00 85 43); null if malformed or 0
export function getFreq(b, off) {
	const w = (b[off] | (b[off + 1] << 8) | (b[off + 2] << 16) | (b[off + 3] << 24)) >>> 0;
	const v = bcd(w, 8);
	return v ? v * 10 : null;
}

export function putFreq(b, off, hz) {
	const w = toBcd(Math.round(hz / 10), 8);
	b[off] = w & 0xFF;
	b[off + 1] = (w >>> 8) & 0xFF;
	b[off + 2] = (w >>> 16) & 0xFF;
	b[off + 3] = (w >>> 24) & 0xFF;
}

// DMR ID or talkgroup: 8 BCD digits, bytes in reading order; 0 if malformed or above 24 bits
export function getBcdId(b, off) {
	const w = ((b[off] << 24) | (b[off + 1] << 16) | (b[off + 2] << 8) | b[off + 3]) >>> 0;
	const v = bcd(w, 8);
	return v === null || v > 0xFFFFFF ? 0 : v;
}

export function putBcdId(b, off, id) {
	const w = toBcd(id, 8);
	b[off] = (w >>> 24) & 0xFF;
	b[off + 1] = (w >>> 16) & 0xFF;
	b[off + 2] = (w >>> 8) & 0xFF;
	b[off + 3] = w & 0xFF;
}

// Tone as text: "" none, "88.5" CTCSS, "D023N" / "D023I" DCS normal / inverted. Malformed words read as none
export function getTone(b, off) {
	const v = b[off] | (b[off + 1] << 8);
	if (v === 0 || v === 0xFFFF) return "";
	if (v & 0x8000) {
		let code = "";
		for (let i = 2; i >= 0; i--) {
			const d = (v >> (4 * i)) & 0xF;
			if (d > 7) return "";
			code += d;
		}
		return "D" + code + (v & 0x4000 ? "I" : "N");
	}
	if (v & 0x4000) return "";
	const tenths = bcd(v, 4);
	if (tenths === null || tenths < 300 || tenths > 3000) return "";
	return (tenths / 10).toFixed(1);
}

// Normalized tone text, or null if it can't be a tone: accepts "", "none", "88.5", "D23", "D023", "D023N", "D023I"
export function parseTone(text) {
	const s = String(text ?? "").trim().toUpperCase();
	if (s === "" || s === "NONE" || s === "OFF") return "";
	let m = /^D?([0-7]{1,3})([NI])$/.exec(s) || /^D([0-7]{1,3})$/.exec(s);
	if (m) return "D" + m[1].padStart(3, "0") + (m[2] || "N");
	m = /^(\d{2,3})(?:\.(\d))?$/.exec(s);
	if (m) {
		const tenths = Number(m[1]) * 10 + Number(m[2] || 0);
		if (tenths >= 300 && tenths <= 3000) return (tenths / 10).toFixed(1);
	}
	return null;
}

export function putTone(b, off, text) {
	const t = parseTone(text) ?? "";
	let w = 0xFFFF;
	if (t.startsWith("D")) {
		w = 0x8000 | (t.endsWith("I") ? 0x4000 : 0) | (Number(t[1]) << 8) | (Number(t[2]) << 4) | Number(t[3]);
	} else if (t) {
		w = toBcd(Math.round(Number(t) * 10), 4);
	}
	b[off] = w & 0xFF;
	b[off + 1] = w >> 8;
}

// A name field: text up to the first 0x00 or 0xFF. Bytes are Latin-1, as the radio's font takes them
export function getName(b, off, len) {
	let s = "";
	for (let i = 0; i < len; i++) {
		const c = b[off + i];
		if (c === 0x00 || c === 0xFF) break;
		s += String.fromCharCode(c);
	}
	return s;
}

// The text, then 0xFF padding. Characters outside Latin-1 become '?'
export function putName(b, off, len, text) {
	b.fill(0xFF, off, off + len);
	const s = String(text ?? "");
	for (let i = 0; i < len && i < s.length; i++) {
		const c = s.charCodeAt(i);
		b[off + i] = c < 0x100 && c !== 0xFF ? c : 0x3F;
	}
}

// 24-bit coordinate (7.3.3): bit 23 sign (south / west), bits 22-15 whole degrees, bits 14-0 1/10000 degree
export function getCoord(hi, mid, lo) {
	const v = (hi << 16) | (mid << 8) | lo;
	const deg = (v >> 15) & 0xFF;
	const frac = Math.min(v & 0x7FFF, 9999);
	const x = deg + frac / 10000;
	return v & 0x800000 ? -x : x;
}

export function coordBytes(x) {
	const a = Math.min(Math.abs(Number(x) || 0), 255.9999);
	let deg = Math.floor(a);
	let frac = Math.round((a - deg) * 10000);
	if (frac > 9999) {
		deg++;
		frac = 0;
	}
	const v = (x < 0 ? 0x800000 : 0) | (deg << 15) | frac;
	return [(v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF];
}

// Letters without a decomposed form, spelled in plain Latin (the rest lose their accents through NFD)
const FOLD = { Ł: "L", ł: "l", Đ: "D", đ: "d", Ħ: "H", ħ: "h", ı: "i", Œ: "OE", œ: "oe", Æ: "AE", æ: "ae", Ø: "O", ø: "o", ß: "ss", Þ: "Th", þ: "th", Ð: "D", ð: "d" };
export const foldLetters = (s) => String(s ?? "").replace(/[ŁłĐđĦħıŒœÆæØøßÞþÐð]/g, (c) => FOLD[c]);

export const u16 = (b, off) => b[off] | (b[off + 1] << 8);

export function put16(b, off, v) {
	b[off] = v & 0xFF;
	b[off + 1] = (v >> 8) & 0xFF;
}

export function mhz(hz) {
	return hz == null ? "" : (hz / 1e6).toFixed(5).replace(/0{1,2}$/, "");
}

// "446.00625" or "446006250" (Hz) to Hz on the 10 Hz grid; null if it isn't a frequency below 1 GHz
export function parseFreq(text) {
	const s = String(text ?? "").trim();
	if (!/^\d+(\.\d+)?$/.test(s)) return null;
	const n = Number(s);
	const hz = n < 1e4 ? Math.round(n * 1e5) * 10 : Math.round(n / 10) * 10;
	return hz > 0 && hz < 1e9 ? hz : null;
}
