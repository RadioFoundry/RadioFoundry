// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The factory update mode's write step (iapui.js iapPanel) against a small stand-in for the DOM: a step the user left
// while the page listens for update mode stops listening and draws nothing. Run with site/tests/run.sh.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

// ---- just enough DOM for dom.js h(); isConnected: in the page (attach / detach)
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
	replaceChildren(...cs) { this.children = []; this.append(...cs); }
	setAttribute(k, v) { this.attrs[k] = v; }
	addEventListener(ev, f) { (this.listeners[ev] ||= []).push(f); }
	click() { for (const f of this.listeners.click || []) f({ preventDefault() {} }); }
	get textContent() { return this.children.map((c) => (c instanceof El ? c.textContent : c.text)).join(""); }
	set textContent(v) { this.children = [{ text: String(v) }]; }
	get firstChild() { return this.children[0]; }
}
const strings = readFileSync(fileURLToPath(new URL("../i18n/en.json", import.meta.url)), "utf8");
globalThis.Node = El;
globalThis.document = { createElement: (t) => new El(t), body: new El("body"), getElementById: (id) => (id === "i18n" ? { textContent: strings } : null) };

const { makeStockRoutes } = await import("../static/js/flasher/iapui.js");

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
const json = (n) => JSON.parse(readFileSync(join(DIR, n), "utf8"));
const C = json("flash-boards.json").c7000;
// a radio on the Baofeng updater, found by what it is, not by name
const radio = json("radios.json").radios.find((r) => r.flash.update_mode === "baofeng");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function route() {
	const state = { c7: { log: { add() {}, el: new El("div") } }, radio, forward: null };
	const forwards = [];
	const S = () => state;
	state.forward = (b) => forwards.push(b);
	// found: resolves the listening as the radio entering update mode does (bfloader.js ready)
	const calls = { made: 0, listen: 0, write: 0, aborted: false, found: null };
	const loader = () => {
		calls.made++;
		const l = {
			s: state.c7.stream, ready: false,
			listen: (ms, { signal }) => new Promise((resolve, reject) => {
				calls.listen++;
				calls.found = () => { l.ready = true; resolve({ id: "x" }); };
				signal.addEventListener("abort", () => { calls.aborted = true; reject(new Error("stopped")); });
			}),
			write: async () => { calls.write++; l.ready = false; },
		};
		return l;
	};
	const ctx = { flashStarted: () => 0, flashFinished() {}, flashMark() {}, errorText: (e) => e.message, helpLink: () => null, next() {} };
	const routes = makeStockRoutes({ S, ctx, C: () => C, boardId: () => radio.flash.board, session: () => state.c7, busy: async () => {},
		loader, fill: (el, ...p) => el.replaceChildren(...p.flat().filter(Boolean)), stubImage: async () => new Uint8Array(0) });
	return { state, forwards, calls, routes };
}

// a whole program memory backup with an application in it
function backup() {
	const p = new Uint8Array(C.progmem_size).fill(0xFF);
	for (let i = C.app_start; i < C.app_start + 4096; i++) p[i] = (i * 7) & 0xFF;
	return p;
}

test("factory update mode: leaving the step while the page listens stops the listening and draws nothing", async () => {
	assert.ok(radio, "a Baofeng-loader radio in radios.json");
	const { forwards, calls, routes } = route();
	let done = 0;
	const box = routes.iapRestoreView(backup(), null, () => done++);
	box.isConnected = true;
	const go = forwards.at(-1);
	go.click();
	await sleep(10);
	assert.equal(calls.listen, 1);
	const shown = forwards.length;
	box.isConnected = false;	// Back: the panel is off the page
	await sleep(400);
	assert.ok(calls.aborted, "the listening was stopped");
	assert.equal(calls.write, 0, "nothing written");
	assert.equal(forwards.length, shown, "nothing put in the nav bar of the step that replaced it");
	assert.equal(done, 0);

	// Start over: a new session, the same
	const r2 = route();
	const box2 = r2.routes.iapRestoreView(backup(), null, () => {});
	box2.isConnected = true;
	r2.forwards.at(-1).click();
	await sleep(10);
	r2.state.c7 = { log: { add() {}, el: new El("div") } };
	await sleep(400);
	assert.ok(r2.calls.aborted && r2.calls.write === 0);
});

test("factory update mode: on the page, Cancel stops the listening and offers the step again", async () => {
	const { forwards, calls, routes } = route();
	const box = routes.iapRestoreView(backup(), null, () => {});
	box.isConnected = true;
	forwards.at(-1).click();
	await sleep(10);
	forwards.at(-1).click();	// Cancel
	await sleep(50);
	assert.ok(calls.aborted && calls.write === 0);
	assert.match(forwards.at(-1).textContent, /\S/, "a button to start again");
});

test("factory update mode: a loader that identified itself is kept for the next try, never listened for again", async () => {
	// the radio answers steps 1-3 once per power-on (hardware-dm32.md 13.2)
	const { state, forwards, calls, routes } = route();
	const box = routes.iapRestoreView(backup(), null, () => {});
	box.isConnected = true;
	forwards.at(-1).click();
	await sleep(10);
	box.isConnected = false;	// Back, as the radio reports update mode
	calls.found();
	await sleep(50);
	assert.equal(calls.write, 0, "nothing written from a step no longer on the page");
	assert.ok(state.c7.loader.ready);
	// the step again, same session: the kept loader writes, nothing listens
	let done = 0;
	const again = routes.iapRestoreView(backup(), null, () => done++);
	again.isConnected = true;
	forwards.at(-1).click();
	await sleep(1500);
	assert.deepEqual([calls.made, calls.listen, calls.write, done], [1, 1, 1, 1]);
	// after a write the next try makes a new loader and listens (the user switches the radio off and on with the keys)
	forwards.length = 0;
	const third = routes.iapRestoreView(backup(), null, () => {});
	third.isConnected = true;
	forwards.at(-1).click();
	await sleep(10);
	assert.deepEqual([calls.made, calls.listen], [2, 2]);
	third.isConnected = false;
	await sleep(300);
});
