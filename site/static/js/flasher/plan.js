// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Sector planning: which flash sectors to erase and write. A port of tools/odrflash/odrflash/plan.py (keep, merge),
// plus one mode odrflash doesn't have yet:
//
//   restore  put back a whole flash image exactly (a backup's internal flash, or a stock application): every
//            sector the image spans is erased, and written where the image has data. Codec windows may be inside
//            the image: it is the radio's own earlier content, or the stock firmware with its codec.
//
// The tests (site/tests) check keep and merge against plan.py's output for the same inputs.

import { hex } from "./bytes.js";

export const KEEP = "keep", MERGE = "merge", RESTORE = "restore";
export const BLOCK = 1024, FIRST_BLOCK = 2;

export class PlanError extends Error {}

const end = (s) => s.addr + s.size;
const overlaps = (s, lo, hi) => lo < end(s) && hi > s.addr;

export function codecSectors(board) {
	const out = [];
	for (const [lo, n] of board.codec.windows)
		for (const s of board.sectors) if (overlaps(s, lo, lo + n) && !out.includes(s)) out.push(s);
	return out.sort((a, b) => a.addr - b.addr);
}

function checkSegments(board, segments) {
	const spans = [];
	for (const [addr, data] of segments) {
		if (!data.length) continue;
		const lo = addr, hi = addr + data.length;
		if (lo < board.app_base || hi > board.flash_end)
			throw new PlanError(`data at ${hex(lo)}..${hex(hi)} is outside the application flash`);
		for (const [plo, phi] of board.protected)
			if (lo < phi && hi > plo) throw new PlanError(`data at ${hex(lo)}..${hex(hi)} overlaps the bootloader`);
		for (const [slo, shi] of spans) if (lo < shi && hi > slo) throw new PlanError(`segments overlap at ${hex(Math.max(lo, slo))}`);
		spans.push([lo, hi]);
	}
	if (!spans.length) throw new PlanError("nothing to write");
	if (Math.min(...spans.map((s) => s[0])) !== board.app_base)
		throw new PlanError(`the image must start at the application base ${hex(board.app_base)} (the vector table)`);
}

// segments: the firmware as [[addr, Uint8Array]]; codecSegments: the codec (merge mode)
export function makePlan(board, segments, mode, codecSegments = []) {
	if (![KEEP, MERGE, RESTORE].includes(mode)) throw new PlanError(`unknown codec mode ${mode}`);
	const codecSecs = codecSectors(board);
	const codecIdx = new Set(codecSecs.map((s) => s.index));
	if (mode !== RESTORE) {
		for (const [addr, data] of segments) {
			if (!data.length) continue;
			const win = board.codec.windows.find(([lo, n]) => addr < lo + n && addr + data.length > lo);
			if (win) throw new PlanError(`the firmware image (${hex(addr)}..${hex(addr + data.length)}) overlaps the codec window ${hex(win[0])}+0x${win[1].toString(16).toUpperCase()}; the linker keeps the codec windows free`);
			const hit = codecSecs.filter((s) => overlaps(s, addr, addr + data.length)).map((s) => s.index);
			if (hit.length && (mode === KEEP || board.keep_codec_supported))
				throw new PlanError(`the firmware image reaches into codec sector(s) ${hit.join(", ")}; a release image never does`);
			if (addr !== board.app_base && !(board.data_regions || []).some(([lo, n]) => lo <= addr && addr + data.length <= lo + n))
				throw new PlanError(`firmware data at ${hex(addr)} is neither the application nor inside a data region of the ${board.name}`);
		}
	}
	if (mode !== MERGE && codecSegments.length) throw new PlanError(`codec data given in ${mode} mode`);
	if (mode === MERGE) {
		for (const [wlo, wlen] of board.codec.windows)
			if (!codecSegments.some(([a, d]) => a <= wlo && wlo + wlen <= a + d.length))
				throw new PlanError(`merge mode needs the whole codec window ${hex(wlo)}+0x${wlen.toString(16).toUpperCase()}`);
	}
	checkSegments(board, [...segments, ...codecSegments].filter(([, d]) => d.length));

	const windowsNote = {};
	for (const s of codecSecs) {
		const parts = board.codec.windows.filter(([lo, n]) => overlaps(s, lo, lo + n))
			.map(([lo, n]) => `${hex(Math.max(lo, s.addr))}..${hex(Math.min(lo + n, end(s)) - 1)}`);
		windowsNote[s.index] = "codec window " + parts.join(", ");
	}
	const spanLo = Math.min(...segments.map(([a]) => a));
	const spanHi = Math.max(...segments.map(([a, d]) => a + d.length));
	const steps = [];
	for (const s of board.sectors) {
		let buf = null, last = 0;
		const kinds = new Set();
		for (const [kind, segs] of [["app", segments], ["codec", codecSegments]]) {
			for (const [addr, data] of segs) {
				const lo = Math.max(addr, s.addr), hi = Math.min(addr + data.length, end(s));
				if (lo >= hi) continue;
				if (!buf) buf = new Uint8Array(s.size).fill(0xFF);
				buf.set(data.subarray(lo - addr, hi - addr), lo - s.addr);
				last = Math.max(last, hi - s.addr);
				kinds.add(kind);
			}
		}
		if (mode === RESTORE && buf) {
			// only the blocks that hold anything but 0xFF need writing; the erase leaves the rest as it was
			while (last > 0 && buf[last - 1] === 0xFF) last--;
		}
		if (!buf || (mode === RESTORE && last === 0)) {
			if (mode === RESTORE && overlaps(s, spanLo, spanHi)) steps.push(step(s, "erase", [], "", "empty in the image"));
			else if (codecIdx.has(s.index) && mode === KEEP) steps.push(step(s, "keep", [], "", windowsNote[s.index]));
			else steps.push(step(s, "skip", [], "", codecIdx.has(s.index) ? "" : "free"));
			continue;
		}
		const nblocks = Math.ceil(last / BLOCK);
		const blocks = [];
		for (let i = 0; i < nblocks; i++) blocks.push([FIRST_BLOCK + i, buf.slice(i * BLOCK, (i + 1) * BLOCK)]);
		const content = mode === RESTORE ? "image" : ["app", "codec"].filter((k) => kinds.has(k)).join("+");
		steps.push(step(s, "write", blocks, content));
	}
	const plan = new Plan(board, mode, steps);
	if (mode === KEEP && plan.erase.some((s) => codecIdx.has(s.index))) throw new PlanError("internal: keep mode would erase a codec sector");
	return plan;
}

function step(sector, action, blocks, content, note = "") {
	return { sector, action, blocks, content, note };
}

export class Plan {
	constructor(board, mode, steps) {
		this.board = board;
		this.mode = mode;
		this.steps = steps;
	}

	get erase() { return this.steps.filter((s) => s.action === "write" || s.action === "erase").map((s) => s.sector); }
	get writes() { return this.steps.filter((s) => s.action === "write"); }
	get kept() { return this.steps.filter((s) => s.action === "keep").map((s) => s.sector); }
	get totalBlocks() { return this.writes.reduce((a, s) => a + s.blocks.length, 0); }

	// The same lines as plan.py's describe() for keep and merge (the tests compare them)
	describe() {
		const b = this.board;
		const lines = [`board ${b.name}, codec mode: ${this.mode}`];
		for (const st of this.steps) {
			const s = st.sector;
			const head = `  sector ${String(s.index).padStart(2)}  ${hex(s.addr)}..${hex(end(s) - 1)}  ${String(s.size / 1024).padStart(4)} KB  `;
			if (st.action === "write") {
				const first = st.blocks[0][0], lastB = st.blocks[st.blocks.length - 1][0];
				lines.push(head + `ERASE + WRITE ${st.content}: blocks ${first}..${lastB} (${st.blocks.length} x ${BLOCK} bytes, up to ${hex(s.addr + st.blocks.length * BLOCK - 1)})`);
			} else if (st.action === "erase") {
				lines.push(head + `ERASE (${st.note})`);
			} else if (st.action === "keep") {
				lines.push(head + `keep (codec in the radio stays: ${st.note})`);
			} else {
				lines.push(head + "not touched" + (st.note ? ` (${st.note})` : ""));
			}
		}
		lines.push(`erase ${this.erase.length} sector(s): ${this.erase.map((s) => s.index).join(", ")}`);
		lines.push(`write ${this.totalBlocks} blocks (${this.totalBlocks * BLOCK} bytes)`);
		if (this.kept.length) lines.push(`never erased or written: codec sector(s) ${this.kept.map((s) => s.index).join(", ")}`);
		for (const [lo, hi] of b.protected) lines.push(`never erased or written: the bootloader ${hex(lo)}..${hex(hi - 1)}`);
		return lines;
	}
}
