// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Protected flash regions (site/static/js/flasher/protect.js): the same answers as tools/rfoundry/rfoundry/protect.py,
// and every board's list in each table the page reads (flash-boards.json boards, c7000, f4stub; radios.json flash)
// equal to the registry's (board.toml [[flash.protect]]).

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

import * as protect from "../static/js/flasher/protect.js";

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
const json = (n) => JSON.parse(readFileSync(join(DIR, n), "utf8"));
const registry = JSON.parse(readFileSync("tools/rfoundry/rfoundry/registry.json", "utf8")).boards;
const CAL = { mem: "progmem", start: 0x1FE000, end: 0x1FF000, what: "factory calibration (hardware-x.md 6.1)" };

test("check refuses an overlap, names the region and its reason", () => {
	protect.check([CAL], "progmem", 0xC000, 0x1FE000);
	protect.check([CAL], "data", 0x1FE000, 0x1FF000);
	protect.check(undefined, "progmem", 0, 0x200000);
	assert.throws(() => protect.check([CAL], "progmem", 0x1FD000, 0x1FE001), (e) =>
		e instanceof protect.ProtectedError && e.code === "protected" && /0x1FE000-0x1FF000/.test(e.message) &&
		e.message.includes(CAL.what) && e.regions[0] === CAL);
	assert.throws(() => protect.check([CAL], "eeprom", 0, 1), /unknown memory/);
});

test("a whole-chip erase is refused on a memory with a protected region", () => {
	protect.checkWhole([CAL], "data");
	assert.throws(() => protect.checkWhole([CAL], "progmem"), protect.ProtectedError);
});

test("carve leaves the protected pieces out", () => {
	const two = [CAL, { ...CAL, start: 0x100000, end: 0x102000 }];
	assert.deepEqual(protect.carve(two, "progmem", 0xC000, 0x200000), [[0xC000, 0x100000], [0x102000, 0x1FE000], [0x1FF000, 0x200000]]);
	assert.deepEqual(protect.carve(two, "progmem", 0x1FE000, 0x1FF000), []);
});

test("every table the page reads carries each board's protected regions", () => {
	const fb = json("flash-boards.json");
	const radios = json("radios.json").radios;
	for (const [id, b] of Object.entries(registry)) {
		assert.deepEqual(fb.boards[id].protect, b.protect, `flash-boards boards.${id}`);
		if (fb.c7000.boards[id]) assert.deepEqual(fb.c7000.boards[id].protect, b.protect, `c7000.boards.${id}`);
		if (fb.f4stub.boards[id]) assert.deepEqual(fb.f4stub.boards[id].protect, b.protect, `f4stub.boards.${id}`);
		const r = radios.find((x) => x.flash?.board === id);
		if (r) assert.deepEqual(r.flash.protect, b.protect, `radios.json ${r.id}`);
	}
	assert.ok(Object.values(registry).some((b) => b.protect.length), "some board has a protected region");
});
