// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The Codeplug Editor's record forms (static/js/editor/views.js): what a field shows and what typing into it stores.
// Run with site/tests/run.sh.

import { test } from "node:test";
import assert from "node:assert/strict";

import { channelForm } from "../static/js/editor/views.js";
import { empty } from "../static/js/codeplug/model.js";

const t = (k) => (k === "editor.tot.default" ? "Radio Default" : k);
const field = (key) => channelForm(empty(), t).find((f) => f.key === key);

test("channel TX timeout: 0 shows as the radio default, the rest in the 15 s steps the record stores", () => {
	const f = field("tot");
	assert.equal(f.format(0), "Radio Default");
	assert.equal(f.format(180), "180");
	const typed = (s) => {
		const r = { tot: 99 };
		return f.parse(r, "tot", s) ?? r.tot;
	};
	for (const s of ["", " ", "0", "Radio Default", "radio default", "default"]) assert.equal(typed(s), 0, JSON.stringify(s));
	assert.deepEqual(["1", "15", "16", "180", "3825"].map(typed), [15, 15, 30, 180, 3825]);
	for (const s of ["3826", "-5", "1.5", "3 min", "off"]) assert.equal(typed(s), "editor.err.tot", s);
});
