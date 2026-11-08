// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The AnyTone update mode pages (anytoneui.js) against a small stand-in for the DOM and the simulated radio
// (fakeanytone.js): connect identifies the radio and refuses a variant that has no image, takes another board's image
// or isn't the one the user picked, the boot block step takes the block from a stock file, and the write step installs
// and restores. Run with site/tests/run.sh.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

class El {
	constructor(tag) { Object.assign(this, { tagName: tag.toUpperCase(), children: [], attrs: {}, listeners: {}, className: "", dataset: {}, style: {}, isConnected: true }); }
	append(...cs) { for (const c of cs) this.children.push(c instanceof El ? c : { text: String(c) }); }
	appendChild(c) { this.append(c); return c; }
	replaceChildren(...cs) { this.children = []; this.append(...cs); }
	setAttribute(k, v) { this.attrs[k] = v; }
	setAttributeNS(ns, k, v) { this.attrs[k] = v; }
	addEventListener(ev, f) { (this.listeners[ev] ||= []).push(f); }
	get textContent() { return this.children.map((c) => (c instanceof El ? c.textContent : c.text)).join(""); }
	set textContent(v) { this.children = [{ text: String(v) }]; }
	get firstChild() { return this.children[0]; }
}
const strings = readFileSync(fileURLToPath(new URL("../i18n/en.json", import.meta.url)), "utf8");
globalThis.Node = El;
globalThis.document = { createElement: (t) => new El(t), createElementNS: (ns, t) => new El(t), body: new El("body"),
	getElementById: (id) => (id === "i18n" ? { textContent: strings } : null) };

const ui = await import("../static/js/flasher/anytoneui.js");
const at = await import("../static/js/flasher/anytone.js");
const { FakeAnytone } = await import("../static/js/flasher/fakeanytone.js");
const { concat, fromHex, sha256, equal } = await import("../static/js/flasher/bytes.js");

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
const json = (n) => JSON.parse(readFileSync(join(DIR, n), "utf8"));
const FB0 = json("flash-boards.json");
const RADIO = json("radios.json").radios.find((r) => r.flash.backend === "anytone-update");
const BOARD = FB0.boards[RADIO.flash.board];
const models = BOARD.anytone_models;
const runs = RADIO.variants.find((v) => v.runs?.board === BOARD.id).code;
const other = RADIO.variants.find((v) => v.code !== runs)?.code;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const rand = (n, seed = 1) => { const a = new Uint8Array(n); let x = seed; for (let i = 0; i < n; i++) { x = (x * 1103515245 + 12345) >>> 0; a[i] = x >>> 24; } return a; };

function page(model, task = "install", extra = {}) {
	const fwd = [];
	const S = { radio: RADIO, task, sim: true, fakeAnytone: new FakeAnytone({ model }), forward: (b) => fwd.push(b), ...extra };
	const done = { next: 0, flashes: [] };
	ui.init({ S, fb: () => FB, next: () => { done.next++; }, reset() {}, errorText: (e) => String(e?.message || e),
		flashStarted: (w) => { done.flashes.push(w); return w; }, flashFinished: async () => {}, flashMark: () => {} });
	return { S, fwd, done, last: () => fwd.at(-1) };
}
const press = async (btn) => { for (const f of btn.listeners.click || []) await f({ preventDefault() {} }); await sleep(30); };
let FB = FB0;

test("connect: a radio of the variant with an image passes; the user's wrong pick and an image-less variant are refused", async () => {
	let p = page(models[runs][0]);
	let box = ui.connectView();
	await press(p.last());
	assert.match(box.textContent, /can run RadioFoundry/);
	assert.equal(p.last().textContent, "Continue");
	if (!other) return;
	// the user said the radio is the other variant: refused, nothing goes on
	p = page(models[runs][0], "install", { atChosen: other });
	box = ui.connectView();
	await press(p.last());
	assert.match(box.textContent, /variant you picked is not the radio's/);
	assert.notEqual(p.last().textContent, "Continue");
	// a radio of the other variant: no install, with the reason (no image yet, or another board's image)
	const otherV = RADIO.variants.find((v) => v.code === other), otherBoard = otherV.blocked ? null : otherV.runs?.board;
	p = page(models[other][0]);
	box = ui.connectView();
	await press(p.last());
	assert.match(box.textContent, otherBoard ? new RegExp(`takes the ${otherBoard} image`) : /can't be installed on this radio/);
	assert.notEqual(p.last().textContent, "Continue");
});

test("install: boot block from the stock file, then every frame written and the end confirmed", async () => {
	const blk = concat(fromHex(FB0.modem_boot.prefix), rand(FB0.modem_boot.len - 8, 5));
	FB = { ...FB0, modem_boot: { ...FB0.modem_boot, sha256: await sha256(blk) } };
	const app = rand(3000, 9);
	const p = page(models[runs][0], "install", { firmware: { image: app, source: "test.bin", manifest: { version: "v0" } } });
	ui.connectView();
	await press(p.last());
	const boot = ui.bootView();
	const input = (function find(e) { if (e.tagName === "INPUT") return e; for (const c of e.children || []) { const f = c instanceof El && find(c); if (f) return f; } return null; })(boot);
	input.files = [{ name: "stock.CDD", arrayBuffer: async () => concat(rand(64, 3), blk).buffer }];
	for (const f of input.listeners.change) await f();
	assert.match(boot.textContent, /Boot block found and checked \(from stock.CDD\)/);
	const box = ui.writeView();
	await press(p.last());
	await sleep(200);
	assert.match(box.textContent, /Written/);
	assert.ok(equal(p.S.fakeAnytone.image(), at.installImage(BOARD, app, blk)));
	assert.equal(p.done.flashes[0].task, "install");
	FB = FB0;
});

test("restore: the vendor set of the radio's model is written; another model's set is refused at connect", async () => {
	const img = rand(2000, 11);
	const set = { addr: BOARD.app_base, image: img, models: models[runs], names: {} };
	let p = page(models[runs][0], "restore");
	p.S.at = { log: { add() {}, el: new El("pre") }, set };
	ui.connectView();
	await press(p.last());
	const box = ui.restoreView();
	await press(p.last());
	await sleep(200);
	assert.match(box.textContent, /original firmware/);
	assert.ok(equal(p.S.fakeAnytone.image().subarray(0, img.length), img));
	if (!other) return;
	p = page(models[other][0], "restore");
	p.S.at = { log: { add() {}, el: new El("pre") }, set };
	const c = ui.connectView();
	await press(p.last());
	assert.match(c.textContent, /not for this radio/);
});

const findButton = (e, text) => {
	if (e?.tagName === "BUTTON" && e.textContent === text) return e;
	for (const c of e?.children || []) { const f = c instanceof El && findButton(c, text); if (f) return f; }
	return null;
};

test("connect: Auto-detect asks the radio, picks its variant and keeps the update mode session", async () => {
	const p = page(models[runs][0]);
	const box = ui.connectView();
	const go = findButton(box, "Auto-detect");
	assert.ok(go, "the Auto-detect button beside the variant chooser");
	await press(go);
	await sleep(100);
	assert.equal(p.S.atChosen, runs);
	assert.equal(p.S.variant.via, "update-mode");
	assert.match(box.textContent, /can run RadioFoundry/);
	assert.equal(p.last().textContent, "Continue", "no second connect needed");
});

test("build: a variant with no firmware is refused, and Auto-detect closes the port it opened", async () => {
	const p = page(models[other][0]);
	const gate = ui.buildGate(() => {});
	assert.equal(gate.refused(), false, "not knowing the variant is allowed");
	p.S.atChosen = other;
	assert.equal(gate.refused(), true);
	assert.match(gate.view().textContent, /can't be installed|does not support|needs the/);
	p.S.atChosen = runs;
	assert.equal(gate.refused(), false);
	p.S.atChosen = "NOT-THIS-RADIO";
	assert.equal(gate.refused(), false, "a pick from another radio doesn't count");
	p.S.atChosen = null;
	let drawn = 0, closed = 0;
	const stream = p.S.fakeAnytone.stream, close = stream.close;
	stream.close = async () => { closed++; return close?.call(stream); };
	const found = ui.buildGate(() => { drawn++; });
	await press(findButton(found.view(), "Auto-detect"));
	await sleep(100);
	assert.equal(p.S.atChosen, other);
	assert.equal(found.refused(), true);
	assert.equal(drawn, 1);
	assert.ok(closed, "no update mode session kept for the steps before connect");
	const red = found.view().textContent.match(/can't be installed/g) || [];
	assert.ok(red.length <= 1, "one refusal, not two");
});

test("write: the radio is asked again just before the first frame; another radio stops the install", async () => {
	if (!other) return;
	const app = rand(3000, 9);
	const p = page(models[runs][0], "install", { firmware: { image: app, source: "test.bin", manifest: { version: "v0" } } });
	ui.connectView();
	await press(p.last());
	p.S.at.block = concat(fromHex(FB0.modem_boot.prefix), rand(FB0.modem_boot.len - 8, 5));
	p.S.fakeAnytone.model = models[other][0];	// a different radio answers now
	const box = ui.writeView();
	await press(p.last());
	await sleep(100);
	assert.match(box.textContent, /check just before writing stopped here/);
	assert.equal(p.S.fakeAnytone.frames, 0, "nothing written");
	assert.equal(p.done.flashes.length, 0, "no flash started");
	assert.equal(p.last().textContent, "Connect again");
});
