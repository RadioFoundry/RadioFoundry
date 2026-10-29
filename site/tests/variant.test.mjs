// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A radio model's hardware variants (variant.js): the code read from a stock version text, and what it means for the
// image about to be written. A synthetic radio covers every kind of verdict; the real board data must satisfy the same
// rules tools/boards.py checks.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

import { versionText, variantCode, verdict, writable, groups } from "../static/js/flasher/variant.js";

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
const radios = JSON.parse(readFileSync(join(DIR, "radios.json"), "utf8")).radios;

// One model, four kinds of hardware: A and B share the default image, C needs a build option of it, D another board's
// image, E nothing yet; B is in development
const radio = {
	flash: { board: "m1", build_options: [{ name: "rf", values: [{ id: "one" }, { id: "two" }] }] },
	variant_pattern: "V1\\.([A-Z])\\.X",
	variants: [
		{ code: "A", name: "first", level: "supported", runs: { board: "m1" } },
		{ code: "B", name: "second", level: "testing", notice: "careful", runs: { board: "m1" } },
		{ code: "C", name: "third", level: "supported", runs: { board: "m1", options: { rf: "two" } } },
		{ code: "D", name: "fourth", level: "supported", runs: { board: "m2" } },
		{ code: "E", name: "fifth" },
	],
};

test("the code and the version text come from the radio's pattern", () => {
	const reply = "  Read_2M_\0\0\x03\0V1.B.X.017\0Jan 1\0+MODEL+";
	assert.equal(variantCode(radio, reply), "B");
	assert.equal(versionText(radio, reply), "V1.B.X.017");
	assert.equal(variantCode(radio, "V1.b.X"), null);
	assert.equal(variantCode({}, "V1.A.X"), null, "a radio without variants has no code");
	assert.equal(versionText(radio, null), null);
	// a long printable run is cut, not taken whole
	assert.ok(versionText(radio, `${"x".repeat(200)}V1.A.X${"y".repeat(200)}`).length <= 48);
});

test("a verdict for every kind of variant", () => {
	const kind = (code, board = "m1", opts = {}) => verdict(radio, code, board, opts).kind;
	assert.equal(kind("A"), "ok");
	assert.equal(kind("B"), "dev");
	assert.equal(verdict(radio, "B", "m1").variant.notice, "careful");
	// a variant that needs a build option: the default value is not it, the chosen one is
	assert.equal(kind("C"), "options");
	assert.deepEqual(verdict(radio, "C", "m1").need, { rf: "two" });
	assert.equal(kind("C", "m1", { rf: "two" }), "ok");
	assert.equal(kind("A", "m1", { rf: "two" }), "ok", "an option the variant does not name does not matter");
	assert.equal(kind("D"), "other_board");
	assert.equal(verdict(radio, "D", "m1").board, "m2");
	assert.equal(kind("D", "m2"), "ok");
	assert.equal(kind("E"), "unsupported");
	assert.equal(kind("Z"), "unsupported");
	assert.equal(verdict(radio, "Z", "m1").variant, null);
	assert.deepEqual(["A", "B", "C", "D", "E"].map((c) => writable(verdict(radio, c, "m1"))), [true, true, false, false, false]);
});

test("the variants grouped for the page", () => {
	const g = groups(radio);
	assert.deepEqual(g.supported.map((v) => v.code), ["A", "C", "D"]);
	assert.deepEqual(g.dev.map((v) => v.code), ["B"]);
	assert.deepEqual(g.none.map((v) => v.code), ["E"]);
	assert.deepEqual(groups({}), { supported: [], dev: [], none: [] });
});

test("every radio that lists variants: a supported one runs its image, and the in-development ones say why", () => {
	const listed = radios.filter((r) => r.variants?.length);
	assert.ok(listed.length > 0);
	for (const r of listed) {
		assert.ok(r.variant_pattern && r.variant_check, r.id);
		const ok = r.variants.filter((v) => verdict(r, v.code, r.flash.board).kind === "ok");
		assert.ok(r.status !== "supported" || ok.length > 0, `${r.id}: a supported radio needs a supported variant`);
		for (const v of r.variants) {
			if (v.runs && v.level !== "supported") assert.ok(v.notice, `${r.id} ${v.code}`);
			// the pattern finds each code in a version text shaped like the check text's example
			assert.equal(new RegExp(r.variant_pattern).test(r.variant_check), true, `${r.id}: the check text shows a version`);
		}
	}
});
