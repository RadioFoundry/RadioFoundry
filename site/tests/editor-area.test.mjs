// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 Alex Bloom
// Exercise the editor's load/write preparation without a DOM or connected radio.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { runInNewContext } from "node:vm";
import { CodeplugImage } from "../static/js/codeplug/image.js";
import { decode, encode, empty, newChannel } from "../static/js/codeplug/model.js";
import { newSystem, setKind } from "../static/js/codeplug/channelmodes.js";
import { areaForWrite, decodeArea, putKeys } from "../static/js/codeplug/kv.js";

const app = readFileSync(new URL("../static/js/editor/app.js", import.meta.url), "utf8");
const load = app.slice(app.indexOf("function load("), app.indexOf("// Run fn as one undoable change"));
const prepare = app.match(/areaForWrite\(file\.bytes, c\.image\.bytes, .*\);/)[0];
function fixture() {
	const m = empty(), s = newSystem(m, 3, "County");
	s.tx = false;
	s.fields.noReg = true;
	s.fields.skipEnc = true;
	m.systems.push(s);
	const ch = newChannel({ name: "Fire Dispatch" });
	m.channels.push(ch);
	setKind(m, ch, 3);
	ch.system = s;
	ch.tagTx = false;
	return encode(m);
}
function send(base, radio, fromRadio) {
	const c = { image: radio }, S = { conn: c, history: { reset() {} } };
	const ctx = { S, c, model: decode(base), file: encode(decode(base)), areaForWrite, refresh() {}, draftSoon() {} };
	runInNewContext(load + `load(model, "test", {fromRadio: ${fromRadio}});` + prepare, ctx);
	return ctx;
}
test("opening an unchanged P25 file sends its systems and RX-only tags to a blank or damaged radio", () => {
	for (const damaged of [false, true]) {
		const radio = CodeplugImage.blank();
		if (damaged) radio.bytes[0xE080] = 0;
		const { file } = send(fixture(), radio, false);
		const m = decode(file);
		assert.equal(decodeArea(file.bytes).state, "valid");
		assert.equal(m.systems.length, 1);
		assert.equal(m.systems[0].tx, false);
		assert.equal(m.systems[0].fields.noReg, true);
		assert.equal(m.channels[0].kind, 3);
		assert.equal(m.channels[0].rxOnly, true);
		assert.equal(m.channels[0].tagTx, false);
	}
});
test("editing on the source radio preserves a menu setting changed since the read", () => {
	const base = fixture(), radio = base.clone();
	putKeys(radio.bytes, { arsServer: 200 });
	const { file } = send(base, radio, true);
	assert.deepEqual(file.bytes.subarray(0xE000, 0xF000), radio.bytes.subarray(0xE000, 0xF000));
});
test("opening a replacement file replaces the previous system's transmit permissions", () => {
	const base = fixture(), old = decode(base);
	old.systems[0].tx = true;
	old.systems[0].fields.noReg = false;
	const { file } = send(base, encode(old), false);
	const m = decode(file);
	assert.equal(m.systems[0].tx, false);
	assert.equal(m.systems[0].fields.noReg, true);
});
test("a changed or unidentified radio clears the merge association", async () => {
	const S = { areaSource: {} }, c = { image: {}, uid: "old", transport: { async who() { return "new"; } } };
	const code = app.slice(app.indexOf("async function onLine("), app.indexOf("// The offer to convert a format N"));
	await runInNewContext(code + "onLine(c)", { S, c, sizedRadio() {}, diag: { note() {} } });
	assert.equal(S.areaSource, null);
});
