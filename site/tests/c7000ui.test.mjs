// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The cable radio pages (c7000ui.js) against a small stand-in for the DOM: a check the user left (Start over, Back)
// draws nothing into the step that replaced it. Run with site/tests/run.sh.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

// ---- just enough DOM for dom.js h() and the radio picture
class El {
	constructor(tag) {
		this.tagName = tag.toUpperCase();
		this.children = [];
		this.attrs = {};
		this.listeners = {};
		this.className = "";
		this.dataset = {};
		this.style = {};
		this.isConnected = false;
	}
	append(...cs) { for (const c of cs) this.children.push(c instanceof El ? c : { text: String(c) }); }
	appendChild(c) { this.append(c); return c; }
	replaceChildren(...cs) { this.children = []; this.append(...cs); }
	setAttribute(k, v) { this.attrs[k] = v; }
	setAttributeNS(ns, k, v) { this.attrs[k] = v; }
	addEventListener(ev, f) { (this.listeners[ev] ||= []).push(f); }
	click() { for (const f of this.listeners.click || []) f({ preventDefault() {} }); }
	get textContent() { return this.children.map((c) => (c instanceof El ? c.textContent : c.text)).join(""); }
	set textContent(v) { this.children = [{ text: String(v) }]; }
	get firstChild() { return this.children[0]; }
}
const strings = readFileSync(fileURLToPath(new URL("../i18n/en.json", import.meta.url)), "utf8");
globalThis.Node = El;
globalThis.document = { createElement: (t) => new El(t), createElementNS: (ns, t) => new El(t), body: new El("body"),
	getElementById: (id) => (id === "i18n" ? { textContent: strings } : null) };

const c7ui = await import("../static/js/flasher/c7000ui.js");

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
const json = (n) => JSON.parse(readFileSync(join(DIR, n), "utf8"));
const fb = json("flash-boards.json");
const radio = json("radios.json").radios.find((r) => fb.c7000.boards[r.id]);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

test("install done: a check still running when the user starts over draws nothing and calls no nav bar", async () => {
	assert.ok(radio, "an HR_C7000 radio in radios.json");
	const forwards = [];
	const state = { radio, task: "install", fakeC7: {}, forward: (b) => forwards.push(b), data: { fb } };
	c7ui.init({ S: state, fb: () => fb, helpLink: () => null, reset() {} });
	// no cable left: the check ends at once with no answer, as after a radio that stays silent
	state.c7 = { log: { add() {}, el: new El("div") }, stream: null, resetAt: Date.now() - 5000 };
	const box = c7ui.doneView();
	box.isConnected = true;
	// Start over: a new session and the next step's nav bar
	const after = [];
	state.c7 = { log: { add() {}, el: new El("div") }, stream: null };
	state.forward = (b) => after.push(b);
	await sleep(100);
	assert.equal(after.length, 0, "nothing put in the nav bar of the step that replaced it");
	assert.ok(forwards.length >= 1);
});

test("restore: a backup without the program memory (the calibration or the data memory alone) says so, offers no restore", () => {
	const forwards = [];
	const parts = new Map([["c7000-calibration", { what: "c7000-calibration", file: "calibration.bin", complete: true, addr: 0, data: new Uint8Array(16) }]]);
	const state = { radio, task: "restore", forward: (b) => forwards.push(b), data: { fb },
		bundle: { meta: { radio: { name: radio.name }, created: new Date().toISOString(), firmware_now: { kind: "stock" } }, parts } };
	c7ui.init({ S: state, fb: () => fb, helpLink: () => null, reset() {} });
	state.c7 = { log: { add() {}, el: new El("div") }, stream: null };
	const box = c7ui.restoreView();
	assert.match(box.textContent, /no program memory/);
	assert.deepEqual(forwards, [null], "no Restore button");
});

test("backup: unticking the program memory says such a backup can't restore the radio's firmware", () => {
	const state = { radio, task: "backup", forward() {}, data: { fb } };
	c7ui.init({ S: state, fb: () => fb, helpLink: () => null, reset() {}, noteReadout() {} });
	state.c7 = { log: { add() {}, el: new El("div") }, stream: null, size: {}, stopSizing: true,
		stub: { size: {}, sums: async () => { throw new Error("no radio"); } } };
	const box = c7ui.backupView();
	const all = (el) => [el, ...(el.children || []).filter((c) => c instanceof El).flatMap(all)];
	const note = all(box).find((e) => /initial radio firmware/.test(e.textContent) && e.className?.includes?.("notice"));
	assert.ok(note, "the note is on the page");
	assert.equal(note.hidden, true, "hidden while the program memory is ticked");
	const progmem = all(box).find((e) => e.tagName === "INPUT" && e.attrs.type === "checkbox");
	progmem.checked = false;
	for (const f of progmem.listeners.change) f({ target: progmem });
	assert.equal(note.hidden, false, "shown once it is unticked");
});
