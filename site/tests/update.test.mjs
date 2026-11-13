// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The Update task's build decision (flasher/update.js): which build a radio runs, from the VERSION keys of current
// firmware or the features= list of older firmware, and the build its update installs. Run with site/tests/run.sh, or:
// node --test site/tests/

import { test } from "node:test";
import assert from "node:assert/strict";

import { DROP_OK, currentBuild, planUpdate, releaseOf } from "../static/js/flasher/update.js";
import { Selection } from "../static/js/flasher/select.js";

// A radio with five switches: B needs A; NEW came in the second release
const FEATURES = ["A", "B", "C", "D", "NEW"].map((name) => ({ name, needs: name === "B" ? { all: ["A"], any: [] } : { all: [], any: [] } }));
const RADIO = { id: "r1", unsupported: [], not_written: [], required: [],
	config: { full: ["A", "B", "C", "NEW"], amateur: ["A", "C"] } };
const REPORTED = ["A", "B", "C", "NEW"];	// D is never named in features=
const rel = (version, git, git_time, variant, features) => ({ board: "r1", version, git, git_time, variant, features: features.join(",") });
const RELEASES = [
	rel("v26.9", "aaaa111", 1000, "full", ["A", "B", "C"]), rel("v26.9", "aaaa111", 1000, "amateur", ["A", "C"]),
	rel("v26.10", "bbbb222", 2000, "full", ["A", "B", "C", "NEW"]), rel("v26.10", "bbbb222", 2000, "amateur", ["A", "C"]),
];
const sel = () => new Selection(RADIO, FEATURES, [{ id: "full" }, { id: "amateur" }]);
const probe = (version, features = "") => ({ git: version.git, features, version });

test("the release a build came from: its commit's, else the newest one before it", () => {
	assert.deepEqual(releaseOf(probe({ git: "aaaa111" }), RELEASES).map((e) => e.variant), ["full", "amateur"]);
	assert.equal(releaseOf(probe({ git: "aaaa111-dirty" }), RELEASES)[0].version, "v26.9");
	assert.equal(releaseOf(probe({ git: "cccc333", git_time: "1500" }), RELEASES)[0].version, "v26.9");
	assert.deepEqual(releaseOf(probe({ git: "cccc333", git_time: "500" }), RELEASES), []);
});

test("a preset build updates to the same preset; its new features are only said", () => {
	const cur = currentBuild(probe({ git: "aaaa111", preset: "full", off: "D" }), RADIO, RELEASES, REPORTED);
	assert.equal(cur.kind, "preset");
	const s = sel();
	const plan = planUpdate(cur, s);
	assert.deepEqual(plan, { preset: "full", added: ["NEW"], needed: [], dropped: [], ask: [], ready: true });
	assert.deepEqual([...s.on].sort(), ["A", "B", "C", "NEW"]);
	// the amateur preset gained nothing
	const am = planUpdate(currentBuild(probe({ git: "aaaa111", preset: "amateur", off: "B,D,NEW" }), RADIO, RELEASES, REPORTED), s);
	assert.deepEqual(am.added, []);
	assert.equal(s.matches(), "amateur");
});

// releases whose manifests list what they have off (tools/release.py): every switch they knew
const WITH_OFF = RELEASES.map((e) => ({ ...e, off: ["A", "B", "C", "D", ...(e.version === "v26.10" ? ["NEW"] : [])]
	.filter((n) => !e.features.split(",").includes(n)).join(",") }));

test("a custom build keeps its switches and must decide the features added since", () => {
	const cur = currentBuild(probe({ git: "aaaa111", preset: "custom", off: "C" }), RADIO, WITH_OFF, REPORTED);
	assert.equal(cur.kind, "custom");
	const s = sel();
	let plan = planUpdate(cur, s);
	assert.deepEqual(plan.ask, ["NEW"]);
	assert.equal(plan.ready, false);
	assert.deepEqual([...s.on].sort(), ["A", "B", "D"]);	// D was on: it isn't in off
	plan = planUpdate(cur, s, { NEW: true });
	assert.equal(plan.ready, true);
	assert.deepEqual([...s.on].sort(), ["A", "B", "D", "NEW"]);
	plan = planUpdate(cur, s, { NEW: false });
	assert.deepEqual([...s.on].sort(), ["A", "B", "D"]);
});

test("a start-language switch added since is not asked about: off, the radio keeps its language", () => {
	const feats = [...FEATURES, { name: "LANG_DEFAULT_ES", needs: { all: [], any: [] } }];
	const cur = currentBuild(probe({ git: "aaaa111", preset: "custom", off: "C" }), RADIO, WITH_OFF, REPORTED);
	const s = new Selection(RADIO, feats, [{ id: "full" }, { id: "amateur" }]);
	const plan = planUpdate(cur, s);
	assert.deepEqual(plan.ask, ["NEW"]);
	assert.ok(!s.on.has("LANG_DEFAULT_ES"));
});

test("from a release without off lists, a switch on in no variant is asked about too", () => {
	const cur = currentBuild(probe({ git: "aaaa111", preset: "custom", off: "C" }, "A,B"), RADIO, RELEASES, REPORTED);
	assert.deepEqual(planUpdate(cur, sel()).ask, ["D", "NEW"]);
	// one features= names is known
	const named = currentBuild(probe({ git: "aaaa111", preset: "custom", off: "C" }, "A,B,D"), RADIO, RELEASES, REPORTED);
	assert.deepEqual(planUpdate(named, sel()).ask, ["NEW"]);
});

test("a build the service called custom that equals a variant is that variant", () => {
	const cur = currentBuild(probe({ git: "aaaa111", preset: "custom", off: "B,D" }), RADIO, RELEASES, REPORTED);
	assert.deepEqual([cur.kind, cur.preset], ["preset", "amateur"]);
});

test("a custom build from before every release asks nothing (nothing is known to be new)", () => {
	const cur = currentBuild(probe({ git: "dead000", git_time: "10", preset: "custom", off: "C" }), RADIO, RELEASES, REPORTED);
	const plan = planUpdate(cur, sel());
	assert.deepEqual([plan.ask, plan.ready], [[], true]);
});

test("older firmware: a features= list that fits one variant is that preset, else unknown with the closest", () => {
	const old = (features) => currentBuild(probe({ git: "aaaa111" }, features), RADIO, RELEASES, REPORTED);
	assert.deepEqual([old("A,B,C").kind, old("A,B,C").preset], ["preset", "full"]);
	assert.deepEqual([old("A,C").kind, old("A,C").preset], ["preset", "amateur"]);
	const odd = old("A,B");
	assert.deepEqual([odd.kind, odd.guess], ["unknown", "full"]);
	const s = sel();
	assert.deepEqual(planUpdate(odd, s), { preset: null, added: [], needed: [], dropped: [], ask: [], ready: true });
	assert.equal(s.matches(), "full");
	// a derived name features= carries (DMR_DATA_CORE) is no switch: it doesn't count against a variant
	assert.deepEqual([old("A,B,C,DMR_DATA_CORE").kind, old("A,B,C,DMR_DATA_CORE").preset], ["preset", "full"]);
});

test("a custom build equal to a variant, from releases with off lists", () => {
	const cur = currentBuild(probe({ git: "aaaa111", preset: "custom", off: "D" }), RADIO, WITH_OFF, REPORTED);
	assert.deepEqual([cur.kind, cur.preset], ["preset", "full"]);
});

test("a variant built to start in another language is still that variant, and its update keeps the language", () => {
	const feats = [...FEATURES, { name: "LANG_DEFAULT_ES", needs: { all: [], any: [] } }];
	const off = (variant, list) => ({ ...rel("v26.9", "aaaa111", 1000, variant, list[0]), off: list[1].join(",") });
	const releases = [off("full", [["A", "B", "C"], ["D", "LANG_DEFAULT_ES"]]), off("amateur", [["A", "C"], ["B", "D", "LANG_DEFAULT_ES"]])];
	const cur = currentBuild(probe({ git: "aaaa111", preset: "custom", off: "D" }), RADIO, releases, REPORTED);
	assert.deepEqual([cur.kind, cur.preset, cur.start], ["preset", "full", ["LANG_DEFAULT_ES"]]);
	const s = new Selection(RADIO, feats, [{ id: "full" }, { id: "amateur" }]);
	assert.equal(planUpdate(cur, s).preset, "full");
	assert.ok(s.on.has("LANG_DEFAULT_ES"));
});

test("a custom build's update leaves off a switch whose needs the radio can't have", () => {
	// REQ came after the radio's build and needs NET, which the radio can't have: on, the build would fail
	const feats = [...FEATURES, { name: "NET", needs: { all: [], any: [] } }, { name: "REQ", needs: { all: ["NET"], any: [] } }];
	const radio = { ...RADIO, unsupported: ["NET"] };
	const s = new Selection(radio, feats, [{ id: "full" }, { id: "amateur" }]);
	assert.equal(s.supported("REQ"), false);
	const cur = currentBuild(probe({ git: "bbbb222", preset: "custom", off: "B,D" }), radio, RELEASES, REPORTED);
	const plan = planUpdate(cur, s);
	assert.deepEqual(plan.ask, []);
	assert.equal(s.on.has("REQ"), false);
	s.set("REQ", true);
	assert.equal(s.on.has("REQ"), false);
});

test("a switch that needs any of several is offered while one of them is", () => {
	const feats = [{ name: "X", needs: { all: [], any: [] } }, { name: "Y", needs: { all: [], any: [] } },
		{ name: "Z", needs: { all: [], any: ["X", "Y"] } }];
	const s = (unsupported) => new Selection({ ...RADIO, unsupported, config: { full: [] } }, feats, [{ id: "full" }]);
	assert.equal(s(["X"]).supported("Z"), true);
	assert.equal(s(["X", "Y"]).supported("Z"), false);
});

test("a custom build's update turns on a new switch that a feature it keeps now needs", () => {
	// since the radio's build, X needs the new switch Y: Y comes on with X, said and not asked (no would drop X)
	const F = (name, all = []) => ({ name, needs: { all, any: [] } });
	const radio = { ...RADIO, config: { full: ["X", "Y", "Z"] } };
	const rels = [{ board: "r1", version: "v1", git: "aaaa111", git_time: 1, variant: "full", features: "X,Z" }];
	const cur = currentBuild(probe({ git: "aaaa111", preset: "custom", off: "Z" }), radio, rels, ["X", "Y", "Z"]);
	assert.equal(cur.kind, "custom");
	for (const decided of [{}, { Y: false }]) {
		const s = new Selection(radio, [F("X", ["Y"]), F("Y"), F("Z")], [{ id: "full" }]);
		assert.deepEqual(planUpdate(cur, s, decided), { preset: null, added: ["Y"], needed: [], dropped: [], ask: [], ready: true });
		assert.deepEqual([...s.on].sort(), ["X", "Y"]);
	}
});

test("a custom build's update turns on a switch the build had off when a feature it keeps now needs it", () => {
	const F = (name, all = []) => ({ name, needs: { all, any: [] } });
	const radio = { ...RADIO, config: { full: ["W", "X", "Z"] } };
	const rels = [{ board: "r1", version: "v1", git: "aaaa111", git_time: 1, variant: "full", features: "W,X,Z" }];
	const cur = currentBuild(probe({ git: "aaaa111", preset: "custom", off: "W,Z" }), radio, rels, ["W", "X", "Z"]);
	const s = new Selection(radio, [F("W"), F("X", ["W"]), F("Z")], [{ id: "full" }]);
	assert.deepEqual(planUpdate(cur, s), { preset: null, added: [], needed: ["W"], dropped: [], ask: [], ready: true });
	assert.deepEqual([...s.on].sort(), ["W", "X"]);
});

test("a custom build's feature this release can't build on the radio is dropped once the user agrees", () => {
	// since the radio's build, X needs U, which the radio can't have: X goes, said with what blocks it
	const F = (name, all = []) => ({ name, needs: { all, any: [] } });
	const radio = { ...RADIO, unsupported: ["U"], config: { full: ["X", "Z"] } };
	const rels = [{ board: "r1", version: "v1", git: "aaaa111", git_time: 1, variant: "full", features: "X,Z", off: "" }];
	const cur = currentBuild(probe({ git: "aaaa111", preset: "custom", off: "Z" }), radio, rels, ["X", "Z"]);
	const s = new Selection(radio, [F("U"), F("X", ["U"]), F("Z")], [{ id: "full" }]);
	let plan = planUpdate(cur, s);
	assert.deepEqual(plan.dropped, [{ name: "X", blockers: ["U"] }]);
	assert.equal(plan.ready, false);
	assert.deepEqual([...s.on], []);
	plan = planUpdate(cur, s, { [DROP_OK]: true });
	assert.equal(plan.ready, true);
	// a switch the radio never could have (off= leaves it out) isn't one the build had on
	assert.ok(!plan.dropped.some((d) => d.name === "U"));
});

test("what blocks a switch is named down to what the radio can't have", () => {
	const F = (name, all = []) => ({ name, needs: { all, any: [] } });
	const s = new Selection({ ...RADIO, unsupported: ["U"], config: { full: [] } }, [F("U"), F("M", ["U"]), F("X", ["M"]), F("Z")], [{ id: "full" }]);
	assert.deepEqual(s.blockers("X"), ["U"]);
	assert.deepEqual(s.blockers("U"), ["U"]);
	assert.deepEqual(s.blockers("Z"), []);
});

test("a need already found unbuildable stays unbuildable on another path", () => {
	// B needs D or E; E needs D; D needs U, which the radio can't have: neither path works
	const F = (name, all = [], any = []) => ({ name, needs: { all, any } });
	const s = new Selection({ ...RADIO, unsupported: ["U"], config: { full: [] } },
		[F("U"), F("D", ["U"]), F("E", ["D"]), F("B", [], ["D", "E"])], [{ id: "full" }]);
	assert.equal(s.supported("B"), false);
	s.set("B", true);
	assert.deepEqual([...s.on], []);
});

test("a new feature answered On keeps a new one it needs on, whatever the order they're asked in", () => {
	const F = (name, all = []) => ({ name, needs: { all, any: [] } });
	const radio = { ...RADIO, config: { full: ["M", "N", "Y", "Z"] } };
	const rels = [{ board: "r1", version: "v1", git: "aaaa111", git_time: 1, variant: "full", features: "Y,Z", off: "" }];
	const cur = currentBuild(probe({ git: "aaaa111", preset: "custom", off: "Y" }), radio, rels, ["Y", "Z"]);
	assert.equal(cur.kind, "custom");
	for (const feats of [[F("M", ["N"]), F("N"), F("Y"), F("Z")], [F("N"), F("M", ["N"]), F("Y"), F("Z")]]) {
		for (const decided of [{ M: true, N: false }, { M: true }]) {
			const s = new Selection(radio, feats, [{ id: "full" }]);
			const plan = planUpdate(cur, s, decided);
			assert.deepEqual([...s.on].sort(), ["M", "N", "Z"]);
			assert.deepEqual(plan.needed, ["N"]);
			assert.equal(plan.ready, true);
		}
	}
});
