// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The keys to hold for update mode on the radio's picture (radiotop.js): which keys, where the marks go, and the
// caption that names them. Every board's data is checked in site/tests/radio_images_test.py (each key to hold is a key
// of the radio, on its drawing or listed undrawn, with a label here).

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

import { holdKeys, keyLabel, holdCaption, keyMarks } from "../static/js/flasher/radiotop.js";

const en = JSON.parse(readFileSync(new URL("../i18n/en.json", import.meta.url), "utf8"));
const tr = (key, fmt = {}) => (en[key] || key).replace(/\{(\w+)\}/g, (_, k) => (k in fmt ? fmt[k] : `{${k}}`));

test("the keys to hold come from the radio's data", () => {
	assert.deepEqual(holdKeys({ flash: { bootloader_hold: ["ptt", "sk1"] } }), ["ptt", "sk1"]);
	assert.deepEqual(holdKeys({ flash: {} }), []);
	assert.deepEqual(holdKeys(null), []);
});

test("the caption names the keys by their labels, in a list", () => {
	assert.equal(holdCaption(["ptt", "sk1"], tr, "en"), "Hold PTT and SK1");
	assert.equal(holdCaption(["p1"], tr, "en"), "Hold P1");
	assert.equal(holdCaption(["ptt", "sk1", "p1"], tr, "en"), "Hold PTT, SK1, and P1");
	assert.equal(holdCaption([], tr, "en"), "");
	// a key without a label of its own reads as its name in capitals
	assert.equal(keyLabel("vfo_mr", tr), "VFO_MR");
	assert.equal(keyLabel("ptt", tr), "PTT");
});

test("a mark for each key the picture shows, where the data puts it", () => {
	const radio = { frame_geometry: { keys: { ptt: { left: 3.3, top: 58.4, width: 3.9, height: 8.5 }, up: { left: 30, top: 80, width: 10, height: 3 } } } };
	// sk1 isn't drawn (a front view): no mark, the caption names it
	assert.deepEqual(keyMarks(radio, ["ptt", "sk1"]), [{ name: "ptt", left: 3.3, top: 58.4, width: 3.9, height: 8.5 }]);
	assert.deepEqual(keyMarks({}, ["ptt"]), []);
	assert.deepEqual(keyMarks(radio, []), []);
});

// The pages need the DOM, so what they must keep is checked in their source: every list of bootloader steps sits beside
// the picture with the keys to hold marked
test("every update mode step list marks the keys to hold", () => {
	const app = readFileSync(new URL("../static/js/flasher/app.js", import.meta.url), "utf8");
	assert.match(app, /function withPicture\(steps\) \{\n\tconst pic = radioPicture\(S\.radio, \{ keys: holdKeys\(S\.radio\) \}\);/);
	const iap = readFileSync(new URL("../static/js/flasher/iapui.js", import.meta.url), "utf8");
	assert.match(iap, /radioPicture\(S\(\)\.radio, \{ keys: holdKeys\(S\(\)\.radio\) \}\)/);
	const lists = [...iap.matchAll(/\.\.\.steps\.map\(/g)].length;
	assert.ok(lists >= 1);
	assert.equal([...iap.matchAll(/withPicture\(h\("ol", \{ class: "steps[^"]*" \},[^\n]*\.\.\.steps\.map\(/g)].length, lists);
});
