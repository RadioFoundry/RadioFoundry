// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The DMR ID database (docs/spec/open-firmware-cps.md 7.5): names for DMR IDs that aren't contacts. Written in
// format N: a 12-byte header, then records sorted by ID, each a 3-byte LE ID and text packed at 6 bits per character
// (4 characters in 3 bytes). Area 1 holds the header and as many whole records as fit in 256 KB; the rest continue at
// the start of area 2, up to the end of the radio's codeplug space. Image addresses as in image.js.

import { FL } from "./image.js";
import { foldLetters } from "./fields.js";

export const AREA1 = FL + 0x30000;
export const AREA1_SIZE = 0x40000;
export const AREA2 = FL + 0xB8000;
export const HEADER = 12;
export const TEXT_MAX = 48;	// DMR_IDDB_TEXT_MAX: the radio keeps no more
export const TEXT_LENGTHS = [16, 24, 32, 48];
const ALPHABET = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz.";

export const recordSize = (textLen) => 3 + (textLen * 3) / 4;

// Text the 6-bit alphabet can hold: accents dropped (é -> e), anything else outside it a space, spaces collapsed
export function clean(text) {
	return foldLetters(text)
		.normalize("NFD").replace(/[̀-ͯ]/g, "")
		.replace(/[^ 0-9A-Za-z.]/g, " ")
		.replace(/ +/g, " ").trim();
}

// Records that fit between AREA1 and end (image address), for a record size
export function capacity(rec, end) {
	const per1 = Math.floor((AREA1_SIZE - HEADER) / rec);
	return per1 + Math.max(0, Math.floor((end - AREA2) / rec));
}

// The header as stored, or null without a database
export function readHeader(b) {
	const h = b.subarray(AREA1, AREA1 + HEADER);
	if (h.length < HEADER || h[0] !== 0x49 || h[1] !== 0x64 || h[3] < 0x4A) return null;
	const count = (h[8] | (h[9] << 8) | (h[10] << 16) | (h[11] << 24)) >>> 0;
	const packed = h[2] === 0x4E || h[2] === 0x6E;
	const rec = h[3] - 0x4A;
	if (!count || rec <= (packed ? 3 : 4)) return null;
	// Whole characters only: a record size another program chose (qdmr takes any) can end in spare bits, as the
	// radio's decoder (dmr_iddb.c) rounds down too
	return { format: String.fromCharCode(h[2]), rec, count, textLen: packed ? Math.floor(((rec - 3) * 4) / 3) : rec - 4 };
}

const recAddr = (i, rec) => {
	const per1 = Math.floor((AREA1_SIZE - HEADER) / rec);
	return i < per1 ? AREA1 + HEADER + i * rec : AREA2 + (i - per1) * rec;
};

// The database written into the image, which grows as needed. records: [{id, text}]; duplicates keep the first;
// texts are cleaned and cut to textLen. Returns {count, bytes (the image span it reaches), dropped}
export function write(image, records, { textLen = 32, end = Infinity } = {}) {
	if (!TEXT_LENGTHS.includes(textLen)) throw new Error(`text length ${textLen}: one of ${TEXT_LENGTHS.join(", ")}`);
	const rec = recordSize(textLen);
	const seen = new Set();
	let list = records
		.filter((r) => r.id >= 1 && r.id <= 0xFFFFFF && !seen.has(r.id) && seen.add(r.id))
		.sort((a, b) => a.id - b.id);
	const fit = capacity(rec, end);
	const dropped = Math.max(0, list.length - fit);
	if (dropped) list = list.slice(0, fit);
	const top = list.length ? recAddr(list.length - 1, rec) + rec : AREA1 + HEADER;
	image.ensure(top);
	const b = image.bytes;
	const h = b.subarray(AREA1, AREA1 + HEADER);
	h.set([0x49, 0x64, 0x4E, rec + 0x4A, 0x30, 0x30, 0x31, 0x00]);
	const n = list.length;
	h.set([n & 0xFF, (n >> 8) & 0xFF, (n >> 16) & 0xFF, (n >>> 24) & 0xFF], 8);
	list.forEach((r, i) => packRecord(b, recAddr(i, rec), r.id, clean(r.text), textLen));
	return { count: n, top, dropped, rec };
}

// No database: the header's tag erased (the radio then ignores the records)
export function erase(image) {
	image.bytes.fill(0xFF, AREA1, AREA1 + HEADER);
}

function packRecord(b, a, id, text, textLen) {
	b[a] = id & 0xFF;
	b[a + 1] = (id >> 8) & 0xFF;
	b[a + 2] = (id >> 16) & 0xFF;
	const t = text.slice(0, textLen).padEnd(textLen, " ");
	for (let k = 0; k < textLen / 4; k++) {
		let w = 0;
		for (let j = 0; j < 4; j++) w = (w << 6) | Math.max(0, ALPHABET.indexOf(t[4 * k + j]));
		b[a + 3 + 3 * k] = (w >> 16) & 0xFF;
		b[a + 4 + 3 * k] = (w >> 8) & 0xFF;
		b[a + 5 + 3 * k] = w & 0xFF;
	}
}

// Every record of a format N database, for a preview or a check: [{id, text}]
export function read(b) {
	const h = readHeader(b);
	if (!h || (h.format !== "N" && h.format !== "n")) return [];
	if (h.format === "n") return [];	// area 2 in the voice prompt area: not written by the editor
	const out = [];
	for (let i = 0; i < h.count; i++) {
		const a = recAddr(i, h.rec);
		if (a + h.rec > b.length) break;
		const id = b[a] | (b[a + 1] << 8) | (b[a + 2] << 16);
		// Bit by bit, most significant first: textLen need not be a multiple of 4
		let text = "";
		for (let j = 0; j < h.textLen; j++) {
			let c = 0;
			for (let k = 6 * j; k < 6 * j + 6; k++) c = (c << 1) | ((b[a + 3 + (k >> 3)] >> (7 - (k & 7))) & 1);
			text += ALPHABET[c];
		}
		out.push({ id, text: text.replace(/ +$/, "") });
	}
	return out;
}
