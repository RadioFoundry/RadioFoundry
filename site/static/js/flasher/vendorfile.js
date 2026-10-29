// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Whatever vendor file a user picks, opened the same way for every radio: a firmware .bin, a program flash readout, or
// a vendor's zip (nested zips too) with updaters, manuals and several builds inside. Every file is identified against
// every radio the flasher knows, the most certain way first:
//   1. its SHA-256 in a codec profile or the known files (stockfw PROFILES / KNOWN_FILES / NO_CODEC_FILES, the C-SKY
//      profiles);
//   2. an unknown TYT/Baofeng container (the "OutSecurityBin" header): every known bootloader key is tried on it, which
//      tells which radio it is for, and the decrypted application is searched for the codec's signature word;
//   3. a C-SKY radio's stock file or readout, by the codec's own hashes wherever the file holds it (c7000.js);
//   4. an STM32 program flash readout, by the codec windows' hashes (stockfw.fromFlash): named, not used.
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

// Every file in what the user picked: [{path, data}] for the candidates, [{path, why, problem}] for what was set aside
// (problem: "not-firmware", "damaged" or "too-deep", for explain.js)
export async function unpack(name, data, depth = 0, out = { files: [], skipped: [] }) {
	if (!isZip(data)) {
		const why = notFirmware(data);
		if (why) out.skipped.push({ path: name, why, problem: "not-firmware" });
		else out.files.push({ path: name, data });
		return out;
	}
	if (depth >= MAX_DEPTH) {
		out.skipped.push({ path: name, why: "a zip nested too deep", problem: "too-deep" });
		return out;
	}
	let entries;
	try {
		entries = await unzip(data);
	} catch (e) {
		out.skipped.push({ path: name, why: `a damaged zip (${e.message})`, problem: "damaged" });
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

// One file against every radio: {board, kind, how, profile?, known?, key?, codecAt?, codec?} or {board: null, why,
// problem}. kind "codec": the codec can be taken from it; "key": it gives a radio's bootloader key, not a codec
// RadioFoundry can place; "codec-unmapped": it holds the codec of a build without a map; "progmem": a C-SKY radio's whole
// program flash read with no codec known (a restore source, never a codec source); "readout": an STM32 radio's
// flash read, which the page doesn't take. problem (no radio): "damaged", "wrong-key" (a vendor container no known
// radio's key opens: another model, or a modified build), "unmapped" (a C-SKY codec no profile describes, for one of
// boards), "internal" (a known file failing its own check) or "not-vendor"
export async function identifyFile(data, fb, boardId = null) {
	const sha = await sha256(data);
	const prof = fb.profiles[sha];
	if (prof) return { sha, board: prof.board, kind: "codec", how: "sha256", profile: prof, known: fb.known_files[sha] || prof.name };
	// a file known to hold no codec (stockfw NO_CODEC_FILES): named, and like any other non-vendor file, never a source
	const bare = fb.no_codec_files?.[sha];
	if (bare) return { sha, board: null, known: bare.name, how: "sha256", problem: "not-vendor", why: stockfw.noCodec(bare) };
	const c = fb.c7000;
	const cprof = c?.profiles.find((p) => p.stock_file?.sha256 === sha);
	if (cprof) {
		// a known file whose codec areas fail their check: the page's data is wrong, not the file; the other files in a
		// zip are still reported
		try {
			return { sha, board: cprof.board, kind: "codec", how: "sha256", known: cprof.name, codec: await c7.codecFromFile(data, c, cprof.board) };
		} catch (e) {
			return { sha, board: null, known: cprof.name, problem: "internal", why: e.message };
		}
	}
	const known = fb.known_files[sha];
	if (startsWith(data, fb.magic)) {
		let segs;
		try {
			segs = stockfw.parseContainer(data, fb);
		} catch (e) {
			return { sha, board: null, why: e.message, problem: e.code === "not-vendor" ? "not-vendor" : "damaged" };
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
			// a known file without a profile (KNOWN_FILES only) is known to give the key, not a codec RadioFoundry can place
			return { sha, board: b.id, kind: codecAt === null || known ? "key" : "codec-unmapped", how: "key", key, known, codecAt };
		}
		return { sha, board: null, known, problem: "wrong-key", why: "a TYT/Baofeng firmware file for a radio whose bootloader key RadioFoundry doesn't know" };
	}
	if (c) {
		// the codec signature is the same on every C-SKY radio, so a signature without a profile's hashes tells no radio:
		// such a file is for none of them by name, and is never written to a radio as its original firmware
		let unmapped = null;
		for (const id of Object.keys(c.boards).sort((a, b) => (b === boardId) - (a === boardId))) {
			try {
				const codec = await c7.codecFromFile(data, c, id);
				return { sha, board: id, kind: "codec", how: "codec hashes", known: codec.profile?.name, codec };
			} catch (e) {
				if (e.code === "unmapped") unmapped ||= e.message;
				else if (e.code === "internal") return { sha, board: null, problem: "internal", why: e.message };
			}
		}
		// a whole program flash read of this radio with no codec to take: still what a restore puts back as it is
		// (unmapped: it holds a codec build without a recipe, which the codec step says rather than "no codec")
		if (boardId && c7.isProgmemDump(data, c, boardId))
			return { sha, board: boardId, kind: "progmem", how: "bootloader id", unmapped: !!unmapped,
				why: unmapped ? "a program memory dump of this radio whose codec build has no recipe" : "a program memory dump of this radio with no codec RadioFoundry knows" };
		if (unmapped) return { sha, board: null, boards: Object.keys(c.boards), known, problem: "unmapped", how: "codec signature", why: unmapped };
	}
	// a readout of an STM32 radio's flash: identified so the page can say what it is, but the page takes the codec and
	// the key from the manufacturer's file only
	const flash = await stockfw.fromFlash(data, fb);
	// named by its codec's layout, not by the first file that has it: several files can put the same codec in flash
	if (flash) return { sha, board: flash.profile.board, kind: "readout", how: "readout", profile: flash.profile,
		known: `${fb.boards[flash.profile.board]?.name || flash.profile.board} ${stockfw.layoutName(flash.profile, fb.profiles)}` };
	return { sha, board: null, known, problem: "not-vendor", why: known ? `${known}, which gives no codec` : "not a firmware file RadioFoundry knows" };
}

const RANK = { codec: 3, key: 2, "codec-unmapped": 1, progmem: 1, readout: 0 };

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
			: m.kind === "codec" && m.profile?.file_radio ? `${m.known || "known"}: ${m.profile.file_radio} firmware, the codec only (no bootloader key; never restored as this radio's firmware)`
			: m.kind === "codec" ? `${m.known || "known"}: the codec${fb.boards[m.board]?.key_sha256 ? " and the bootloader key" : ""}`
			: m.kind === "readout" ? `a flash readout of ${m.known}: the installer takes the manufacturer's firmware file instead`
			: m.kind === "progmem" ? `${m.why}: a restore puts it back, but it gives no codec`
			: m.kind === "codec-unmapped" ?`${m.known || "a build"} with the codec, but RadioFoundry has no recipe for this build yet${m.key ? " (the bootloader key works)" : ""}`
			: `${m.known || "a build"}: the bootloader key, not the codec`;
		return `${f.path}: ${what}${f === opened.best ? " (used)" : ""}`;
	});
	return lines.concat(opened.skipped.map((s) => `${s.path}: skipped, ${s.why}`));
}

// Why nothing in a pick serves, in a few words for the log: what the first file was found to be, else why it was set
// aside (a file too small or too large to be firmware is skipped before it is identified)
export const whyNot = (opened) => opened.files[0]?.match.why || opened.skipped[0]?.why || null;

// A short string for the flash log (at most 64 characters): what the page took a picked vendor file (pickedStock's
// result) for. Names of known builds and board ids only, nothing from the user's file
export function logId(st) {
	const m = st.match, i = st.info || {};
	const id = m ? (m.kind === "codec" ? `codec:${m.known || m.how}` : m.kind === "codec-unmapped" ? `no-recipe:${m.board}` : `${m.kind}:${m.board}`)
		: st.problem ? `problem:${st.problem.problem}` : i.profile ? `profile:${i.profile.name || i.profile.board}`
		: i.known ? `known:${i.known}` : "unknown";
	return id.slice(0, 64);
}

// The file to offer for analysis when the page can't take the codec from what was picked: a vendor firmware container
// (the headers the backend accepts) or a program memory dump of this radio, never a readout of at most maxBytes, with what the page found as its reason. Never
// a file identified as another radio's: the page already knows what it is and says so
export function analysisCandidate(opened, fb, boardId = null, maxBytes = 4 << 20) {
	const magics = [fb.magic, fb.c7000?.stock_magic].filter(Boolean);
	// a program memory dump has no updater header: the backend takes it by the bootloader ID the page matched
	const f = opened.files.find((x) => x.match.kind !== "codec" && (!boardId || !x.match.board || x.match.board === boardId) &&
		x.data.length <= maxBytes && (x.match.kind === "progmem" || magics.some((m) => startsWith(x.data, m))));
	if (!f) return null;
	const m = f.match;
	return { file: f, reason: m.kind === "progmem" ? "progmem-dump" : m.board ? m.kind : m.problem === "unmapped" ? "codec-unmapped" : m.problem === "wrong-key" ? "key-unknown"
		: m.problem === "internal" ? "internal" : "unknown-container" };
}

// A file the page identified for this radio but failed on later (placing the codec, restoring): offered with reason,
// under the same rules (a vendor container, at most maxBytes), or null
export function offerFile(file, fb, reason, maxBytes = 4 << 20) {
	const magics = [fb.magic, fb.c7000?.stock_magic].filter(Boolean);
	if (!file?.data || file.data.length > maxBytes || !magics.some((m) => startsWith(file.data, m))) return null;
	return { file, reason };
}
