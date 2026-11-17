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

test("led_hints false: the steps describe no LED, on the radio's own chip or its second", () => {
	const quiet = json("radios.json").radios.filter((r) => fb.c7000.boards[r.flash.board] && r.flash.led_hints === false);
	for (const r of quiet) {
		const s = r.flash.second;
		assert.ok(!s || s.led_hints === false, `${r.id}: its second SoC follows the radio`);
		// as app.js secondRadio() makes it
		const chips = [r, ...(s ? [{ ...r, id: s.board, second_of: r.id, flash: { ...s, via: r.flash.board } }] : [])];
		for (const radio of chips) {
			const state = { radio, task: "install", forward() {}, data: { fb } };
			c7ui.init({ S: state, fb: () => fb, helpLink: () => null, reset() {} });
			state.c7 = { log: { add() {}, el: new El("div") }, stream: null };
			const box = c7ui.connectView();
			const lamps = (e) => (e instanceof El ? (e.className === "rt-lamp" ? 1 : 0) + e.children.reduce((n, c) => n + lamps(c), 0) : 0);
			assert.equal(lamps(box), 0, `${radio.id}: no LED drawn on the radio's picture`);
			assert.doesNotMatch(box.textContent, /\bLEDs?\b|blink/i, `${radio.id}: ${box.textContent}`);
		}
	}
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
	const slot0 = C.boards[radio.flash.board].slot[0];
	pm.fill(0x22, slot0, slot0 + 3 * C.sector);
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
			writeProgram: async (data, start, { onSector }) => {
				onSector(C.app_start, "todo");
				onSector(C.app_start, "written");
				for (let a = slot0; a < slot0 + 3 * C.sector; a += C.sector) onSector(a, "same");
				return { written: 1, same: 3 };
			} } };
	state.fakeC7 = {};
	const box = c7ui.restoreView();
	forwards.at(-1).click();
	for (let i = 0; i < 300 && !/Restore complete/.test(box.textContent); i++) await sleep(10);
	assert.match(box.textContent, /Restore complete/);
	// what was not written is said in words: a codec the radio already had as the backup has it looked skipped
	assert.match(box.textContent, /Parts written: 1\. Parts that already matched the backup and were not written again: 3\./);
	assert.match(box.textContent, /codec in the radio already matched the backup/);
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
	// the simulated radio runs its original firmware: Update has nothing to update
	assert.deepEqual(forwards.at(-1).map((b) => b.textContent), ["Install", "Finish"]);
	// another radio picked meanwhile (Back, then the radio step) starts its own backup at the connect step
	const other = { ...radio, id: `${radio.id}-other` };
	state.radio = other;
	assert.match(c7ui.backupView().textContent, /Connect Your/);
	state.radio = radio;
	c7ui.backupView();
	forwards.at(-1)[0].click();
	assert.equal(state.task, "install");
	assert.equal(state.step, 0);
	assert.equal(state.c7, null, "the install starts with a new session: its check step reads the radio itself");
	assert.equal(state.backupDone, undefined, "nothing tells the next task its backup is done");
});

// A radio with a second chip (flash.second): the write step installs both, one after the other (c7000ui.js S.chain)
const twoChip = json("radios.json").radios.find((r) => r.flash.second && fb.c7000.boards[r.id]);
const installImage = (r, seed) => {
	const img = Uint8Array.from({ length: 8192 }, (_, i) => (i * seed + 1) & 0xFF);
	const fs = r.flash.first_stage;
	if (fs) {	// an install image: the first stage's signature, then RadioFoundry's reset handler (iap.js checkInstallImage)
		img.set(fs.head.match(/../g).map((x) => parseInt(x, 16)), 0);
		img.set([0x80, 0xC0, 0x20, 0x70], fs.size);
	}
	return img;
};
function twoChipHost({ second = true } = {}) {
	globalThis.window ??= { addEventListener() {}, removeEventListener() {} };
	if (!("classList" in El.prototype)) Object.defineProperty(El.prototype, "classList", { get() { return { add() {}, remove() {} }; } });
	const C = fb.c7000;
	const forwards = [], calls = { next: 0, render: 0, exit: 0, writes: [], resets: [] };
	const other = { ...twoChip, id: twoChip.flash.second.board, second_of: twoChip.id, flash: { ...twoChip.flash.second, via: twoChip.flash.board } };
	const state = { radio: twoChip, task: "install", step: 4, forward: (b) => forwards.push(b), data: { fb, radios: [twoChip] }, fakeC7: {},
		firmware: { image: installImage(twoChip, 7), source: "radio image", manifest: { version: "t" },
			second: second ? { image: installImage(other, 11), source: "second image", manifest: { version: "t" } } : null } };
	const stub = (id = state.radio.id) => ({ attach: async () => true, dump: async () => new Uint8Array(C.app_start).fill(0xA5), ledSequence: async () => {},
		resetChip: async () => { calls.resets.push(id); }, passExit: async () => { calls.exit++; return true; },
		writeProgram: async (data, start, { onSector }) => {
			onSector(C.app_start, "todo");
			onSector(C.app_start, "written");
			calls.writes.push(state.radio.id);
			return { written: 1, same: 0 };
		} });
	const session = () => ({ log: { add() {}, el: new El("div") }, stream: null, decision: { mode: "keep" }, analysis: {}, stub: stub() });
	state.c7 = session();
	c7ui.init({ S: state, fb: () => fb, helpLink: () => null, reset() {}, flashStarted: () => 0, flashFinished() {}, flashMark() {},
		errorText: (e) => e.message, next() { calls.next++; }, render() { calls.render++; }, stepName: () => "write",
		// as app.js toSecond()
		toSecond() {
			if (!state.firmware.second) return false;
			state.first = { radio: state.radio, firmware: state.firmware };
			state.radio = other;
			state.firmware = state.firmware.second;
			return true;
		} });
	return { state, forwards, calls, other, session };
}

test("a radio with a second chip: one Install writes the radio's chip and goes on in the same step, the second chip's write starts by itself", async () => {
	assert.ok(twoChip, "a radio with flash.second in radios.json");
	const { state, forwards, calls, other, session } = twoChipHost();
	const { allBackends } = await import("../static/js/flasher/backends.js");
	const backend = allBackends().find((b) => b.id === "c7000-stub");
	const box = c7ui.writeView();
	assert.doesNotMatch(box.textContent, /takes two firmware images/, "no separate narration: one card of both parts");
	assert.match(box.textContent, new RegExp(`^Install${twoChip.name}.*radio image.*Receiver B.*second image`));
	assert.equal(state.chain.stage, "write");
	assert.ok(!backend.noBack("write"), "Back still leads to the radio's own steps before its write");
	forwards.at(-1).click();
	for (let i = 0; i < 200 && !calls.render; i++) await sleep(20);
	assert.deepEqual(calls.writes, [twoChip.id]);
	assert.equal(state.chain.stage, "connect", "on to the second chip, still in the write step");
	assert.equal(state.radio.id, other.id);
	assert.deepEqual(calls.resets, [], "the radio's chip doesn't restart: its loader passes the cable to the second chip");
	assert.ok(state.c7.main, "the radio chip's loader goes with the second chip's session");
	assert.equal(calls.next, 0, "no step of its own in between");
	assert.ok(backend.noBack("write"), "no Back into the first chip's steps once it is written");
	// the second chip's loader answers through the first (the connect stage, not run here): its write needs no click
	state.c7 = { ...session(), main: state.c7.main, passing: true };
	state.chain.stage = "write2";
	const shown = forwards.length;
	const part = c7ui.writeView();
	assert.match(part.textContent, new RegExp(`^Install${twoChip.name}.*Written. It restarts once receiver B is done.*Receiver B.*Installing`));
	for (let i = 0; i < 200 && !calls.next; i++) await sleep(20);
	assert.deepEqual(calls.writes, [twoChip.id, other.id], "written without a click");
	assert.ok(forwards.slice(shown).every((b) => !b || !b.listeners || b.disabled !== undefined), "its Install button was never needed");
	assert.equal(calls.exit, 1, "the radio chip's loader takes the cable back");
	assert.deepEqual(calls.resets, [other.id, twoChip.id], "the second chip restarts, then the first");
	assert.equal(calls.next, 1, "on to the done step");
	assert.equal(state.chain.stage, "done");
	assert.ok(backend.noBack("done"));
});

test("a radio with a second chip and no image for it: the radio's chip alone, as on any radio", async () => {
	const { state, forwards, calls } = twoChipHost({ second: false });
	const box = c7ui.writeView();
	assert.doesNotMatch(box.textContent, /Receiver B/);
	assert.equal(state.chain, null);
	forwards.at(-1).click();
	for (let i = 0; i < 200 && !calls.next; i++) await sleep(20);
	assert.equal(calls.next, 1);
	assert.equal(calls.render, 0);
});

test("the second chip's stages: the steps' own ways back stay within them", async () => {
	const { state, calls } = twoChipHost();
	c7ui.writeView();
	// a write that lost its loader goes two steps back: from the second chip's write, to its connect stage
	state.chain.stage = "write2";
	state.c7.decision = { mode: "keep" };
	state.c7.stub.writeProgram = async () => { throw new Error("cable"); };
	state.c7.stub.attach = async () => false;
	state.radio = { ...twoChip, id: twoChip.flash.second.board, second_of: twoChip.id, flash: { ...twoChip.flash.second, via: twoChip.flash.board } };
	state.first = { radio: twoChip, firmware: state.firmware };
	state.firmware = state.firmware.second;
	const forwards = [];
	state.forward = (b) => forwards.push(b);
	c7ui.writeView();
	for (let i = 0; i < 200 && !/again/i.test(forwards.at(-1)?.textContent || ""); i++) await sleep(20);
	forwards.at(-1).click();
	for (let i = 0; i < 200 && !calls.render; i++) await sleep(20);
	assert.equal(state.chain.stage, "connect");
	assert.equal(state.step, 4, "the page stays on the write step");
});

// A cable session as the connect step finds it when the task already opened the cable: nothing answers on it
function deadCable(state, stream) {
	state.c7 = { log: { add() {}, el: new El("div") }, stream };
	delete state.fakeC7;
}

// Field: a low-quality cable left the connect step at "Searching for the radio" for good
test("connect step: a cable that keeps sending data nobody asked for is named, with the way on", async (t) => {
	const { limits } = await import("../static/js/flasher/stub.js");
	const was = limits.drain;
	limits.drain = 200;
	const stream = { buf: new Uint8Array(0), clear() { this.buf = new Uint8Array(0); }, take(n) { const o = this.buf.slice(0, n); this.buf = this.buf.slice(n); return o; },
		async write() {}, async close() {} };
	const noise = setInterval(() => { stream.buf = Uint8Array.of(...stream.buf.slice(-64), 0x00); }, 5);
	t.after(() => { limits.drain = was; clearInterval(noise); });
	const forwards = [];
	const state = { radio, task: "backup", forward: (b) => forwards.push(b), data: { fb }, board: fb.boards[radio.flash.board] };
	c7ui.init({ S: state, fb: () => fb, helpLink: () => null, reset() {}, render() {}, errorText: (e) => e.message });
	deadCable(state, stream);
	const box = c7ui.connectView();
	box.isConnected = true;
	assert.match(box.textContent, /Searching for the radio/);
	for (let i = 0; i < 400 && !/Cable Problem Detected/.test(box.textContent); i++) await sleep(20);
	assert.match(box.textContent, /keeps delivering data that nothing asked for \(\d+ bytes in \d+ s\)/);
	assert.match(box.textContent, /Unplug the cable from the computer/);
	const [anyway, again] = forwards.at(-1);
	assert.equal(anyway.textContent, "Continue Anyway");
	assert.equal(again.textContent, "Try Again");
	assert.equal(state.c7.stream, stream, "the cable is kept for Continue Anyway");
	anyway.click();
	for (let i = 0; i < 400 && /Cable Problem Detected/.test(box.textContent); i++) await sleep(20);
	assert.doesNotMatch(box.textContent, /Cable Problem Detected/, "the search goes on to the next way in");
});

test("connect step: a cable that left the computer's USB is said as that, not as a radio that doesn't answer", async () => {
	let closed = 0;
	const stream = { buf: new Uint8Array(0), lost: true, clear() {}, take() { return new Uint8Array(0); },
		async write() { throw Object.assign(new Error("The device has been lost."), { name: "NetworkError" }); }, async close() { closed++; } };
	const forwards = [];
	const state = { radio, task: "backup", forward: (b) => forwards.push(b), data: { fb }, board: fb.boards[radio.flash.board] };
	c7ui.init({ S: state, fb: () => fb, helpLink: () => null, reset() {}, render() {}, errorText: (e) => e.message });
	// the cable goes while the step searches (one already gone when the step is drawn is asked for again at once)
	deadCable(state, { ...stream, lost: false });
	const box = c7ui.connectView();
	box.isConnected = true;
	Object.assign(state.c7.stream, { lost: true, write: stream.write, close: stream.close });
	for (let i = 0; i < 400 && !/Cable Problem Detected/.test(box.textContent); i++) await sleep(20);
	assert.match(box.textContent, /disconnected from the computer while the page was using it/);
	assert.equal(state.c7.stream, null, "the session forgot the cable");
	assert.equal(closed, 1);
	assert.equal(forwards.at(-1).textContent, "Try Again");
});

// Field: a Prolific cable on a Mac stopped sending after a port close, and the page went on to listen for the boot ROM
// through it. The send check as the cable opens finds it: the step names the fault, says to replug it and, if it comes
// back, to use a better cable, and offers Try Again
test("connect step: a cable that takes no data is found as it opens, with what to do and Try Again", async (t) => {
	const { limits } = await import("../static/js/flasher/link.js");
	const was = { ...limits };
	Object.assign(limits, { sendCheck: 50 });
	const nav = Object.getOwnPropertyDescriptor(globalThis, "navigator");
	t.after(() => { Object.assign(limits, was); Object.defineProperty(globalThis, "navigator", nav); });
	let taken = 0;
	const dead = { isOpen: false, getInfo: () => ({ usbVendorId: 0x067B, usbProductId: 0x2303 }),
		async open() { this.isOpen = true; this.readable = null; this.writable = { getWriter: () => ({
			write: () => (taken++ < 3 ? Promise.resolve() : new Promise(() => {})), async abort() {}, releaseLock() {} }) }; },
		async close() { this.isOpen = false; } };
	Object.defineProperty(globalThis, "navigator", { value: { serial: { getPorts: async () => [dead], requestPort: async () => dead } }, configurable: true });
	const forwards = [];
	const state = { radio, task: "backup", forward: (b) => forwards.push(b), data: { fb }, board: fb.boards[radio.flash.board] };
	c7ui.init({ S: state, fb: () => fb, helpLink: () => null, reset() {}, render() {}, errorText: (e) => e.message });
	delete state.fakeC7;
	state.c7 = null;
	const box = c7ui.connectView();
	box.isConnected = true;
	const choose = [forwards.at(-1)].flat().find((b) => b?.textContent === "Choose Cable");
	assert.ok(choose, `the cable step: ${[forwards.at(-1)].flat().map((b) => b?.textContent)}`);
	choose.click();
	for (let i = 0; i < 300 && !/Cable Problem Detected/.test(box.textContent); i++) await sleep(20);
	assert.match(box.textContent, /stopped taking data from the computer/);
	assert.match(box.textContent, /Unplug the cable from the computer/);
	assert.match(box.textContent, /FTDI chip/);
	assert.equal(dead.isOpen, false, "closed again, for Try Again");
	assert.equal([forwards.at(-1)].flat().filter(Boolean).at(-1).textContent, "Try Again");
});

// Field: a cable that held the radio's line low. The boot ROM's greeting came through, the radio heard nothing, and
// the page went on to load the stub into a radio that had started its firmware
test("connect step: a radio that is heard and hears nothing is said as a cable that only carries data one way", async () => {
	const sent = [];
	const stream = { buf: new Uint8Array(0), received: 0, lineErrors: 0, clear() { this.buf = new Uint8Array(0); },
		take(n) { const o = this.buf.slice(0, n); this.buf = this.buf.slice(n); return o; },
		async write(b) { sent.push(b); }, async close() {} };
	const say = (...b) => { stream.buf = Uint8Array.of(...stream.buf, ...b); stream.received += b.length; };
	const forwards = [];
	const state = { radio, task: "backup", forward: (b) => forwards.push(b), data: { fb }, board: fb.boards[radio.flash.board] };
	c7ui.init({ S: state, fb: () => fb, helpLink: () => null, reset() {}, render() {}, errorText: (e) => e.message });
	deadCable(state, stream);
	const box = c7ui.connectView();
	box.isConnected = true;
	// nothing answers: the step asks for the radio to be switched off, then listens while it is switched on
	const press = async (label) => {
		for (let i = 0; i < 500; i++) {
			const b = [forwards.at(-1)].flat().find((x) => x?.textContent === label);
			if (b) return b.click();
			await sleep(20);
		}
		assert.fail(`no ${label} button: ${box.textContent.slice(0, 200)}`);
	};
	await press("Radio Is Off");
	const answer = (b) => b.length === 4 && b[0] === 0x02 && b[1] === 0x14 && b[2] === 0x00 && b[3] === 0x03;
	for (let i = 0; i < 200 && !sent.some(answer); i++) await sleep(10);
	assert.ok(sent.some(answer), "the answers go out ahead of the greeting");
	say(0x00, 0x02, 0x24, 0x00, 0x03);
	await sleep(80);
	say(0x00, 0xC0, 0x00, 0x03);		// the ROM stops waiting and names the application it starts
	for (let i = 0; i < 300 && !/Cable Problem Detected/.test(box.textContent); i++) await sleep(20);
	assert.match(box.textContent, /radio received none of this page's answers and started normally/);
	assert.match(box.textContent, /hold the radio's PTT line/);
	assert.equal(state.c7.stream, null, "the session forgot the cable");
	assert.equal([forwards.at(-1)].flat().filter(Boolean).at(-1).textContent, "Try Again");
});

// Field: a PL2303 adapter in a bad state read the line at another rate; the page listened on for its full 3 minutes
test("connect step: the radio's greeting read at the wrong rate is said as a cable fault at once, with Try Again", async () => {
	const stream = { buf: new Uint8Array(0), received: 0, lineErrors: 0, clear() { this.buf = new Uint8Array(0); },
		take(n) { const o = this.buf.slice(0, n); this.buf = this.buf.slice(n); return o; },
		async write() {}, async close() {} };
	const say = (...b) => { stream.buf = Uint8Array.of(...stream.buf, ...b); stream.received += b.length; };
	const forwards = [];
	const state = { radio, task: "backup", forward: (b) => forwards.push(b), data: { fb }, board: fb.boards[radio.flash.board] };
	c7ui.init({ S: state, fb: () => fb, helpLink: () => null, reset() {}, render() {}, errorText: (e) => e.message });
	deadCable(state, stream);
	const box = c7ui.connectView();
	box.isConnected = true;
	const press = async (label) => {
		for (let i = 0; i < 500; i++) {
			const b = [forwards.at(-1)].flat().find((x) => x?.textContent === label);
			if (b) return b.click();
			await sleep(20);
		}
		assert.fail(`no ${label} button: ${box.textContent.slice(0, 200)}`);
	};
	await press("Radio Is Off");
	await sleep(100);
	say(0x70, 0x80, 0x70, 0x00, 0x7C);
	await sleep(55);
	say(0x00, 0x00, 0x00, 0x7C);
	for (let i = 0; i < 100 && !/Cable Problem Detected/.test(box.textContent); i++) await sleep(20);
	assert.match(box.textContent, /never the radio's greeting/);
	assert.match(box.textContent, /9 bytes, 0 line errors/);
	assert.match(box.textContent, /FTDI chip/);
	assert.equal(state.c7.stream, null, "the session forgot the cable");
	assert.equal([forwards.at(-1)].flat().filter(Boolean).at(-1).textContent, "Try Again");
});
