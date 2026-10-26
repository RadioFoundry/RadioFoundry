// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The serial port as the web tools use it, and what they say and report when a radio doesn't answer: a port closes
// for real (the next connection can open it), a port left open is opened again, a silent port is told from one where
// another firmware answers, the browser's own errors get what to do, and an unknown file is reported by what it is.

import { test, beforeEach } from "node:test";
import assert from "node:assert/strict";

import * as diag from "../static/js/diag.js";
import { SerialStream, Link, probe, silence, browserErrorKey } from "../static/js/flasher/link.js";
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
