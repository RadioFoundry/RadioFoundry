// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The K22F stub over WebSerial: updating a GD-77 that already runs ODR without its update mode, ported from
// stub/k22stub.py (docs/spec/stub-k22f.md). The stub runs the shared protocol engine, so the client is f4stub.js's
// F4Stub with this chip; what is here is the chip, the GD-77's segments and the install.
//
// Every fact comes from Python: data/flash-boards.json gd77.stub (repo.k22_flash(): the chip, each radio's owned flash,
// codec area and board record) and data/k22-stub.json (the embedded stub image). The radio's loader below 0x4000 and
// the last flash sector are never written, so update mode always remains. Unverified: nothing here has run on a GD-77.

import { hex } from "./bytes.js";
import { F4Stub } from "./f4stub.js";
import { StubError } from "./stub.js";

// The chip of gd77.stub as f4stub.js takes it: 2 KB sectors, 8-byte program units, the application from 0x4000
export function k22Chip(st) {
	const sectors = (flashKb) => Array.from({ length: (flashKb * 1024) / st.sector }, (_, i) => ({ index: i, addr: st.flash_base + i * st.sector, size: st.sector }));
	return Object.freeze({
		title: st.title, name: st.name, magic: st.magic, flashBase: st.flash_base, flashKb: st.flash_kb, progUnit: st.prog_unit,
		appBase: st.app_base, eraseTimeout: st.erase_timeout_ms, minVersion: st.protocol, sectors,
		spOk: (sp) => sp % 4 === 0 && sp >= st.stack[0] && sp <= st.stack[1],
	});
}

// The radio of a debug link VERSION board name, or null
export function k22Board(st, name) {
	return Object.keys(st.boards).find((id) => id === name || st.boards[id].debuglink_names.includes(name)) || null;
}

// A client for the stub on stream, for radio boardId of gd77.stub (opts: log, reopen, retries)
export function k22Stub(stream, st, boardId, opts = {}) {
	return new F4Stub(stream, { ...opts, chip: k22Chip(st), board: st.boards[boardId] });
}

// [[addr, bytes]] for an image from the application base (stub/f4stub.py load_segments): the application alone, or for
// an image merged with the codec (what gd77codec.js merge() gives, ending at codec_image_end) the application and the
// whole codec area, the rest of it counted erased
export function k22Segments(board, appBase, data) {
	const [lo, hi] = board.codec;
	if (appBase + data.length <= lo) return [[appBase, data]];
	const end = board.codec_image_end ?? hi;
	if (appBase + data.length !== end)
		throw new StubError(`the image runs to ${hex(appBase + data.length)}: one merged with the codec runs to ${hex(end)}, the application alone ends before ${hex(lo)}`);
	const cut = lo - appBase;
	let n = cut;
	while (n > 0 && data[n - 1] === 0xFF) n--;
	const area = new Uint8Array(hi - lo).fill(0xFF);
	area.set(data.subarray(cut));
	return [[appBase, data.slice(0, n)], [lo, area]];
}

// The flash an install may write: the firmware's own, and the codec area when a segment carries all of it
export function ownedFor(board, segs) {
	const [lo, hi] = board.codec;
	return segs.some(([a, d]) => a === lo && d.length === hi - lo) ? [...board.owned, board.codec] : board.owned;
}

// Install image (from the application base) through a stub already entered: the board record first (its echo checked),
// then only the sectors that differ, in the interlock's order, verified by CRC. The application alone leaves the codec
// area as it is; the sector the application ends in also holds the codec's first bytes, which are read back, handed to
// onKept(sector) and written again unchanged. Returns {written, same}.
export async function k22Install(stub, image, { onKept, progress } = {}) {
	const board = stub.board;
	const segs = k22Segments(board, stub.chip.appBase, image);
	await stub.configure();
	return stub.writeImage(ownedFor(board, segs), segs, { onKept, progress });
}
