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
	let reset = 0;
	const parts = new Map([["c7000-calibration", { what: "c7000-calibration", file: "calibration.bin", complete: true, addr: 0, data: new Uint8Array(16) }]]);
	const state = { radio, task: "restore", forward: (b) => forwards.push(b), data: { fb },
		bundle: { meta: { radio: { name: radio.name }, created: new Date().toISOString(), firmware_now: { kind: "stock" } }, parts } };
	c7ui.init({ S: state, fb: () => fb, helpLink: () => null, reset() { reset++; } });
	state.c7 = { log: { add() {}, el: new El("div") }, stream: null };
	const box = c7ui.restoreView();
	assert.match(box.textContent, /no program memory/);
	// what to do instead, in the notice and as the step's one action: no Restore button
	assert.match(box.textContent, /manufacturer firmware file instead of this backup/);
	assert.equal(forwards.length, 1);
	assert.match(forwards[0].textContent, /Select a Firmware File/);
	forwards[0].click();
	assert.equal(reset, 1);
});

test("install: a write without a codec warns above the button; the write logs its range and that the bootloader is unchanged", async () => {
	globalThis.window ??= { addEventListener() {}, removeEventListener() {} };
	if (!("classList" in El.prototype)) Object.defineProperty(El.prototype, "classList", { get() { return { add() {}, remove() {} }; } });
	const C = fb.c7000;
	const forwards = [];
	const state = { radio, task: "install", forward: (b) => forwards.push(b), data: { fb }, fakeC7: {},
		firmware: { image: Uint8Array.from({ length: 8192 }, (_, i) => (i * 7 + 1) & 0xFF), source: "test", manifest: { version: "t" } } };
	const diag = await import("../static/js/diag.js");
	diag.reset();
	c7ui.init({ S: state, fb: () => fb, helpLink: () => null, reset() {}, flashStarted: () => 0, flashFinished() {}, flashMark() {},
		errorText: (e) => e.message, next() {} });
	const boot = new Uint8Array(C.app_start).fill(0xA5);
	let bootNow = boot;
	const writes = [];
	state.c7 = { log: { add() {}, el: new El("div") }, stream: null, decision: { mode: "none" }, analysis: { notRead: true },
		stub: { attach: async () => true, dump: async () => bootNow.slice(), ledSequence: async () => {}, resetChip: async () => {},
			writeProgram: async (data, start, { onSector }) => {
				for (const a of [C.app_start, C.app_start + 4096]) { onSector(a, "todo"); onSector(a, "written"); writes.push(a); }
				return { written: 2, same: 0 };
			} } };
	const box = c7ui.writeView();
	// the warning says why: nothing backed up, no stock file; and that the codec can be added by installing again
	assert.match(box.textContent, /will not include the DMR voice codec/i);
	assert.match(box.textContent, /was not backed up and no supported stock firmware file was selected/);
	assert.doesNotMatch(box.textContent, /no way back/i);
	forwards.at(-1).click();
	await sleep(1500);
	const tr = diag.trail(50).join("\n");
	assert.match(tr, /write range/);
	assert.match(tr, new RegExp(`"first":"0x${C.app_start.toString(16).toUpperCase().padStart(8, "0")}"`));
	assert.match(tr, /"bootloader":"unchanged"/);
	// a bootloader area that differs after the write is an error, not an install that finished
	diag.reset();
	bootNow = boot;
	state.c7.stub.writeProgram = async (data, start, { onSector }) => { bootNow = new Uint8Array(C.app_start).fill(0); onSector(C.app_start, "written"); return { written: 1, same: 0 }; };
	const box2 = c7ui.writeView();
	forwards.at(-1).click();
	await sleep(300);
	assert.match(box2.textContent, /bootloader area changed/);
	assert.match(diag.trail(50).join("\n"), /"bootloader":"changed"/);
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

test("backup: a backup made through an untried stock build offers its program memory read with a report", async (t) => {
	// what the run touches of the browser: the leave warning and the backup's download
	globalThis.window ??= { addEventListener() {}, removeEventListener() {} };
	globalThis.location ??= { origin: "http://test" };
	const made = URL.createObjectURL;
	URL.createObjectURL = () => "blob:test";
	El.prototype.remove ??= function () {};
	t.after(() => { URL.createObjectURL = made; });
	const forwards = [];
	const state = { radio, task: "backup", forward: (b) => forwards.push(b), data: { fb, site: { git: "test" } } };
	c7ui.init({ S: state, fb: () => fb, helpLink: () => null, reset() {}, noteReadout: async () => {},
		flashStarted: () => 0, flashFinished() {}, errorText: (e) => e.message });
	const progmem = new Uint8Array(fb.c7000.progmem_size).fill(0xFF);
	state.c7 = { log: { add() {}, el: new El("div") }, stream: null, size: {}, stopSizing: true, stockUntried: { version: "909E.DBB.EARSAB.018" },
		stub: { size: {}, sums: async () => { throw new Error("no radio"); }, dump: async () => progmem.slice(), ledSequence: async () => {} } };
	const box = c7ui.backupView();
	const all = (el) => [el, ...(el.children || []).filter((c) => c instanceof El).flatMap(all)];
	// the program memory only: the other parts need a radio
	for (const cb of all(box).filter((e) => e.tagName === "INPUT" && e.attrs.type === "checkbox").slice(1)) {
		cb.checked = false;
		for (const f of cb.listeners.change || []) f({ target: cb });
	}
	forwards.at(-1).click();
	for (let i = 0; i < 100 && !state.stockRead; i++) await sleep(20);
	assert.equal(state.stockRead?.name, `program-memory-${radio.id}-909E.DBB.EARSAB.018.bin`);
	assert.match(box.textContent, /Send the program memory read/);
});

test("backup: a connection lost after the step was drawn says the radio no longer responds, not a script error", async () => {
	globalThis.window ??= { addEventListener() {}, removeEventListener() {} };
	const forwards = [];
	const finished = [];
	const state = { radio, task: "backup", forward: (b) => forwards.push(b), data: { fb, site: { git: "test" } } };
	c7ui.init({ S: state, fb: () => fb, helpLink: () => null, reset() {}, noteReadout: async () => {},
		flashStarted: () => 0, flashFinished: (s, e) => finished.push(e), errorText: (e) => e.message });
	state.c7 = { log: { add() {}, el: new El("div") }, stream: null, size: {}, stopSizing: true,
		stub: { size: {}, sums: async () => { throw new Error("no radio"); } } };
	const box = c7ui.backupView();
	state.c7.stub = null;	// the device lost while the page waited (the blank count fails, the session drops the stub)
	forwards.at(-1).click();
	for (let i = 0; i < 50 && !finished.length; i++) await sleep(10);
	assert.match(box.textContent, /the radio no longer responds/);
	assert.doesNotMatch(box.textContent, /Cannot read properties/);
	assert.match(String(finished[0]?.message), /no longer responds/);
});

test("restore: the write logs its range and that the bootloader is unchanged, as an install does", async () => {
	globalThis.window ??= { addEventListener() {}, removeEventListener() {} };
	const C = fb.c7000;
	const forwards = [];
	const pm = new Uint8Array(C.progmem_size).fill(0xFF);
	pm.fill(0x11, C.app_start, C.app_start + 8192);
	const state = { radio, task: "restore", forward: (b) => forwards.push(b), data: { fb },
		bundle: { meta: { radio: { name: radio.name }, firmware_now: { kind: "stock" } },
			parts: new Map([["c7000-progmem", { what: "c7000-progmem", file: "program-memory.bin", complete: true, addr: 0, data: pm }]]) } };
	const diag = await import("../static/js/diag.js");
	diag.reset();
	c7ui.init({ S: state, fb: () => fb, helpLink: () => null, reset() {}, flashStarted: () => 0, flashFinished() {}, flashMark() {},
		errorText: (e) => e.message });
	const boot = new Uint8Array(C.app_start).fill(0xA5);
	state.c7 = { log: { add() {}, el: new El("div") }, stream: null,
		stub: { dump: async () => boot.slice(), ledSequence: async () => {}, resetChip: async () => {},
			writeProgram: async (data, start, { onSector }) => { onSector(C.app_start, "todo"); onSector(C.app_start, "written"); return { written: 1, same: 0 }; } } };
	state.fakeC7 = {};
	const box = c7ui.restoreView();
	forwards.at(-1).click();
	for (let i = 0; i < 300 && !/Restore complete/.test(box.textContent); i++) await sleep(10);
	assert.match(box.textContent, /Restore complete/);
	const tr = diag.trail(50).join("\n");
	assert.match(tr, /restore bundle \{"progmem":true/);
	assert.match(tr, /write range \{"sectors":1,.*"bootloader":"unchanged"/);
});

test("progress bar: a stall says how long it has waited; an error shown ends the shimmer and the wait line", async (t) => {
	const { progressBar, notice } = await import("../static/js/flasher/dom.js");
	const now = Date.now;
	t.after(() => { Date.now = now; });
	const p = progressBar();
	p.el.isConnected = true;
	p.set(3, 10, "Program Memory");
	const [track, , wait] = p.el.children;
	assert.equal(track.className, "progress active");
	Date.now = () => now() + 6000;	// nothing new for six seconds
	await sleep(1100);
	assert.equal(wait.hidden, false);
	assert.match(wait.textContent, /Still waiting for the radio \(\d+ s\)/);
	notice("the radio no longer responds", "bad");	// the step failed with the bar at 30%
	assert.equal(track.className, "progress");
	assert.equal(wait.hidden, true);
	await sleep(1100);
	assert.equal(wait.hidden, true, "no wait line after the failure");
});
