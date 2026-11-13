// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The codec read back from a radio that runs RadioFoundry and kept in this browser (IndexedDB), so that a later install needs
// no stock file: where an update erases part of it (keep_codec_supported false), where the codec moves to a new layout
// (a radio whose codec is where an earlier release put it: board.codec.former), and on every radio for when it can't
// be kept (a radio back on stock firmware, an install that stopped halfway, a radio in its bootloader). Every codec
// window is read over the debug link (READ_MEM, docs/spec/debuglink.md 3.10) and checked against the stock profile:
// where stock firmware keeps it, where the merge places it now (stockfw.placedWindows) or where an earlier release
// placed it (stockfw.forms, "former"), a placed window by its SHA-256 or by taking it back to the stock windows'
// (stockfw.checkForm). It is checked again when loaded. A copy in a stock or former layout is moved to the current one
// before it is written (codecSegments). The copy is never deleted: it is what lets an install that stopped halfway, at
// any step, be run again (docs/spec/codec-placement.md, "Moving to a new layout"). The same rules as rfoundry/codecsave.py.

import { u32le } from "./bytes.js";
import { checkForm, forms, fromForm, placedWindows } from "./stockfw.js";

// The stock profiles whose codec this board runs
export const profilesFor = (board, fb) =>
	Object.values(fb.profiles).filter((p) => p.board === board.id && p.codec_profile === board.codec.id);

// Where a profile's codec can be in a radio: [[form, [[addr, length, sha256], ...]]], the stock windows (form null)
// first, then the current placement and each former one
const formsOf = (prof) => [[null, prof.windows], ...forms(prof).map((f) => [f, placedWindows(prof, f)])];

// [tag, [window addresses]] from the descriptor in the radio's flash, or null (none there, or it can't be read)
async function described(link, fb) {
	const d = fb.descriptor;
	let b;
	try {
		b = await link.readMem(d.addr, d.len);
	} catch (e) {
		return null;
	}
	if (!b || b.length !== d.len || u32le(b, 0) !== d.magic) return null;
	const n = u32le(b, 24);
	if (n < 1 || n > d.windows) return null;
	return [u32le(b, 8), [u32le(b, 12), ...Array.from({ length: n }, (_, i) => u32le(b, 28 + 12 * i))]];
}

// {profile, segments} through an open debug link, or null when the flash doesn't hold this codec or can't be read. Each
// layout the board has is tried in turn, the ones the radio's descriptor names first (it isn't trusted: the windows
// are checked as for any other)
export async function readCodec(link, board, fb) {
	const desc = await described(link, fb);
	const named = ([, form, wins]) => !!desc && !!form && form.placement.tag === desc[0] && wins.map(([a]) => a).join() === desc[1].join();
	const cands = profilesFor(board, fb).flatMap((prof) => formsOf(prof).map(([form, wins]) => [prof, form, wins]));
	cands.sort((x, y) => named(y) - named(x));
	for (const [prof, form, wins] of cands) {
		const parts = [];
		try {
			for (const [addr, length] of wins) parts.push([addr, await link.readMem(addr, length)]);
		} catch (e) {
			return null;
		}
		if (await checkForm(prof, form, parts, fb)) return { profile: prof, segments: parts };
	}
	return null;
}

// The form (stockfw.forms, null for the stock windows) a copy's window addresses are in
function formOfCopy(prof, parts) {
	const at = parts.map(([a]) => a).join();
	const f = formsOf(prof).find(([, wins]) => wins.map(([a]) => a).join() === at);
	if (!f) throw new Error(`a codec copy at addresses no layout of ${prof.name} has`);
	return f[0];
}

// True when a copy is not in the current placement: writing it moves the codec (from a former layout or stock's)
export const needsMove = (prof, parts) => !!prof.placement && !formOfCopy(prof, parts)?.current;

// A copy (readCodec, loadCodec) as the merge writes it now: in the current placement with its descriptor. A copy in
// the stock windows or a former placement is moved there (stockfw.fromForm)
export async function codecSegments(prof, segments, fb) {
	if (!prof.placement) return segments;
	const out = await fromForm(prof, formOfCopy(prof, segments), segments, fb);
	if (!out) throw new Error(`the codec copy doesn't check against ${prof.name}`);
	return out;
}

// --- the copy in this browser: one record per board and codec, the windows concatenated in profile order

const DB = "rfoundry-flasher", STORE = "codecs";
const recordId = (board, id = board.codec.id) => `${board.id}-${id}`;

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

// {profile, segments} from the saved copy, checked against the profile, or null. A copy saved under an id an earlier
// release reported (board.codec.former) is found too: the codec is the same, only where it was read
export async function loadCodec(board, fb) {
	for (const id of [board.codec.id, ...(board.codec.former || [])]) {
		let blob;
		try {
			blob = await tx("readonly", (s) => s.get(recordId(board, id)));
		} catch (e) {
			return null;
		}
		if (!(blob instanceof Uint8Array)) continue;
		for (const prof of profilesFor(board, fb)) {
			for (const [form, wins] of formsOf(prof)) {
				const parts = [];
				let off = 0;
				for (const [addr, length] of wins) {
					parts.push([addr, blob.subarray(off, off + length)]);
					off += length;
				}
				if (off === blob.length && (await checkForm(prof, form, parts, fb))) return { profile: prof, segments: parts };
			}
		}
	}
	return null;
}
