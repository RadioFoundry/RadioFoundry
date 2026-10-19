// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The web tools' session log and bug report (static/js/diag.js), and the wire trace a codeplug write leaves in it.
// Run with site/tests/run.sh.

import { test, beforeEach } from "node:test";
import assert from "node:assert/strict";

import * as diag from "../static/js/diag.js";
import { encode, empty } from "../static/js/codeplug/model.js";
import { CpsSerial, TransportError } from "../static/js/codeplug/transport.js";
import { FakeCpsRadio } from "../static/js/codeplug/fakecps.js";

const quiet = { debug: console.debug, error: console.error };
beforeEach(() => {
	diag.reset();
	console.debug = () => {};
	console.error = () => {};
});
process.on("exit", () => Object.assign(console, quiet));

test("an error is recorded once, with its stage, code, params and cause", () => {
	const sent = [];
	diag.setSink((rec) => sent.push(rec));
	const e = new TransportError("write_refused", { addr: 0x6000 });
	e.cause = new Error("timeout");
	diag.error("write codeplug", e);
	diag.error("write codeplug", e);		// a second handler sees the same object
	const r = diag.report("it stopped");
	assert.equal(r.errors.length, 1);
	assert.equal(sent.length, 1);
	const rec = r.errors[0];
	assert.equal(rec.stage, "write codeplug");
	assert.equal(rec.code, "write_refused");
	assert.deepEqual(rec.params, { addr: 0x6000 });
	assert.equal(rec.cause.message, "timeout");
	assert.match(rec.stack, /diag\.test\.mjs/);
	assert.equal(rec.expected, false);
	assert.equal(r.description, "it stopped");
	assert.equal(r.format, "odr-bug-report/1");
	assert.ok(r.log.some((l) => l.kind === "error" && l.text.startsWith("write codeplug")));
});

test("the user's own choices are expected errors", () => {
	assert.ok(diag.expected(Object.assign(new Error("x"), { name: "NotFoundError" })));
	assert.ok(diag.expected(new TransportError("stopped")));
	assert.ok(!diag.expected(new TransportError("verify_failed")));
	assert.equal(diag.error("x", "a string").message, "a string");
});

test("the log keeps the newest entries; byte arrays become their length", () => {
	for (let i = 0; i < 2000; i++) diag.wire(`frame ${i}`);
	diag.note("done", { image: new Uint8Array(4096), n: 3 });
	const { log } = diag.report();
	assert.equal(log.length, 1500);
	assert.equal(log[0].text, "frame 501");
	assert.deepEqual(log.at(-1).data, { image: "<4096 bytes>", n: 3 });
});

test("the context is the tool's, and a failing one doesn't break the report", () => {
	diag.configure({ tool: "editor", version: "abc", context: () => ({ radio: "dm1701" }) });
	assert.deepEqual(diag.report().context, { radio: "dm1701" });
	assert.equal(diag.report().tool, "editor");
	diag.configure({ context: () => { throw new Error("no model"); } });
	assert.deepEqual(diag.report().context, { failed: "no model" });
});

test("automatic reports lose callsigns, DMR IDs and email addresses", () => {
	assert.equal(diag.scrub("channel N4NQV id 3101276 mail a.b@example.com"), "channel [call] id [id] mail [email]");
	assert.equal(diag.scrub("VE3ABC, 2E0XYZ, G4A"), "[call], [call], [call]");
	// addresses, versions and protocol words stay
	assert.equal(diag.scrub("X 2 0x6a000 1024 at 0x00123456 v1.2.3 DM-1701 HR_C6000"), "X 2 0x6a000 1024 at 0x00123456 v1.2.3 DM-1701 HR_C6000");
});

test("a refused codeplug write leaves its frames in the log", async () => {
	const radio = new FakeCpsRadio({ board: "DM-1701", end: 0x30000 });	// no room for the flash codeplug
	const tr = new CpsSerial(radio.stream());
	await tr.identify([{ id: "dm1701", codeplug: { base: 0, end: 0x30000, names: ["DM-1701"] } }]);
	await assert.rejects(tr.write(encode(empty()), null), (e) => e.code === "write_refused");
	const wire = diag.report().log.filter((l) => l.kind === "wire").map((l) => l.text);
	assert.ok(wire.some((w) => /^cmd 0x2 seq \d+ len 0: status 0/.test(w)), "the VERSION request");
	assert.equal(wire.filter((w) => /^X 2 0x[0-9a-f]+ \d+: refused$/.test(w)).length, 2, "tried twice");
	assert.match(wire.at(-1), /^cmd 0xf seq \d+ len 1: status 0/, "then the settings region is asked about");
	assert.ok(wire.some((w) => w.startsWith("X 1 sector") && w.endsWith(": ok")));
});
