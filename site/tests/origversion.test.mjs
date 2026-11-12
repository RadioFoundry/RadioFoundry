// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The original firmware version read (origversion.js, a port of rfoundry's origversion.py): the answer's format, the
// version to hardware option table, the exact requests it sends (against fake.js's original firmware), and the HWOPT
// store after the install over the debug link. Run with site/tests/run.sh, or: node --test site/tests/

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

import { parseVersion, choiceFor, readDfuInfo, hwoptAction, pendingHwopt, majorsFor, answerKind, runsOriginal, READS } from "../static/js/flasher/origversion.js";
import { Link } from "../static/js/flasher/link.js";
import { FakeRadio, simulation } from "../static/js/flasher/fake.js";

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
const fb = JSON.parse(readFileSync(join(DIR, "flash-boards.json"), "utf8"));
// a board that declares the read, whichever it is (board.toml [flash.original_version])
const declared = Object.values(fb.boards).filter((b) => b.original_version).sort((a, b) => a.id.localeCompare(b.id));
const SPEC = { read: "dfu-info", hwopt: "Board revision", major: { "06": "A", "16": "B", "26": "C" } };
const v = (...b) => new Uint8Array(b);

async function radio(opts = {}) {
	const sim = await simulation(fb, declared[0].id, 7);
	return new FakeRadio(sim, { spiSize: 0x200000, ...opts });
}

test("the version in the answer; anything else is none", () => {
	assert.equal(parseVersion(v(0x56, 0x00, 0x60, 0x09)), "06.09");
	assert.equal(parseVersion(v(0x56, 0x01, 0x60, 0x45)), "16.45");
	assert.equal(parseVersion(v(0x56, 0x02, 0x60, 0x45)), "26.45");
	for (const bad of [null, v(), v(0x56, 0, 0x60), v(0x56, 0, 0x60, 9, 0), v(0x00, 0x21, 0x41), v(0x55, 0, 0x60, 9),
		v(0x56, 0x10, 0x60, 9), v(0x56, 0, 0x61, 9), v(0x56, 0x0A, 0x60, 9), v(0x56, 0, 0xA0, 9), v(0x56, 0, 0x60, 0x0A)]) {
		assert.equal(parseVersion(bad), null, String(bad));
	}
});

test("the major version settles the choice; an unknown one is not guessed", () => {
	assert.equal(choiceFor(SPEC, "06.09"), "A");
	assert.equal(choiceFor(SPEC, "16.45"), "B");
	assert.equal(choiceFor(SPEC, "26.01"), "C");
	for (const x of ["36.45", "07.09", "6.09", null, ""]) assert.equal(choiceFor(SPEC, x), null);
	assert.equal(choiceFor(null, "06.09"), null);
});

test("the declared boards: a known read, two-digit majors, the bootloader's USB IDs", () => {
	assert.ok(declared.length, "no board declares [flash.original_version]");
	for (const b of declared) {
		assert.ok(READS.includes(b.original_version.read));
		for (const [m, c] of Object.entries(b.original_version.major)) {
			assert.match(m, /^[0-9]{2}$/);
			assert.equal(choiceFor(b.original_version, `${m}.00`), c);
		}
		assert.ok(b.dfu_ids, b.id);
	}
});

test("the read sends exactly its requests to the original firmware and gets the version", async () => {
	const r = await radio();
	assert.deepEqual(await readDfuInfo(r.dfuTransport()), { kind: "original", version: "06.09" });
	assert.deepEqual(r.dfuRequests, [["in", 3, 6, 0], ["out", 1, 0, [0xA2, 0x02]], ["in", 3, 6, 0], ["in", 3, 6, 0], ["out", 6, 0, []], ["in", 2, 1024, 0]]);
	const r2 = await radio({ originalVersion: v(0x56, 0x01, 0x60, 0x45) });
	assert.deepEqual(await readDfuInfo(r2.dfuTransport()), { kind: "original", version: "16.45" });
});

test("a state left from before is cleared first", async () => {
	const r = await radio();
	r.state = 10;
	assert.deepEqual(await readDfuInfo(r.dfuTransport()), { kind: "original", version: "06.09" });
	assert.deepEqual(r.dfuRequests.slice(0, 2), [["in", 3, 6, 0], ["out", 4, 0, []]]);
	const r2 = await radio();
	r2.state = 9;
	assert.deepEqual(await readDfuInfo(r2.dfuTransport()), { kind: "original", version: "06.09" });
	assert.deepEqual(r2.dfuRequests.slice(0, 2), [["in", 3, 6, 0], ["out", 6, 0, []]]);
});

test("a stall (the bootloader, another device) is no version, and nothing more is sent", async () => {
	const r = await radio({ originalVersion: null });
	const log = [];
	assert.deepEqual(await readDfuInfo(r.dfuTransport(), { log: (m) => log.push(m) }), { kind: null, version: null });
	assert.deepEqual(r.dfuRequests, [["in", 3, 6, 0], ["out", 1, 0, [0xA2, 0x02]]]);
	assert.match(log[0], /no version/);
	// the bootloader itself answers 'T': in update mode already, no version, nothing written
	const loader = await radio({ state: "bootloader" });
	assert.deepEqual(await readDfuInfo(loader.dfuTransport()), { kind: "bootloader", version: null });
	assert.equal(loader.erased.length + loader.blocks, 0);
});

test("an odd answer is no version", async () => {
	const r = await radio({ originalVersion: v(0x00, 0x21, 0x41) });
	assert.deepEqual(await readDfuInfo(r.dfuTransport()), { kind: null, version: null });
});

test("what to do with a choice, given the radio's options", () => {
	const opts = (value) => [{ name: "Mode", value: 0, restart: false, choices: ["Off", "On"] },
		{ name: "Board revision", value, restart: true, choices: ["A", "B", "C"] }];
	assert.deepEqual(hwoptAction(opts(-1), "Board revision", "B"), { action: "store", option: 1, choice: 1, held: null });
	assert.deepEqual(hwoptAction(opts(1), "Board revision", "B"), { action: "same", option: 1, choice: 1, held: "B" });
	assert.deepEqual(hwoptAction(opts(2), "Board revision", "B"), { action: "mismatch", option: 1, choice: 1, held: "C" });
	assert.equal(hwoptAction(opts(-1), "Board revision", "D").action, "missing");
	assert.equal(hwoptAction(opts(7), "Board revision", "B").action, "missing");
	assert.equal(hwoptAction(opts(-1), "Other", "B").action, "missing");
	assert.equal(hwoptAction([], "Board revision", "B").action, "missing");
});

test("HWOPT over the debug link: list, store, and a mismatch left alone", async () => {
	const spec = declared[0].original_version;
	const choices = [...new Set(Object.values(spec.major))];
	const r = await radio({ state: "rfoundry" });
	const link = new Link(r.serialStream());
	const opts = await link.hwopts();
	assert.deepEqual(opts, [{ name: spec.hwopt, value: -1, restart: true, choices, state: "ask" }]);
	assert.equal(pendingHwopt(opts).option, 0);
	const a = hwoptAction(opts, spec.hwopt, choices[0]);
	assert.equal(a.action, "store");
	await link.hwoptSet(a.option, a.choice);
	assert.equal((await link.hwopts())[0].value, 0);
	assert.equal(pendingHwopt(await link.hwopts()), null);
	assert.deepEqual(r.events, [`hwopt ${choices[0]}`]);
	if (choices.length > 1) {
		const m = hwoptAction(await link.hwopts(), spec.hwopt, choices[1]);
		assert.equal(m.action, "mismatch");
		assert.equal(m.held, choices[0]);
	}
	await assert.rejects(link.hwoptSet(0, choices.length), /status 2/);
});

test("what the radio still needs from a person: a question, a restart due, nothing", () => {
	const o = (state) => ({ name: "Board revision", value: -1, restart: true, choices: ["A", "B"], state });
	assert.equal(pendingHwopt([]), null);
	assert.equal(pendingHwopt([o("ok"), o("hidden")]), null);
	assert.deepEqual(pendingHwopt([o("hidden"), o("ask")]), { option: 1, ...o("ask") });
	assert.equal(pendingHwopt([o("restart")]).state, "restart");
	assert.deepEqual(majorsFor(SPEC, "B"), ["16"]);
	assert.deepEqual(majorsFor({ major: { "26": "C", "06": "C" } }, "C"), ["06", "26"]);
	assert.deepEqual(majorsFor(null, "A"), []);
});

test("a radio the install couldn't tell: asked, refused, restart due, stored", async () => {
	const spec = declared[0].original_version;
	const choices = [...new Set(Object.values(spec.major))];
	if (choices.length < 3) return;
	// runs the first choice; refuses the last (as a radio that found a chip the last never has)
	const r = await radio({ state: "rfoundry", hwoptRunning: 0, hwoptRefuse: choices.length - 1 });
	const link = new Link(r.serialStream());
	assert.equal(pendingHwopt(await link.hwopts()).state, "ask");
	await assert.rejects(link.hwoptSet(0, choices.length - 1), /status 7/);
	await link.hwoptSet(0, 1);
	assert.equal(pendingHwopt(await link.hwopts()).state, "restart");
	await link.hwoptSet(0, 0);
	assert.equal(pendingHwopt(await link.hwopts()), null);
});

// A firmware without the state key: a question while nothing is stored
test("HWOPT without a state: derived from the stored value", async () => {
	const r = await radio({ state: "rfoundry" });
	const link = new Link(r.serialStream());
	const orig = link.request.bind(link);
	link.request = async (...a) => new TextEncoder().encode(new TextDecoder().decode(await orig(...a)).replace(/hwopt\.0\.state=\w+\n/, ""));
	assert.equal((await link.hwopts())[0].state, "ask");
});

test("the original firmware refuses what the bootloader would take for a write", async () => {
	const r = await radio();
	await assert.rejects(Promise.resolve().then(() => r.dfuTransport().out(1, 0, v(0x41, 0, 0xC0, 0, 8))), /sent to the original firmware/);
	r.enterBootloader();
	assert.deepEqual(Array.from(await r.dfuTransport().in(3, 6)).slice(4, 5), [2]);
});

test("the answer's kind: the original firmware's 'V', the bootloader's 'T', else unknown", () => {
	assert.deepEqual(answerKind(v(0x56, 0x01, 0x60, 0x45)), { kind: "original", version: "16.45" });
	assert.deepEqual(answerKind(v(0x54, 0x02, 0x10, 0x06)), { kind: "bootloader", version: null });
	assert.deepEqual(answerKind(v(0x54, 0x02, 0x10)), { kind: null, version: null });
	assert.deepEqual(answerKind(null), { kind: null, version: null });
});

test("the interface strings tell the original firmware from the bootloader before any request", async () => {
	const spec = { ...SPEC, interface: "@Internal Flash   /0x08000000/03*016Kg" };
	assert.ok(runsOriginal(spec, ["@Internal Flash   /0x08000000/03*016Kg", "@SPI Flash Memory /0x00000000/16*064Kg"]));
	assert.ok(runsOriginal(spec, ["@Internal Flash /0x08000000/03*016Kg"]));
	assert.ok(!runsOriginal(spec, ["@Internal Flash   /0x0800C000/01*016Kg,01*064Kg,07*128Kg"]));
	assert.ok(!runsOriginal(spec, [null]));
	assert.ok(!runsOriginal(spec, []));
	assert.ok(!runsOriginal(null, ["@Internal Flash   /0x08000000/03*016Kg"]));
	// the fake shows the original firmware's strings while it runs, the bootloader's in update mode
	const r = await radio();
	const spec0 = declared[0].original_version;
	assert.ok(runsOriginal(spec0, (await r.dfuTransport().fingerprint()).interfaces));
	r.enterBootloader();
	assert.ok(!runsOriginal(spec0, (await r.dfuTransport().fingerprint()).interfaces));
	assert.deepEqual(r.dfuRequests, []);
});
