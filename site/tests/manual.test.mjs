// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The manual install kit (manual.js): which radios get one and where, its README per install route, its files, the
// tool zip's check against site.json, and the panel and line the page shows, against a small stand-in for the DOM.
// Run with site/tests/run.sh.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

import { sha256 } from "../static/js/flasher/bytes.js";
import { zip, unzip } from "../static/js/flasher/zip.js";

// ---- just enough DOM for dom.js h()
class El {
	constructor(tag) {
		this.tagName = tag.toUpperCase();
		this.children = [];
		this.attrs = {};
		this.listeners = {};
		this.className = "";
		this.dataset = {};
	}
	append(...cs) { for (const c of cs) this.children.push(c instanceof El ? c : { text: String(c) }); }
	replaceChildren(...cs) { this.children = []; this.append(...cs); }
	setAttribute(k, v) { this.attrs[k] = v; }
	addEventListener(ev, f) { (this.listeners[ev] ||= []).push(f); }
	get textContent() { return this.children.map((c) => (c instanceof El ? c.textContent : c.text)).join(""); }
}
const strings = readFileSync(fileURLToPath(new URL("../i18n/en.json", import.meta.url)), "utf8");
globalThis.Node = El;
globalThis.document = { createElement: (t) => new El(t), body: new El("body"), getElementById: (id) => (id === "i18n" ? { textContent: strings } : null) };
const en = JSON.parse(strings);
const all = (el) => [el, ...el.children.filter((c) => c instanceof El).flatMap(all)];

const kit = await import("../static/js/flasher/manual.js");

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
const json = (n) => JSON.parse(readFileSync(join(DIR, n), "utf8"));
const fb = json("flash-boards.json");
const radios = Object.fromEntries(json("radios.json").radios.map((r) => [r.id, r]));
const SITE = "https://radiofoundry.org/";
const REPO = "https://github.com/radiofoundry/radiofoundry";
const manifest = (board) => ({ format: "rfoundry-release/1", board, version: "v0.2.0", variant: "full", image: { file: `rfoundry-${board}-v0.2.0.bin`, size: 4, sha256: "x" } });
const readme = (id, stock) => kit.kitReadme({ radio: radios[id], board: fb.boards[radios[id].flash.board], base: `rfoundry-${id}-v0.2.0`,
	manifest: manifest(id), stock, site: SITE, repo: REPO, git: "abcd1234", date: "2026-10-26" });

test("kit: a route for each radio rfoundry installs, none for the RAM stub radios", () => {
	for (const r of Object.values(radios)) {
		const family = fb.boards[r.flash.board].family;
		assert.equal(kit.kitRoute(r), { "usb-dfu": "dfu", "serial-loader": "serial" }[family] || null, r.id);
	}
	assert.equal(kit.kitRoute(radios.dm1701), "dfu");
	assert.equal(kit.kitRoute(radios.dm32), "serial");
	assert.equal(kit.kitRoute(radios.db25d), null);
});

test("kit: offered at the step that needs the API the browser lacks", () => {
	const none = { usb: false, serial: false }, both = { usb: true, serial: true };
	assert.equal(kit.missingApi(radios.dm1701, "install", "write", none), "usb");
	assert.equal(kit.missingApi(radios.dm1701, "install", "write", { usb: true, serial: false }), null);
	assert.equal(kit.missingApi(radios.dm1701, "install", "write", { usb: true, serial: false, stub: true }), "serial");
	assert.equal(kit.missingApi(radios.dm1701, "install", "write", both), null);
	for (const step of ["radio", "build", "now", "codec", "done"]) assert.equal(kit.missingApi(radios.dm1701, "install", step, none), null, step);
	assert.equal(kit.missingApi(radios.dm1701, "backup", "write", none), null);
	assert.equal(kit.missingApi(radios.dm32, "install", "connect", none), "serial");
	assert.equal(kit.missingApi(radios.dm32, "install", "connect", both), null);
	assert.equal(kit.missingApi(radios.dm32, "install", "build", none), null);
	// the DB25-D's step is blocked too: the panel then says rfoundry can't install it
	assert.equal(kit.missingApi(radios.db25d, "install", "connect", none), "serial");
});

test("kit: file names", () => {
	assert.equal(kit.kitBase(manifest("dm1701"), "dm1701"), "rfoundry-dm1701-v0.2.0");
	assert.equal(kit.kitBase({ image: { file: "local" } }, "dm32"), "rfoundry_dm32");
	assert.equal(kit.kitBase({ image: { file: "../x.bin" } }, "dm32"), "rfoundry_dm32");
	assert.equal(kit.kitBase(null, "md380"), "rfoundry_md380");
	assert.equal(kit.kitName(radios.dm1701, new Date("2026-10-26T12:00:00Z")), "RadioFoundry_Baofeng-DM-1701_manual_install_2026-10-26.zip");
});

test("kit README: a USB DFU radio with its stock file", () => {
	const r = readme("dm1701", "DM-1701(CSV)-V02.06.bin");
	const cmd = 'rfoundry.py flash "firmware/rfoundry-dm1701-v0.2.0.bin" --stock "stock/DM-1701(CSV)-V02.06.bin"';
	assert.ok(r.includes(`venv/bin/python ${cmd} --dry-run`), r);
	assert.ok(r.includes(`venv\\Scripts\\python ${cmd} --dry-run`));
	assert.ok(r.includes(`venv/bin/python ${cmd}\n`));
	assert.ok(!r.includes("--port"));
	assert.ok(r.includes("Zadig") && r.includes("50-rfoundry-dfu.rules") && r.includes("dialout"));
	assert.ok(r.includes("py -m venv venv") && r.includes("-m pip install -r requirements.txt"));
	for (const s of radios.dm1701.flash.bootloader) assert.ok(r.includes(s), s);
	assert.ok(r.includes("the codec and the bootloader key"));
	assert.ok(r.includes(`${SITE}docs/user/20-troubleshooting.html#command-line-installs-advanced`));
	assert.ok(r.includes("Ask the community: https://www.facebook.com/groups/radiofoundry/"));
	assert.ok(r.includes(`Report a bug: ${REPO}/issues`));
	assert.ok(r.includes("v0.2.0, full"));
	assert.ok(!/\bodr\b|colour|behaviour/i.test(r));
});

test("kit README: a USB DFU radio without a stock file says where to put it", () => {
	const r = readme("dm1701", null);
	assert.ok(r.includes("THE STOCK FILE"));
	assert.ok(r.includes(`--stock "stock/${fb.boards.dm1701.codec.sources[0]}"`));
});

test("kit README: a serial radio with its stock file", () => {
	const r = readme("dm32", "DM32.01.01.049.bin");
	assert.ok(r.includes('venv/bin/python rfoundry.py flash "firmware/rfoundry-dm32-v0.2.0.bin" --stock "stock/DM32.01.01.049.bin" --port PORT --dry-run'), r);
	assert.ok(!r.includes("Zadig") && !r.includes("50-rfoundry-dfu.rules"));
	assert.ok(r.includes("/dev/ttyUSB0") && r.includes("/dev/cu.usbserial-") && r.includes("COM3"));
	assert.ok(r.includes("serial.tools.list_ports"));
	assert.ok(r.includes("update mode"));
	for (const s of radios.dm32.flash.bootloader) assert.ok(r.includes(s), s);
	assert.ok(!r.includes("stubtool.py"));
});

test("kit README: the DM-1801 Pro reads its codec from the radio, rfoundry names the keys", () => {
	const r = readme("dm1801pro", null);
	assert.ok(r.includes('stub/stubtool.py PORT --board dm1801pro --progmem "stock/progmem.bin"'), r);
	assert.ok(r.includes('--stock "stock/progmem.bin" --port PORT'));
	assert.ok(r.includes("rfoundry names the keys to hold"));
	assert.ok(r.includes(`${SITE}docs/user/23-radios.html`));
	assert.ok(!r.includes("THE STOCK FILE"));
});

test("kit README: none for a radio rfoundry can't install", () => {
	assert.throws(() => readme("db25d", null), kit.KitError);
});

// a stand-in for downloads/rfoundry-tool.zip and the fetch that serves it
async function toolSite(entries = [{ name: "rfoundry.py", data: "print()\n" }, { name: "tools/rfoundry/rfoundry/cli.py", data: "x = 1\n" }]) {
	const bytes = await zip(entries);
	const site = { git: "abcd1234", repo: REPO, tool: { zip: "downloads/rfoundry-tool.zip", sha256: await sha256(bytes), size: bytes.length } };
	const urls = [];
	const fetchFn = async (url) => { urls.push(url); return { ok: true, status: 200, arrayBuffer: async () => bytes.slice().buffer }; };
	return { site, fetchFn, urls, bytes };
}

test("kit: the zip has the README, the firmware, the stock file and the tool", async () => {
	const { site, fetchFn, urls } = await toolSite();
	const image = new Uint8Array([1, 2, 3, 4]);
	const stock = { name: "DM-1701(CSV)-V02.06.bin", data: new Uint8Array(300).fill(7) };
	const k = await kit.makeKit({ radio: radios.dm1701, board: fb.boards.dm1701, firmware: { manifest: manifest("dm1701"), image },
		stock, site, root: "../", url: SITE, when: new Date("2026-10-26T12:00:00Z") }, fetchFn);
	assert.deepEqual(urls, ["../downloads/rfoundry-tool.zip"]);
	assert.equal(k.name, "RadioFoundry_Baofeng-DM-1701_manual_install_2026-10-26.zip");
	const files = await unzip(k.bytes);
	assert.deepEqual([...files.keys()], ["README.txt", "firmware/rfoundry-dm1701-v0.2.0.bin", "firmware/rfoundry-dm1701-v0.2.0.json",
		"stock/DM-1701(CSV)-V02.06.bin", "rfoundry.py", "tools/rfoundry/rfoundry/cli.py"]);
	assert.deepEqual(files.get("firmware/rfoundry-dm1701-v0.2.0.bin"), image);
	assert.deepEqual(files.get("stock/DM-1701(CSV)-V02.06.bin"), stock.data);
	assert.deepEqual(JSON.parse(new TextDecoder().decode(files.get("firmware/rfoundry-dm1701-v0.2.0.json"))), manifest("dm1701"));
	const text = new TextDecoder().decode(files.get("README.txt"));
	assert.ok(text.includes("\r\n") && !/[^\r]\n/.test(text));
	assert.ok(text.includes("Made 2026-10-26 by https://radiofoundry.org/flash/ (RadioFoundry abcd1234)"));
});

test("kit: without a stock file the folder is there, with a note", async () => {
	const { site, fetchFn } = await toolSite();
	const k = await kit.makeKit({ radio: radios.dm32, board: fb.boards.dm32, firmware: { manifest: manifest("dm32"), image: new Uint8Array(4) },
		stock: null, site, root: "", url: SITE }, fetchFn);
	const names = [...(await unzip(k.bytes)).keys()];
	assert.ok(names.includes("stock/PUT-YOUR-STOCK-FILE-HERE.txt"));
	assert.ok(!names.some((n) => n.startsWith("stock/") && n.endsWith(".bin")));
});

test("kit: a tool zip that doesn't match site.json is refused", async () => {
	const { site, fetchFn } = await toolSite();
	const o = { radio: radios.dm1701, board: fb.boards.dm1701, firmware: { manifest: manifest("dm1701"), image: new Uint8Array(4) }, stock: null, root: "", url: SITE };
	await assert.rejects(kit.makeKit({ ...o, site: { ...site, tool: { ...site.tool, sha256: "0".repeat(64) } } }, fetchFn), kit.KitError);
	await assert.rejects(kit.makeKit({ ...o, site: { ...site, tool: { ...site.tool, size: site.tool.size + 1 } } }, fetchFn), kit.KitError);
	await assert.rejects(kit.makeKit({ ...o, site: { ...site, tool: undefined } }, fetchFn), kit.KitError);
	await assert.rejects(kit.makeKit({ ...o, site }, async () => ({ ok: false, status: 404 })), /404/);
	await assert.rejects(kit.makeKit({ ...o, firmware: null, site }, fetchFn), kit.KitError);
});

const args = (id, extra = {}) => () => ({ radio: radios[id], board: fb.boards[radios[id].flash.board], site: {}, root: "", url: SITE,
	firmware: { manifest: manifest(id), image: new Uint8Array(4) }, stock: null, ...extra });

test("kit panel: in place of a step the browser can't run", () => {
	const { el, action } = kit.kitPanel(args("dm1701"), "usb", { pickStock: () => {} });
	const text = el.textContent;
	assert.ok(text.includes(en["flash.err.no_usb"]));
	assert.ok(text.includes(en["flash.kit.title"]) && text.includes(en["flash.kit.lead"]));
	assert.equal(action.textContent, en["flash.kit.download"]);
	assert.ok(action.className.includes("btn-primary"));
	// no stock file yet: the picker is offered
	assert.ok(all(el).some((e) => e.tagName === "INPUT" && e.attrs.type === "file"));
	const withStock = kit.kitPanel(args("dm1701", { stock: { name: "S.bin", data: new Uint8Array(1) } }), "usb").el;
	assert.ok(!all(withStock).some((e) => e.tagName === "INPUT"));
	assert.ok(withStock.textContent.includes("S.bin"));
	// the DM-1801 Pro: no file to pick, the README reads the codec
	const pro = kit.kitPanel(args("dm1801pro"), "serial", { pickStock: () => {} }).el;
	assert.ok(pro.textContent.includes(en["flash.kit.readout"]) && pro.textContent.includes(en["flash.err.no_serial"]));
	assert.ok(!all(pro).some((e) => e.tagName === "INPUT"));
});

test("kit panel: a radio rfoundry can't install gets the browser advice, no download", () => {
	const { el, action } = kit.kitPanel(args("db25d"), "serial");
	assert.equal(action, null);
	assert.ok(el.textContent.includes(en["flash.kit.none"].replace("{radio}", radios.db25d.name)));
	assert.ok(el.textContent.includes(en["flash.err.no_serial"]));
});

test("kit line: under a failed install, only where it can help", () => {
	const line = kit.kitLine(args("dm32"));
	assert.ok(line.textContent.includes(en["flash.kit.line_title"]) && line.textContent.includes(en["flash.kit.download"]));
	assert.equal(kit.kitLine(args("db25d")), null);
	assert.equal(kit.kitLine(args("dm32", { firmware: null })), null);
});
