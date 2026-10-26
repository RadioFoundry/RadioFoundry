// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The serial port as the web tools use it, and what they say and report when a radio doesn't answer: a port closes
// for real (the next connection can open it), a port left open is opened again, a silent port is told from one where
// another firmware answers, the browser's own errors get what to do, and an unknown file is reported by what it is.

import { test, beforeEach } from "node:test";
import assert from "node:assert/strict";

import * as diag from "../static/js/diag.js";
import { SerialStream, Link, probe, silence, whyNoAnswer, noAnswerKey, writtenWithoutLink, parseRadioInfo, browserErrorKey } from "../static/js/flasher/link.js";
import { CpsSerial, TransportError } from "../static/js/codeplug/transport.js";
import { openFile, FileError } from "../static/js/editor/files.js";

beforeEach(() => diag.reset());

// A port's readable stream as the browser has it: cancel() resolves at once, but the pending read settles a moment
// later, so the reader is still locked when cancel() returns
class FakeReadable {
	constructor() {
		this.locked = false;
		this.queue = [];
		this.waiting = null;
	}

	push(chunk) {
		if (this.waiting) {
			this.waiting({ value: chunk, done: false });
			this.waiting = null;
		} else this.queue.push(chunk);
	}

	getReader() {
		this.locked = true;
		return {
			read: () => (this.queue.length ? Promise.resolve({ value: this.queue.shift(), done: false })
				: new Promise((r) => { this.waiting = r; })),
			cancel: async () => {
				const w = this.waiting;
				this.waiting = null;
				setTimeout(() => w?.({ value: undefined, done: true }), 20);
			},
			releaseLock: () => { this.locked = false; },
		};
	}
}

// A WebSerial port: close() refuses while the readable stream is locked, and open() refuses a port that is open
class FakePort {
	constructor() {
		this.isOpen = false;
		this.opens = 0;
	}

	async open() {
		if (this.isOpen) throw Object.assign(new Error("Failed to execute 'open' on 'SerialPort': The port is already open."), { name: "InvalidStateError" });
		this.isOpen = true;
		this.opens++;
		this.readable = new FakeReadable();
	}

	async close() {
		if (this.readable.locked) throw new TypeError("Failed to execute 'close' on 'SerialPort': Cannot cancel a locked stream");
		this.isOpen = false;
	}
}

test("a stream closes its port only after the pump lets go of the reader, so the port can be opened again", async () => {
	const port = new FakePort();
	const s = await SerialStream.open(port);
	port.readable.push(new Uint8Array([1, 2, 3]));
	await new Promise((r) => setTimeout(r, 10));
	assert.equal(s.received, 3);
	await s.close();
	assert.equal(port.isOpen, false, "closed");
	await SerialStream.open(port);
	assert.equal(port.opens, 2);
});

test("a port this page left open is closed and opened again", async () => {
	const port = new FakePort();
	await port.open();
	const s = await SerialStream.open(port);
	assert.equal(port.opens, 2);
	await s.close();
	assert.ok(diag.report().log.some((e) => /still open/.test(e.text)));
	// one that another stream of this page is reading is left alone: the error stands, the stream keeps the port
	const live = await SerialStream.open(port);
	await assert.rejects(SerialStream.open(port), { name: "InvalidStateError" });
	port.readable.push(new Uint8Array([7]));
	await new Promise((r) => setTimeout(r, 10));
	assert.equal(live.received, 1);
	await live.close();
});

// A stream with nothing behind it, or with another firmware's bytes coming back to every request
const stream = (answer = null) => ({
	buf: new Uint8Array(0), received: 0, first: new Uint8Array(0),
	async write() {
		if (!answer) return;
		this.buf = answer.slice();
		this.received += answer.length;
		if (!this.first.length) this.first = answer.slice(0, 16);
	},
	clear() { this.buf = new Uint8Array(0); },
	take(n) { const o = this.buf.slice(0, n); this.buf = this.buf.slice(n); return o; },
	async close() {},
});

test("no answer is told apart: nothing at all, another firmware's bytes, debug link frames", async () => {
	const quiet = new Link(stream(), { timeout: 30 });
	assert.equal(await probe(quiet), null);
	assert.deepEqual(silence(quiet), { kind: "silent", cause: "no answer to command 0x2", received: 0, first: null });
	const other = new Link(stream(new TextEncoder().encode("\x02PROGRAM\x06")), { timeout: 30 });
	await probe(other);
	assert.equal(silence(other).kind, "other");
	assert.equal(silence(other).first, "0250524f4752414d06");
	const frames = new Link(stream(new Uint8Array([0xA5, 0x5A, 0x99, 0, 0, 0, 0, 0])), { timeout: 30 });
	await probe(frames);
	assert.equal(silence(frames).kind, "link");
});

test("the codeplug editor names the case, and the report carries what the port gave", async () => {
	const ask = async (s, cable) => {
		const tr = new CpsSerial(s, { cable });
		tr.link.timeout = 30;
		return tr.identify([]).then(() => null, (e) => e);
	};
	const usb = await ask(stream(), false);
	assert.ok(usb instanceof TransportError);
	assert.equal(usb.code, "no_answer");
	assert.equal(usb.params.received, 0);
	assert.equal((await ask(stream(), true)).code, "no_answer_cable");
	const other = await ask(stream(new TextEncoder().encode("junk from stock firmware")), false);
	assert.equal(other.code, "not_rfoundry");
	assert.equal(diag.describe(other).params.kind, "other");
});

// The open-firmware CPS's radio info (open-firmware-cps.md 4), 46 bytes
const radioInfo = ({ version = 3, type = 6, git = "abc1234", built = "20260101120000" } = {}) => {
	const b = new Uint8Array(46);
	const dv = new DataView(b.buffer);
	dv.setUint32(0, version, true);
	dv.setUint32(4, type, true);
	b.set(new TextEncoder().encode(git), 8);
	b.set(new TextEncoder().encode(built), 24);
	dv.setUint32(40, 0x4018, true);
	dv.setUint16(44, 2, true);
	return b;
};

// A radio on another open firmware: "-" to anything it doesn't know (a debug link frame among them), its radio info to
// R 9. info: the bytes it gives for area 9, or null to refuse that too. Every request is kept, to check what was sent
const openFw = (info = radioInfo()) => ({
	...stream(),
	sent: [],
	async write(d) {
		this.sent.push(d.slice());
		const answer = d[0] === 0x52 && d[1] === 9 && info ? new Uint8Array([0x52, 0, info.length, ...info]) : new Uint8Array([0x2D]);
		this.buf = answer;
		this.received += answer.length;
		if (!this.first.length) this.first = answer.slice(0, 16);
	},
});

test("the radio info is read only when it has that structure", () => {
	assert.deepEqual(parseRadioInfo(radioInfo()), { structVersion: 3, radioType: 6, git: "abc1234", built: "20260101120000", flashId: 0x4018, features: 2 });
	assert.equal(parseRadioInfo(null), null);
	assert.equal(parseRadioInfo(radioInfo().subarray(0, 30)), null, "too short");
	assert.equal(parseRadioInfo(radioInfo({ version: 0 })), null, "no struct version");
	assert.equal(parseRadioInfo(radioInfo({ type: 1000 })), null, "no radio type");
	assert.equal(parseRadioInfo(radioInfo({ built: "yesterday" })), null, "not a build date");
	const junk = radioInfo();
	junk[10] = 0x01;
	assert.equal(parseRadioInfo(junk), null, "control bytes in the revision");
	const tail = radioInfo();
	tail[22] = 0x41;
	assert.equal(parseRadioInfo(tail), null, "text after the padding");
	// our shim fills both; one of them is enough, neither is not the structure (46 zero bytes after struct version 1)
	assert.equal(parseRadioInfo(radioInfo({ git: "", built: "" })), null, "no revision, no build date");
	const zeros = new Uint8Array(46);
	zeros[0] = 1;
	assert.equal(parseRadioInfo(zeros), null, "all zeros");
	assert.equal(parseRadioInfo(radioInfo({ git: "" })).built, "20260101120000");
	assert.equal(parseRadioInfo(radioInfo({ built: "" })).git, "abc1234");
});

// A RadioFoundry build without the debug link answers like another open firmware; right after the installer wrote it,
// its revision or its feature list says it is that build
test("a RadioFoundry build without the debug link is told apart right after it was written", async () => {
	const info = { git: "1a4901ae-dirty" };
	const full = "1a4901ae7c0ffee0000000000000000000000000";
	assert.equal(writtenWithoutLink(info, { git: full, features: "" }), true, "the same commit");
	assert.equal(writtenWithoutLink({ git: "1A4901AE" }, { git: full, features: "" }), true);
	assert.equal(writtenWithoutLink(info, { git: "ffff" + full.slice(4), features: "FM,DMR" }), true, "no DEBUGLINK in its features");
	assert.equal(writtenWithoutLink(info, { git: "ffff" + full.slice(4), features: "FM,DMR,DEBUGLINK" }), false);
	assert.equal(writtenWithoutLink(info, { git: "unknown", features: "" }), false, "a local build: not known");
	assert.equal(writtenWithoutLink({ git: "v1.2" }, { git: full, features: "" }), false);
	assert.equal(writtenWithoutLink(info, null), false);
	const link = new Link(openFw(radioInfo({ git: "1a4901ae" })), { timeout: 30 });
	await probe(link);
	assert.equal((await whyNoAnswer(link, { written: { git: full, features: "FM,DMR,DEBUGLINK" } })).kind, "rfoundry-nodebug");
	const other = new Link(openFw(radioInfo({ git: "1a4901ae" })), { timeout: 30 });
	await probe(other);
	assert.equal((await whyNoAnswer(other)).kind, "openfw");
	// each kind's message
	assert.equal(noAnswerKey({ kind: "openfw" }), "flash.err.openfw");
	assert.equal(noAnswerKey({ kind: "rfoundry-nodebug" }), "flash.err.rfoundry_nodebug");
	assert.equal(noAnswerKey({ kind: "other" }), "flash.err.not_rfoundry");
	assert.equal(noAnswerKey({ kind: "silent" }), "flash.err.no_answer");
	assert.equal(noAnswerKey({ kind: "link" }), "flash.err.no_answer");
});

test("another open firmware is recognized by its refusal and its radio info, with reads only", async () => {
	const s = openFw();
	const link = new Link(s, { timeout: 30 });
	assert.equal(await probe(link), null);
	assert.equal(silence(link).kind, "other");
	const why = await whyNoAnswer(link);
	assert.equal(why.kind, "openfw");
	assert.equal(why.radioType, 6);
	assert.equal(why.git, "abc1234");
	// after the probe, one R 9 and nothing else: never a C control command
	const after = s.sent.filter((d) => d[0] !== 0xA5);
	assert.deepEqual(after.map((d) => [...d]), [[0x52, 9, 0, 0, 0, 0, 0, 46]]);
	// "-" to everything, the info too: some other firmware, not told apart
	const refuses = new Link(openFw(null), { timeout: 30 });
	await probe(refuses);
	assert.equal((await whyNoAnswer(refuses)).kind, "other");
	// an info area that isn't the structure
	const bad = new Link(openFw(new Uint8Array(46).fill(0xFF)), { timeout: 30 });
	await probe(bad);
	assert.equal((await whyNoAnswer(bad)).kind, "other");
	// other bytes than "-" are never asked
	const stock = stream(new TextEncoder().encode("junk"));
	const quiet = new Link(stock, { timeout: 30 });
	await probe(quiet);
	stock.write = async () => { throw new Error("asked"); };
	assert.equal((await whyNoAnswer(quiet)).kind, "other");
	// silent: nothing asked either
	assert.equal((await whyNoAnswer(new Link(stream(), { timeout: 30 }))).kind, "silent");
	// the codeplug editor says so too
	const tr = new CpsSerial(openFw(), { cable: false });
	tr.link.timeout = 30;
	const e = await tr.identify([]).then(() => null, (x) => x);
	assert.equal(e.code, "openfw");
	assert.equal(diag.describe(e).params.kind, "openfw");
});

test("the browser's own serial and USB errors get what to do", () => {
	const err = (name, message) => Object.assign(new Error(message), { name });
	assert.equal(browserErrorKey(err("NetworkError", "Failed to execute 'open' on 'SerialPort': Failed to open serial port.")), "flash.err.port_busy");
	assert.equal(browserErrorKey(err("InvalidStateError", "The port is already open.")), "flash.err.port_open");
	assert.equal(browserErrorKey(err("NotFoundError", "The device has been disconnected.")), "flash.err.disconnected");
	assert.equal(browserErrorKey(err("BreakError", "Break received")), "flash.err.connection_lost");
	assert.equal(browserErrorKey(new Error("anything else")), null);
});

test("an unknown file is reported by its type, size and first 8 bytes, never its name", async () => {
	const bytes = new Uint8Array(5000).fill(0x41);
	bytes.set([0x44, 0x44, 0x4D, 0x52], 0);
	const e = await openFile("my codeplug.dat", bytes).then(() => null, (x) => x);
	assert.ok(e instanceof FileError);
	assert.equal(e.code, "unknown_file");
	assert.deepEqual(e.params, { ext: "dat", size: 5000, head: "44444d5241414141" });
	assert.ok(!JSON.stringify(e.params).includes("codeplug"));
});

// one string per step: Sentry keeps only three levels of an event's context, so objects in a list would arrive as
// "[Object]"
test("an automatic report carries the last steps, without the wire log, shown messages or USB serials", () => {
	diag.note("port chosen", { usbVendorId: 0x1A86 });
	diag.wire("cmd 0x2 seq 1 len 0: no answer");
	diag.note("shown: Johns_radio.csv is not a file the codeplug editor can open.");
	diag.note("bootloader: AnyRoad / Digital Radio in USB mode / serial 00000000010C / DFU");
	diag.note("serial read stopped", "BreakError");
	diag.note("big", { text: "x".repeat(1000) });
	const trail = diag.trail();
	assert.equal(trail.length, 4);
	assert.match(trail[2], /serial read stopped/);
	assert.ok(trail.every((l) => typeof l === "string" && l.length <= 301));
	assert.match(trail[0], /port chosen \{"usbVendorId":6790\}/);
	assert.match(trail[1], /serial \[serial\] \/ DFU/);
	assert.ok(!trail.join().includes("Johns_radio"));
});

test("a message shows the record of its own error, also for an error handled twice", () => {
	const a = new Error("first"), b = new Error("second");
	const ra = diag.error("one", a);
	diag.error("two", b);
	assert.equal(diag.error("one again", a), ra);
	assert.equal(diag.currentError(), ra);
});
