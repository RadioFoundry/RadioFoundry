// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Why a vendor file can't be used, as one of a few problems the page explains with what happened, why and what to do
// (docs/dev/web-flasher.md, "When a file can't be used"). The modules under the page say exactly what failed (stockfw.js
// StockError and c7000.js CodecError carry a code; vendorfile.js marks every file it opened); this sorts that into a
// problem. The exact message stays for the technical details and the error report. Also what the codec step says about
// it, and the "current firmware" step's choices. No DOM here, so the host tests check it.

// other-radio  a file for another radio RadioFoundry knows (board names it)
// wrong-key    a vendor container this radio's bootloader key doesn't open: another model, or a modified build
// not-vendor   not a vendor firmware file at all: a plain image, another firmware project's build, a zip without one
// unmapped     this radio's build (or, on a C-SKY radio, a codec no profile describes), but the codec can't be taken
//              from it yet (no codec map); a C-SKY file of this kind is never written back as original firmware
// damaged      a cut-short or damaged file or zip
// internal     a check failing after the file was identified: a fault in the flasher or its data, not in the file
export const PROBLEMS = ["other-radio", "wrong-key", "not-vendor", "unmapped", "damaged", "internal"];

// Problems where sending the file can help: a build the project can add, or a fault the file reproduces
export const OFFER = new Set(["unmapped", "wrong-key", "internal", "dump"]);

const base = (path) => (path || "").split("/").pop();

// The problem with what the user picked (vendorfile.openVendorFile's result), or null when a file in it serves.
// need "codec": the codec must come from it; "key": only the bootloader key and the application (restoring the
// original firmware), so any build of this radio does
export function pickProblem(opened, boardId, { need = "codec" } = {}) {
	const best = opened.best;
	// an STM32 flash readout (vendorfile.js kind "readout") is named, but neither the codec nor the restore takes one
	if (best && best.match.kind !== "readout") {
		if (need === "key" || best.match.kind === "codec") return null;
		// a program memory dump of this radio: it restores, but gives no codec (unless it holds a build without a recipe)
		if (best.match.kind === "progmem" && !best.match.unmapped) return { problem: "dump", file: base(best.path) };
		// key: the radio's bootloader key opens it, so it still unlocks update mode (STM32 kinds "key", "codec-unmapped")
		return { problem: "unmapped", file: base(best.path), known: best.match.known || null, ...(best.match.key ? { key: true } : {}) };
	}
	if (best) return { problem: "not-vendor", file: base(best.path), known: best.match.known || null };
	const other = opened.files.find((f) => f.match.board && f.match.board !== boardId);
	if (other) return { problem: "other-radio", file: base(other.path), board: other.match.board, known: other.match.known || null };
	const marked = (p) => opened.files.find((f) => f.match.problem === p && (p !== "unmapped" || f.match.boards?.includes(boardId)));
	const f = marked("unmapped") || marked("wrong-key") || marked("damaged") || marked("internal");
	if (f) return { problem: f.match.problem, file: base(f.path), known: f.match.known || null };
	if (!opened.files.length && opened.skipped.some((s) => s.problem === "damaged" || s.problem === "too-deep"))
		return { problem: "damaged", file: base(opened.skipped.find((s) => s.problem !== "not-firmware").path) };
	return { problem: "not-vendor", file: base(opened.files[0]?.path || opened.skipped[0]?.path || "") };
}

// What the codec can be taken from on boardId, for the "select one of these instead" list: {files, also}. files: stock
// file names. A C-SKY radio (fb.c7000.boards) lists the profiles that describe a whole stock file first, then any other
// file its codec sources name; an STM32 radio its codec sources (rfoundry boards.py). also: the sources that aren't a
// file, such as a program flash read of the radio, for a sentence of their own
const FILE_NAME = /\.[A-Za-z0-9]{2,4}\)?$/;
export function supportedFiles(fb, boardId) {
	const sources = fb.boards?.[boardId]?.codec?.sources || [];
	// a board that runs another board's codec (c7000 codec_boards, preferred first: a second SoC that runs the main SoC's codec) lists that
	// board's files first
	const ids = fb.c7000?.boards?.[boardId]?.codec_boards || [boardId];
	const profiles = fb.c7000?.boards?.[boardId]
		? ids.flatMap((id) => (fb.c7000.profiles || []).filter((p) => p.board === id && p.stock_file).map((p) => p.name)) : [];
	const files = [...profiles];
	for (const n of sources) if (FILE_NAME.test(n) && !files.includes(n)) files.push(n);
	return { files, also: sources.filter((n) => !FILE_NAME.test(n) && !files.includes(n)) };
}

// The supported file list is folded above this many (the user is choosing one, so it is never cut short)
export const SUPPORTED_OPEN_MAX = 12;
export const foldSupported = (files) => files.length > SUPPORTED_OPEN_MAX;

// What the codec step says when the picked file gives no codec (problem "unmapped") but the install goes on anyway
// (decide.js decision), as the suffix of its flash.codec.keep_* strings: "none" when the decision keeps a codec it knows
// is missing (expectCodec false: the radio reported none, or its original firmware never stores it where RadioFoundry
// uses it); "unverified" when the codec already in the radio is kept without being checked, so DMR voice depends on it;
// "ok" when the radio reported its codec; null when the file isn't the issue (no problem, no decision yet, or the codec
// comes from elsewhere)
export function keepNote(problem, decision) {
	if (problem?.problem !== "unmapped" || !decision || decision.mode !== "keep") return null;
	if (!decision.expectCodec) return "none";
	return decision.risk ? "unverified" : "ok";
}

// The "current firmware" step's choices, by id. "openfw" (another open firmware) and "unknown" (any other firmware, or
// not known) both leave the install with the same question unanswered, so both are kind "unknown" for the codec
// decision and the steps; openfw only marks which button was pressed (and goes into the error report)
export const NOW_CHOICES = ["rfoundry", "stock", "openfw", "unknown"];
export function nowFor(id) {
	return id === "stock" ? { kind: "stock", version: null } : id === "openfw" ? { kind: "unknown", openfw: true } : { kind: "unknown" };
}
// Which choice now (S.now) is, or null when there is none yet
export function nowChoice(now) {
	if (!now?.kind) return null;
	return now.kind === "unknown" && now.openfw ? "openfw" : now.kind;
}

// The problem behind a thrown error, or null when it isn't a file problem (a cable, a radio, a user's choice)
export function errorProblem(e) {
	if (!e || !PROBLEMS.includes(e.code) || !["StockError", "CodecError"].includes(e.name)) return null;
	return { problem: e.code, board: e.params?.board || null, file: e.params?.file || null, known: e.params?.known || null, raw: e.message };
}

// What an error report carries about the vendor file the user picked: never its bytes. picked: {name, size, sha256}
// of what the user chose (a zip, or the file itself); opened: openVendorFile's result
export function vendorContext({ boardId = null, picked = null, opened = null, problem = null, keySource = null, decision = null } = {}) {
	if (!picked && !opened && !problem) return null;
	const used = opened?.best || null;
	const prof = used?.match.profile;
	return {
		board: boardId,
		picked: picked ? { name: String(picked.name || "").slice(0, 80), size: picked.size, sha256: picked.sha256 || null } : null,
		files: (opened?.files || []).slice(0, 20).map((f) => ({ path: String(f.path).slice(0, 80), size: f.size ?? f.data?.length,
			sha256: f.match.sha || null, board: f.match.board || null, kind: f.match.kind || null, how: f.match.how || null,
			known: f.match.known || null, problem: f.match.problem || null })),
		skipped: (opened?.skipped || []).slice(0, 20).map((s) => ({ path: String(s.path).slice(0, 80), problem: s.problem || null })),
		used: used ? String(used.path).slice(0, 80) : null,
		profile: prof ? { name: prof.name, board: prof.board } : null,
		key: keySource,
		decision,
		problem: problem ? { problem: problem.problem, board: problem.board || null, raw: problem.raw ? String(problem.raw).slice(0, 300) : null } : null,
	};
}
