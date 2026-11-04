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
	showModal() {}
	close() {}
	remove() { document.body.children = document.body.children.filter((c) => c !== this); }
	appendChild(c) { this.append(c); return c; }
	replaceChildren(...cs) { this.children = []; this.append(...cs); }
	setAttribute(k, v) { this.attrs[k] = v; }
	getAttribute(k) { return this.attrs[k] ?? null; }
	setAttributeNS(ns, k, v) { this.attrs[k] = v; }
	addEventListener(ev, f) { (this.listeners[ev] ||= []).push(f); }
	click() { if (!this.disabled) for (const f of this.listeners.click || []) f({ preventDefault() {} }); }
	get textContent() { return this.children.map((c) => (c instanceof El ? c.textContent : c.text)).join(""); }
	set textContent(v) { this.children = [{ text: String(v) }]; }
	get firstChild() { return this.children[0]; }
	// ".class" only
	querySelector(sel) {
		for (const c of this.children) {
			if (!(c instanceof El)) continue;
			if (String(c.className || c.attrs.class || "").split(" ").includes(sel.slice(1))) return c;
			const deeper = c.querySelector(sel);
			if (deeper) return deeper;
		}
		return null;
	}
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
	// Install asks once more (nocodec.js): Escape stays, Go Back returns to the check step, Install Anyway writes
	const dialogs = () => document.body.children.filter((c) => c.tagName === "DIALOG");
	const pick = (label) => {
		const find = (el) => (el.tagName === "BUTTON" && el.textContent === label ? el : el.children?.map((c) => (c instanceof El ? find(c) : null)).find(Boolean));
		find(dialogs()[0]).listeners.click[0]({ preventDefault() {} });
	};
	forwards.at(-1).click();
	await sleep(0);
	assert.equal(dialogs().length, 1);
	assert.match(dialogs()[0].textContent, /Install Without DMR Voice\?.*not be able to send or receive DMR voice calls/);
	dialogs()[0].listeners.cancel[0]({ preventDefault() {} });
	await sleep(0);
	assert.equal(dialogs().length, 0);
	assert.equal(writes.length, 0, "nothing written when the dialog is dismissed");
	let rendered = 0;
	state.step = 4;
	c7ui.init({ S: state, fb: () => fb, helpLink: () => null, reset() {}, flashStarted: () => 0, flashFinished() {}, flashMark() {},
		errorText: (e) => e.message, next() {}, render() { rendered++; } });
	forwards.at(-1).click();
	await sleep(0);
	state.c7none = true;
	pick("Go Back and Add the Codec");
	await sleep(0);
	assert.equal(state.step, 3, "back to the check step");
	assert.equal(state.c7none, false, "the file picker shows again there");
	assert.equal(rendered, 1);
	assert.equal(writes.length, 0);
	forwards.at(-1).click();
	await sleep(0);
	pick("Install Anyway");
	await sleep(1500);
	assert.match(diag.trail(50).join("\n"), /no codec confirm \{"answer":"go"\}/);
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
	await sleep(0);
	pick("Install Anyway");
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

test("backup: a connection lost after the step was drawn goes back to the connect step, which says why", async () => {
	globalThis.window ??= { addEventListener() {}, removeEventListener() {} };
	const forwards = [];
	const started = [];
	let rendered = 0;
	const state = { radio, task: "backup", forward: (b) => forwards.push(b), data: { fb, site: { git: "test" } } };
	c7ui.init({ S: state, fb: () => fb, helpLink: () => null, reset() {}, noteReadout: async () => {}, render: () => rendered++,
		flashStarted: (u) => started.push(u), flashFinished() {}, errorText: (e) => e.message });
	state.c7 = { log: { add() {}, el: new El("div") }, stream: null, size: {}, stopSizing: true,
		stub: { size: {}, sums: async () => { throw new Error("no radio"); } } };
	c7ui.backupView();
	state.c7.stub = null;	// the device lost while the page waited (the blank count fails, the session drops the stub)
	forwards.at(-1).click();
	for (let i = 0; i < 50 && !rendered; i++) await sleep(10);
	assert.equal(rendered, 1, "the step is drawn again");
	assert.equal(started.length, 0, "no backup is started on a loader that is gone");
	const box = c7ui.backupView();
	assert.match(box.textContent, /stopped answering on the cable/);
	assert.match(box.textContent, /Choose Cable/);
});

// Field (Sentry OPENDIGITALRADIO-M): the cable's reading stopped (the device lost, or the reading gave up) while the
// session kept its loader; the backup step then asked it for every request seven times and failed "no reply header"
test("backup: a cable whose reading stopped is chosen again, not asked seven times per request", async () => {
	const forwards = [];
	const state = { radio, task: "backup", forward: (b) => forwards.push(b), data: { fb } };
	c7ui.init({ S: state, fb: () => fb, helpLink: () => null, reset() {}, render() {}, errorText: (e) => e.message });
	let closed = 0, asked = 0;
	state.c7 = { log: { add() {}, el: new El("div") }, size: {}, stopSizing: true,
		stream: { lost: true, close: async () => { closed++; } }, stub: { size: {}, attach: async () => { asked++; return null; } } };
	const box = c7ui.backupView();
	assert.match(box.textContent, /stopped answering on the cable/);
	assert.equal(state.c7.stub, null, "the session dropped the loader");
	assert.equal(state.c7.stream, null, "and the cable");
	await sleep(0);
	assert.equal(closed, 1, "the port is closed so it can be opened again");
	assert.equal(asked, 0, "nothing was sent to it");
});

test("backup: a failed read whose loader no longer answers sends Try Again to the connect step", async () => {
	globalThis.window ??= { addEventListener() {}, removeEventListener() {} };
	const forwards = [];
	const finished = [];
	let rendered = 0;
	const state = { radio, task: "backup", forward: (b) => forwards.push(b), data: { fb, site: { git: "test" } } };
	c7ui.init({ S: state, fb: () => fb, helpLink: () => null, reset() {}, noteReadout: async () => {}, render: () => rendered++,
		flashStarted: () => 0, flashFinished: (s, e) => finished.push(e), errorText: (e) => e.message });
	let dumps = 0;
	state.c7 = { log: { add() {}, el: new El("div") }, stream: { lost: false, close: async () => {} }, size: {}, stopSizing: true,
		stub: { size: {}, sums: async () => { throw new Error("no radio"); },
			dump: async () => { dumps++; throw new Error("op 0x5: no clean reply after 7 tries (no or malformed reply header)"); },
			attach: async () => null } };	// the radio was switched off: HELLO goes unanswered
	const box = c7ui.backupView();
	forwards.at(-1).click();
	for (let i = 0; i < 50 && !finished.length; i++) await sleep(10);
	await sleep(10);
	assert.match(box.textContent, /no clean reply/);
	assert.equal(state.c7.stub, null, "the dead loader is dropped");
	forwards.at(-1).click();
	for (let i = 0; i < 50 && !rendered; i++) await sleep(10);
	assert.equal(rendered, 1, "Try Again draws the step again: the connect step");
	assert.equal(dumps, 1, "the dead loader was not asked again");
});

// Field (Sentry OPENDIGITALRADIO-1F): Back from the check step while it read; the connect step's stub check ran on the
// same cable at once, and both failed on each other's frames
test("install: Back from a check that reads stops the read before the connect step asks the stub", async () => {
	globalThis.window ??= { addEventListener() {}, removeEventListener() {} };
	const forwards = [];
	const state = { radio, task: "install", forward: (b) => forwards.push(b), data: { fb, site: { git: "test" } } };
	c7ui.init({ S: state, fb: () => fb, helpLink: () => null, reset() {}, render() {}, noteReadout: async () => {}, errorText: (e) => e.message });
	const order = [];
	let signal = null;
	state.c7 = { log: { add() {}, el: new El("div") }, stream: { lost: false, close: async () => {} }, how: "rom",
		stub: { size: {}, sums: async () => { throw new Error("no count"); },
			dump: (mem, a, b, opts) => new Promise((ok, fail) => {
				signal = opts.signal;
				order.push("read");
				opts.signal.addEventListener("abort", () => setTimeout(() => { order.push("read stopped"); fail(new Error("stopped")); }, 30));
			}),
			drain: async () => {},
			attach: async () => { order.push("attach"); return null; } } };
	c7ui.checkView();
	forwards.at(-1).click();
	for (let i = 0; i < 50 && !signal; i++) await sleep(10);
	assert.ok(signal, "the read runs");
	c7ui.connectView();
	for (let i = 0; i < 50 && !order.includes("attach"); i++) await sleep(10);
	assert.ok(signal.aborted, "leaving the check step stops its read");
	assert.deepEqual(order, ["read", "read stopped", "attach"], "the stub is asked only once the read has ended");
});

test("backup: Start over while the blank sectors are counted: the late count draws nothing and throws nothing", async (t) => {
	const thrown = [];
	const catcher = (e) => thrown.push(e);
	process.on("unhandledRejection", catcher);
	t.after(() => process.off("unhandledRejection", catcher));
	let lose;
	const state = { radio, task: "backup", forward() {}, data: { fb } };
	c7ui.init({ S: state, fb: () => fb, helpLink: () => null, reset() {}, noteReadout() {} });
	state.c7 = { log: { add() {}, el: new El("div") }, stream: null, size: {},
		stub: { size: {}, sums: () => new Promise((_, no) => { lose = () => no(new Error("The device has been lost.")); }) } };
	c7ui.backupView();
	assert.ok(lose, "the count is under way");
	// what reset() leaves (OPENDIGITALRADIO-39): no radio, no cable session
	state.radio = null;
	delete state.c7;
	lose();
	await sleep(50);
	assert.deepEqual(thrown.map(String), []);
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

test("spinner: hidden from screen readers; a redraw starts at the angle the last one reached", async (t) => {
	const { spinner } = await import("../static/js/flasher/dom.js");
	const now = Date.now;
	t.after(() => { Date.now = now; });
	Date.now = () => 7200 * 1000 + 300;
	const s = spinner();
	assert.equal(s.className, "spinner");
	assert.equal(s.attrs["aria-hidden"], "true");
	assert.equal(s.attrs.style, "animation-delay: -300ms");
	globalThis.matchMedia = (q) => ({ matches: q.includes("reduce") });	// reduced motion: the 2.4 s turn in site.css
	t.after(() => { delete globalThis.matchMedia; });
	Date.now = () => 7200 * 1000 + 2000;
	assert.equal(spinner().attrs.style, "animation-delay: -2000ms");
});

test("beta notice: once per session and again after 12 hours; storage that throws shows it again rather than failing", async () => {
	const { showBetaNotice, BETA_NOTICE_EVERY_MS } = await import("../static/js/flasher/beta.js");
	const make = () => {
		const dlg = new El("dialog");
		dlg.open = false;
		dlg.showModal = () => { dlg.open = true; };
		dlg.close = () => { dlg.open = false; for (const f of dlg.listeners.close || []) f(); };
		return { dlg, doc: { getElementById: (id) => (id === "beta-notice" ? dlg : null) } };
	};
	const kept = new Map();
	const store = () => ({ getItem: (k) => kept.get(k) ?? null, setItem: (k, v) => kept.set(k, v) });
	let t = 1_000_000;
	const now = () => t;
	const a = make();
	assert.equal(showBetaNotice(a.doc, store, now), true);
	assert.equal(a.dlg.open, true);
	a.dlg.close();
	t += BETA_NOTICE_EVERY_MS - 1;
	const b = make();
	assert.equal(showBetaNotice(b.doc, store, now), false, "dismissed: not shown again within 12 hours");
	assert.equal(b.dlg.open, false);
	t += 1;
	const d = make();
	assert.equal(showBetaNotice(d.doc, store, now), true, "12 hours after it was closed: shown again");
	d.dlg.close();
	assert.equal(kept.get("rfoundry.betaNoticeSeen"), String(t), "the new close restarts the 12 hours");
	kept.set("rfoundry.betaNoticeSeen", "1");
	assert.equal(showBetaNotice(make().doc, store, now), true, "an old flag value counts as long ago");
	const c = make();
	assert.equal(showBetaNotice(c.doc, () => { throw new Error("SecurityError"); }), true);
	c.dlg.close();	// closing without storage doesn't throw
	assert.equal(showBetaNotice({ getElementById: () => null }, store), false, "a page without the notice");
});

// ---- leaving loader mode (#352, #353), against the simulated radio

const { FakeC7000, c7000Simulation } = await import("../static/js/flasher/fakec7000.js");
const { Stub, stubImages } = await import("../static/js/flasher/stub.js");
const stubImage = stubImages(json("c7000-stub.json"));

// A radio whose loader runs already, as after a backup interrupted by a cable pull or a closed tab
async function loaderRunning(t, state) {
	globalThis.window ??= { addEventListener() {}, removeEventListener() {} };
	const sim = await c7000Simulation(fb.c7000, radio.id, 7000, radio);
	const r = new FakeC7000({ progmem: sim.progmem.slice(), dataSize: 0x200000, speed: 200, romWindow: 50, bootTime: 50, board: sim.board, codecId: sim.codecId });
	t.after(() => r.stop());
	setTimeout(() => r.powerOn(), 20);
	const s = new Stub(r.stream, { log() {} });
	await s.waitForRom(5000);
	await s.loadThroughRom(stubImage.image, stubImage.load);
	assert.equal(r.state, "stub");
	state.fakeC7 = r;
	state.c7 = { log: { add() {}, el: new El("div") }, stream: r.stream };
	return r;
}
const all = (el) => [el, ...(el.children || []).filter((c) => c instanceof El).flatMap(all)];

test("restart task: a radio left in loader mode is reset and the page ends on a finished screen", async (t) => {
	const forwards = [];
	const state = { radio, task: "restart", forward: (b) => forwards.push(b), data: { fb }, board: fb.boards[radio.flash.board] };
	c7ui.init({ S: state, fb: () => fb, helpLink: () => null, reset() {}, render() {}, errorText: (e) => e.message });
	const r = await loaderRunning(t, state);
	const box = c7ui.restartView();
	box.isConnected = true;
	for (let i = 0; i < 300 && !/restarted into normal mode/.test(box.textContent); i++) await sleep(20);
	assert.match(box.textContent, /restarted into normal mode/);
	assert.ok(r.events.includes("reset"), "the chip was reset");
	assert.equal(state.c7.stream, null, "the session let go of the cable");
	assert.equal(forwards.at(-1).textContent, "Finish");
});

test("connect step: a loader already running offers Restart Radio next to Continue and goes nowhere by itself", async (t) => {
	const forwards = [];
	const state = { radio, task: "backup", forward: (b) => forwards.push(b), data: { fb }, board: fb.boards[radio.flash.board] };
	let went = 0;
	c7ui.init({ S: state, fb: () => fb, helpLink: () => null, reset() {}, render() {}, errorText: (e) => e.message, next: () => went++ });
	const r = await loaderRunning(t, state);
	const box = c7ui.connectView();
	box.isConnected = true;
	for (let i = 0; i < 300 && !(Array.isArray(forwards.at(-1)) && forwards.at(-1).length === 2); i++) await sleep(20);
	const [restart, cont] = forwards.at(-1);
	assert.equal(restart.textContent, "Restart Radio");
	assert.equal(cont.textContent, "Continue");
	await sleep(1700);
	assert.equal(went, 0, "the step does not go on without the user");
	assert.ok(!r.events.includes("reset"));
	restart.click();
	assert.equal(forwards.at(-1), null, "the actions go at once: no second reset, no Continue into a radio restarting");
	for (let i = 0; i < 300 && !/restarted into normal mode/.test(box.textContent); i++) await sleep(20);
	assert.match(box.textContent, /restarted into normal mode/);
	assert.equal(r.events.filter((e) => e === "reset").length, 1);
	assert.equal(went, 0);
});

test("backup: after Restart Radio the task ends on a finished screen, not at the connect step again", async (t) => {
	const made = URL.createObjectURL;
	URL.createObjectURL = () => "blob:test";
	El.prototype.remove ??= function () {};
	t.after(() => { URL.createObjectURL = made; });
	globalThis.location ??= { origin: "http://test" };
	const forwards = [];
	let rendered = 0;
	const state = { radio, task: "backup", step: 1, forward: (b) => forwards.push(b), data: { fb, site: { git: "test" } }, board: fb.boards[radio.flash.board] };
	c7ui.init({ S: state, fb: () => fb, helpLink: () => null, reset() {}, render: () => rendered++, noteReadout: async () => {}, errorText: (e) => e.message,
		flashStarted: () => 0, flashFinished() {} });
	const r = await loaderRunning(t, state);
	const stub = new Stub(r.stream, { log() {} });
	assert.ok(await stub.attach());
	Object.assign(state.c7, { stub, how: "attach" });
	const box = c7ui.backupView();
	// the program memory only keeps the run short
	for (const cb of all(box).filter((e) => e.tagName === "INPUT" && e.attrs.type === "checkbox").slice(1)) {
		cb.checked = false;
		for (const f of cb.listeners.change || []) f({ target: cb });
	}
	forwards.at(-1).click();
	for (let i = 0; i < 500 && !/Backup saved/.test(box.textContent); i++) await sleep(20);
	assert.match(box.textContent, /Backup saved/);
	const leave = forwards.at(-1);
	assert.equal(leave.textContent, "Restart Radio");
	leave.click();
	assert.ok(leave.disabled, "one reset: a second click does nothing");
	for (let i = 0; i < 100 && !rendered; i++) await sleep(20);
	assert.equal(rendered, 1);
	assert.ok(r.events.includes("reset"));
	const after = c7ui.backupView();
	assert.match(after.textContent, /Backup saved as/);
	assert.match(after.textContent, /back in normal mode/);
	assert.doesNotMatch(after.textContent, /Choose Cable/);
	assert.deepEqual(forwards.at(-1).map((b) => b.textContent), ["Install", "Update", "Finish"]);
	forwards.at(-1)[1].click();
	assert.equal(state.task, "update");
	assert.equal(state.backupDone, true, "the next task does not ask for a backup again");
	assert.equal(state.step, 0);
});
