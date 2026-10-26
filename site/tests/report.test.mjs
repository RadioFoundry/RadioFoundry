// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The USB adapter names (adapter.js), the CH340 banner, and Report a Problem's form and requests (report.js,
// problemreport.js), against a small stand-in for the DOM. Run with site/tests/run.sh.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

import * as adapter from "../static/js/flasher/adapter.js";
import { reportBody, sendProblemReport, contactOk, practiceFetch } from "../static/js/problemreport.js";

// ---- just enough DOM for dom.js h() and the dialog
class El {
	constructor(tag) {
		this.tagName = tag.toUpperCase();
		this.children = [];
		this.attrs = {};
		this.listeners = {};
		this.className = "";
		this.value = "";
		this.dataset = {};
	}
	append(...cs) {
		for (const c of cs) this.children.push(c instanceof El ? c : { text: String(c) });	// as the DOM: a null shows as "null"
	}
	replaceChildren(...cs) {
		this.children = [];
		this.append(...cs);
	}
	setAttribute(k, v) {
		this.attrs[k] = v;
		if (k === "checked") this.checked = true;
		else if (["type", "placeholder", "id"].includes(k)) this[k] = v;
	}
	addEventListener(ev, f) { (this.listeners[ev] ||= []).push(f); }
	get textContent() { return this.children.map((c) => (c instanceof El ? c.textContent : c.text)).join(""); }
	set textContent(v) { this.children = [{ text: String(v) }]; }
	get isConnected() { return true; }
	showModal() {}
	close() { for (const f of this.listeners.close || []) f(); }
	focus() {}
	remove() {}
	click() {}
}
const strings = readFileSync(fileURLToPath(new URL("../i18n/en.json", import.meta.url)), "utf8");
globalThis.Node = El;
globalThis.document = { createElement: (t) => new El(t), body: new El("body"), getElementById: (id) => (id === "i18n" ? { textContent: strings } : null) };
globalThis.addEventListener ||= () => {};
const quiet = console.debug;
console.debug = () => {};
process.on("exit", () => { console.debug = quiet; });
const en = JSON.parse(strings);

const { initReporting, reportDialog, scrubEvent } = await import("../static/js/report.js");
const { cableBanner } = await import("../static/js/flasher/c7000ui.js");
const diag = await import("../static/js/diag.js");

const all = (el) => [el, ...el.children.filter((c) => c instanceof El).flatMap(all)];
const find = (el, pred) => all(el).filter(pred);
const press = (b) => Promise.all((b.listeners.click || []).map((f) => f()));

// ---------------------------------------------------------------------------------------------------------------

test("adapter: the chip from the USB vendor id, the radio's own USB, WebUSB's product name", () => {
	const s = (usbVendorId, usbProductId) => adapter.fromSerialInfo({ usbVendorId, usbProductId });
	assert.deepEqual(s(0x0403, 0x6001), { vid: 0x0403, pid: 0x6001, chip: "FTDI", product: "" });
	assert.equal(s(0x1A86, 0x7523).chip, "CH340");
	assert.equal(s(0x10C4, 0xEA60).chip, "CP210x");
	assert.equal(s(0x067B, 0x2303).chip, "PL2303");
	assert.equal(s(0x1FC9, 0x0094).chip, "radio USB");
	assert.equal(s(0x1FC9, 0x0095).chip, "other");
	assert.equal(s(0x2E8A, 0x000A).chip, "other");
	assert.equal(adapter.fromSerialInfo({}), null, "a port without USB ids");
	assert.equal(adapter.fromSerialInfo(undefined), null);
	const u = adapter.fromUsbDevice({ vendorId: 0x0483, productId: 0xDF11, productName: "Digital Radio in USB mode", serialNumber: "00000000010C" });
	assert.deepEqual(u, { vid: 0x0483, pid: 0xDF11, chip: "DFU bootloader", product: "Digital Radio in USB mode" });
	assert.ok(!JSON.stringify(u).includes("010C"), "never the serial number");
	assert.equal(adapter.describe(s(0x1A86, 0x7523)), "1a86:7523 CH340");
	assert.ok(adapter.isCh340(s(0x1A86, 0x7523)));
	assert.ok(!adapter.isCh340(s(0x0403, 0x6001)));
	assert.ok(!adapter.isCh340(null));
});

test("the CH340 banner shows for a 1a86 cable, not for an FTDI one", () => {
	const b = cableBanner(adapter.fromSerialInfo({ usbVendorId: 0x1A86, usbProductId: 0x7523 }));
	assert.ok(b);
	assert.equal(find(b, (e) => e.tagName === "B")[0].textContent, "CH340 Programming Cable Detected");
	assert.ok(b.textContent.includes("Programming cables with a CH340 chip can be unreliable when taking a backup or programming the radio. "
		+ "If the backup or programming fails, try another cable with an FTDI, CP210x or PL2303 chip, which are known to work well."));
	assert.equal(cableBanner(adapter.fromSerialInfo({ usbVendorId: 0x0403, usbProductId: 0x6001 })), null);
	assert.equal(cableBanner(null), null);
});

// ---------------------------------------------------------------------------------------------------------------

const META = {
	tool: "web", tool_version: "v0.9.0", board: "x", task: "install", step: "done", flash_id: "f-123", result: "failed",
	adapter: { vid: 0x1FC9, pid: 0x0094, chip: "radio USB", product: "" }, error: "flash/write: the radio stopped answering",
	decisions: { codec_mode: "merge", codec_source: "stock", key_source: "file (opener)", plan: "merge: erase 3,4; write 3:app; keep -; 9 blocks",
		files: [{ role: "stock", name: "stock-V01.00.bin", len: 1024, sha256: "ab".repeat(32), id: "codec:V01.00" }] },
};

test("report body: the page's decisions, the trail capped, empty optional fields left out", () => {
	const trail = Array.from({ length: 250 }, (_, i) => `${i}s ${"x".repeat(400)}`);
	const b = reportBody({ ...META, trail }, { message: "  ", contact: "" });
	assert.equal(b.result, "failed");
	assert.equal(b.flash_id, "f-123");
	assert.equal(b.trail.length, 200);
	assert.ok(b.trail[0].startsWith("50s "), "the newest 200");
	assert.ok(b.trail.every((l) => l.length <= 300));
	assert.ok(!("message" in b) && !("contact" in b));
	assert.deepEqual(b.decisions, META.decisions);
	const c = reportBody(META, { message: " it stopped at 40% ", contact: " me@example.org ", result: "problem" });
	assert.equal(c.message, "it stopped at 40%");
	assert.equal(c.contact, "me@example.org");
	assert.equal(c.result, "problem");
	assert.ok(contactOk("") && contactOk("a@b.co") && !contactOk("a@b") && !contactOk("me at home"));
});

// a backend stand-in: records each request, answers files from `files` (status per name)
function backend({ report = 201, files = {} } = {}) {
	const calls = [];
	const fetchFn = async (url, init) => {
		const u = new URL(url);
		calls.push({ url: u, init });
		const reply = (status, j) => ({ status, ok: status >= 200 && status < 300, json: async () => j });
		if (u.pathname === "/v1/reports") return report === 201 ? reply(201, { id: "R7K2", token: "tok", upload_until: "", max_files: 3, file_max_bytes: 1 << 20, max_bytes: 3 << 20 })
			: reply(report, { detail: "rate limited" });
		if (u.pathname === "/data/site.json") return reply(200, {});
		const name = u.searchParams.get("name");
		const st = files[name] ?? 201;
		return reply(st, st === 201 ? { name } : { detail: `refused ${st}` });
	};
	return { calls, fetchFn };
}

test("sending: the report first, then each file with its kind, name and token; a refused file doesn't fail the report", async () => {
	const be = backend({ files: { "big.bin": 413, "late.zip": 410 } });
	const files = [
		{ name: "stock.bin", kind: "stock", data: () => new Uint8Array([1, 2, 3]) },
		{ name: "big.bin", kind: "readout", data: () => new Uint8Array(10) },
		{ name: "late.zip", kind: "backup", data: () => new Uint8Array(5) },
	];
	const r = await sendProblemReport({ api: "https://api.radiofoundry.org/v1/", body: reportBody(META), files, fetchFn: be.fetchFn });
	assert.equal(r.id, "R7K2");
	assert.deepEqual(r.sent, ["stock.bin"]);
	assert.deepEqual(r.refused.map((x) => [x.name, x.why]), [["big.bin", "too-large"], ["late.zip", "too-late"]]);
	assert.equal(be.calls.length, 4);
	assert.equal(be.calls[0].init.method, "POST");
	assert.equal(be.calls[0].init.headers["Content-Type"], "application/json");
	const f = be.calls[1];
	assert.equal(f.url.pathname, "/v1/reports/R7K2/files");
	assert.deepEqual(Object.fromEntries(f.url.searchParams), { kind: "stock", name: "stock.bin", token: "tok" });
	assert.equal(f.init.headers["Content-Type"], "application/octet-stream");
	assert.deepEqual([...f.init.body], [1, 2, 3]);
	// the report itself refused: that is an error
	await assert.rejects(sendProblemReport({ api: "https://api.radiofoundry.org/v1/", body: reportBody(META), fetchFn: backend({ report: 429 }).fetchFn }), /rate limited/);
	// a 201 whose answer has no report ID: an error that says so, never "Report sent: undefined"
	const noId = async () => ({ status: 201, ok: true, json: async () => ({}) });
	await assert.rejects(sendProblemReport({ api: "https://api.radiofoundry.org/v1/", body: reportBody(META), fetchFn: noId }), /no report ID/);
	const badJson = async () => ({ status: 201, ok: true, json: async () => { throw new SyntaxError("bad json"); } });
	await assert.rejects(sendProblemReport({ api: "https://api.radiofoundry.org/v1/", body: reportBody(META), fetchFn: badJson }), /no report ID/);
	// practice mode answers here
	const p = await sendProblemReport({ api: "https://api.radiofoundry.org/v1/", body: reportBody(META), files: files.slice(0, 1), fetchFn: practiceFetch });
	assert.deepEqual([p.id, p.sent], ["practice", ["stock.bin"]]);
});

test("Report a Problem: what is sent, the files checked by default, both fields optional, the 90 days; Send posts it all", async () => {
	const be = backend({ files: { "internal-flash.bin": 413 } });
	globalThis.fetch = be.fetchFn;
	diag.reset();
	diag.note("file picked", { name: "stock.bin", size: 3 });
	diag.note("write plan merge: erase 3,4; write 3:app; keep -; 9 blocks");
	const attachments = () => [
		{ name: "stock.bin", kind: "stock", size: 3, checked: true, label: "The stock firmware file: stock.bin", data: () => new Uint8Array([1, 2, 3]) },
		{ name: "internal-flash.bin", kind: "readout", size: 2048, checked: true, label: "The firmware read from the radio", data: () => new Uint8Array(2048) },
		{ name: "radio.zip", kind: "backup", size: 4096, checked: true, label: "The backup: radio.zip", data: () => new Uint8Array(4096) },
	];
	await initReporting({ tool: "flasher", context: () => ({}), attachments, backend: { api: "https://api.radiofoundry.org/v1/", meta: () => META } });
	const d = reportDialog();
	const text = d.textContent;
	assert.ok(!text.includes("null") && !text.includes("undefined"), "no empty part written out");
	assert.ok(text.includes(en["report.text_backend"]), "the lead says what is sent automatically");
	assert.ok(text.includes("Message (optional)"));
	assert.ok(text.includes("Email, if you'd like a reply (optional)"));
	assert.ok(text.includes("Attached files and your email address are deleted within 90 days."));
	assert.ok(text.includes("A backup holds your channels, contacts and the radio's calibration."));
	const boxes = find(d, (e) => e.tagName === "INPUT" && e.type === "checkbox");
	assert.equal(boxes.length, 3);
	assert.ok(boxes.every((b) => b.checked), "every file checked by default");
	for (const [label, size] of [["stock.bin", "1 KB"], ["The firmware read from the radio", "2 KB"], ["radio.zip", "4 KB"]]) {
		assert.ok(text.includes(label) && text.includes(size), `${label} with its size`);
	}
	boxes[2].checked = false;	// the backup left out
	const [message] = find(d, (e) => e.tagName === "TEXTAREA");
	const [email] = find(d, (e) => e.tagName === "INPUT" && e.type === "email");
	message.value = "Stopped at 40%.";
	email.value = "ham@example.org";
	const send = find(d, (e) => e.tagName === "BUTTON" && e.textContent === en["report.send"])[0];
	await press(send);
	const posts = be.calls.filter((c) => c.url.pathname.startsWith("/v1/reports"));
	assert.equal(posts.length, 3, "the report and the two checked files");
	const body = JSON.parse(posts[0].init.body);
	assert.equal(body.result, "failed");
	assert.equal(body.tool, "web");
	assert.equal(body.flash_id, "f-123");
	assert.equal(body.message, "Stopped at 40%.");
	assert.equal(body.contact, "ham@example.org");
	assert.deepEqual(body.adapter, META.adapter);
	assert.deepEqual(body.decisions, META.decisions);
	assert.ok(body.trail.some((l) => l.includes("write plan merge")), "the trail");
	assert.deepEqual(posts.slice(1).map((c) => c.url.searchParams.get("kind")), ["stock", "readout"]);
	const after = d.textContent;
	assert.ok(after.includes("Report sent: R7K2. Quote it if you write to us."));
	assert.ok(after.includes("internal-flash.bin was not sent: the file is larger than a report can take"), "the refused file is named");

	// an address that isn't one stops the send; an empty one is fine
	const d2 = reportDialog();
	find(d2, (e) => e.tagName === "INPUT" && e.type === "email")[0].value = "ham@";
	const n = be.calls.length;
	await press(find(d2, (e) => e.tagName === "BUTTON" && e.textContent === en["report.send"])[0]);
	assert.equal(be.calls.length, n, "nothing sent");
	assert.ok(d2.textContent.includes(en["report.email_bad"]));
});

// The rendered text of every state of the dialog: never an empty part written out as "null" or "undefined"
const clean = (el, what) => {
	const s = el.textContent;
	assert.ok(!/null|undefined/.test(s), `${what}: ${s}`);
	return s;
};

test("Report a Problem renders cleanly in every state: no files, some files, sending, sent, a refused file, failed", async () => {
	// the plain dialog (no backend, no Sentry: the editor with reporting off): a fresh module
	const plain = await import("../static/js/report.js?plain");
	await plain.initReporting({ tool: "editor", context: () => ({}) });
	clean(plain.reportDialog(), "download only, no files");
	await plain.initReporting({ tool: "editor", context: () => ({}), attachments: () => [{ name: "a.bin", label: "Include the codeplug I am editing", data: () => new Uint8Array(1) }] });
	clean(plain.reportDialog(), "download only, one file");
	clean(plain.reportLine(), "report line");
	clean(plain.errorNotice("It failed."), "error notice");

	const fresh = await import("../static/js/report.js?backend");
	let release;
	const gate = new Promise((r) => { release = r; });
	const be = backend({ files: { "late.zip": 410 } });
	globalThis.fetch = async (url, init) => {
		if (String(url).includes("/v1/reports")) await gate;
		return be.fetchFn(url, init);
	};
	const meta = { api: "https://api.radiofoundry.org/v1/", meta: () => META };
	await fresh.initReporting({ tool: "flasher", context: () => ({}), attachments: () => [], backend: meta });
	clean(fresh.reportDialog(), "backend, no files");
	const files = [
		{ name: "stock.bin", kind: "stock", size: 3, checked: true, label: "The stock firmware file: stock.bin", data: () => new Uint8Array(3) },
		{ name: "late.zip", kind: "backup", size: 5, checked: true, label: "The backup: late.zip", data: () => new Uint8Array(5) },
	];
	await fresh.initReporting({ tool: "flasher", context: () => ({}), attachments: () => files, backend: meta });
	const d = fresh.reportDialog();
	clean(d, "backend, two files");
	const sending = press(find(d, (e) => e.tagName === "BUTTON" && e.textContent === en["report.send"])[0]);
	await new Promise((r) => setTimeout(r, 5));
	assert.ok(clean(d, "sending").includes(en["report.sending"]));
	release();
	await sending;
	const sent = clean(d, "sent, one file refused");
	assert.ok(sent.includes("Report sent: R7K2.") && sent.includes("late.zip was not sent"));
	// the report itself refused
	globalThis.fetch = backend({ report: 429 }).fetchFn;
	const d2 = fresh.reportDialog();
	await press(find(d2, (e) => e.tagName === "BUTTON" && e.textContent === en["report.send"])[0]);
	assert.ok(clean(d2, "failed").includes("rate limited"));
	// the other parts this work added
	clean(fresh.reportLine(), "report line");
	clean(fresh.errorNotice("It failed."), "error notice");
	clean(cableBanner(adapter.fromSerialInfo({ usbVendorId: 0x1A86, usbProductId: 0x7523 })), "CH340 banner");
});

test("a Sentry event's context is scrubbed value by value: an ID in a list stays valid, a callsign and an email go", () => {
	const ev = scrubEvent({ message: "N4NQV failed", contexts: { rfoundry: { iddb: [3101276, 12], dirty: false, step: 0,
		trail: ["0.1s contact a@b.com, call W1AW", { n: 7 }] } } });
	assert.equal(ev.message, "[call] failed");
	assert.deepEqual(ev.contexts.rfoundry, { iddb: ["[id]", 12], dirty: false, step: 0, trail: ["0.1s contact [email], call [call]", { n: 7 }] });
});
