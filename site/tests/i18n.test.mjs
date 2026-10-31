// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The page scripts' strings (static/js/i18n.js): plural forms, numbers and dates in the page's language, the catalog
// overrides of repository texts; and the build step's offer of a firmware language pack (flasher/langsuggest.js).

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { catalog } from "../static/js/i18n.js";
import { packCode, browserCodes, suggestPack } from "../static/js/flasher/langsuggest.js";

const en = JSON.parse(readFileSync(new URL("../i18n/en.json", import.meta.url), "utf8"));

test("placeholders, plural forms and numbers follow the page's language", () => {
	const c = catalog(en, "en");
	assert.equal(c.t("editor.delete_confirm", { n: 1 }), "Delete 1 record?");
	assert.equal(c.t("editor.delete_confirm", { n: 1200 }), "Delete 1,200 records?");
	// a count that isn't a number takes the other form as it is
	assert.equal(c.t("flash.build.tip_bytes", { n: "~2 k" }), "~2 k bytes");
	assert.equal(c.t("no.such.key"), "no.such.key");
	assert.equal(c.t("flash.build.count", { total: 9 }), "{n} of 9 features");
	assert.ok(c.has("editor.delete_confirm"));

	const es = catalog({ "a.one": "{n} archivo", "a.other": "{n} archivos", b: "{x} y {n}" }, "es");
	assert.equal(es.t("a", { n: 1 }), "1 archivo");
	assert.equal(es.t("a", { n: 2 }), "2 archivos");
	assert.equal(es.t("a", { n: 12345 }), "12.345 archivos");
	assert.equal(es.t("b", { x: 1234, n: 5 }), "1234 y 5");	// only n is formatted: the rest is the caller's
	assert.equal(es.num(1.5, { minimumFractionDigits: 1 }), "1,5");
	assert.match(es.date(new Date(Date.UTC(2026, 9, 31, 12)), { dateStyle: "long", timeZone: "UTC" }), /31 de octubre de 2026/);
});

test("a catalog override of a repository text, else the text", () => {
	const c = catalog({ "feature.FM.title": "Radio FM" }, "es");
	assert.equal(c.opt("feature.FM.title", "FM radio"), "Radio FM");
	assert.equal(c.opt("feature.DMR.title", "DMR"), "DMR");
});

test("every catalog string the build step's language offer uses exists", () => {
	for (const k of ["flash.build.pack_text", "flash.build.pack_on", "flash.build.pack_no"]) assert.ok(en[k], k);
	assert.match(en["flash.build.pack_text"], /\{language\}/);
});

const sel = (supported, on) => ({ supported: (n) => supported.includes(n), on: new Set(on) });
const features = [{ name: "FM" }, { name: "LANG_ES" }, { name: "LANG_DE" }, { name: "LANGUAGE" }];

test("the language pack switches are LANG_<CODE>", () => {
	assert.equal(packCode("LANG_ES"), "es");
	assert.equal(packCode("LANG_FIL"), "fil");
	assert.equal(packCode("LANGUAGE"), null);
	assert.equal(packCode("FM"), null);
	assert.deepEqual(browserCodes(["es-MX", "es", "en-US", "DE"]), ["es", "en", "de"]);
	assert.deepEqual(browserCodes(undefined), []);
});

test("the build step offers the pack the browser asks for, once, and only when the build leaves it out", () => {
	const all = ["FM", "LANG_ES", "LANG_DE"];
	assert.deepEqual(suggestPack(features, ["es-MX", "en"], sel(all, ["FM"])), { code: "es", name: "LANG_ES" });
	// already on: nothing to offer
	assert.equal(suggestPack(features, ["es-MX"], sel(all, ["FM", "LANG_ES"])), null);
	// English is the firmware's own language
	assert.equal(suggestPack(features, ["en-US", "en"], sel(all, ["FM"])), null);
	// a language the radio has no pack for, then one it has
	assert.deepEqual(suggestPack(features, ["fr", "de"], sel(all, ["FM"])), { code: "de", name: "LANG_DE" });
	// a radio that can't have the pack (not supported) is skipped
	assert.equal(suggestPack(features, ["es"], sel(["FM"], ["FM"])), null);
	// said no to: not offered again
	assert.equal(suggestPack(features, ["es"], sel(all, ["FM"]), new Set(["es"])), null);
	// no pack switch at all (an older build): nothing
	assert.equal(suggestPack([{ name: "FM" }], ["es"], sel(all, [])), null);
});

// app.js needs the DOM, so its use of the offer is checked in its source
test("the build step shows the offer with a button that turns the pack on", () => {
	const src = readFileSync(new URL("../static/js/flasher/app.js", import.meta.url), "utf8");
	assert.match(src, /suggestPack\(S\.data\.features, globalThis\.navigator\?\.languages, sel, S\.packsDismissed\)/);
	assert.match(src, /button\(t\("flash\.build\.pack_on"[^)]*\)[^)]*\), \(\) => change\(pack\.name, true\)/);
	assert.match(src, /S\.packsDismissed\.add\(pack\.code\)/);
});
