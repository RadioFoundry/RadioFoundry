// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// K22F: place a radio's own AMBE+2 codec where ODR wants it, and build the update file the page sends
// (docs/spec/codec-gd77.md, docs/spec/update-gd77.md). A port of tools/odrflash/odrflash/k22fcodec.py and of
// odrflash's hid_update_file (cli.py); site/tests/hidboot.test.mjs checks them against the Python byte for byte.
//
// The merge moves the codec's main block to CODE_BASE and its low block after it, leaves the table window where it is,
// rewrites the code addresses and the branches the spec lists, and writes a descriptor at DESC_ADDR. G is
// data/flash-boards.json "hid": the constants and the spec of each supported stock build (addresses, lengths, hashes;
// never codec bytes). The codec bytes come from the user's own update file and go only to the user's radio.

import { crc32, sha256, u16le, u32le } from "./bytes.js";
import { UpdateFile, unpack, pack as packFile } from "./hidboot.js";

export class PackError extends Error {
	constructor(code, message) {
		super(message);
		this.code = code;
	}
}

const hex = (n) => "0x" + (n >>> 0).toString(16).toUpperCase();

function windowOf(G, image, w) {
	const lo = w.flash - G.flash_base;
	return lo >= 0 && lo + w.len <= image.length ? image.subarray(lo, lo + w.len) : null;
}

// The spec whose three windows hash to what the flat stock image holds, or null
export async function findSpec(G, image, specs = G.specs) {
	for (const s of Object.values(specs)) {
		let ok = true;
		for (const p of ["main", "low", "tab"]) {
			const w = windowOf(G, image, s[p]);
			if (!w || (await sha256(w)) !== s[p].sha256) { ok = false; break; }
		}
		if (ok) return s;
	}
	return null;
}

export function decodeBranch(h1, h2) {
	const s = (h1 >> 10) & 1, i10 = h1 & 0x3FF, j1 = (h2 >> 13) & 1, j2 = (h2 >> 11) & 1, i11 = h2 & 0x7FF;
	const i1 = ~(j1 ^ s) & 1, i2 = ~(j2 ^ s) & 1;
	const imm = (s << 24) | (i1 << 23) | (i2 << 22) | (i10 << 12) | (i11 << 1);
	return s ? imm - (1 << 25) : imm;
}

export function encodeBranch(imm, h2Old) {
	const s = imm < 0 ? 1 : 0;
	const v = imm & 0x1FFFFFF;
	const i1 = (v >> 23) & 1, i2 = (v >> 22) & 1;
	const h1 = 0xF000 | (s << 10) | ((v >> 12) & 0x3FF);
	const h2 = (h2Old & 0xD000) | ((~(i1 ^ s) & 1) << 13) | ((~(i2 ^ s) & 1) << 11) | ((v >> 1) & 0x7FF);
	return [h1, h2];
}

// Where the moved parts go: {mainNew, lowNew, end}; end - codeBase is the code region's length
export function layout(G, spec, codeBase = G.code_base) {
	const { main, low } = spec;
	const mainNew = codeBase + (((main.flash - codeBase) % 4) + 4) % 4;
	let lowNew = mainNew + main.len;
	lowNew += (((low.flash - lowNew) % 4) + 4) % 4;
	return { mainNew, lowNew, end: lowNew + low.len };
}

// Move the codec of a flat stock image (spec: one of G.specs) and patch what the move changes.
// {spec, codeAddr, code, tabAddr, tab, mainNew, lowNew}
export async function pack(G, image, spec, codeBase = G.code_base) {
	const parts = {};
	for (const p of ["main", "low", "tab"]) {
		const w = windowOf(G, image, spec[p]);
		if (!w || (await sha256(w)) !== spec[p].sha256)
			throw new PackError("window", `the ${p} window of this stock firmware is not what the ${spec.profile} spec describes`);
		parts[p] = w.slice();
	}
	const { main, low, tab } = spec;
	if (tab.flash < G.table_lo || tab.flash + tab.len > G.table_hi)
		throw new PackError("tables", "the table window is outside the range the board reserves");
	const { mainNew, lowNew, end } = layout(G, spec, codeBase);
	if (end > tab.flash)
		throw new PackError("overlap", `the moved code (${hex(codeBase)}..${hex(end)}) reaches the table window at ${hex(tab.flash)}`);
	const code = new Uint8Array(end - codeBase).fill(0xFF);
	code.set(parts.main, mainNew - codeBase);
	code.set(parts.low, lowNew - codeBase);
	const inside = (x, w) => x >= w.flash && x < w.flash + w.len;
	const newAddr = (x) => (inside(x, main) ? mainNew + x - main.flash : inside(x, low) ? lowNew + x - low.flash : x);
	// [buffer, offset] of the patched copy of old address x
	const where = (x) => {
		if (inside(x, main)) return [code, mainNew - codeBase + x - main.flash];
		if (inside(x, low)) return [code, lowNew - codeBase + x - low.flash];
		if (inside(x, tab)) return [parts.tab, x - tab.flash];
		throw new PackError("address", `address ${hex(x)} is in no codec window`);
	};
	for (const [at, value] of spec.words) {
		const [buf, off] = where(at);
		if (u32le(buf, off) !== value) throw new PackError("word", `the word at ${hex(at)} is not ${hex(value)}`);
		new DataView(buf.buffer, buf.byteOffset).setUint32(off, (newAddr(value & ~1) | (value & 1)) >>> 0, true);
	}
	for (const [at, target] of spec.branches) {
		const [buf, off] = where(at);
		const h1 = u16le(buf, off), h2 = u16le(buf, off + 2);
		if ((h1 & 0xF800) !== 0xF000 || ![0xD000, 0x9000].includes(h2 & 0xD000) || at + 4 + decodeBranch(h1, h2) !== target)
			throw new PackError("branch", `no branch to ${hex(target)} at ${hex(at)}`);
		const [n1, n2] = encodeBranch(newAddr(target) - (newAddr(at) + 4), h2);
		const dv = new DataView(buf.buffer, buf.byteOffset);
		dv.setUint16(off, n1, true);
		dv.setUint16(off + 2, n2, true);
	}
	return { spec, codeAddr: codeBase, code, tabAddr: tab.flash, tab: parts.tab, mainNew, lowNew };
}

export function descriptor(G, packed, tag) {
	const words = [G.desc_magic, G.desc_version, tag, packed.codeAddr, packed.code.length, crc32(packed.code), 1,
		packed.tabAddr, packed.tab.length, crc32(packed.tab), ...new Array(12).fill(0)];
	const out = new Uint8Array(G.desc_len);
	const dv = new DataView(out.buffer);
	words.forEach((w, i) => dv.setUint32(4 * i, w >>> 0, true));
	dv.setUint32(G.desc_len - 4, crc32(out.subarray(0, G.desc_len - 4)), true);
	return out;
}

// [[flash address, bytes]] the merge writes: the code region, the table window, the descriptor
export function segments(G, packed, tag = G.placed[packed.spec.profile].tag) {
	return [[packed.codeAddr, packed.code], [packed.tabAddr, packed.tab], [G.desc_addr, descriptor(G, packed, tag)]];
}

// The flat image (from FLASH_BASE) with the application and the codec segments, 0xFF between
export function merge(G, app, packed, tag) {
	if (G.flash_base + app.length > packed.codeAddr)
		throw new PackError("app_too_large", `the application image ends at ${hex(G.flash_base + app.length)}, inside the codec at `
			+ `${hex(packed.codeAddr)}: rebuild with the window reserved (boards/gd77/memory.ld)`);
	const segs = segments(G, packed, tag);
	const end = Math.max(...segs.map(([a, d]) => a + d.length));
	const out = new Uint8Array(end - G.flash_base).fill(0xFF);
	out.set(app);
	for (const [a, d] of segs) out.set(d, a - G.flash_base);
	return out;
}

// What a user's stock update file gives: {file: UpdateFile, flat, spec (or null: a build the page can't take the codec
// from)}. Throws UpdateFileError for a file that isn't a usable update file.
export async function readDonor(G, data) {
	const file = new UpdateFile(data);
	const flat = unpack(file);
	return { file, flat, spec: await findSpec(G, flat) };
}

// odrflash's hid_update_file: the image with the codec merged in (codec "auto"), or without it ("keep"), written into
// a copy of the donor. {file: the update file's bytes, image: the flat image, profile: the codec's build or null}
export async function updateFile(G, donor, app, { codec = "auto" } = {}) {
	let image = app, profile = null;
	if (codec !== "keep") {
		if (!donor.spec)
			throw new PackError("unsupported", `${donor.file.label()} is not one of the supported stock builds (${Object.keys(G.specs).sort().join(", ")}), `
				+ "so its codec cannot be placed");
		image = merge(G, app, await pack(G, donor.flat, donor.spec));
		profile = donor.spec.profile;
	}
	if (image.length > donor.file.room)
		throw new PackError("too_large", `the image is ${image.length} bytes, the stock update file has room for ${donor.file.room}`);
	return { file: packFile(image, donor.file), image, profile };
}

// A user's own image: a build with the codec already merged (make's odr_<board>_codec.bin, with a descriptor at DESC_ADDR)
// gives back its application part, since the codec comes from the update file anyway. Anything else is returned as is.
export function ownImage(G, data) {
	const at = G.desc_addr - G.flash_base;
	if (data.length < at + 4 || u32le(data, at) !== G.desc_magic) return data;
	let end = Math.min(data.length, G.code_base - G.flash_base);
	while (end > 0 && data[end - 1] === 0xFF) end--;
	return data.slice(0, Math.ceil(end / 4) * 4);
}
