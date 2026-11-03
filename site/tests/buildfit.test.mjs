// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A build too big for the radio (flasher/buildfit.js): the build service's error read as the memory and how far over,
// and the build step telling the user what to turn off instead of reporting a fault.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { tooBig } from "../static/js/flasher/buildfit.js";

test("the service's too-big error gives the memory and the kilobytes over, rounded up", () => {
	assert.deepEqual(tooBig("too big: FLASH overflowed by 11208 bytes"), { region: "FLASH", bytes: 11208, kb: 11, memory: "flash" });
	assert.deepEqual(tooBig("too big: SRAM_L overflowed by 4100 bytes"), { region: "SRAM_L", bytes: 4100, kb: 5, memory: "ram" });
	assert.equal(tooBig("too big: CCM overflowed by 1 bytes").kb, 1);
	assert.equal(tooBig("too big: ROM overflowed by 2048 bytes").memory, "flash");
	// any other failure is not a fit problem
	for (const e of ["[build/custom/rfoundry_gd77.elf] Error 1", "unknown feature X", "", null, undefined])
		assert.equal(tooBig(e), null);
});

test("the build step shows a build that doesn't fit as what to change, in every language", () => {
	const src = readFileSync(new URL("../static/js/flasher/app.js", import.meta.url), "utf8");
	assert.match(src, /const fit = tooBig\(why\);\n\t+if \(fit\) throw new FitError\(fit\);/);
	assert.match(src, /err\.fit \? notice\(err\.text, "bad"\) : errorNotice\(err\.text, err\.rec\)/);
	assert.equal((src.match(/buildErrorNotice\(S\.build\.error\)/g) || []).length, 2);	// the build step and the update's
	for (const lang of ["en", "es", "it"]) {
		const cat = JSON.parse(readFileSync(new URL(`../i18n/${lang}.json`, import.meta.url), "utf8"));
		for (const k of ["flash.build.too_big", "flash.build.too_big_ram"]) assert.match(cat[k], /\{kb\}/, `${lang}: ${k}`);
		assert.match(cat["flash.build.too_big_lang"], /\{language\}/, lang);
	}
});
