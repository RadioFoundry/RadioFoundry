// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The codeplug settings area (docs/spec/codeplug-settings.md): keyed values in one 4 KB sector of the EEPROM image,
// for settings the open-firmware layout has no field for. Entries are {key, value: Uint8Array}, in the area's order.
// A writer keeps the keys it doesn't know byte for byte and never writes over an area of a version it doesn't know.

import { EE } from "./image.js";
import { crc32 } from "../flasher/bytes.js";

export const KV_ADDR = EE + 0x0E000;
export const KV_SIZE = 0x1000;
export const KV_VERSION = 1;
export const KV_HDR = 16;	// the header length this version writes
const MAGIC = [0x52, 0x46, 0x4B, 0x56];	// "RFKV"

// The highest DMR ID a key may hold (CODEPLUG_DMR_ID_MAX); above it the key reads as absent
export const ID_MAX = 16776415;

// The scalar keys the editor knows (3), one row each: the codeplug model's field, the key, its type (3.1: uint, int,
// bool, enum or id), the bytes a writer stores and the setting's range. A key's meaning never changes. A new key is
// a row here, a row in the spec's table and a field on the Radio tab; decoding, encoding and validation follow the row
export const KEYS = [
	{ name: "arsServer", key: 0x0001, type: "id", size: 4, min: 0, max: ID_MAX },
	{ name: "ipGateway", key: 0x0002, type: "id", size: 4, min: 0, max: ID_MAX },
	{ name: "callEndTone", key: 0x0003, type: "bool", size: 1, min: 0, max: 1 },
	{ name: "totWarnTone", key: 0x0004, type: "bool", size: 1, min: 0, max: 1 },
	{ name: "batteryTone", key: 0x0005, type: "bool", size: 1, min: 0, max: 1 },
	{ name: "txDenyTone", key: 0x0006, type: "bool", size: 1, min: 0, max: 1 },
];
export const KEY = Object.fromEntries(KEYS.map((k) => [k.name, k.key]));
const SCALAR_MAX = 8;	// a scalar's widest value

const u16 = (b, o) => b[o] | (b[o + 1] << 8);
const u32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;

// The area in an image's bytes: {state, entries}. state: "valid"; "empty" (an erased sector); "bad" (anything else
// that isn't a valid area, version 0 too); "newer" (our magic with a later version). Only a valid area has entries
export function decodeArea(bytes) {
	const s = bytes.subarray(KV_ADDR, KV_ADDR + KV_SIZE);
	if (s.length < KV_SIZE) return { state: "empty", entries: [] };
	if (!MAGIC.every((x, i) => s[i] === x)) return { state: s.every((x) => x === 0xFF) ? "empty" : "bad", entries: [] };
	// no version 0 was ever written: a damaged byte, not a later version's area that must be left alone
	if (s[4] !== KV_VERSION) return { state: s[4] > KV_VERSION ? "newer" : "bad", entries: [] };
	const hdr = s[5], len = u16(s, 6);
	if (hdr < KV_HDR || hdr + len > KV_SIZE) return { state: "bad", entries: [] };
	const data = s.subarray(hdr, hdr + len);
	if (crc32(data) !== u32(s, 8)) return { state: "bad", entries: [] };
	const entries = [];
	for (let o = 0; o < len;) {
		if (o + 4 > len) return { state: "bad", entries: [] };
		const key = u16(data, o), n = u16(data, o + 2);
		if (o + 4 + n > len) return { state: "bad", entries: [] };
		entries.push({ key, value: data.slice(o + 4, o + 4 + n) });
		o += 4 + n;
	}
	return { state: "valid", entries };
}

// A key's value: the first entry's (4, "one entry per key"), or null when absent
export function getValue(entries, key) {
	return entries.find((e) => e.key === key)?.value ?? null;
}

// Entries with key set to value: replaced in place, appended when new; later entries of the key dropped
export function setValue(entries, key, value) {
	const out = [];
	let done = false;
	for (const e of entries) {
		if (e.key !== key) out.push(e);
		else if (!done) {
			out.push({ key, value: Uint8Array.from(value) });
			done = true;
		}
	}
	if (!done) out.push({ key, value: Uint8Array.from(value) });
	return out;
}

// Entries without key (every entry of it)
export function deleteValue(entries, key) {
	return entries.filter((e) => e.key !== key);
}

// Entries as a writer keeps them: the first of each key, in order
export function dedupe(entries) {
	const seen = new Set();
	return entries.filter((e) => !seen.has(e.key) && seen.add(e.key));
}

// Whether v is a value row's setting takes: a bool is true or false, the others an integer in the row's range
export function keyValid(row, v) {
	return row.type === "bool" ? typeof v === "boolean" : Number.isInteger(v) && v >= row.min && v <= row.max;
}

// A scalar key's setting as row describes it: a number, or true or false for a bool; null when the key is absent or
// holds a value the setting can't take (empty, longer than a scalar, past 32 bits, outside the range), which reads as
// absent (3.1). A shorter value zero-extends, an int sign-extends from its stored length
export function getKey(entries, row) {
	const v = getValue(entries, row.key);
	if (!v || v.length === 0 || v.length > SCALAR_MAX) return null;
	if (row.type === "bool") return v.some((x) => x !== 0);
	const fill = row.type === "int" && (v[v.length - 1] & 0x80) ? 0xFF : 0;
	let n = 0;
	for (let i = 0; i < 4; i++) n += (i < v.length ? v[i] : fill) * 2 ** (8 * i);
	for (let i = 4; i < v.length; i++) if (v[i] !== fill) return null;
	if (row.type === "int") {
		if (v.length > 4 && (n >= 2 ** 31) !== (fill !== 0)) return null;
		if (n >= 2 ** 31) n -= 2 ** 32;
	}
	return keyValid(row, n) ? n : null;
}

// The value a writer stores for a scalar: row.size bytes, little endian. A scalar is written whole (4)
export function keyValue(row, v) {
	const n = row.type === "bool" ? (v ? 1 : 0) : v;
	return Uint8Array.from({ length: row.size }, (_, i) => Math.floor(n / 2 ** (8 * i)) & 0xFF);
}

export function idValue(id) {
	return Uint8Array.of(id & 0xFF, (id >>> 8) & 0xFF, (id >>> 16) & 0xFF, (id >>> 24) & 0xFF);
}

// The sector for entries (duplicates dropped): header, entries, 0xFF to the end. Throws when they don't fit. prev, a
// valid area's sector, gives its header length and the header bytes past the CRC, kept as found (2.4: a later
// version's fields and the continuation); a new area has them 0
export function encodeArea(entries, prev = null) {
	entries = dedupe(entries);
	const hdr = prev ? prev[5] : KV_HDR;
	const len = entries.reduce((n, e) => n + 4 + e.value.length, 0);
	if (hdr + len > KV_SIZE) throw new RangeError("codeplug settings area full");
	const s = new Uint8Array(KV_SIZE).fill(0xFF);
	let o = hdr;
	for (const e of entries) {
		s[o] = e.key & 0xFF;
		s[o + 1] = e.key >> 8;
		s[o + 2] = e.value.length & 0xFF;
		s[o + 3] = e.value.length >> 8;
		s.set(e.value, o + 4);
		o += 4 + e.value.length;
	}
	s.set(MAGIC, 0);
	s[4] = KV_VERSION;
	s[5] = hdr;
	s[6] = len & 0xFF;
	s[7] = len >> 8;
	const crc = crc32(s.subarray(hdr, hdr + len));
	s.set(idValue(crc), 8);
	if (prev) s.set(prev.subarray(12, hdr), 12);
	else s.fill(0, 12, hdr);
	return s;
}

// Write entries into an image's bytes as the area, only when the sector's bytes change. Refuses (returns false,
// writes nothing) over an area of a newer version. True when the sector holds the entries afterwards
export function putArea(bytes, entries) {
	const state = decodeArea(bytes).state;
	if (state === "newer") return false;
	const have = bytes.subarray(KV_ADDR, KV_ADDR + KV_SIZE);
	const s = encodeArea(entries, state === "valid" ? have : null);
	if (!s.every((x, i) => x === have[i])) have.set(s);
	return true;
}

// The keys an edit changes, written onto the area: vals is {name: value, or null for absent}, the names KEYS'. The
// area is left alone when every key already reads as asked, and refused when it is newer (false)
export function putKeys(bytes, vals) {
	const area = decodeArea(bytes);
	const todo = KEYS.filter((row) => row.name in vals && getKey(area.entries, row) !== vals[row.name]);
	if (!todo.length) return true;
	if (area.state === "newer") return false;
	// an invalid area starts a new one: nothing in it could be read (4)
	let entries = dedupe(area.entries);
	for (const row of todo) {
		const v = vals[row.name];
		entries = v == null ? deleteValue(entries, row.key) : setValue(entries, row.key, keyValue(row, v));
	}
	return putArea(bytes, entries);
}

const sameBytes = (a, b) => a === b || (a != null && b != null && a.length === b.length && a.every((x, i) => x === b[i]));

// The area a write to a radio sends (6, 7), put into file's bytes. base is the image the editor opened.
// - The radio's area is of a newer version: the radio's, as it is.
// - The edit didn't change the area from base, or the file's isn't valid: the radio's. A file without an area
//   (another tool's, 0xFF there) never erases the keys.
// - The edit changed it and the radio has a valid area: the radio's area with only the keys the edit changed, set or
//   removed. A key the radio's menu or debug link changed since the file was read keeps the radio's value.
// - The radio has no valid area, or no base is given (a file sent as it is): the file's.
// Throws RangeError when the radio's keys and the edit's don't fit together
export function areaForWrite(file, radio, base = null) {
	const sector = (b) => b.subarray(KV_ADDR, KV_ADDR + KV_SIZE);
	const f = decodeArea(file), r = decodeArea(radio);
	const edited = !base || !sector(file).every((x, i) => x === sector(base)[i]);
	if (r.state === "newer" || !edited || f.state !== "valid") {
		sector(file).set(sector(radio));
		return;
	}
	if (!base || r.state !== "valid") return;
	const was = decodeArea(base).entries;
	let entries = dedupe(r.entries);
	for (const key of new Set([...was, ...f.entries].map((e) => e.key))) {
		const now = getValue(f.entries, key);
		if (sameBytes(getValue(was, key), now)) continue;
		entries = now == null ? deleteValue(entries, key) : setValue(entries, key, now);
	}
	sector(file).set(encodeArea(entries, sector(radio)));
}
