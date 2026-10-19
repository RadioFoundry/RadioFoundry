// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The HR_C7000 radios' program flash, seen from the web flasher: what a read of it holds (ODR, stock firmware, the
// codec and where), the codec slot ODR boots from, and the bytes an install writes. A port of tools/dm32_codec.py
// (locate, slot_bytes, parse_slot, the stock-file windows); the layout and the codec profiles come from that file and
// stub/stubtool.py through data/flash-boards.json ("c7000"), so the two read the same table.
//
// Program flash, as offsets from 0x03000000 (1 MB):
//   0x00000 .. 0x0C000   the Baofeng bootloader: never written
//   0x0C000 .. slot      the application (stock firmware, or ODR)
//   slot .. 0x100000     ODR's codec slot: a 128-byte header, the radio's own stock codec code and tables
//
// The codec is DVSI's and Baofeng's. It only ever moves between the user's radio, files and this browser.

import { concat, crc32, sha256, u32le, le32 } from "./bytes.js";

export class CodecError extends Error {}

const HEAD = 64;	// the tables are found by the CRC-32 of their first 64 bytes, then checked by SHA-256
const ENTRIES = 7;

const find = (buf, pat, from = 0) => {
	outer: for (let i = from; i + pat.length <= buf.length; i++) {
		for (let k = 0; k < pat.length; k++) if (buf[i + k] !== pat[k]) continue outer;
		return i;
	}
	return -1;
};

export const sigBytes = (c) => concat(le32(c.codec_sig[0]), le32(c.codec_sig[1]));
export const profilesFor = (c, boardId) => c.profiles.filter((p) => p.board === boardId);

// The size of the board's whole program flash (the DB25-D's is 2 MB), and the first progmem_size bytes of a read of it,
// which is where the codec and the profiles' addresses lie
export const totalSize = (c, boardId) => c.boards[boardId]?.progmem_size || c.progmem_size;
export const lowPart = (c, progmem) => (progmem.length > c.progmem_size ? progmem.subarray(0, c.progmem_size) : progmem);

// A known codec anywhere in a program flash read: each stock version puts it at its own addresses. Returns
// {profile, codeAddr, tablesAddr, code, tables} (addresses as flash offsets) or null.
export async function locateCodec(progmem, c, boardId) {
	const profs = profilesFor(c, boardId);
	if (progmem.length !== c.progmem_size || !profs.length) return null;
	const sig = sigBytes(c);
	let heads = null;
	for (let at = find(progmem, sig); at >= 0; at = find(progmem, sig, at + 1)) {
		if (at % 4) continue;
		// stock versions with the same codec bytes (DM-32 037, 039, 040) are told apart by where they put it
		const here = (p) => p.stock_file?.code === c.progmem_base + at;
		// a profile without codec hashes (a stock file known only by its whole-file hash) can't be found in a read
		for (const p of [...profs.filter(here), ...profs.filter((p) => !here(p))].filter((p) => p.code_sha256)) {
			const code = progmem.subarray(at, at + p.code_len);
			if (code.length !== p.code_len || (await sha256(code)) !== p.code_sha256) continue;
			if (!heads) {
				heads = new Map();
				for (let i = 0; i + HEAD < progmem.length; i += 4) {
					const h = crc32(progmem.subarray(i, i + HEAD));
					if (!heads.has(h)) heads.set(h, []);
					heads.get(h).push(i);
				}
			}
			for (const t of heads.get(p.tables_head_crc) || []) {
				const tables = progmem.subarray(t, t + p.tables_len);
				if (tables.length === p.tables_len && (await sha256(tables)) === p.tables_sha256)
					return { profile: p, codeAddr: at, tablesAddr: t, code: code.slice(), tables: tables.slice() };
			}
		}
	}
	return null;
}

// The flash offset of the first codec signature on a word boundary in buf[from, to), or -1. A stock build whose codec
// is a compile no profile describes (the DB25-D's 020: other code bytes, another RAM address) still starts with it,
// which tells "a codec ODR has no recipe for" from "no codec at all". The signature alone never makes a codec usable.
export function codecSignature(buf, c, from = 0, to = buf.length) {
	const sig = sigBytes(c);
	for (let at = find(buf, sig, from); at >= 0 && at + sig.length <= to; at = find(buf, sig, at + 1)) if (at % 4 === 0) return at;
	return -1;
}

// A stock file not known by its SHA-256, laid out as the radio would hold it (after the header its profiles give, at
// the application start) and searched as a program flash read is: a re-release that carries a known codec unchanged
// is accepted by the codec's own hashes, anything else is not
async function locateInFile(data, c, boardId) {
	const at = c.app_base - c.progmem_base;
	for (const hdr of new Set(profilesFor(c, boardId).map((p) => p.stock_header ?? c.stock_header))) {
		const body = data.subarray(hdr);
		if (body.length > c.progmem_size - at) continue;
		const progmem = new Uint8Array(c.progmem_size).fill(0xFF);
		progmem.set(body, at);
		const got = await locateCodec(progmem, c, boardId);
		if (got) return got;
	}
	return null;
}

const supported = (c, boardId) => {
	const names = profilesFor(c, boardId).filter((x) => x.stock_file).map((x) => x.name);
	return names.length ? ` (supported: ${names.join(", ")})` : "";
};

// The codec out of a Baofeng stock firmware file ("BFUV32-V2", file offset = address - 0x0300C000 + 0x100; the DB25-D's
// are plain), or out of a program flash backup (1 or 2 MB). A file is known by its SHA-256 or, failing that, by the
// codec's hashes wherever it holds it. Returns the same shape as locateCodec, or throws with what the file is.
export async function codecFromFile(data, c, boardId) {
	if (data.length === c.progmem_size || data.length === totalSize(c, boardId)) {
		const low = lowPart(c, data);
		const got = await locateCodec(low, c, boardId);
		if (got) return got;
		throw new CodecError(codecSignature(low, c, c.app_start, c.boards[boardId].slot[0]) >= 0
			? `this program memory backup holds a stock codec build OpenDigitalRadio has no recipe for; take the codec from a stock firmware file${supported(c, boardId)}`
			: "this program memory backup contains no codec recognized for this radio");
	}
	const sha = await sha256(data);
	const p = profilesFor(c, boardId).find((x) => x.stock_file?.sha256 === sha);
	if (!p) {
		const got = await locateInFile(data, c, boardId);
		if (got) return got;
		throw new CodecError(codecSignature(data, c) >= 0
			? `this file holds a codec build OpenDigitalRadio has no recipe for${supported(c, boardId)}`
			: `not a stock firmware file the codec can be taken from${supported(c, boardId)}`);
	}
	// the whole file is identified by its SHA-256 above; the codec hashes, where the profile has them, check the carve too
	const hdr = p.stock_header ?? c.stock_header;
	const off = (a) => a - c.app_base + hdr;
	const code = data.slice(off(p.stock_file.code), off(p.stock_file.code) + p.code_len);
	const tables = data.slice(off(p.stock_file.tables), off(p.stock_file.tables) + p.tables_len);
	if (code.length !== p.code_len || tables.length !== p.tables_len || u32le(code, 0) !== c.codec_sig[0] || u32le(code, 4) !== c.codec_sig[1] ||
		(p.code_sha256 && (await sha256(code)) !== p.code_sha256) || (p.tables_sha256 && (await sha256(tables)) !== p.tables_sha256))
		throw new CodecError("the codec areas in this file do not match");
	return { profile: p, codeAddr: p.stock_file.code - c.progmem_base, tablesAddr: p.stock_file.tables - c.progmem_base, code, tables };
}

// The slot contents: header, code, tables (4-byte aligned). The board's linker leaves the codec its RAM windows.
export function slotBytes(c, boardId, { profile: p, code, tables }) {
	const lay = c.boards[boardId];
	const codeOff = c.slot_header;
	const tabOff = codeOff + ((code.length + 3) & ~3);
	if (!(lay.run[0] <= p.run && p.run + code.length <= lay.run[1])) throw new CodecError("code run address outside the reserved RAM window");
	if (!(lay.tables[0] <= p.tables_ram && p.tables_ram + tables.length <= lay.tables[1])) throw new CodecError("table address outside the reserved modem SRAM window");
	const words = [c.slot_magic, c.slot_version, codeOff, code.length, p.run, tabOff, tables.length, p.tables_ram,
		...p.entries, p.tag, crc32(tables, crc32(code))];
	const out = new Uint8Array(tabOff + tables.length).fill(0xFF);
	out.fill(0, 0, codeOff);
	words.forEach((w, i) => out.set(le32(w), 4 * i));
	out.set(code, codeOff);
	out.set(tables, tabOff);
	if (out.length > lay.slot[1] - lay.slot[0]) throw new CodecError("the codec does not fit the codec slot");
	return out;
}

// A slot read back (from the radio, a backup or this browser), checked: magic, version, windows, CRC, signature and
// the board's RAM windows. Returns {length, tag, crc, codeSha} or null.
export async function parseSlot(c, boardId, slot) {
	if (!slot || slot.length < c.slot_header) return null;
	const w = (i) => u32le(slot, 4 * i);
	if (w(0) !== c.slot_magic || w(1) !== c.slot_version) return null;
	const [codeOff, codeLen, run, tabOff, tabLen, tabRam] = [w(2), w(3), w(4), w(5), w(6), w(7)];
	if (codeOff < c.slot_header || !codeLen || codeOff + codeLen > slot.length || tabOff + tabLen > slot.length) return null;
	const code = slot.subarray(codeOff, codeOff + codeLen), tables = slot.subarray(tabOff, tabOff + tabLen);
	if (crc32(tables, crc32(code)) !== w(8 + ENTRIES + 1) || u32le(code, 0) !== c.codec_sig[0] || u32le(code, 4) !== c.codec_sig[1]) return null;
	const lay = c.boards[boardId];
	if (lay && !(lay.run[0] <= run && run + codeLen <= lay.run[1] && lay.tables[0] <= tabRam && tabRam + tabLen <= lay.tables[1])) return null;
	const codeSha = await sha256(code);
	const known = profilesFor(c, boardId).find((p) => p.code_sha256 === codeSha);
	return { length: tabOff + tabLen, tag: w(8 + ENTRIES), crc: w(8 + ENTRIES + 1), codeSha, name: known?.name || null };
}

const ODR_MARK = new TextEncoder().encode("OpenDigitalRadio");

// What a program flash read holds: {firmware: "odr" | "stock" | "empty" | "unknown", slot: parseSlot() | null,
// codec: locateCodec() | null (the stock codec in the application area), otherCodec: whether firmware that isn't ODR
// carries a codec no profile describes (a stock version without a recipe)}
export async function analyze(progmem, c, boardId) {
	const app = progmem.subarray(c.app_start, c.boards[boardId].slot[0]);
	const slotRaw = progmem.subarray(c.boards[boardId].slot[0], c.boards[boardId].slot[1]);
	const slot = await parseSlot(c, boardId, slotRaw);
	const codec = await locateCodec(lowPart(c, progmem), c, boardId);
	const firmware = app.subarray(0, 4096).every((b) => b === 0xFF) ? "empty"
		: find(app, ODR_MARK) >= 0 ? "odr" : codec ? "stock" : "unknown";
	const otherCodec = firmware === "unknown" && codecSignature(app, c) >= 0;
	return { firmware, slot, slotBytes: slot ? slotRaw.slice(0, slot.length) : null, codec, otherCodec };
}

// The codec decision for an install. have: {slotInFlash, savedSlot, flashCodec, fileCodec} (any may be null): slots
// (Uint8Array) or located codecs (fileCodec either: a merged image's slot, or a codec out of a stock file). Returns {mode: "keep" | "write" | "none", slot, why, source}.
export async function decide(c, boardId, { slotInFlash = null, savedSlot = null, flashCodec = null, fileCodec = null, allowNone = false } = {}) {
	if (slotInFlash) return { mode: "keep", slot: null, source: "flash", why: "keep" };
	if (flashCodec) return { mode: "write", slot: slotBytes(c, boardId, flashCodec), source: "radio", name: flashCodec.profile.name, tag: flashCodec.profile.tag };
	if (savedSlot && (await parseSlot(c, boardId, savedSlot))) return { mode: "write", slot: savedSlot, source: "browser" };
	if (fileCodec instanceof Uint8Array && (await parseSlot(c, boardId, fileCodec))) return { mode: "write", slot: fileCodec, source: "image" };
	if (fileCodec?.profile) return { mode: "write", slot: slotBytes(c, boardId, fileCodec), source: "file", name: fileCodec.profile.name };
	if (allowNone) return { mode: "none", slot: null, source: null };
	return null;
}

// What to write: [start offset, bytes] in whole sectors. keep: the image only, the slot's sectors stay as they are.
// write: the image, 0xFF up to the slot, then the slot, 0xFF to the end of flash (the old slot's tail goes).
export function target(c, boardId, image, slot = null) {
	const lay = c.boards[boardId];
	const room = lay.slot[0] - c.app_start;
	if (image.length > room) throw new CodecError(`the image (${image.length} bytes) runs into the codec slot (${room} bytes free)`);
	if (u32le(image, 0) === 0 || new Set(image.subarray(0, 64)).size < 2) throw new CodecError("this file is not a firmware image");
	const end = slot ? c.app_end : c.app_start + Math.ceil(image.length / c.sector) * c.sector;
	const out = new Uint8Array(end - c.app_start).fill(0xFF);
	out.set(image, 0);
	if (slot) out.set(slot, room);
	return [c.app_start, out];
}

// A program flash backup put back: the application area and the slot, exactly (the bootloader isn't written)
export function restoreTarget(c, progmem) {
	if (progmem.length < c.progmem_size) throw new CodecError("not a whole program memory backup");
	return [c.app_start, progmem.slice(c.app_start, c.app_end)];
}

// Which radio a program flash read belongs to, by the Baofeng bootloader's ID string in its first 48 KB
// (tools/dm32_flash.py MODELS): a board id, or null
export function bootloaderModel(c, head) {
	const text = new TextDecoder("latin1").decode(head.subarray(0, Math.min(head.length, c.app_start)));
	for (const [id, b] of Object.entries(c.boards)) if (b.bootloader_id && text.includes(b.bootloader_id)) return id;
	return null;
}

// The stock codeplug area at the start of the data memory, which holds the factory calibration: 4 KB pages up to
// 0xC8FFF, the calibration the first whose last byte (its logical ID) is 2 (hardware-dm32.md 11.1,
// hardware-dm1801pro.md 6.4). Returns its address in area, or -1.
export const CAL_AREA_END = 0xC9000;
export function findCalibration(area) {
	for (let a = 0x1000; a + 0x1000 <= Math.min(area.length, CAL_AREA_END); a += 0x1000) if (area[a + 0xFFF] === 2) return a;
	return -1;
}
