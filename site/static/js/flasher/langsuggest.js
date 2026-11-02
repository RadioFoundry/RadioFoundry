// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The firmware's language packs: one switch per language, LANG_<CODE> (CONFIG_LANG_ES, the code upper-cased). When
// the browser asks for a language the radio has a pack for and the build leaves it out, the build step offers to turn
// it on. Each pack also has LANG_DEFAULT_<CODE>, the language the radio starts in: the build step sets it from its
// Radio language choice, never as a feature of its own; a language picked there is built in. No DOM here, so the host
// tests check it.

const SWITCH = /^LANG_([A-Z]{2,3})$/;
const START = /^LANG_DEFAULT_([A-Z]{2,3})$/;

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

// The language code (lower case) of a start-language switch (LANG_DEFAULT_<CODE>), or null for any other switch
export function startCode(name) {
	const m = START.exec(name || "");
	return m ? m[1].toLowerCase() : null;
}

// The languages a selection can start in: English ("en", no switch) and each pack the radio can have, with its start
// switch and whether the build includes the pack already (picking one that it doesn't turns the pack on)
export function startLanguages(features, sel) {
	const out = [{ code: "en", name: null, included: true }];
	for (const f of features) {
		const code = startCode(f.name);
		const pack = code && `LANG_${code.toUpperCase()}`;
		if (code && sel.supported(f.name) && sel.supported(pack)) out.push({ code, name: f.name, pack, included: sel.on.has(pack) });
	}
	return out;
}

// The language the selection starts in: the first start switch on (as the firmware picks), else English
export function startLanguage(features, sel) {
	const f = features.find((x) => startCode(x.name) && sel.on.has(x.name));
	return f ? startCode(f.name) : "en";
}

// Start in this language: its switch on and every other off. include: a language picked by hand, whose pack is turned
// on with it; without, a pack the build leaves out can't be the start language, and the radio starts in English.
// Returns the code it starts in now.
export function setStartLanguage(features, sel, code, { include = false } = {}) {
	const pick = startLanguages(features, sel).find((l) => l.code === code && (include || l.included));
	for (const f of features) if (startCode(f.name) && f.name !== pick?.name && sel.on.has(f.name)) sel.set(f.name, false);
	if (pick?.name) sel.set(pick.name, true);	// its pack too (the start switch needs it)
	return pick?.name && sel.on.has(pick.name) ? code : "en";
}
