// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The codec read back from a radio that runs RadioFoundry and kept in this browser (IndexedDB), so that a later install needs
// no stock file: where an update erases part of it (the MD-380: RadioFoundry's image shares a flash sector with the codec,
// keep_codec_supported false), and on every other radio for when it can't be kept (a radio back on stock firmware, an
// install that stopped halfway, a radio in its bootloader). Every codec window is read over the debug link (READ_MEM,
// docs/spec/debuglink.md 3.10) and checked against the stock profile's SHA-256, either where stock firmware keeps it or
// where the merge placed it (stockfw.placedWindows: the DM-1701's code in sector 11). It is checked again when loaded.
// The same rules as rfoundry/codecsave.py.

import { sha256, u32le } from "./bytes.js";
import { descriptor, place, placedWindows } from "./stockfw.js";

// The stock profiles whose codec this board runs
export const profilesFor = (board, fb) =>
	Object.values(fb.profiles).filter((p) => p.board === board.id && p.codec_profile === board.codec.id);

// Where a profile's codec can be in a radio: [[addr, length, sha256], ...] where stock firmware keeps it, and where the
// merge placed it when the profile moves the code
const forms = (prof) => (prof.placement ? [prof.windows, placedWindows(prof)] : [prof.windows]);

// [[addr, bytes], ...] when every window matches its hash and the code starts with the codec's signature, else null
async function check(wins, parts, fb) {
	if (parts.length !== wins.length) return null;
	for (const [i, [addr, length, want]] of wins.entries()) {
		const [a, d] = parts[i];
		if (a !== addr || d.length !== length || (await sha256(d)) !== want) return null;
	}
	return u32le(parts[0][1], 0) === fb.codec_sig ? parts : null;
}

// {profile, segments} through an open debug link, or null when the flash doesn't hold this codec or can't be read
export async function readCodec(link, board, fb) {
	for (const prof of profilesFor(board, fb)) {
		for (const wins of forms(prof)) {
			const parts = [];
			try {
				for (const [addr, length] of wins) parts.push([addr, await link.readMem(addr, length)]);
			} catch (e) {
				return null;
			}
			if (await check(wins, parts, fb)) return { profile: prof, segments: parts };
		}
	}
	return null;
}

// A copy (readCodec, loadCodec) as the merge writes it: windows at their stock addresses placed (stockfw.place), placed
// windows with their descriptor
export async function codecSegments(prof, segments, fb) {
	if (!prof.placement) return segments;
	if (segments[0][0] === prof.windows[0][0]) return place(prof, segments, fb);
	return [...segments, [fb.descriptor.addr, descriptor(prof, fb)]];
}

// --- the copy in this browser: one record per board and codec, the windows concatenated in profile order

const DB = "rfoundry-flasher", STORE = "codecs";
const recordId = (board) => `${board.id}-${board.codec.id}`;

function open() {
	return new Promise((resolve, reject) => {
		const r = indexedDB.open(DB, 1);
		r.onupgradeneeded = () => r.result.createObjectStore(STORE);
		r.onsuccess = () => resolve(r.result);
		r.onerror = () => reject(r.error);
	});
}

async function tx(mode, fn) {
	const db = await open();
	try {
		return await new Promise((resolve, reject) => {
			const t = db.transaction(STORE, mode);
			const r = fn(t.objectStore(STORE));
			t.oncomplete = () => resolve(r.result);
			t.onerror = t.onabort = () => reject(t.error);
		});
	} finally {
		db.close();
	}
}

// Any other bytes this page keeps for a radio (the C7000 program memory read before a first install, its codec slot):
// true when saved, false where the browser keeps nothing
export async function putBlob(id, bytes) {
	try {
		await tx("readwrite", (s) => s.put(bytes, id));
		return true;
	} catch (e) {
		return false;
	}
}

export async function getBlob(id) {
	try {
		const v = await tx("readonly", (s) => s.get(id));
		return v instanceof Uint8Array ? v : null;
	} catch (e) {
		return null;
	}
}

// true when saved; false where the browser keeps nothing (private window, storage off)
export async function saveCodec(board, segments) {
	const blob = new Uint8Array(segments.reduce((n, [, d]) => n + d.length, 0));
	let off = 0;
	for (const [, d] of segments) {
		blob.set(d, off);
		off += d.length;
	}
	try {
		await tx("readwrite", (s) => s.put(blob, recordId(board)));
		return true;
	} catch (e) {
		return false;
	}
}

// {profile, segments} from the saved copy, checked against the profile, or null
export async function loadCodec(board, fb) {
	let blob;
	try {
		blob = await tx("readonly", (s) => s.get(recordId(board)));
	} catch (e) {
		return null;
	}
	if (!(blob instanceof Uint8Array)) return null;
	for (const prof of profilesFor(board, fb)) {
		for (const wins of forms(prof)) {
			const parts = [];
			let off = 0;
			for (const [addr, length] of wins) {
				parts.push([addr, blob.subarray(off, off + length)]);
				off += length;
			}
			if (off === blob.length && (await check(wins, parts, fb))) return { profile: prof, segments: parts };
		}
	}
	return null;
}
