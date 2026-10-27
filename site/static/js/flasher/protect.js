// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Protected flash regions: the one check every write in the web flasher makes first. A board's [[flash.protect]]
// (docs/dev/board-registry.md, "Protected regions"; data/flash-boards.json boards[id].protect) lists the factory data
// nothing may erase or write, the calibration first. Each entry is {mem, start, end, what}: the memory ("progmem",
// "data", "nand", "secreg") and an offset range in it, progmem from the platform's flash base. A write that overlaps
// one is refused before anything is erased (check); a restore of a whole memory writes around them (carve). The same
// rules as tools/rfoundry/rfoundry/protect.py.

export const NAMES = { progmem: "program memory", data: "data memory", nand: "NAND", secreg: "security registers" };

export class ProtectedError extends Error {
	constructor(message, regions) {
		super(message);
		this.name = "ProtectedError";
		this.code = "protected";
		this.regions = regions;
	}
}

const hex = (x) => "0x" + x.toString(16).toUpperCase();
export const describe = (p) => `${NAMES[p.mem]} ${hex(p.start)}-${hex(p.end)}, ${p.what}`;

// The entries of mem that overlap [start, end)
export function overlapping(protect, mem, start, end) {
	if (!(mem in NAMES)) throw new Error(`unknown memory ${mem}`);
	return (protect || []).filter((p) => p.mem === mem && p.start < end && start < p.end);
}

// Throws ProtectedError when [start, end) of mem overlaps a protected region
export function check(protect, mem, start, end, action = "write") {
	const hit = overlapping(protect, mem, start, end);
	if (hit.length) {
		throw new ProtectedError(`refusing to ${action} ${NAMES[mem]} ${hex(start)}-${hex(end)}: it overlaps a protected region ` +
			`(${hit.map(describe).join("; ")}). Nothing was written`, hit);
	}
}

// Refuses a whole-chip erase of a memory with any protected region
export function checkWhole(protect, mem) {
	const hit = (protect || []).filter((p) => p.mem === mem);
	if (hit.length) throw new ProtectedError(`refusing to erase the whole ${NAMES[mem]}: it holds protected regions (${hit.map(describe).join("; ")})`, hit);
}

// [start, end) without mem's protected regions: [[lo, hi]] in order, the pieces a restore writes
export function carve(protect, mem, start, end) {
	const out = [];
	let at = start;
	for (const p of overlapping(protect, mem, start, end).sort((a, b) => a.start - b.start)) {
		if (p.start > at) out.push([at, p.start]);
		at = Math.max(at, p.end);
	}
	if (at < end) out.push([at, end]);
	return out;
}
