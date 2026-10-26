// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The codec decision: keep the codec already in the radio, or write it from the copy this browser saved from the radio
// or from the user's stock file. The same rules as decide_codec() in tools/rfoundry/rfoundry/cli.py (docs/dev/release.md
// "Codec handling"): where the radio's codec can't be kept or checked, the saved copy comes before the stock file.
// Returns {mode, why, risk, expectCodec} or throws DecisionError with what the user can do.

import { KEEP, MERGE } from "./plan.js";

// message: the whole sentence (rfoundry's wording); reason: why, without the advice, for a page that gives its own
export class DecisionError extends Error {
	constructor(message, reason = message) {
		super(message);
		this.reason = reason;
	}
}

// probe: what a running RadioFoundry radio said (link.probe), or null. stockInstalled: the stock version the user says the
// radio runs, or null. runsStock: the user says the radio runs its original firmware, version known or not (implied by
// stockInstalled). haveStock: the user gave a stock file that can be a codec source. haveSaved: the codec read
// back from this radio (codecsave.js), used where an update can't keep it or the radio's codec can't be checked. A merge says where its codec comes from:
// source "stock" or "saved".
export function decideCodec(board, manifest, { requested = null, haveStock = false, probe = null, stockInstalled = null, runsStock = false, haveSaved = false }) {
	const profile = (manifest.codec || {}).profile || board.codec.id;
	const mergeHint = "select your stock firmware file, and the codec is taken from it";
	if (requested === MERGE) {
		if (!haveStock) throw new DecisionError("Using the codec from your stock firmware file requires that file.");
		return { mode: MERGE, why: "the codec is taken from your stock file, as selected", expectCodec: true };
	}
	if (!board.keep_codec_supported) {
		// RadioFoundry's own layout on this radio, not the radio or what it runs now: the image shares a flash sector with the codec
		const why = `On the ${board.name}, every installation overwrites part of the codec, so the codec is written back`;
		const lower = `${why.charAt(0).toLowerCase()}${why.slice(1)}`;
		if (haveSaved) return { mode: MERGE, source: "saved", why: `${lower}. The copy saved from your radio is used`, expectCodec: true };
		if (haveStock) return { mode: MERGE, source: "stock", why: `${lower}. It is taken from your stock file`, expectCodec: true };
		throw new DecisionError(`${why}; ${mergeHint}.`, `${why}.`);
	}
	if (probe) {
		if (!board.debuglink_names.includes(probe.board)) throw new DecisionError(`The radio reports that it is a ${probe.board}, but this firmware is for the ${board.name}.`);
		if (probe.codec === profile) return { mode: KEEP, why: "the radio has its codec, which is kept", expectCodec: true };
		const what = probe.codec ? `it reports ${probe.codec}` : "its firmware is too old to report it";
		if (haveSaved && requested !== KEEP) return { mode: MERGE, source: "saved", why: `the radio has no codec (${what}), so the copy saved from your radio is used`, expectCodec: true };
		if (haveStock && requested !== KEEP) return { mode: MERGE, why: `the radio has no codec (${what}), so it is taken from your stock file`, expectCodec: true };
		if (requested === KEEP) return { mode: KEEP, why: `the codec is not changed, as selected, although the radio has no codec (${what})`, risk: "DMR voice is not available (FM works) until the radio is reinstalled with your stock file", expectCodec: false };
		throw new DecisionError(`The radio has no codec (${what}); ${mergeHint}.`, `The radio has no codec (${what}).`);
	}
	if (haveSaved && requested !== KEEP) return { mode: MERGE, source: "saved", why: "the codec is written from the copy saved from your radio, so it does not depend on what the radio holds now", expectCodec: true };
	// no version of the original firmware holds the codec where RadioFoundry keeps it, so a kept codec is known to be missing
	if ((runsStock || stockInstalled) && requested === KEEP && !board.codec.installed.length)
		return { mode: KEEP, why: "the codec is not changed, as selected, although the original firmware never stores it where RadioFoundry uses it",
			risk: "DMR voice is not available (FM works) until the radio is reinstalled with your stock file", expectCodec: false };
	if (stockInstalled) {
		if (board.codec.installed.some((v) => v.toUpperCase() === stockInstalled.toUpperCase()))
			return { mode: KEEP, why: `the original firmware ${stockInstalled} stores the codec where RadioFoundry uses it, so it is kept`,
				risk: "if the radio runs a different version, DMR voice is not available (FM works) until the radio is reinstalled with your stock file", expectCodec: true };
		if (haveStock) return { mode: MERGE, why: `the original firmware ${stockInstalled} stores its codec elsewhere, so it is taken from your stock file`, expectCodec: true };
		throw new DecisionError(`The original firmware ${stockInstalled} stores its codec elsewhere; ${mergeHint}.`, `The original firmware ${stockInstalled} stores its codec elsewhere.`);
	}
	if (haveStock && requested !== KEEP) return { mode: MERGE, why: "the radio's codec cannot be verified, so it is taken from your stock file", expectCodec: true };
	if (requested === KEEP) return { mode: KEEP, why: "the codec is kept as selected, without verification", risk: "if the radio has no codec, DMR voice is not available until the radio is reinstalled with your stock file", expectCodec: true };
	throw new DecisionError(`The radio programming utility cannot determine whether the radio has its codec; ${mergeHint}.`, `The radio programming utility cannot determine whether the radio has its codec.`);
}
