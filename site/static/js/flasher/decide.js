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
	const mergeHint = "choose your stock firmware file so the codec can be written from it";
	if (requested === MERGE) {
		if (!haveStock) throw new DecisionError("Writing the codec needs your stock firmware file.");
		return { mode: MERGE, why: "the codec is written from your stock file, as asked", expectCodec: true };
	}
	if (!board.keep_codec_supported) {
		// ODR's own layout on this radio, not the radio or what it runs now: the image shares a flash sector with the codec
		const why = `On the ${board.name}, ODR's firmware shares a flash sector with the codec, so every install or update erases part of the codec`;
		const lower = `${why.charAt(0).toLowerCase()}${why.slice(1)}`;
		if (haveSaved) return { mode: MERGE, source: "saved", why: `${lower}, and the copy read back from the radio is written again`, expectCodec: true };
		if (haveStock) return { mode: MERGE, source: "stock", why: `${lower}, and it is written again from your stock file`, expectCodec: true };
		throw new DecisionError(`${why}: ${mergeHint}.`, `${why}.`);
	}
	if (probe) {
		if (!board.debuglink_names.includes(probe.board)) throw new DecisionError(`The radio says it is a ${probe.board}, but this firmware is for the ${board.name}.`);
		if (probe.codec === profile) return { mode: KEEP, why: "ODR on the radio checked its codec, so the codec sectors are left alone", expectCodec: true };
		const what = probe.codec ? `codec=${probe.codec}` : "firmware too old to report its codec";
		if (haveStock && requested !== KEEP) return { mode: MERGE, why: `the radio has no codec to keep (${what}), so it is written from your stock file`, expectCodec: true };
		if (requested === KEEP) return { mode: KEEP, why: `kept as asked, although the radio reported ${what}`, risk: "DMR voice stays off (FM works) until you flash with your stock file", expectCodec: false };
		throw new DecisionError(`The radio has no codec to keep (${what}): ${mergeHint}.`, `The radio has no codec to keep (${what}).`);
	}
	if (stockInstalled) {
		if (board.codec.installed.some((v) => v.toUpperCase() === stockInstalled.toUpperCase()))
			return { mode: KEEP, why: `the radio runs stock ${stockInstalled}, which holds this codec at the right addresses`,
				risk: "if the radio runs anything else, DMR voice stays off (FM works) until you flash again with your stock file", expectCodec: true };
		if (haveStock) return { mode: MERGE, why: `stock ${stockInstalled} keeps its codec elsewhere, so it is written from your stock file`, expectCodec: true };
		throw new DecisionError(`Stock ${stockInstalled} keeps its codec at other addresses: ${mergeHint}.`, `Stock ${stockInstalled} keeps its codec at other addresses.`);
	}
	if (haveStock && requested !== KEEP) return { mode: MERGE, why: "the radio's codec is unknown, so it is written from your stock file", expectCodec: true };
	if (requested === KEEP) return { mode: KEEP, why: "kept as asked; the radio's codec could not be checked", risk: "if the radio doesn't hold the codec, DMR voice stays off until you flash again with your stock file", expectCodec: true };
	throw new DecisionError(`The flasher can't tell whether the radio holds the codec: ${mergeHint}, or start the radio with ODR and connect it first.`, `The flasher can't tell whether the radio holds the codec.`);
}
