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
	const otherBoard = RADIO.variants.find((v) => v.code === other).runs?.board;
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
