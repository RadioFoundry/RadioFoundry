// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The firmware's language packs: one switch per language, LANG_<CODE> (CONFIG_LANG_ES, the code upper-cased). When
// the browser asks for a language the radio has a pack for and the build leaves it out, the build step offers to turn
// it on. No DOM here, so the host tests check it.

const SWITCH = /^LANG_([A-Z]{2,3})$/;

// The language code (lower case) of a pack switch, or null for any other switch
export function packCode(name) {
	const m = SWITCH.exec(name || "");
	return m ? m[1].toLowerCase() : null;
}

// The browser's languages as primary subtags, in its order, each once ("es-MX" and "es" are "es")
export function browserCodes(languages) {
	const out = [];
	for (const l of languages || []) {
		const c = String(l).split("-")[0].toLowerCase();
		if (c && !out.includes(c)) out.push(c);
	}
	return out;
}

// The pack to suggest for a selection (select.js Selection: supported(name), on): {code, name} of the first language
// the browser asks for whose pack this radio can have and the selection leaves out, or null. English is the
// firmware's own language, never a pack. dismissed: codes the user said no to.
export function suggestPack(features, languages, sel, dismissed = new Set()) {
	const packs = new Map();
	for (const f of features) {
		const code = packCode(f.name);
		if (code) packs.set(code, f.name);
	}
	for (const code of browserCodes(languages)) {
		const name = packs.get(code);
		if (!name || dismissed.has(code)) continue;
		if (!sel.supported(name)) continue;
		if (sel.on.has(name)) return null;	// the first language the browser asks for is in the build already
		return { code, name };
	}
	return null;
}
