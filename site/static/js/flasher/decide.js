// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The codec decision: keep the codec already in the radio, or write it from the user's stock file. The same rules as
// decide_codec() in tools/odrflash/odrflash/cli.py (docs/dev/release.md "Codec handling"). Returns
// {mode, why, risk, expectCodec} or throws DecisionError with what the user can do.

import { KEEP, MERGE } from "./plan.js";

// message: the whole sentence (odrflash's wording); reason: why, without the advice, for a page that gives its own
export class DecisionError extends Error {
	constructor(message, reason = message) {
		super(message);
		this.reason = reason;
	}
}

// probe: what a running ODR radio said (link.probe), or null. stockInstalled: the stock version the user says the
// radio runs, or null. haveStock: the user gave a stock file that can be a codec source. haveSaved: the codec read
// back from this radio (codecsave.js), used where an update can't keep it. A merge says where its codec comes from:
// source "stock" or "saved".
export function decideCodec(board, manifest, { requested = null, haveStock = false, probe = null, stockInstalled = null, haveSaved = false }) {
	const profile = (manifest.codec || {}).profile || board.codec.id;
	const mergeHint = "choose your stock firmware file, and the page takes the codec from it";
	if (requested === MERGE) {
		if (!haveStock) throw new DecisionError("Taking the codec from your stock firmware file needs that file.");
		return { mode: MERGE, why: "it comes from your stock file, as you asked", expectCodec: true };
	}
	if (!board.keep_codec_supported) {
		// ODR's own layout on this radio, not the radio or what it runs now: the image shares a flash sector with the codec
		const why = `On the ${board.name}, every install overwrites part of the codec, so it has to be put back`;
		const lower = `${why.charAt(0).toLowerCase()}${why.slice(1)}`;
		if (haveSaved) return { mode: MERGE, source: "saved", why: `${lower}. The copy saved from your radio is used`, expectCodec: true };
		if (haveStock) return { mode: MERGE, source: "stock", why: `${lower}. It comes from your stock file`, expectCodec: true };
		throw new DecisionError(`${why}: ${mergeHint}.`, `${why}.`);
	}
	if (probe) {
		if (!board.debuglink_names.includes(probe.board)) throw new DecisionError(`The radio says it is a ${probe.board}, but this firmware is for the ${board.name}.`);
		if (probe.codec === profile) return { mode: KEEP, why: "the radio has its codec, and it stays there", expectCodec: true };
		const what = probe.codec ? `it reports ${probe.codec}` : "its firmware is too old to say";
		if (haveStock && requested !== KEEP) return { mode: MERGE, why: `the radio has no codec (${what}), so it comes from your stock file`, expectCodec: true };
		if (requested === KEEP) return { mode: KEEP, why: `nothing is changed, as you asked, although the radio has no codec (${what})`, risk: "DMR voice stays off (FM works) until you install again with your stock file", expectCodec: false };
		throw new DecisionError(`The radio has no codec (${what}): ${mergeHint}.`, `The radio has no codec (${what}).`);
	}
	if (stockInstalled) {
		if (board.codec.installed.some((v) => v.toUpperCase() === stockInstalled.toUpperCase()))
			return { mode: KEEP, why: `the original firmware ${stockInstalled} has the codec where OpenDigitalRadio uses it, so it stays`,
				risk: "if the radio runs a different version, DMR voice stays off (FM works) until you install again with your stock file", expectCodec: true };
		if (haveStock) return { mode: MERGE, why: `the original firmware ${stockInstalled} keeps its codec elsewhere, so it comes from your stock file`, expectCodec: true };
		throw new DecisionError(`The original firmware ${stockInstalled} keeps its codec elsewhere: ${mergeHint}.`, `The original firmware ${stockInstalled} keeps its codec elsewhere.`);
	}
	if (haveStock && requested !== KEEP) return { mode: MERGE, why: "the page can't check the radio's codec, so it comes from your stock file", expectCodec: true };
	if (requested === KEEP) return { mode: KEEP, why: "it stays as you asked, unchecked", risk: "if the radio has no codec, DMR voice stays off until you install again with your stock file", expectCodec: true };
	throw new DecisionError(`The page can't tell whether the radio has its codec: ${mergeHint}.`, `The page can't tell whether the radio has its codec.`);
}
