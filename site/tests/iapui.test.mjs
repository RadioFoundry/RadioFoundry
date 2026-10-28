// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The factory update mode's write step (iapui.js iapPanel) against a small stand-in for the DOM: a step the user left
// while the page listens for update mode stops listening and draws nothing. The stock console route against a simulated
// radio: the hardware variant it reports decides what the page offers. Run with site/tests/run.sh.

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

function route(r = radio, host = {}) {
	const state = { c7: { log: { add() {}, el: new El("div") } }, radio: r, forward: null };
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
	const routes = makeStockRoutes({ S, ctx, C: () => C, boardId: () => r.flash.board, session: () => state.c7, busy: async () => {},
		loader, fill: (el, ...p) => el.replaceChildren(...p.flat().filter(Boolean)), stubImage: async () => new Uint8Array(64), ...host });
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

// ---- the hardware variant on the stock console route (a radio with a stock console and variants, found by its data)

const stockRadio = json("radios.json").radios.find((r) => r.flash.stock_console && r.variants?.length);
const { FakeC7000, c7000Simulation } = await import("../static/js/flasher/fakec7000.js");
const { BUILDS, FLASH_BASE } = await import("../static/js/flasher/usmart.js");
const { stubImages } = await import("../static/js/flasher/stub.js");

// The radio on its stock firmware with this variant code; what the route shows and offers
async function stockRoute(t, { variant, radioData = stockRadio, faults = {} } = {}) {
	const sim = await c7000Simulation(C, stockRadio.id, 7000, stockRadio);
	const r = new FakeC7000({ progmem: sim.progmem.slice(), dataSize: 0x200000, speed: 200, romWindow: 50, bootTime: 50,
		usmart: stockRadio.flash.stock_versions[0].slice(-3), variant, faults });
	t.after(() => r.stop());
	r.powerOn();
	await sleep(300);
	const rt = route(radioData, { stubImage: async () => stubImages(json("c7000-stub.json")).image });
	rt.state.c7.stream = r.stream;
	rt.state.fakeC7 = r;
	const seen = { shown: null, acts: [] };
	const parts = { show: (_, ...els) => { seen.shown = new El("div"); seen.shown.append(...els.flat().filter(Boolean)); },
		act: (...bs) => { seen.acts = bs.filter(Boolean); }, onLoaded: async () => {}, onUpdateMode: () => { seen.updateMode = true; }, onAgain: () => {} };
	return { r, rt, seen, parts };
}
const label = (b) => b.textContent;

test("stock console: the supported variant goes on to the stub load with no warning", async (t) => {
	assert.ok(stockRadio, "a radio with a stock console and variants in radios.json");
	const ok = stockRadio.variants.find((v) => v.level === "supported" && v.runs?.board === stockRadio.flash.board);
	const { r, rt, seen, parts } = await stockRoute(t, { variant: ok.code });
	await rt.routes.stockFound(parts);
	assert.match(seen.shown.textContent, new RegExp(`\\.${ok.code}\\.`), "the version as the radio filled it in");
	assert.doesNotMatch(seen.shown.textContent, /in development/i);
	assert.deepEqual(rt.state.variant, { version: rt.state.variant.version, code: ok.code, via: "console" });
	assert.ok(r.frames.every((f) => f.startsWith("read_addr(")), "nothing written yet");
	assert.ok(seen.acts.map(label).includes("Switch to Update Mode"));
});

test("stock console: a variant in development shows its notice and still offers the load", async (t) => {
	const dev = stockRadio.variants.find((v) => v.runs && v.level !== "supported");
	const { rt, seen, parts } = await stockRoute(t, { variant: dev.code });
	await rt.routes.stockFound(parts);
	assert.match(seen.shown.textContent, /in development/i);
	assert.ok(seen.shown.textContent.includes(dev.notice));
	assert.ok(seen.acts.map(label).includes("Switch to Update Mode"), "the stub load is offered");
});

test("stock console: a variant the image doesn't run stops before anything is written", async (t) => {
	// the same radio data with the variant listed without an image, and with another board's
	for (const runs of [undefined, { board: "elsewhere" }]) {
		const code = "D6T";
		const data = { ...stockRadio, variants: stockRadio.variants.map((v) => (v.code === code ? { code, name: v.name, ...(runs ? { runs, level: "supported" } : {}) } : v)) };
		const { r, rt, seen, parts } = await stockRoute(t, { variant: code, radioData: data });
		await rt.routes.stockFound(parts);
		assert.match(seen.shown.textContent, /Nothing was written/);
		assert.match(seen.shown.textContent, runs ? /needs the elsewhere firmware/ : /does not support yet/);
		assert.deepEqual(seen.acts.map(label), ["Search Again"]);
		assert.ok(r.frames.every((f) => f.startsWith("read_addr(")));
	}
});

// The radio runs an older build: its version string where that build has it, and none where the proven builds do
function olderBuild(r) {
	const old = BUILDS.find((b) => !b.stub);
	for (const b of BUILDS.filter((x) => x.stub)) r.progmem.fill(0x20, b.at - FLASH_BASE - 16, b.at - FLASH_BASE + 32);
	r.progmem.set(new TextEncoder().encode(`909E.D6x.EARSAB.${old.name}\0`), old.at - FLASH_BASE - 8);
	r.usmart = old.name;
	r.build = old;
	return old;
}

test("stock console: a build without a proven stub load offers the try, then the two ways on without a backup", async (t) => {
	const { r, rt, seen, parts } = await stockRoute(t, { variant: "DBB" });
	olderBuild(r);
	await rt.routes.stockFound(parts);
	assert.match(seen.shown.textContent, /has not made a backup through this version/);
	assert.match(seen.shown.textContent, /Try the backup anyway/);
	assert.match(seen.shown.textContent, /Update the original firmware first/);
	assert.match(seen.shown.textContent, /Install without a backup/);
	assert.deepEqual(seen.acts.map(label), ["Try Backup Anyway", "Install Without Backup", "Search Again"]);
	seen.acts.find((b) => label(b) === "Install Without Backup").click();
	assert.ok(seen.updateMode);
	assert.ok(r.frames.every((f) => f.startsWith("read_addr(")));
});

test("stock console: the try on an untried build loads the stub and marks the session for the firmware offer", { timeout: 60000 }, async (t) => {
	const { r, rt, seen, parts } = await stockRoute(t, { variant: "DBB" });
	olderBuild(r);
	let loaded = 0;
	parts.onLoaded = async () => { loaded++; };
	await rt.routes.stockFound(parts);
	seen.acts.find((b) => label(b) === "Try Backup Anyway").click();
	await sleep(50);
	assert.match(seen.shown.textContent, /new to the radio programming utility/);
	seen.acts.find((b) => label(b) === "Switch to Update Mode").click();
	for (let i = 0; i < 400 && !loaded; i++) await sleep(50);
	assert.equal(loaded, 1);
	assert.equal(r.state, "stub");
	assert.deepEqual(rt.state.c7.stockUntried, { version: "909E.DBB.EARSAB.018" });
});

test("stock console: the try stops before any write when the command table isn't where the known builds keep it", { timeout: 30000 }, async (t) => {
	const { r, rt, seen, parts } = await stockRoute(t, { variant: "DBB" });
	olderBuild(r);
	r.cmdRec = 0x00012345;
	await rt.routes.stockFound(parts);
	seen.acts.find((b) => label(b) === "Try Backup Anyway").click();
	await sleep(50);
	seen.acts.find((b) => label(b) === "Switch to Update Mode").click();
	for (let i = 0; i < 200 && !/which was expected/.test(seen.shown.textContent); i++) await sleep(50);
	assert.match(seen.shown.textContent, /which was expected/);
	assert.match(seen.shown.textContent, /command table/);
	assert.ok(!seen.acts.map(label).includes("Try Backup Anyway"), "no second try offered");
	assert.ok(r.frames.every((f) => f.startsWith("read_addr(")), "nothing written, RAM included");
});

test("stock console: a build the page doesn't know still names its version, found in RAM", { timeout: 30000 }, async (t) => {
	const { r, rt, seen, parts } = await stockRoute(t, { variant: "D6T" });
	for (const b of BUILDS) r.progmem.fill(0x20, b.at - FLASH_BASE - 16, b.at - FLASH_BASE + 32);
	await rt.routes.stockFound(parts);
	assert.equal(rt.state.variant.code, "D6T");
	assert.match(seen.shown.textContent, /909E\.D6T\.EARSAB\.018/);
	assert.match(seen.shown.textContent, /in development/i);
	assert.deepEqual(seen.acts.map(label), ["Try Backup Anyway", "Install Without Backup", "Search Again"]);
	assert.ok(!r.events.includes("identify"), "the console was not silenced by an identify");
});

test("identify only: the console is silent but the identify names the variant; no backup, two ways on", async (t) => {
	const { rt, seen, parts } = await stockRoute(t, { variant: "DBB" });
	rt.routes.stockIdentified(parts, "909E.D6T.EARSAB.018");
	assert.equal(rt.state.variant.code, "D6T");
	assert.equal(rt.state.variant.via, "identify");
	assert.match(seen.shown.textContent, /in development/i);
	assert.match(seen.shown.textContent, /Install without a backup/);
});

test("update mode: the picker holds the install until the user names the hardware or says they can't", () => {
	const { state, routes } = route(stockRadio);
	const go = new El("button");
	const box = routes.variantPicker((ok) => { go.disabled = !ok; });
	assert.equal(go.disabled, true);
	const buttons = [];
	const walk = (e) => { if (e.tagName === "BUTTON") buttons.push(e); (e.children || []).forEach((c) => c instanceof El && walk(c)); };
	walk(box);
	assert.equal(buttons.length, stockRadio.variants.length + 1);
	const dev = stockRadio.variants.find((v) => v.runs && v.level !== "supported");
	buttons.find((b) => b.textContent.startsWith(`${dev.code}:`)).click();
	assert.equal(go.disabled, false);
	assert.equal(state.variant.code, dev.code);
	assert.match(box.textContent, /in development/i);
	buttons.at(-1).click();
	assert.equal(go.disabled, false);
	assert.equal(state.variant.code, null);
	assert.match(box.textContent, /can't be checked/);
});

test("update mode: a pick changed to hardware the image doesn't run while the page listens writes nothing", async () => {
	// a variant listed without an image, so the pick is refused
	const radioData = { ...stockRadio, variants: [...stockRadio.variants, { code: "DZZ", name: "test hardware" }] };
	const { state, forwards, calls, routes } = route(radioData);
	state.firmware = { image: Uint8Array.from({ length: 4096 }, (_, i) => (i * 7 + 1) & 0xFF), source: "test", manifest: {} };
	state.c7.decision = { mode: "none" };
	const box = routes.iapWriteView();
	box.isConnected = true;
	const buttons = [];
	const walk = (e) => { if (e.tagName === "BUTTON") buttons.push(e); (e.children || []).forEach((c) => c instanceof El && walk(c)); };
	walk(box);
	const ok = stockRadio.variants.find((v) => v.runs && v.level === "supported");
	buttons.find((b) => b.textContent.startsWith(`${ok.code}:`)).click();
	forwards.at(-1).click();
	await sleep(10);
	assert.equal(calls.listen, 1);
	buttons.find((b) => b.textContent.startsWith("DZZ:")).click();
	calls.found();
	await sleep(50);
	assert.equal(calls.write, 0, "nothing written");
	assert.match(box.textContent, /Nothing was written/);
	box.isConnected = false;
});
