// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// What the page decided, as breadcrumbs for diag.js: the files picked, the codec decision and why, the key's source,
// the write plan, the cable radio's choices. Names, sizes, hashes and reasons; never file bytes or radio data. Each
// returns [text, data] for diag.note. No DOM here, so the Node tests can use it.

import { logId } from "./vendorfile.js";

// A vendor file picked: picked {name, size, sha256}; got: the stock object made from it (app.js pickedStock)
export function filePicked(picked, got) {
	const m = got?.match;
	return ["file picked", { name: String(picked.name || "").slice(0, 80), size: picked.size, sha: String(picked.sha256 || "").slice(0, 12),
		id: logId(got || {}), match: m?.kind || null, board: m?.board || null, known: got?.info?.known || m?.known || null,
		problem: got?.problem?.problem || null, why: String(got?.why || m?.why || "").slice(0, 120) || null }];
}

// decideCodec's answer (decide.js), or why there is none
export function decision(d, { err = null, stock = false, saved = null } = {}) {
	return ["codec decision", d ? { mode: d.mode, source: d.source || "stock", why: d.why || null, risk: d.risk || null }
		: { mode: null, why: err, stock, saved }];
}

export const key = (source) => ["key", { source }];

// One line: "merge: erase 3,4,10; write 3:app,4:app,10:codec; keep -; 120 blocks"
export function planSummary(p) {
	const n = (ss) => ss.map((s) => s.index).join(",") || "-";
	return `${p.mode}: erase ${n(p.erase)}; write ${p.writes.map((s) => `${s.sector.index}${s.content ? `:${s.content}` : ""}`).join(",") || "-"}; keep ${n(p.kept)}; ${p.totalBlocks} blocks`;
}

export const plan = (p, prof = null) => [`write plan ${planSummary(p)}${prof ? `; codec ${prof.name}` : ""}`, undefined];

export const verify = (p, good) => ["verify", { git: p.git, codec: p.codec || null, good }];

// The cable radio's check: what the user chose to read
export const c7Check = (want, extra) => ["cable radio check", { ...want, ...extra }];

// The cable radio's codec decision (c7000.js decide)
export function c7Decision(d, analysis, fileCodec) {
	return ["cable radio codec decision", d ? { mode: d.mode, source: d.source || null, name: d.name || null, tag: d.tag ?? null }
		: { mode: null, firmware: analysis?.firmware || null, notRead: !!analysis?.notRead, file: !!fileCodec }];
}
