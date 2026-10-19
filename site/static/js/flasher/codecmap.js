// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Codec maps in the web flasher: a port of tools/odrflash/odrflash/codecmap.py (docs/spec/codec-placement.md, "Codec
// maps"). The maps come from that package through data/flash-boards.json ("codec_maps"), already normalized (every
// address a number). Moving a codec rewrites the words that hold addresses inside it; both ports must give the same
// bytes (the shared vectors in tools/odrflash/tests/codecmap_vectors.json).

import { sha256, u32le, hex } from "./bytes.js";

export class MapError extends Error {}

const put32 = (b, off, v) => {
	v >>>= 0;
	b[off] = v & 0xFF; b[off + 1] = (v >>> 8) & 0xFF; b[off + 2] = (v >>> 16) & 0xFF; b[off + 3] = v >>> 24;
};

// The maps whose carved code has this SHA-256
export const byCodeSha = (maps, sha) => Object.values(maps || {}).filter((m) => m.code.sha256 === sha);

export function objectOf(m, addr) {
	const o = (m.objects || []).find((x) => x.start <= addr && addr < x.start + Math.max(x.len, 1));
	return o ? o.start : null;
}

export const islandOf = (m, addr) => {
	const i = (m.islands || []).find((x) => x.start <= addr && addr < x.start + x.len);
	return i ? i.start : null;
};

const u16 = (b, off) => b[off] | (b[off + 1] << 8);
const put16 = (b, off, v) => { b[off] = v & 0xFF; b[off + 1] = (v >>> 8) & 0xFF; };

// Thumb-2 MOVW and MOVT (encoding T3): imm16 = imm4:i:imm3:imm8 across the two halfwords
const MOVW = 0xF240, MOVT = 0xF2C0;
function imm16(b, off, op) {
	const h1 = u16(b, off), h2 = u16(b, off + 2);
	if ((h1 & 0xFBF0) !== op || (h2 & 0x8000)) return null;
	return ((h1 & 0xF) << 12) | (((h1 >> 10) & 1) << 11) | (((h2 >> 12) & 7) << 8) | (h2 & 0xFF);
}
function putImm16(b, off, v) {
	put16(b, off, (u16(b, off) & 0xFBF0) | ((v >> 12) & 0xF) | (((v >> 11) & 1) << 10));
	put16(b, off + 2, (u16(b, off + 2) & 0x8F00) | (((v >> 8) & 7) << 12) | (v & 0xFF));
}

// The address a movw/movt pair builds, or null when the two instructions aren't there
export function pairValue(b, r) {
	const lo = imm16(b, r.off, MOVW), hi = imm16(b, r.off2, MOVT);
	return lo === null || hi === null ? null : ((hi << 16) | lo) >>> 0;
}

// The target of the Thumb-2 BL (encoding T1) at b[off], which sits at address at; null if it isn't one
export function blTarget(b, off, at) {
	const h1 = u16(b, off), h2 = u16(b, off + 2);
	if ((h1 & 0xF800) !== 0xF000 || (h2 & 0xD000) !== 0xD000) return null;
	const s = (h1 >> 10) & 1;
	const i1 = 1 - (((h2 >> 13) & 1) ^ s), i2 = 1 - (((h2 >> 11) & 1) ^ s);
	let imm = (s << 24) | (i1 << 23) | (i2 << 22) | ((h1 & 0x3FF) << 12) | ((h2 & 0x7FF) << 1);
	if (s) imm -= 1 << 25;
	return (at + 4 + imm) >>> 0;
}

export function putBl(b, off, at, target) {
	let imm = target - (at + 4);
	if (imm % 2 || imm < -(1 << 24) || imm >= (1 << 24)) throw new MapError(`bl at ${hex(at)} can't reach ${hex(target)}`);
	imm &= 0x1FFFFFF;
	const s = imm >>> 24, i1 = (imm >>> 23) & 1, i2 = (imm >>> 22) & 1;
	const j1 = (1 - i1) ^ s, j2 = (1 - i2) ^ s;
	put16(b, off, 0xF000 | (s << 10) | ((imm >>> 12) & 0x3FF));
	put16(b, off + 2, 0xD000 | (j1 << 13) | (j2 << 11) | ((imm >>> 1) & 0x7FF));
}

const kind = (r) => r.kind || "word";
const read = (b, r) => (kind(r) === "word" ? u32le(b, r.off) : pairValue(b, r));
function write(b, r, v) {
	if (kind(r) === "word") put32(b, r.off, v);
	else { putImm16(b, r.off, v & 0xFFFF); putImm16(b, r.off2, (v >>> 16) & 0xFFFF); }
}

// Every site the map lists still holds the value it says (no hash needed: a stale or wrong map stops here)
export function checkWords(code, m) {
	for (const r of [...(m.self_refs || []).map((x) => [x, x.value]), ...(m.island_ptrs || []).map((x) => [x, x.target]),
		...(m.table_refs || []).map((x) => [x, x.target])]) {
		const [ref, want] = r;
		if (ref.off + 4 > code.length || (kind(ref) !== "word" && ref.off2 + 4 > code.length) || read(code, ref) !== want >>> 0)
			throw new MapError(`${m.profile}: site +${hex(ref.off, 1)} doesn't hold ${hex(want)}`);
	}
}

// The code as it must be to run at newRun (codecmap.move_code): the build is checked by its hash and every listed
// site by its value, so a wrong map stops here. Islands stay where they are; their calls into the code: moveBranches
export async function moveCode(code, m, newRun) {
	const c = m.code;
	if (code.length !== c.len) throw new MapError(`${m.profile}: code is ${code.length} bytes, the map says ${c.len}`);
	if (c.sha256 && (await sha256(code)) !== c.sha256) throw new MapError(`${m.profile}: the code is not the build this map describes`);
	const delta = newRun - c.run;
	if (delta % (c.move_alignment || 4)) throw new MapError(`${m.profile}: a move by ${delta} breaks the code's alignment`);
	const out = code.slice();
	for (const r of [...(m.self_refs || []), ...(m.island_ptrs || [])]) {
		const want = r.value ?? r.target;
		const got = read(out, r);
		if (got !== want >>> 0) throw new MapError(`${m.profile}: site +${hex(r.off, 1)} holds ${got === null ? "nothing" : hex(got)}, the map says ${hex(want)}`);
	}
	for (const r of m.self_refs || []) write(out, r, (r.value + delta) >>> 0);
	return out;
}

// data (at address addr: a table window holding islands) with every listed BL in it that calls the code re-encoded
// for the code at newRun (codecmap.move_branches)
export function moveBranches(addr, data, m, newRun) {
	const out = data.slice();
	const delta = newRun - m.code.run;
	for (const b of m.branches || []) {
		if (b.at < addr || b.at > addr + data.length - 4) continue;
		if (blTarget(out, b.at - addr, b.at) !== b.target >>> 0) throw new MapError(`${m.profile}: no bl to ${hex(b.target)} at ${hex(b.at)}`);
		putBl(out, b.at - addr, b.at, b.target + delta);
	}
	return out;
}

export const branchesIn = (m, addr, len) => (m.branches || []).filter((b) => addr <= b.at && b.at < addr + len).length;

// The code with its table refs rewritten for moved objects (codecmap.move_tables). moves: Map or object of
// {object start: new start}
export function moveTables(code, m, moves) {
	const to = (s) => (moves instanceof Map ? moves.get(s) : moves[s]) ?? s;
	for (const g of m.groups || []) if (new Set(g.map((s) => to(s) - s)).size > 1) throw new MapError(`${m.profile}: objects of one group moved by different amounts`);
	for (const r of m.island_refs || []) {
		const o = objectOf(m, r.target);
		if (o !== null && to(o) !== o) throw new MapError(`${m.profile}: object ${hex(r.target)} is referred to from an island; not supported yet`);
	}
	const out = code.slice();
	for (const r of m.table_refs || []) {
		if (read(out, r) !== r.target >>> 0) throw new MapError(`${m.profile}: site +${hex(r.off, 1)} doesn't hold ${hex(r.target)}`);
		const o = objectOf(m, r.target);
		if (to(o) === o) continue;
		if (kind(r) !== "word") throw new MapError(`${m.profile}: table ref at +${hex(r.off, 1)} is a ${r.kind}; not supported yet`);
		write(out, r, (r.target + (to(o) - o)) >>> 0);
	}
	return out;
}

// The offsets a firmware that moves the code itself adds the delta to (the HR_C7000 slot, version 2): whole words
// only, and no islands
export function relocations(m) {
	if ((m.self_refs || []).some((r) => kind(r) !== "word") || (m.islands || []).length) throw new MapError(`${m.profile}: the code can't be moved by adding to words`);
	return (m.self_refs || []).map((r) => r.off).sort((a, b) => a - b);
}
