// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Updating a radio that runs RadioFoundry: which build it runs (the debug link's preset= and off=, or on older firmware
// its features= list matched against the releases), the build the update installs, and the features that came after it.
//
//   currentBuild(probe, radio, releases, reported)   what runs: {kind: "preset" | "custom" | "unknown", ...}
//   planUpdate(cur, sel, decided)                    the update's selection, and what it adds or must ask

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
// old: the release entries it came from (releaseOf); each also carries the probe. reported: the names features= can carry (features.json reported)
export function currentBuild(probe, radio, releases, reported) {
	const v = probe.version || {};
	const old = releaseOf(probe, releases);
	const said = { probe };
	if (v.preset && v.off !== undefined) {
		const off = split(v.off);
		if (v.preset !== "custom") return { ...said, kind: "preset", preset: v.preset, off, old };
		// the build service names every build custom: one equal to a variant is that variant
		const preset = variantOf(radio, off, old);
		return preset ? { ...said, kind: "preset", preset, off, old } : { ...said, kind: "custom", off, old };
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

// A custom build that is a variant after all: its off list equals the variant's (in the release it came from, else now)
function variantOf(radio, off, old) {
	const offSet = new Set(off);
	const universe = new Set([...off, ...old.flatMap((e) => [...split(e.features), ...split(e.off)])]);
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
	if (!cur.old.length) return null;
	return new Set([...(cur.off || []), ...split(probe?.features), ...cur.old.flatMap((e) => [...split(e.features), ...split(e.off)])]);
}

// The update's build on sel (select.js Selection for the radio, current release):
//   preset: the same variant again; added: its features the old build didn't have (said, nothing to decide)
//   custom: the same switches; ask: the features since (the user turns each on or off: decided {name: bool})
//   unknown: the closest variant, for the build step to show
// Returns {preset, added, ask, ready}: ready once every new feature of a custom build is decided
export function planUpdate(cur, sel, decided = {}) {
	const known = knownBefore(cur, cur.probe);
	const fresh = (names) => (known ? names.filter((n) => !known.has(n) && sel.supported(n)) : []);
	const variants = Object.keys(sel.radio.config);
	if (cur.kind === "unknown") {
		sel.useVariant(variants.includes(cur.guess) ? cur.guess : "full");
		return { preset: null, added: [], ask: [], ready: true };
	}
	if (cur.kind === "preset" && variants.includes(cur.preset)) {
		sel.useVariant(cur.preset);
		return { preset: cur.preset, added: fresh(sel.radio.config[cur.preset]), ask: [], ready: true };
	}
	// a custom build (or a variant this release no longer has): every switch as it was, the new ones as decided
	const off = new Set(cur.off || []);
	const ask = fresh(sel.features.map((f) => f.name));
	sel.useVariant("full");
	for (const f of sel.features) if (sel.supported(f.name) && !off.has(f.name) && !ask.includes(f.name)) sel.enable(f.name);
	for (const n of off) sel.set(n, false);
	for (const n of ask) sel.set(n, decided[n] === true);
	return { preset: null, added: [], ask, ready: ask.every((n) => n in decided) };
}
