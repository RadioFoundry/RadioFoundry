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

// Keys (3): a key's meaning never changes
export const KEY = { arsServer: 0x0001, ipGateway: 0x0002 };
// The highest DMR ID a key may hold (CODEPLUG_DMR_ID_MAX); above it the key reads as absent
export const ID_MAX = 16776415;

const u16 = (b, o) => b[o] | (b[o + 1] << 8);
const u32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;

// The area in an image's bytes: {state, entries}. state: "valid"; "empty" (an erased sector); "bad" (anything else
// that isn't a valid area); "newer" (our magic with a version this reader doesn't know). Only a valid area has entries
export function decodeArea(bytes) {
	const s = bytes.subarray(KV_ADDR, KV_ADDR + KV_SIZE);
	if (s.length < KV_SIZE) return { state: "empty", entries: [] };
	if (!MAGIC.every((x, i) => s[i] === x)) return { state: s.every((x) => x === 0xFF) ? "empty" : "bad", entries: [] };
	if (s[4] !== KV_VERSION) return { state: "newer", entries: [] };
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

// A DMR ID key (a u32): the ID, or null when absent, shorter than 4 bytes or above ID_MAX. A longer value is a later
// version's: its first 4 bytes are the ID
export function getId(entries, key) {
	const v = getValue(entries, key);
	if (!v || v.length < 4) return null;
	const id = u32(v, 0);
	return id > ID_MAX ? null : id;
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

// The ID keys an edit changes, written onto the area: ids is {key: id or null (absent)}. The area is left alone when
// every key already reads as asked, and refused when it is newer (false)
export function putIds(bytes, ids) {
	const area = decodeArea(bytes);
	const todo = Object.entries(ids).map(([k, v]) => [Number(k), v]).filter(([k, v]) => getId(area.entries, k) !== v);
	if (!todo.length) return true;
	if (area.state === "newer") return false;
	// an invalid area starts a new one: nothing in it could be read (4)
	let entries = dedupe(area.entries);
	for (const [k, v] of todo) {
		if (v == null) {
			entries = deleteValue(entries, k);
			continue;
		}
		// a longer value is a later version's: the ID replaced, the fields after it kept
		const was = getValue(entries, k);
		const value = was && was.length > 4 ? was.slice() : new Uint8Array(4);
		value.set(idValue(v), 0);
		entries = setValue(entries, k, value);
	}
	return putArea(bytes, entries);
}

// The area a write to a radio sends (6): the file's when it is valid and the radio's isn't of a newer version, else
// the radio's own, put into file's bytes. A file without an area (another tool's, 0xFF there) never erases the keys
export function areaForWrite(file, radio) {
	if (decodeArea(file).state === "valid" && decodeArea(radio).state !== "newer") return;
	file.subarray(KV_ADDR, KV_ADDR + KV_SIZE).set(radio.subarray(KV_ADDR, KV_ADDR + KV_SIZE));
}
