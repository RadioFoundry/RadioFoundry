// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Whatever vendor file a user picks, opened the same way for every radio: a firmware .bin, a program flash readout, or
// a vendor's zip (nested zips too) with updaters, manuals and several builds inside. Every file is identified against
// every radio the flasher knows, the most certain way first:
//   1. its SHA-256 in a codec profile or the known files (stockfw PROFILES / KNOWN_FILES, the C-SKY profiles);
//   2. an unknown TYT/Baofeng container (the "OutSecurityBin" header): every known bootloader key is tried on it, which
//      tells which radio it is for, and the decrypted application is searched for the codec's signature word;
//   3. a C-SKY radio's stock file or readout, by the codec's own hashes wherever the file holds it (c7000.js);
//   4. an STM32 program flash readout, by the codec windows' hashes (stockfw.fromFlash).
// The signature alone never makes a codec usable: moving the codec needs that build's map (docs/spec/codec-placement.md),
// so a build without one is reported as "has the codec, no recipe for this build", not taken.

import { sha256, u32le, startsWith } from "./bytes.js";
import { unzip } from "./zip.js";
import * as stockfw from "./stockfw.js";
import * as c7 from "./c7000.js";

const MAX_DEPTH = 3;	// zips inside zips
const MIN_SIZE = 64 * 1024;	// smaller than any firmware image or readout

// What a file is not, by its first bytes: an updater or DLL (MZ), an Office document (OLE2), a PDF, a zip (opened
// separately), a text file. Extensions are never trusted: vendors name firmware .bin, .img, .hex or nothing at all
function notFirmware(d) {
	if (d.length < MIN_SIZE) return "too small for a firmware image";
	if (d[0] === 0x4D && d[1] === 0x5A) return "a Windows program";
	if (d[0] === 0xD0 && d[1] === 0xCF && d[2] === 0x11 && d[3] === 0xE0) return "an Office document";
	if (startsWith(d, "%PDF")) return "a PDF";
	let text = 0;
	const n = Math.min(d.length, 4096);
	for (let i = 0; i < n; i++) if (d[i] === 9 || d[i] === 10 || d[i] === 13 || (d[i] >= 32 && d[i] < 127)) text++;
	return text > n * 0.97 ? "a text file" : null;
}

const isZip = (d) => d.length > 4 && d[0] === 0x50 && d[1] === 0x4B && d[2] === 0x03 && d[3] === 0x04;

// Every file in what the user picked: [{path, data}] for the candidates, [{path, why}] for what was set aside
export async function unpack(name, data, depth = 0, out = { files: [], skipped: [] }) {
	if (!isZip(data)) {
		const why = notFirmware(data);
		if (why) out.skipped.push({ path: name, why });
		else out.files.push({ path: name, data });
		return out;
	}
	if (depth >= MAX_DEPTH) {
		out.skipped.push({ path: name, why: "a zip nested too deep" });
		return out;
	}
	let entries;
	try {
		entries = await unzip(data);
	} catch (e) {
		out.skipped.push({ path: name, why: `a damaged zip (${e.message})` });
		return out;
	}
	for (const [path, body] of entries) {
		const base = path.split("/").pop();
		if (!base || path.startsWith("__MACOSX/") || base.startsWith("._")) continue;
		await unpack(depth ? `${name}/${path}` : path, body, depth + 1, out);
	}
	return out;
}

// The STM32 radios that take a TYT/Baofeng container, one entry per bootloader key; the radio being flashed first
const keyedBoards = (fb, first) => Object.values(fb.boards).filter((b) => b.key_sha256).sort((a, b) => (b.id === first) - (a.id === first));

// One file against every radio: {board, kind, how, profile?, known?, key?, codecAt?, codec?} or {board: null, why}.
// kind "codec": the codec can be taken from it; "key": it gives a radio's bootloader key, not a codec RadioFoundry can
// place; "codec-unmapped": it holds the codec of a build without a map
export async function identifyFile(data, fb, boardId = null) {
	const sha = await sha256(data);
	const prof = fb.profiles[sha];
	if (prof) return { sha, board: prof.board, kind: "codec", how: "sha256", profile: prof, known: fb.known_files[sha] || prof.name };
	const c = fb.c7000;
	const cprof = c?.profiles.find((p) => p.stock_file?.sha256 === sha);
	if (cprof) return { sha, board: cprof.board, kind: "codec", how: "sha256", known: cprof.name, codec: await c7.codecFromFile(data, c, cprof.board) };
	const known = fb.known_files[sha];
	if (startsWith(data, fb.magic)) {
		let segs;
		try {
			segs = stockfw.parseContainer(data, fb);
		} catch (e) {
			return { sha, board: null, why: e.message };
		}
		for (const b of keyedBoards(fb, boardId)) {
			let key;
			try {
				key = await stockfw.deriveKey(data, b.key_sha256, fb);
			} catch (e) {
				continue;
			}
			const seg = segs.find(([base]) => base === b.app_base);
			let codecAt = null;
			if (seg) {
				const app = stockfw.decrypt(data, stockfw.appFileOffset(segs, seg[0], fb), seg[1], key, fb);
				for (let i = 0; i + 4 <= app.length; i += 4) if (u32le(app, i) === fb.codec_sig) { codecAt = seg[0] + i; break; }
			}
			// a known file without a profile (the MD-380 D2.30) is known to give the key only
			return { sha, board: b.id, kind: codecAt === null || known ? "key" : "codec-unmapped", how: "key", key, known, codecAt };
		}
		return { sha, board: null, known, why: "a TYT/Baofeng firmware file for a radio whose bootloader key RadioFoundry doesn't know" };
	}
	if (c) {
		for (const id of Object.keys(c.boards).sort((a, b) => (b === boardId) - (a === boardId))) {
			try {
				const codec = await c7.codecFromFile(data, c, id);
				return { sha, board: id, kind: "codec", how: "codec hashes", known: codec.profile?.name, codec };
			} catch (e) {
				if (/no recipe/.test(e.message)) return { sha, board: id, kind: "codec-unmapped", how: "codec signature", why: e.message };
			}
		}
	}
	const flash = await stockfw.fromFlash(data, fb);
	if (flash) return { sha, board: flash.profile.board, kind: "codec", how: "readout", profile: flash.profile, known: flash.profile.name, flash };
	return { sha, board: null, known, why: known ? `${known}, which gives no codec` : "not a firmware file RadioFoundry knows" };
}

const RANK = { codec: 3, "codec-unmapped": 1, key: 2 };

// Everything in what the user picked, identified, and the best file for boardId: the one with the codec first, then one
// that gives the bootloader key, then by name. {files: [{path, size, data, match}], skipped, best}; best is null when
// no file is for this radio
export async function openVendorFile(name, data, fb, boardId) {
	const { files, skipped } = await unpack(name, data);
	for (const f of files) {
		f.size = f.data.length;
		f.match = await identifyFile(f.data, fb, boardId);
	}
	const mine = files.filter((f) => f.match.board === boardId).sort((a, b) =>
		RANK[b.match.kind] - RANK[a.match.kind] || (a.path < b.path ? -1 : a.path > b.path ? 1 : 0));
	return { files, skipped, best: mine[0] || null };
}

// One line per file for the page: what each one is, and why the chosen one was taken
export function describe(opened, fb, boardId) {
	const name = (id) => fb.boards[id]?.name || id;
	const lines = opened.files.map((f) => {
		const m = f.match;
		const what = !m.board ? m.why
			: m.board !== boardId ? `for the ${name(m.board)}, not this radio`
			: m.kind === "codec" ? `${m.known || "known"}: the codec${fb.boards[m.board]?.key_sha256 ? " and the bootloader key" : ""}`
			: m.kind === "codec-unmapped" ? `${m.known || "a build"} with the codec, but RadioFoundry has no recipe for this build yet${m.key ? " (the bootloader key works)" : ""}`
			: `${m.known || "a build"}: the bootloader key, not the codec`;
		return `${f.path}: ${what}${f === opened.best ? " (used)" : ""}`;
	});
	return lines.concat(opened.skipped.map((s) => `${s.path}: skipped, ${s.why}`));
}
