// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A radio model's hardware variants (board.toml [hardware] variants, radios.json variants): which one a radio is, read
// from the version text its stock firmware reports (variant_pattern), and whether the image the page is about to write
// is the one it runs. Several variants may share one board's image; one that needs its own names another board, or
// build option values of this one. No DOM here, so the Node tests can use it.

// The printable run of text around the first match of the radio's variant_pattern ("909E.DBB.EARSAB.020"), or null
export function versionText(radio, text) {
	if (!radio?.variant_pattern || !text) return null;
	const m = new RegExp(radio.variant_pattern).exec(text);
	if (!m) return null;
	const printable = (c) => c >= " " && c <= "~";
	let a = m.index, b = m.index + m[0].length;
	while (a > 0 && b - a < 48 && printable(text[a - 1])) a--;
	while (b < text.length && b - a < 48 && printable(text[b])) b++;
	return text.slice(a, b).trim();
}

// The variant code in a version text ("DBB"), or null
export function variantCode(radio, text) {
	if (!radio?.variant_pattern || !text) return null;
	const m = new RegExp(radio.variant_pattern).exec(text);
	return m ? m[1] : null;
}

// What a variant means for writing `boardId`'s image built with `options` ({name: value}; missing names are the
// defaults). kind:
//   ok           the variant runs this image and is supported
//   dev          the variant runs this image, its level below supported: write, with its notice shown
//   options      the variant runs this board with other build option values (need: {name: value})
//   other_board  the variant needs another board's image (board: its id)
//   unsupported  the variant is listed without an image, or the code is not listed at all (variant null)
export function verdict(radio, code, boardId, options = {}) {
	const v = (radio?.variants || []).find((x) => x.code === code) || null;
	if (!v || !v.runs) return { kind: "unsupported", variant: v, code };
	if (v.runs.board !== boardId) return { kind: "other_board", variant: v, code, board: v.runs.board };
	const defaults = Object.fromEntries((radio.flash?.build_options || []).map((o) => [o.name, o.values[0].id]));
	const need = Object.entries(v.runs.options || {}).filter(([n, val]) => (options[n] ?? defaults[n]) !== val);
	if (need.length) return { kind: "options", variant: v, code, need: Object.fromEntries(need) };
	return { kind: v.level === "supported" ? "ok" : "dev", variant: v, code };
}

// Whether a verdict lets the page write
export const writable = (vd) => vd.kind === "ok" || vd.kind === "dev";

// The variants a radio lists, for the page: the supported ones, those in development, the ones not supported
export function groups(radio) {
	const vs = radio?.variants || [];
	return {
		supported: vs.filter((v) => v.runs && v.level === "supported"),
		dev: vs.filter((v) => v.runs && v.level !== "supported"),
		none: vs.filter((v) => !v.runs),
	};
}
