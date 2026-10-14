// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Release manifests (odr-release/1): checking one against its image and the radio. A port of verify() in
// tools/odrflash/odrflash/manifest.py; the format is described in docs/dev/release.md.

import { hex, sha256, startsWith } from "./bytes.js";
import { codecSectors } from "./plan.js";

export class ManifestError extends Error {}

const num = (v) => (typeof v === "string" ? Number(v) : v);

export function ranges(m) {
	return (m.ranges || []).map((r) => [num(r.addr), num(r.size)]);
}

export function segments(m, image) {
	const out = [];
	let pos = 0;
	for (const [addr, size] of ranges(m)) {
		out.push([addr, image.subarray(pos, pos + size)]);
		pos += size;
	}
	return out;
}

function codecConflicts(board, lo, hi) {
	const secs = codecSectors(board);
	if (!board.keep_codec_supported && !board.codec.windows.some(([a, n]) => lo < a + n && hi > a)) return [];
	return secs.filter((s) => lo < s.addr + s.size && hi > s.addr).map((s) => s.index);
}

// Returns the board the image is for. fb: data/flash-boards.json
export async function verify(m, image, fb, boardId = null) {
	if (m.format !== fb.manifest_format)
		throw new ManifestError(`unknown manifest format ${m.format} (this flasher reads ${fb.manifest_format}; reload the page)`);
	const board = fb.boards[m.board];
	if (!board) throw new ManifestError(`unknown radio ${m.board}`);
	if (boardId && boardId !== board.id)
		throw new ManifestError(`this image is for the ${board.name}, but the radio is a ${fb.boards[boardId]?.name || boardId}: refusing`);
	if (startsWith(image, fb.magic)) throw new ManifestError("this is a stock firmware file, not an OpenDigitalRadio image");
	const img = m.image || {};
	if (num(img.size) !== image.length) throw new ManifestError(`the image is ${image.length} bytes, the manifest says ${img.size}`);
	const got = await sha256(image);
	if (got !== img.sha256) throw new ManifestError("the image's SHA-256 doesn't match its manifest: the download is damaged or the files don't belong together");
	const rs = ranges(m);
	if (!rs.length || rs.reduce((a, r) => a + r[1], 0) !== image.length) throw new ManifestError("the manifest's ranges don't add up to the image size");
	if (rs[0][0] !== board.app_base) throw new ManifestError(`the first range starts at ${hex(rs[0][0])}, not at the application base ${hex(board.app_base)}`);
	for (const [addr, n] of rs.slice(1))
		if (!(board.data_regions || []).some(([lo, size]) => lo <= addr && addr + n <= lo + size))
			throw new ManifestError(`range ${hex(addr)} is not inside a flash region the ${board.name} image may use for data`);
	for (const [addr, n] of rs) {
		if (addr < board.app_base || addr + n > board.flash_end) throw new ManifestError(`range ${hex(addr)} is outside the application flash`);
		for (const [lo, hi] of board.protected) if (addr < hi && addr + n > lo) throw new ManifestError(`range ${hex(addr)} overlaps the bootloader`);
		const hit = codecConflicts(board, addr, addr + n);
		if (hit.length && !(m.codec || {}).included) throw new ManifestError(`range ${hex(addr)} reaches into codec sector(s) ${hit.join(", ")}`);
	}
	const codec = m.codec || {};
	if (codec.profile && codec.profile !== board.codec.id)
		throw new ManifestError(`the image expects codec profile ${codec.profile}; this flasher knows ${board.codec.id} (reload the page)`);
	return board;
}

// A manifest for a local build (developers: build/<board>/odr_<board>.bin)
export async function forDevBuild(board, image) {
	return {
		format: "odr-release/1", board: board.id, board_name: board.name, version: "dev", git: "unknown", dev_build: true,
		image: { file: "local", size: image.length, sha256: await sha256(image) },
		ranges: [{ addr: hex(board.app_base), size: image.length }],
		codec: { included: false, profile: board.codec.id, keep_supported: board.keep_codec_supported },
		features: "",
	};
}
