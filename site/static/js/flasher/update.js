// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Updating a radio that runs RadioFoundry: which build it runs (the debug link's preset= and off=, or on older firmware
// its features= list matched against the releases), the build the update installs, and the features that came after it.
//
//   currentBuild(probe, radio, releases, reported, match)   what runs: {kind: "preset" | "custom" | "unknown", ...}
//   planUpdate(cur, sel, decided)                           the update's selection, and what it adds or must ask
//
// An update never moves a radio off its variant, and never changes the language it starts in unless the user picks
// one: a build from the build service is matched to its variant by its exact switch set (the service's record of it,
// match: GET /v1/builds/match), and the start language comes from VERSION start= or that record.

import { START_SINCE, startCode, startKnownAt } from "./langsuggest.js";

const split = (s) => String(s || "").split(",").filter(Boolean);

// The release entries (releases.json, this board's) of the build on the radio: its commit's, else the newest release
// made before it (a development build between two releases). [] when none is older
export function releaseOf(probe, releases) {
	const git = String(probe.git || "").replace(/-dirty$/, "");
	const same = git ? releases.filter((e) => e.git && (e.git.startsWith(git) || git.startsWith(e.git))) : [];
	if (same.length) return same;
	const at = Number(probe.version?.git_time) || 0;
	const before = releases.filter((e) => at && e.git_time && e.git_time <= at);
	const newest = before.reduce((a, e) => (!a || e.git_time > a.git_time ? e : a), null);
	return newest ? before.filter((e) => e.version === newest.version) : [];
}

// What the radio runs:
//   {kind: "preset", preset, off, old}   a release variant (off: null on firmware that doesn't say)
//   {kind: "custom", off, old}           a build of its own, every switch known
//   {kind: "unknown", guess, old}        older firmware whose features= list fits no one variant; guess: the closest
// start: the start-language switches it has on (planUpdate keeps them); null: not known (the off= rule applies)
// startAsk: start switches an earlier update may have turned on by mistake (an Update asked for this build, and the
// firmware doesn't say start=): the user keeps the language or goes back to English, decided[START_KEEP]
// known: with match, every switch the build knew (its features and off=), exactly
// old: the release entries it came from (releaseOf); each also carries the probe. reported: the names features= can
// carry (features.json reported). match: the build service's record of the radio's build, {features, from_update}
// (GET /v1/builds/match), or null
export function currentBuild(probe, radio, releases, reported, match = null) {
	const v = probe.version || {};
	const old = releaseOf(probe, releases);
	const said = { probe };
	if (v.preset && v.off !== undefined) {
		const off = split(v.off);
		const on = match ? match.features : null;
		const told = v.start !== undefined ? startSwitches(v.start) : null;
		const start = told ?? (on ? on.filter((n) => startCode(n)) : null);
		const startAsk = told === null && match?.from_update && start?.length ? start : null;
		const extra = { ...(start ? { start } : {}), ...(startAsk ? { startAsk } : {}), ...(on ? { known: new Set([...on, ...off]) } : {}) };
		if (v.preset !== "custom") return { ...said, kind: "preset", preset: v.preset, off, old, ...extra };
		// the service's record: the variant whose switches the build has, the start language aside, as far as the
		// build knew them (a switch newer than the build doesn't count)
		if (on) {
			const preset = variantByRecord(radio, on, extra.known);
			if (preset) return { ...said, kind: "preset", preset, off, old, ...extra };
			return { ...said, kind: "custom", off, old, ...extra };
		}
		// the build service names every build custom: one equal to a variant is that variant. The start language
		// (langsuggest.js) is left out of the comparison and kept: Full in Spanish is still Full
		const preset = variantOf(radio, off, old);
		if (!preset) return { ...said, kind: "custom", off, old };
		// a start switch newer than the release but not newer than the build (langsuggest.js START_SINCE) counts too
		const fromRelease = [...new Set([...old.flatMap((e) => [...split(e.features), ...split(e.off)]),
			...Object.keys(START_SINCE).filter((n) => startKnownAt(n, v.git_time))])].filter((n) => startCode(n) && !off.includes(n));
		return { ...said, kind: "preset", preset, off, old, start: told ?? fromRelease };
	}
	// older firmware: its features= list against each variant, by the names it can report
	const rep = new Set(reported);
	// only the switches: features= also names derived ones (DMR_DATA_CORE) that no variant lists
	const have = new Set(split(probe.features).filter((n) => rep.has(n)));
	const candidates = old.length ? old.map((e) => ({ id: e.variant, on: split(e.features) }))
		: Object.entries(radio.config).map(([id, on]) => ({ id, on }));
	const miss = (c) => {
		const want = c.on.filter((n) => rep.has(n));
		return want.filter((n) => !have.has(n)).length + [...have].filter((n) => !want.includes(n)).length;
	};
	const scored = candidates.map((c) => ({ id: c.id, miss: miss(c) })).sort((a, b) => a.miss - b.miss);
	const exact = scored.filter((c) => c.miss === 0);
	if (exact.length === 1) return { ...said, kind: "preset", preset: exact[0].id, off: null, old };
	return { ...said, kind: "unknown", guess: scored[0]?.id || "full", old };
}

// The start switches a VERSION start= code means: none for English
function startSwitches(code) {
	const c = String(code || "").toLowerCase();
	return c && c !== "en" ? [`LANG_DEFAULT_${c.toUpperCase()}`] : [];
}

// Whether a build's switches are a variant's, the start language aside: its start switch and the pack that brought it
// (when the variant lacks the pack) don't count. Full in Spanish is still Full. The build service decides the same way
// (odr-backend buildhost/odr_build.py same_variant)
export function sameVariant(on, variantOn) {
	const v = new Set(variantOn);
	const packs = new Set(on.filter((n) => startCode(n)).map((n) => `LANG_${startCode(n).toUpperCase()}`));
	const mine = on.filter((n) => !startCode(n) && !(packs.has(n) && !v.has(n)));
	const want = [...v].filter((n) => !startCode(n));
	return mine.length === want.length && mine.every((n) => v.has(n));
}

// The variant a service build is, from its exact switches; known: every switch it knew (a variant's switch added
// since doesn't count). null: none
function variantByRecord(radio, on, known) {
	for (const [id, list] of Object.entries(radio.config || {}))
		if (sameVariant(on, list.filter((n) => known.has(n)))) return id;
	return null;
}

// A custom build that is a variant after all: its off list equals the variant's (in the release it came from, else now)
function variantOf(radio, off, old) {
	const offSet = new Set(off);
	const universe = new Set([...off, ...old.flatMap((e) => [...split(e.features), ...split(e.off)])].filter((n) => !startCode(n)));
	for (const e of old) {
		const on = new Set(split(e.features));
		if ([...universe].every((n) => on.has(n) !== offSet.has(n))) return e.variant;
	}
	return null;
}

// The switches the radio's build knew, null without its release (nothing counts as new then). A release lists every
// switch it has off (manifest off, tools/release.py), so its variants name them all; one from before that names only
// its variants' features, and a switch on in no variant reads as new (asked once: the next update knows it)
function knownBefore(cur, probe) {
	if (cur.known) return cur.known;
	if (!cur.old.length) return null;
	return new Set([...(cur.off || []), ...split(probe?.features), ...cur.old.flatMap((e) => [...split(e.features), ...split(e.off)])]);
}

// The update's build on sel (select.js Selection for the radio, current release):
//   preset: the same variant again; added: its features the old build didn't have (said, nothing to decide)
//   custom: the same switches; ask: the features since (the user turns each on or off: decided {name: bool})
//   unknown: the closest variant, for the build step to show
// A custom build's kept features bring what they need now: added, a new switch (said, not asked: no would drop the
// feature); needed, one the build had off. dropped: [{name, blockers}] the build had on that this release can't build
// on the radio, because something it now needs the radio can't have; the user agrees (decided[DROP_OK]) or edits the build.
// Returns {preset, added, needed, dropped, ask, ready}: ready once every new feature of a custom build is decided
export const DROP_OK = "*drop";
// decided[START_KEEP]: true keeps cur.startAsk's language, false starts the radio in English
export const START_KEEP = "*start";
export function planUpdate(cur, sel, decided = {}) {
	const plan = planSwitches(cur, sel, decided);
	// the start language: as the radio has it, or as the user answered when an earlier update may have set it
	const starts = sel.features.map((f) => f.name).filter((n) => startCode(n));
	if (cur.start) {
		const keep = cur.startAsk && decided[START_KEEP] !== true ? cur.start.filter((n) => !cur.startAsk.includes(n)) : cur.start;
		for (const n of starts) if (sel.on.has(n) && !keep.includes(n)) sel.set(n, false);
		for (const n of keep) if (sel.supported(n)) sel.set(n, true);
	}
	const startAsk = cur.startAsk || [];
	return { ...plan, startAsk, ready: plan.ready && (!startAsk.length || START_KEEP in decided) };
}

function planSwitches(cur, sel, decided) {
	const known = knownBefore(cur, cur.probe);
	const fresh = (names) => (known ? names.filter((n) => !known.has(n) && sel.supported(n)) : []);
	const variants = Object.keys(sel.radio.config);
	const none = { needed: [], dropped: [], ask: [], ready: true };
	if (cur.kind === "unknown") {
		sel.useVariant(variants.includes(cur.guess) ? cur.guess : "full");
		return { ...none, preset: null, added: [] };
	}
	if (cur.kind === "preset" && variants.includes(cur.preset)) {
		sel.useVariant(cur.preset);
		return { ...none, preset: cur.preset, added: fresh(sel.radio.config[cur.preset]) };
	}
	// a custom build (or a variant this release no longer has): every switch as it was, the new ones as decided. A
	// start-language switch (langsuggest.js) is no feature to ask about: one off= doesn't name is on only if the radio's
	// build could have had it (START_SINCE), else off and the radio keeps the language it has. Not by release: without
	// releases nothing counts as new (the newest language came up on every radio updated), and a build made after a
	// language's merge but before the next release that starts in it would lose it. The service's record of the build
	// (cur.known) is exact, so it decides when there is one
	const names = sel.features.map((f) => f.name);
	const gitTime = cur.probe?.version?.git_time;
	const had = (n) => (cur.known ? cur.known.has(n) : startKnownAt(n, gitTime));
	const news = fresh(names).filter((n) => !(startCode(n) && had(n)));
	const off = new Set([...(cur.off || []), ...names.filter((n) => startCode(n) && !had(n))]);
	const kept = names.filter((n) => sel.supported(n) && (!off.has(n) || sel.required(n)) && !news.includes(n));
	sel.useVariant("full");
	// all kept ones first, so an "any of" need finds one already on; then enable() brings each one's needs
	sel.on = new Set(kept);
	for (const n of kept) {
		sel.on.delete(n);
		sel.enable(n);
	}
	const added = news.filter((n) => !startCode(n) && sel.on.has(n));
	// on in the radio's build (known, not in off= that leaves out what the radio can't have), now not buildable. Only
	// with off=: a variant this release dropped, on firmware that doesn't say off=, has no list of what was on
	const dropped = known && cur.off ? names.filter((n) => known.has(n) && !off.has(n) && !news.includes(n) && sel.allowed(n) && !sel.on.has(n))
		.map((name) => ({ name, blockers: sel.blockers(name) })) : [];
	const ask = news.filter((n) => !startCode(n) && !sel.on.has(n));
	// Off answers first, then On: a feature turned on brings what it needs, whatever order they're asked in, and an
	// asked one it needs is on and needs no answer
	for (const n of ask) if (decided[n] !== true) sel.set(n, false);
	for (const n of ask) if (decided[n] === true) sel.set(n, true);
	// after the answers: a new feature turned on brings what it needs too
	const forced = ask.filter((n) => decided[n] !== true && sel.on.has(n));
	const needed = [...[...off].filter((n) => sel.on.has(n) && !news.includes(n) && !sel.required(n)), ...forced];
	const ready = ask.every((n) => n in decided || sel.on.has(n)) && (!dropped.length || decided[DROP_OK] === true);
	return { preset: null, added, needed, dropped, ask, ready };
}
