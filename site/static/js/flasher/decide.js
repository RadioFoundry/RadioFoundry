// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The codec decision: keep the codec already in the radio, or write it from the copy this browser saved from the radio
// or from the user's stock file. The same rules as decide_codec() in tools/rfoundry/rfoundry/cli.py (docs/dev/release.md
// "Codec handling"): where the radio's codec can't be kept or checked, the saved copy comes before the stock file.
// Returns {mode, why, risk, expectCodec} or throws DecisionError with what the user can do. why and risk are rfoundry's
// English (the trail and the tests read them); the page shows whyKey and riskKey from its catalog, with args (a value
// {key, args} is itself a catalog string).

import { KEEP, MERGE } from "./plan.js";

// message: the whole sentence (rfoundry's wording); reason: why, without the advice, for a page that gives its own
// key, args: the reason in the page's catalog; hint: the page adds that a stock file gives the codec
export class DecisionError extends Error {
	constructor(message, reason = message, { key = null, args = {}, hint = false } = {}) {
		super(message);
		this.reason = reason;
		this.key = key;
		this.args = args;
		this.hint = hint;
	}
}

const W = "flash.codec.why.", R = "flash.codec.risk.", E = "flash.codec.why_not.";

// probe: what a running RadioFoundry radio said (link.probe), or null. stockInstalled: the stock version the user says the
// radio runs, or null. runsStock: the user says the radio runs its original firmware, version known or not (implied by
// stockInstalled). haveStock: the user gave a stock file that can be a codec source. haveSaved: the codec read
// back from this radio (codecsave.js), used where an update can't keep it or the radio's codec can't be checked. A merge says where its codec comes from:
// source "stock" or "saved".
export function decideCodec(board, manifest, { requested = null, haveStock = false, probe = null, stockInstalled = null, runsStock = false, haveSaved = false }) {
	const profile = (manifest.codec || {}).profile || board.codec.id;
	const mergeHint = "select your stock firmware file, and the codec is taken from it";
	if (requested === MERGE) {
		if (!haveStock) throw new DecisionError("Using the codec from your stock firmware file requires that file.", undefined, { key: E + "need_stock" });
		return { mode: MERGE, why: "the codec is taken from your stock file, as selected", whyKey: W + "merge_selected", expectCodec: true };
	}
	if (!board.keep_codec_supported) {
		// RadioFoundry's own layout on this radio, not the radio or what it runs now: the image shares a flash sector with the codec
		const why = `On the ${board.name}, every installation overwrites part of the codec, so the codec is written back`;
		const lower = `${why.charAt(0).toLowerCase()}${why.slice(1)}`;
		const args = { board: board.name };
		if (haveSaved) return { mode: MERGE, source: "saved", why: `${lower}. The copy saved from your radio is used`, whyKey: W + "overwrite_saved", args, expectCodec: true };
		if (haveStock) return { mode: MERGE, source: "stock", why: `${lower}. It is taken from your stock file`, whyKey: W + "overwrite_stock", args, expectCodec: true };
		throw new DecisionError(`${why}; ${mergeHint}.`, `${why}.`, { key: E + "overwrite", args, hint: true });
	}
	if (probe) {
		if (!board.debuglink_names.includes(probe.board)) throw new DecisionError(`The radio reports that it is a ${probe.board}, but this firmware is for the ${board.name}.`, undefined,
			{ key: E + "wrong_radio", args: { reported: probe.board, board: board.name } });
		if (probe.codec === profile) return { mode: KEEP, why: "the radio has its codec, which is kept", whyKey: W + "kept", expectCodec: true };
		const what = probe.codec ? `it reports ${probe.codec}` : "its firmware is too old to report it";
		const args = { what: probe.codec ? { key: W + "what_reports", args: { codec: probe.codec } } : { key: W + "what_old" } };
		if (haveSaved && requested !== KEEP) return { mode: MERGE, source: "saved", why: `the radio has no codec (${what}), so the copy saved from your radio is used`, whyKey: W + "missing_saved", args, expectCodec: true };
		if (haveStock && requested !== KEEP) return { mode: MERGE, why: `the radio has no codec (${what}), so it is taken from your stock file`, whyKey: W + "missing_stock", args, expectCodec: true };
		if (requested === KEEP) return { mode: KEEP, why: `the codec is not changed, as selected, although the radio has no codec (${what})`, whyKey: W + "keep_missing", args, risk: "DMR voice is not available (FM works) until the radio is reinstalled with your stock file", riskKey: R + "no_voice", expectCodec: false };
		throw new DecisionError(`The radio has no codec (${what}); ${mergeHint}.`, `The radio has no codec (${what}).`, { key: E + "missing", args, hint: true });
	}
	if (haveSaved && requested !== KEEP) return { mode: MERGE, source: "saved", why: "the codec is written from the copy saved from your radio, so it does not depend on what the radio holds now", whyKey: W + "saved_first", expectCodec: true };
	// no version of the original firmware holds the codec where RadioFoundry keeps it, so a kept codec is known to be missing
	if ((runsStock || stockInstalled) && requested === KEEP && !board.codec.installed.length)
		return { mode: KEEP, why: "the codec is not changed, as selected, although the original firmware never stores it where RadioFoundry uses it",
			whyKey: W + "keep_never_stored", risk: "DMR voice is not available (FM works) until the radio is reinstalled with your stock file", riskKey: R + "no_voice", expectCodec: false };
	if (stockInstalled) {
		if (board.codec.installed.some((v) => v.toUpperCase() === stockInstalled.toUpperCase()))
			return { mode: KEEP, why: `the original firmware ${stockInstalled} stores the codec where RadioFoundry uses it, so it is kept`, whyKey: W + "installed_kept", args: { version: stockInstalled },
				risk: "if the radio runs a different version, DMR voice is not available (FM works) until the radio is reinstalled with your stock file", riskKey: R + "other_version", expectCodec: true };
		if (haveStock) return { mode: MERGE, why: `the original firmware ${stockInstalled} stores its codec elsewhere, so it is taken from your stock file`, whyKey: W + "installed_elsewhere", args: { version: stockInstalled }, expectCodec: true };
		throw new DecisionError(`The original firmware ${stockInstalled} stores its codec elsewhere; ${mergeHint}.`, `The original firmware ${stockInstalled} stores its codec elsewhere.`,
			{ key: E + "installed_elsewhere", args: { version: stockInstalled }, hint: true });
	}
	if (haveStock && requested !== KEEP) return { mode: MERGE, why: "the radio's codec cannot be verified, so it is taken from your stock file", whyKey: W + "unverified_stock", expectCodec: true };
	if (requested === KEEP) return { mode: KEEP, why: "the codec is kept as selected, without verification", whyKey: W + "keep_unverified", risk: "if the radio has no codec, DMR voice is not available until the radio is reinstalled with your stock file", riskKey: R + "maybe_no_voice", expectCodec: true };
	throw new DecisionError(`The radio programming utility cannot determine whether the radio has its codec; ${mergeHint}.`, `The radio programming utility cannot determine whether the radio has its codec.`,
		{ key: E + "unknown", hint: true });
}
