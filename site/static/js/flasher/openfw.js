// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A radio that runs another open firmware (one that speaks the open-firmware CPS protocol), read before RadioFoundry is installed: a full backup
// over that firmware's own CPS serial protocol (docs/spec/open-firmware-cps.md), and from it the bootloader key and the
// voice codec, so the install needs no stock firmware file.
//
// Read only, and only R requests: area 9 (radio info), 5 (the MCU flash) and 1 (the SPI flash). Never a C control
// command (C 6 2 saves settings, and once hung a radio's USB), never area 10 (some builds don't answer it), never a write.
// Every request asks at most CLIP bytes, the original firmware's reply limit. Area 5 has no bounds check on those
// builds: a read past the end of the MCU flash can fault the radio, so exactly [rom_base, flash_end) is read, from the
// board data. Area 1 overlays the live VFO settings on a read that spans the VFO pair (2.2): that range is read in
// 32-byte pieces, so the bytes are the flash's own.
//
// A read that goes unanswered is asked again (a read changes nothing), then the backup stops cleanly: what was read
// is kept, the user is told to turn the radio off and on, and the next run continues where this one stopped.
//
// Which radios: a board with CPS radio types ([cps] in board.toml) that installs through its USB bootloader with a
// known key (family usb-dfu, key_sha256). Everything else about the radio comes from the board data.

import { hex, sha256, u32le } from "./bytes.js";
import { LinkError, parseRadioInfo, RADIO_INFO_LEN } from "./link.js";
import { findKey } from "./bootkey.js";
import { fromFlash, place } from "./stockfw.js";
import { byCodeSha, unmoveCode, unmoveBranches } from "./codecmap.js";
import { profilesFor } from "./codecsave.js";

export const CLIP = 2045;			// the most an R request asks (open-firmware-cps.md 2.1)
export const VFO = [0x7590, 0x7600];		// area 1's live VFO overlay (2.2)
export const PIECE = 32;			// the read size inside it, as qdmr reads (never triggers the overlay)
export const AREA_SPI = 1, AREA_MCU = 5, AREA_INFO = 9;
// About what 2045-byte reads give over USB full speed: the 16 MB SPI flash took 87 s on a radio
export const RATE = 190e3;

export class OpenFwError extends Error {
	constructor(message, code, params = undefined) {
		super(message);
		this.name = "OpenFwError";
		this.code = code;
		if (params) this.params = params;
	}
}

// Whether the page can back up and install this board from another open firmware: it knows the firmware's radio
// types for it, and the install goes through the USB bootloader with the key the backup finds
export const supported = (board) => !!(board?.cps_radio_types?.length && board.family === "usb-dfu" && board.key_sha256
	&& Number.isInteger(board.rom_base) && board.protected?.length);

export const typeMatches = (board, info) => !!info && (board.cps_radio_types || []).includes(info.radioType);

// The whole read's size in bytes: the MCU flash and the SPI flash (radios.json backup.spi_size)
export const backupBytes = (board, radio) => board.flash_end - board.rom_base + radio.backup.spi_size;

// The radio info (area 9), parsed (link.js parseRadioInfo) with its raw bytes; null when the port doesn't give one
export async function readInfo(link) {
	let raw;
	try {
		raw = await link.cpsRead(AREA_INFO, 0, RADIO_INFO_LEN);
	} catch (e) {
		if (e instanceof LinkError) return null;
		throw e;
	}
	const info = parseRadioInfo(raw);
	return info ? { ...info, raw } : null;
}

// What a backup holds while it runs and after it stopped: pass it back to readBackup to continue
export function newState(board, radio) {
	return {
		info: null,
		rom: { base: board.rom_base, data: new Uint8Array(board.flash_end - board.rom_base).fill(0xFF), done: 0 },
		spi: { data: new Uint8Array(radio.backup.spi_size).fill(0xFF), done: 0 },
		stopped: null,		// {part: "internal" | "spi", addr, why: "no answer" | "refused"}
		requests: 0,
	};
}

// The requests that read [part.done, size) of an area: CLIP bytes each, the avoid range in PIECE-byte requests
export function pieces(from, size, { chunk = CLIP, avoid = null } = {}) {
	const out = [];
	for (let a = from; a < size;) {
		let n = Math.min(chunk, size - a);
		if (avoid && a < avoid[1] && a + n > avoid[0]) n = a >= avoid[0] ? Math.min(PIECE, avoid[1] - a) : avoid[0] - a;
		out.push([a, n]);
		a += n;
	}
	return out;
}

async function dump(link, area, part, name, state, { avoid = null, progress = () => {}, signal } = {}) {
	const size = part.data.length;
	while (part.done < size) {
		if (signal?.aborted) throw new LinkError("stopped");
		const [[a, n]] = pieces(part.done, size, { avoid });
		let d;
		state.requests++;
		try {
			d = await link.cpsRead(area, a, n);
		} catch (e) {
			if (!(e instanceof LinkError)) throw e;
			state.stopped = { part: name, addr: (part.base || 0) + a, why: "no answer" };
			return false;
		}
		if (!d || !d.length) {
			state.stopped = { part: name, addr: (part.base || 0) + a, why: "refused" };
			return false;
		}
		const got = Math.min(d.length, n);
		part.data.set(d.subarray(0, got), a);
		part.done += got;
		progress(part.done, size);
	}
	return true;
}

// Read the radio: its info, the MCU flash (bootloader included, for the key), then the SPI flash. state: an earlier
// run's, to continue it (the same radio and firmware; otherwise it starts over). Returns the state; state.stopped
// says where and why a run stopped early. Throws OpenFwError "no-info" when the port gives no radio info, "type" when
// the radio type isn't one of this board's.
export async function readBackup(link, board, radio, { state = null, progress = () => {}, signal } = {}) {
	const info = await readInfo(link);
	if (!info) throw new OpenFwError("the radio gave no open-firmware radio info", "no-info");
	if (!typeMatches(board, info)) throw new OpenFwError(`radio type ${info.radioType} is not a ${board.name} (${board.cps_radio_types.join(", ")})`, "type", { radioType: info.radioType });
	if (!state || state.info?.git !== info.git || state.info?.radioType !== info.radioType || state.info?.built !== info.built) state = newState(board, radio);
	state.info = info;
	state.stopped = null;
	const step = (name) => (d, total) => progress(name, d, total);
	if (await dump(link, AREA_MCU, state.rom, "internal", state, { progress: step("internal"), signal }))
		await dump(link, AREA_SPI, state.spi, "spi", state, { avoid: VFO, progress: step("spi"), signal });
	return state;
}

export const complete = (state) => !!state && state.rom.done === state.rom.data.length && state.spi.done === state.spi.data.length;

// The backup bundle's parts (backup.js makeBundle): the radio info, the MCU flash from the application base (the
// bootloader is read for the key, but kept out of the bundle as in every backup), the SPI flash. A part read only in
// part is there as far as it was read, marked incomplete
export function bundleParts(state, board) {
	const parts = [{ file: "radio-info.bin", what: "radio-info", addr: 0, data: state.info.raw, complete: true }];
	const app = board.app_base - board.rom_base, rom = state.rom, spi = state.spi;
	if (rom.done > app) parts.push({ file: "internal-flash.bin", what: "internal-flash", addr: board.app_base, data: rom.data.slice(app, rom.done), complete: rom.done === rom.data.length });
	if (spi.done) parts.push({ file: "spi-flash.bin", what: "spi-flash", addr: 0, data: spi.data.slice(0, spi.done), complete: spi.done === spi.data.length });
	return parts;
}

// backup.json's firmware_now for this radio
export const firmware = (info) => ({ kind: "openfw", radio_type: info.radioType, git: info.git, built: info.built,
	flash_id: info.flashId === null ? null : hex(info.flashId) });

// ---------------------------------------------------------------------------------------------------------------
// what the MCU flash read gives

// The bootloader key: the key-sized block of the bootloader region whose SHA-256 is the board's key_sha256 (bootkey.js;
// no offset assumed). null when the region wasn't read or no block matches
export async function keyFromImage(img, board, keyLen) {
	const [lo, hi] = board.protected[0];
	const s = lo - board.rom_base, e = hi - board.rom_base;
	if (!board.key_sha256 || s < 0 || e > img.length) return null;
	const key = await findKey(img.subarray(s, e), keyLen, board.key_sha256);
	return key && (await sha256(key)) === board.key_sha256 ? key : null;
}

// The table windows of a copy whose code runs elsewhere: at their stock address, as stock has them, or with the
// islands' calls into the code re-encoded for wherever a tool put the code. The full window's SHA-256, or (for a
// profile whose merge cuts the tables, placement.tables_len) the codec's own data at its start; the rest isn't written
async function tablesAt(img, base, prof, m) {
	const [ta, tl, tsha] = prof.windows[1];
	const raw = img.subarray(ta - base, ta - base + tl);
	if (ta < base || raw.length !== tl) return null;
	const tw = (m.table_windows || [])[0];
	const cut = prof.placement?.tables_len;
	for (const t of [raw.slice(), unmoveBranches(ta, raw, m)]) {
		if (!t) continue;
		if ((await sha256(t)) === tsha) return t;
		if (cut && tw?.min_len && tw.min_start === ta && cut <= tw.min_len && (await sha256(t.subarray(0, tw.min_len))) === tw.min_sha256) {
			const out = new Uint8Array(tl);
			out.set(t.subarray(0, tw.min_len));
			return out;
		}
	}
	return null;
}

// The codec in a read of the MCU flash (img from base): {profile, segments (as the merge writes them), save (what
// codecsave.js keeps), where}, or null. First the forms stockfw.fromFlash knows (stock addresses, RadioFoundry's
// placement); then a copy another firmware moved anywhere: found by the codec's first word at a word boundary, the
// move undone with the codec map, the stock SHA-256s checked, then placed and checked as a merge from the stock file
export async function codecFromImage(img, base, board, fb) {
	const profs = profilesFor(board, fb);
	const save = (prof, segs) => segs.filter(([a]) => !(prof.placement && a === fb.descriptor.addr));
	const known = await fromFlash(img, fb, profs, base);
	if (known) return { ...known, save: save(known.profile, known.segments), where: "known" };
	const sig = fb.codec_sig;
	for (const prof of profs) {
		const m = prof.map || byCodeSha(fb.codec_maps, prof.windows[0][2])[0];
		if (!m) continue;
		const len = prof.windows[0][1];
		for (let o = 0; o + len <= img.length; o += 4) {
			if (u32le(img, o) !== sig) continue;
			const code = await unmoveCode(img.subarray(o, o + len), m, base + o);
			if (!code) continue;
			const tables = await tablesAt(img, base, prof, m);
			if (!tables) continue;
			const segs = [[prof.windows[0][0], code], [prof.windows[1][0], tables]];
			let extraOk = true;
			for (const [a, n, want] of prof.windows.slice(2)) {
				const d = img.slice(a - base, a - base + n);
				if (d.length !== n || (await sha256(d)) !== want) { extraOk = false; break; }
				segs.push([a, d]);
			}
			if (!extraOk) continue;
			let segments = segs;
			if (prof.placement) {
				try {
					segments = await place(prof, segs, fb);
				} catch (e) {
					continue;
				}
			} else if ((await sha256(tables)) !== prof.windows[1][2]) continue;
			return { profile: prof, segments, save: save(prof, segments), where: hex(base + o) };
		}
	}
	return null;
}

// The key and the codec from a backup's MCU flash read (as far as it got)
export async function analyze(state, board, fb) {
	const img = state.rom.data.subarray(0, state.rom.done);
	return { key: await keyFromImage(img, board, fb.key_len), codec: await codecFromImage(img, board.rom_base, board, fb) };
}
