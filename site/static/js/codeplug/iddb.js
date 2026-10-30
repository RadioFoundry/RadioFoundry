// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DMR ID database: names for DMR IDs that aren't contacts, in one of two formats. Format N
// (docs/spec/open-firmware-cps.md 7.5): a 12-byte header, then records sorted by ID, each a 3-byte LE ID and text
// packed at 6 bits per character (4 characters in 3 bytes). Area 1 holds the header and as many whole records as fit in
// 256 KB; the rest continue at the start of area 2, up to the end of the radio's codeplug space. Format C
// (docs/spec/iddb-compact.md): the same areas as one database space of 4 KB pages, records in pages found through an
// index, names coded through a dictionary; about 40 % of format N, and written against the database it replaces so
// unchanged pages stay the same bytes. Image addresses as in image.js.

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
	if (h.length === HEADER && h[0] === 0x49 && h[1] === 0x64 && h[2] === 0x43 && h[3] === 0) return compactHeader(b);
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

// Records as the radio takes them: sorted by ID, the first of each ID kept, texts cleaned and cut to max
function prepare(records, max) {
	const seen = new Set();
	return records
		.filter((r) => r.id >= 1 && r.id <= 0xFFFFFF && !seen.has(r.id) && seen.add(r.id))
		.map((r) => ({ id: r.id, text: clean(r.text).slice(0, max).trim() }))
		.sort((a, b) => a.id - b.id);
}

// The database written into the image, which grows as needed. records: [{id, text}]; duplicates keep the first;
// texts are cleaned and cut to textLen (format N) or TEXT_MAX (C). Format C is written against a format C database
// already in the image (iddb-compact.md 4.2) unless fresh. Returns {count, top (the image address it reaches),
// dropped, format}
export function write(image, records, { format = "N", textLen = 32, end = Infinity, fresh = false } = {}) {
	if (format === "C") return writeCompact(image, records, { end, fresh });
	return writeN(image, records, { textLen, end });
}

function writeN(image, records, { textLen, end }) {
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
	return { count: n, top, dropped, rec, format: "N" };
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
	if (h?.format === "C") return readCompact(b);
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

// The image address just past the database (where a file or a read must reach to hold it whole), or 0 without one
export function extent(b) {
	const h = readHeader(b);
	if (!h) return 0;
	if (h.format === "C") return dbAddr(h.size - 1) + 1;
	return recAddr(h.count - 1, h.rec) + h.rec;
}

// The database written again in another format: the records read back, then written as format ("N": each text keeps
// the length the longest one needs, so none is cut). Returns write()'s answer, or null when the image holds no
// database, holds it in that format already, or in one read() can't give back (format n, other programs' formats):
// those are left as they are, never rewritten empty
export function convert(image, format, { end = Infinity } = {}) {
	const h = readHeader(image.bytes);
	if (!h || h.format === format || (h.format !== "N" && h.format !== "C")) return null;
	const records = read(image.bytes);
	const longest = records.reduce((n, r) => Math.max(n, r.text.length), 0);
	const textLen = TEXT_LENGTHS.find((n) => n >= longest) || TEXT_MAX;
	return write(image, records, { format, textLen, end, fresh: true });
}

// --- Format C (iddb-compact.md) ---

const PAGE = 4096;
const PAGE_FILL = 3686;		// a fresh page is closed at 90 %: room for the records a refresh adds
const C_HEADER = 32;
const DICT_MAX = 16512;
const LITERAL = 0xC0;		// + length; 0xC0 alone: empty
const SLACK = 1.25;		// the index and the dictionary get their size and a quarter

// Database offset -> image address
const dbAddr = (o) => (o < AREA1_SIZE ? AREA1 + o : AREA2 + (o - AREA1_SIZE));
// The database space a radio has: area 1, then area 2 up to end (an image address), in whole pages
const space = (end) => (end === Infinity ? Infinity : AREA1_SIZE + Math.max(0, Math.floor((end - AREA2) / PAGE) * PAGE));
const le = (b, o, n) => { let v = 0; for (let i = n - 1; i >= 0; i--) v = v * 256 + b[o + i]; return v; };
const put = (b, o, v, n) => { for (let i = 0; i < n; i++, v = Math.floor(v / 256)) b[o + i] = v & 0xFF; };

function compactHeader(b) {
	const h = b.subarray(AREA1, AREA1 + C_HEADER);
	if (h.length < C_HEADER || h[4] !== 1) return null;
	const count = le(h, 8, 4), size = le(h, 12, 4);
	if (!count || !size || size % PAGE) return null;
	return { format: "C", count, textLen: h[5], size, rec: null, first: le(h, 6, 2), index: le(h, 16, 4), pages: le(h, 20, 2),
		dict: le(h, 22, 2), table: le(h, 24, 4), strings: le(h, 28, 4) };
}

// n bytes at a database offset (they may run from area 1 into area 2)
function dbBytes(b, o, n) {
	const out = new Uint8Array(n);
	for (let i = 0; i < n; i++) out[i] = b[dbAddr(o + i)] ?? 0xFF;
	return out;
}

function pack6(text) {
	const out = new Uint8Array(Math.ceil((text.length * 6) / 8));
	for (let j = 0; j < text.length; j++) {
		const c = Math.max(0, ALPHABET.indexOf(text[j]));
		for (let k = 0; k < 6; k++) if (c & (0x20 >> k)) out[(6 * j + k) >> 3] |= 0x80 >> ((6 * j + k) & 7);
	}
	return out;
}

function unpack6(b, o, chars) {
	let s = "";
	for (let j = 0; j < chars; j++) {
		let c = 0;
		for (let k = 6 * j; k < 6 * j + 6; k++) c = (c << 1) | ((b[o + (k >> 3)] >> (7 - (k & 7))) & 1);
		s += ALPHABET[c];
	}
	return s;
}

// A text's three tokens (3.4): the encoder's split at the first two spaces
function slots(text) {
	const a = text.indexOf(" ");
	if (a < 0) return [text, "", ""];
	const b = text.indexOf(" ", a + 1);
	return b < 0 ? [text.slice(0, a), text.slice(a + 1), ""] : [text.slice(0, a), text.slice(a + 1, b), text.slice(b + 1)];
}

const literalSize = (s) => 1 + Math.ceil((s.length * 6) / 8);
const stringSize = literalSize;

function varint(v, out) {
	while (v >= 0x80) {
		out.push((v & 0x7F) | 0x80);
		v = Math.floor(v / 128);
	}
	out.push(v);
}

function token(s, codes, out) {
	if (!s) return out.push(LITERAL);
	const k = codes.get(s);
	if (k === undefined) {
		out.push(LITERAL + s.length);
		for (const x of pack6(s)) out.push(x);
	} else if (k < 128) out.push(k);
	else out.push(0x80 + ((k - 128) >> 8), (k - 128) & 0xFF);
}

// One record's bytes: the ID (whole, or the gap from prev), then its three tokens
function encodeRecord(r, prev, codes) {
	const out = [];
	varint(prev === null ? r.id : r.id - prev, out);
	for (const t of r.slots) token(t, codes, out);
	return out;
}

// A record page's bytes (count, then records), or null when they don't fit limit
function encodePage(records, codes, limit = PAGE) {
	const out = [records.length & 0xFF, records.length >> 8];
	records.forEach((r, i) => out.push(...encodeRecord(r, i ? records[i - 1].id : null, codes)));
	return out.length > limit ? null : out;
}

// The strings worth a dictionary entry (4.1 rule 2), most used first; known: entries that stay, never offered again
function candidates(list, known = new Set()) {
	const uses = new Map();
	for (const r of list) for (const s of r.slots) if (s && !known.has(s)) uses.set(s, (uses.get(s) || 0) + 1);
	return [...uses].filter(([s, n]) => n * literalSize(s) > n * 2 + stringSize(s) + 3)
		.sort((x, y) => y[1] - x[1] || (x[0] < y[0] ? -1 : x[0] > y[0] ? 1 : 0)).map(([s]) => s);
}

// What a format C database in the image holds: header, dictionary, index ({first, page})
function parseCompact(b) {
	const h = readHeader(b);
	if (h?.format !== "C") return null;
	const table = dbBytes(b, h.table, 3 * h.dict);
	const dict = [];
	for (let k = 0; k < h.dict; k++) {
		const o = h.strings + le(table, 3 * k, 3);
		const n = b[dbAddr(o)];
		dict.push(unpack6(dbBytes(b, o + 1, Math.ceil((n * 6) / 8)), 0, n));
	}
	const ix = dbBytes(b, h.index, 5 * h.pages);
	const index = [];
	for (let i = 0; i < h.pages; i++) index.push({ first: le(ix, 5 * i, 3), page: le(ix, 5 * i + 3, 2) });
	return { h, dict, index };
}

function readCompact(b) {
	const db = parseCompact(b);
	if (!db) return [];
	const out = [];
	for (const { page } of db.index) {
		const p = b.subarray(dbAddr(page * PAGE), dbAddr(page * PAGE) + PAGE);
		const count = p[0] | (p[1] << 8);
		let i = 2, id = 0;
		for (let n = 0; n < count && i < PAGE; n++) {
			let v = 0, shift = 1;
			for (;;) {
				const x = p[i++];
				v += (x & 0x7F) * shift;
				shift *= 128;
				if (!(x & 0x80)) break;
			}
			id = n === 0 ? v : id + v;
			const parts = [];
			for (let k = 0; k < 3; k++) {
				const t = p[i++];
				if (t < 0x80) parts.push(db.dict[t]);
				else if (t < 0xC0) parts.push(db.dict[128 + ((t - 0x80) << 8) + p[i++]]);
				else if (t > 0xC0) {
					parts.push(unpack6(p, i, t - 0xC0));
					i += Math.ceil(((t - 0xC0) * 6) / 8);
				}
			}
			out.push({ id, text: parts.filter((s) => s).join(" ") });
		}
	}
	return out;
}

// Records into pages: fresh (each closed at PAGE_FILL), or into the old pages' ranges (one that overflows splits in
// two, the second half to a new page from firstNew on). Returns [{page, records}]
function paginate(list, codes, old, firstNew) {
	const pages = [];
	let next = firstNew;
	if (!old) {
		let cur = [], used = 2;
		for (const r of list) {
			const n = encodeRecord(r, cur.length ? cur[cur.length - 1].id : null, codes).length;
			if (cur.length && used + n > PAGE_FILL) {
				pages.push({ page: next++, records: cur });
				cur = [];
				used = 2 + encodeRecord(r, null, codes).length;
			} else used += n;
			cur.push(r);
		}
		if (cur.length) pages.push({ page: next++, records: cur });
		return pages;
	}
	const groups = old.map((e) => ({ page: e.page, records: [] }));
	let g = 0;
	for (const r of list) {
		while (g + 1 < old.length && old[g + 1].first <= r.id) g++;
		groups[g].records.push(r);
	}
	const place = (records, page) => {
		if (records.length < 2 || encodePage(records, codes)) return pages.push({ page, records });
		const half = Math.ceil(records.length / 2);
		place(records.slice(0, half), page);
		place(records.slice(half), next++);
	};
	for (const grp of groups) if (grp.records.length) place(grp.records, grp.page);
	return pages;
}

const pagesFor = (n) => Math.max(1, Math.ceil((n * SLACK) / PAGE));

// Where everything goes: {dict, codes, offsets (each string's, from the strings section), sbytes, at (each section's
// first page), pages}. Against old: its sections and pages kept, or null when the index or dictionary outgrew its room
function layout(list, old) {
	const dict = old ? old.dict.slice() : [];
	for (const s of candidates(list, new Set(dict))) {
		if (dict.length >= DICT_MAX) break;
		dict.push(s);
	}
	const codes = new Map(dict.map((s, k) => [s, k]));
	const offsets = [];
	let sbytes = 0;
	for (const s of dict) {
		offsets.push(sbytes);
		sbytes += stringSize(s);
	}
	if (old) {
		const h = old.h;
		const at = { index: h.index / PAGE, table: h.table / PAGE, strings: h.strings / PAGE, records: h.first };
		const pages = paginate(list, codes, old.index, h.size / PAGE);
		if (5 * pages.length > (at.table - at.index) * PAGE || 3 * dict.length > (at.strings - at.table) * PAGE ||
			sbytes > (at.records - at.strings) * PAGE) return null;
		return { dict, codes, offsets, sbytes, at, pages };
	}
	const first = paginate(list, codes, null, 0);
	const at = { index: 1 };
	at.table = at.index + pagesFor(5 * first.length);
	at.strings = at.table + pagesFor(3 * dict.length);
	at.records = at.strings + pagesFor(sbytes);
	return { dict, codes, offsets, sbytes, at, pages: first.map((p) => ({ page: at.records + p.page, records: p.records })) };
}

// Pages the layout reaches
const extentOf = (L) => Math.max(L.at.records, ...L.pages.map((p) => p.page + 1));

function writeCompact(image, records, { end = Infinity, fresh = false } = {}) {
	let list = prepare(records, TEXT_MAX).map((r) => ({ ...r, slots: slots(r.text) }));
	const room = space(end);
	const old = fresh ? null : parseCompact(image.bytes);
	let L = old ? layout(list, old) : null;
	if (!L || extentOf(L) * PAGE > room) L = layout(list, null);
	// too big for the radio: the top of the ID range goes, as with format N
	let dropped = 0;
	while (extentOf(L) * PAGE > room && list.length) {
		const keep = Math.min(list.length - 1, Math.floor((list.length * room * 0.995) / (extentOf(L) * PAGE)));
		dropped += list.length - keep;
		list = list.slice(0, keep);
		L = layout(list, null);
	}
	const size = extentOf(L);
	image.ensure(dbAddr(size * PAGE - 1) + 1);
	const b = image.bytes;
	const page = (p) => b.subarray(dbAddr(p * PAGE), dbAddr(p * PAGE) + PAGE);
	const section = (from, to, bytes) => {
		for (let p = from; p < to; p++) page(p).fill(0xFF);
		bytes.forEach((x, i) => { b[dbAddr(from * PAGE + i)] = x; });
	};
	const sorted = L.pages.slice().sort((x, y) => x.records[0].id - y.records[0].id);
	const index = [];
	for (const p of sorted) {
		const id = p.records[0].id;
		index.push(id & 0xFF, (id >> 8) & 0xFF, id >> 16, p.page & 0xFF, p.page >> 8);
	}
	const table = [];
	for (const o of L.offsets) table.push(o & 0xFF, (o >> 8) & 0xFF, o >> 16);
	const strs = [];
	for (const s of L.dict) strs.push(s.length, ...pack6(s));
	section(L.at.index, L.at.table, index);
	section(L.at.table, L.at.strings, table);
	section(L.at.strings, L.at.records, strs);
	for (const p of L.pages) {
		const dst = page(p.page);
		dst.fill(0xFF);
		dst.set(encodePage(p.records, L.codes));
	}
	const h = page(0);
	h.fill(0xFF);
	h.set([0x49, 0x64, 0x43, 0, 1, list.reduce((n, r) => Math.max(n, r.text.length), 0)]);
	put(h, 6, L.at.records, 2);
	put(h, 8, list.length, 4);
	put(h, 12, size * PAGE, 4);
	put(h, 16, L.at.index * PAGE, 4);
	put(h, 20, sorted.length, 2);
	put(h, 22, L.dict.length, 2);
	put(h, 24, L.at.table * PAGE, 4);
	put(h, 28, L.at.strings * PAGE, 4);
	return { count: list.length, top: dbAddr(size * PAGE - 1) + 1, dropped, format: "C" };
}

// About how many bytes format C takes for these records, and how many fit before end: from an even sample, written
// fresh (a larger set shares its dictionary more, so the answer errs large)
export function estimateCompact(records, end = Infinity) {
	const list = prepare(records, TEXT_MAX);
	if (!list.length) return { bytes: 0, fit: 0 };
	// the whole list: a sample makes names that repeat look unique (every k-th record keeps a tenth of a name's uses),
	// and overstated the size by half
	const L = layout(list.map((r) => ({ ...r, slots: slots(r.text) })), null);
	const body = (L.sbytes + 3 * L.dict.length + L.pages.length * PAGE) / list.length;
	const bytes = PAGE + Math.ceil(body * list.length);
	const room = space(end);
	return { bytes, fit: room === Infinity ? Infinity : Math.max(0, Math.floor((room - PAGE * 4) / body)) };
}
