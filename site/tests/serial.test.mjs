// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The serial port as the web tools use it, and what they say and report when a radio doesn't answer: a port closes
// for real (the next connection can open it), a port left open is opened again, a silent port is told from one where
// another firmware answers, the browser's own errors get what to do, and an unknown file is reported by what it is.

import { test, beforeEach } from "node:test";
import assert from "node:assert/strict";

import * as diag from "../static/js/diag.js";
import { SerialStream, CableError, limits, Link, probe, silence, whyNoAnswer, noAnswerKey, writtenWithoutLink, parseRadioInfo, flashBytes, browserErrorKey, grantedPorts, knownRadioPort } from "../static/js/flasher/link.js";
import { CpsSerial, TransportError } from "../static/js/codeplug/transport.js";
import { Stub, limits as stubLimits } from "../static/js/flasher/stub.js";
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
	assert.equal(s.lost, false, "closed by the page, not lost");
	await SerialStream.open(port);
	assert.equal(port.opens, 2);
});

// Web Serial on a framing error or a break: the pending read rejects and the port has a new readable
function lineError(port, name) {
	const old = port.readable;
	port.readable = new FakeReadable();
	const w = old.waiting;
	old.waiting = null;
	w?.(Promise.reject(Object.assign(new Error(name.replace("Error", " error")), { name })));
}

test("a framing error or a break doesn't end the reading: the bytes after it still come in; a lost device does", async () => {
	const port = new FakePort();
	const s = await SerialStream.open(port);
	await new Promise((r) => setTimeout(r, 5));
	lineError(port, "FramingError");
	await new Promise((r) => setTimeout(r, 5));
	lineError(port, "BreakError");
	await new Promise((r) => setTimeout(r, 5));
	port.readable.push(new Uint8Array([0x06]));
	await new Promise((r) => setTimeout(r, 5));
	assert.equal(s.received, 1, "the acknowledgement after the glitches is read");
	assert.ok(diag.report().log.some((e) => /reading on/.test(e.text)));
	assert.equal(s.lost, false, "glitches don't lose the device");
	const tr = new CpsSerial(s);
	assert.equal(tr.lost, false);
	lineError(port, "NetworkError");
	await new Promise((r) => setTimeout(r, 5));
	assert.ok(diag.report().log.some((e) => /read stopped/.test(e.text)));
	assert.equal(s.lost, true, "the page drops this stream and connects afresh (app.js connectSerial)");
	assert.equal(tr.lost, true, "the editor connects again before its next read or write (editor/app.js connect)");
	port.readable.push(new Uint8Array([1]));
	await new Promise((r) => setTimeout(r, 5));
	assert.equal(s.received, 1, "nothing read after a fatal error");
});

// Field (Sentry OPENDIGITALRADIO-M): a radio switched off and on with the cable in gave break after break; 50 of them
// once stopped the reading for good, the radio still there, and every stub request after it failed "no reply header"
test("a radio power-cycled with the cable in: hundreds of breaks in a row never stop the reading", async () => {
	const port = new FakePort();
	const s = await SerialStream.open(port);
	const before = diag.report().log.length;
	for (let i = 0; i < 300; i++) {
		await new Promise((r) => setTimeout(r, 0));
		lineError(port, "BreakError");
	}
	await new Promise((r) => setTimeout(r, 60));
	port.readable.push(new Uint8Array([0x02, 0x05]));
	await new Promise((r) => setTimeout(r, 60));
	assert.equal(s.received, 2, "the stub's reply after the breaks is read");
	assert.equal(s.lost, false, "the device is still there");
	const noted = diag.report().log.slice(before).filter((e) => /serial read/.test(e.text));
	assert.ok(noted.length < 30, `a burst is noted sparingly (${noted.length} lines)`);
	assert.ok(!noted.some((e) => /read stopped/.test(e.text)));
	await s.close();
});

// Field: a low-quality programming cable held the connect step at "Searching for the radio" until the page was
// started over, every time, until the cable was unplugged: the wait for a quiet line had no end
test("a line that never goes quiet ends the wait for it, with what came in; a quiet one ends it at once", async (t) => {
	const was = stubLimits.drain;
	stubLimits.drain = 150;
	const s = { buf: new Uint8Array(0), clear() { this.buf = new Uint8Array(0); }, async write() {} };
	const noise = setInterval(() => { s.buf = Uint8Array.of(...s.buf, 0x55); }, 5);
	t.after(() => { stubLimits.drain = was; clearInterval(noise); });
	const lines = [];
	const stub = new Stub(s, { log: (l) => lines.push(l) });
	const t0 = Date.now();
	assert.equal(await stub.drain(30), false);
	assert.ok(Date.now() - t0 < 1000, "the wait ends");
	assert.ok(stub.noise.bytes > 5 && stub.noise.ms >= 150, JSON.stringify(stub.noise));
	assert.match(lines.join("\n"), /never went quiet: \d+ bytes/);
	clearInterval(noise);
	const quiet = new Stub(s, { log() {} });
	assert.equal(await quiet.drain(30), true);
	assert.equal(quiet.noise, null);
});

test("a write the adapter never takes ends as a cable fault, and so does every write after it, at once", async (t) => {
	const was = { ...limits };
	Object.assign(limits, { write: 60 });
	t.after(() => Object.assign(limits, was));
	const port = new FakePort();
	let aborted = 0, released = 0;
	const s = await SerialStream.open(port);
	port.writable = { getWriter: () => ({ write: () => new Promise(() => {}), abort: async () => { aborted++; }, releaseLock: () => { released++; } }) };
	const t0 = Date.now();
	await assert.rejects(s.write(new Uint8Array(4)), (e) => e instanceof CableError && e.code === "stalled");
	assert.ok(Date.now() - t0 < 2000, "the write is given up");
	assert.deepEqual([aborted, released], [1, 1], "what is queued is dropped and the writer let go");
	assert.ok(diag.report().log.some((e) => /write stalled/.test(e.text)));
	const t1 = Date.now();
	await assert.rejects(s.write(new Uint8Array(4)), CableError);
	assert.ok(Date.now() - t1 < 50, "no second wait");
	// a write the adapter takes is not touched by the limit
	const ok = await SerialStream.open(new FakePort());
	const sent = [];
	ok.port.writable = { getWriter: () => ({ write: async (d) => { sent.push(d.length); }, releaseLock() {} }) };
	await ok.write(new Uint8Array(7));
	assert.deepEqual(sent, [7]);
});

// A USB-serial cable (PL2303) whose writer takes `takes` chunks and then never another, as a Prolific adapter whose
// sending side hung did on a Mac: receiving still worked, so the radio's greeting came in and no answer went out
class FakeCable extends FakePort {
	constructor(takes = Infinity) {
		super();
		this.takes = takes;
		this.taken = 0;
		this.buffers = [];
		this.aborted = 0;
		this.drained = 0;
	}

	getInfo() { return { usbVendorId: 0x067B, usbProductId: 0x2303 }; }

	async open(opts) {
		await super.open(opts);
		this.buffers.push(opts.bufferSize);
		this.writable = { getWriter: () => ({
			write: (d) => (this.taken < this.takes ? (this.taken++, Promise.resolve()) : new Promise(() => {})),
			abort: async () => { this.aborted++; },
			close: () => { this.drained++; return new Promise(() => {}); },
			releaseLock() {},
		}) };
	}
}

test("a cable whose adapter sends nothing fails as it opens, as a cable fault; one that sends opens as before", async (t) => {
	const was = { ...limits };
	Object.assign(limits, { sendCheck: 50 });
	t.after(() => Object.assign(limits, was));
	const dead = new FakeCable(3);
	const t0 = Date.now();
	await assert.rejects(SerialStream.open(dead), (e) => e instanceof CableError && e.code === "stalled" && /768 of 8192/.test(e.message));
	assert.ok(Date.now() - t0 < 3000, "the check ends on its own");
	assert.deepEqual(dead.buffers, [256], "checked through a small buffer, and not opened for use");
	assert.equal(dead.aborted, 1, "the write left pending is dropped");
	assert.equal(dead.drained, 0, "never drained: a drain on such an adapter froze the browser");
	assert.equal(dead.isOpen, false, "the port is closed again, so Try Again can open it");
	assert.ok(diag.report().log.some((e) => /send check failed/.test(e.text)));

	const good = new FakeCable();
	const s = await SerialStream.open(good);
	assert.deepEqual(good.buffers, [256, 65536], "checked, then opened for use with the large buffer");
	assert.equal(good.taken, 32, "8 KB went out");
	assert.equal(good.aborted, 0);
	await s.close();
	// the radio's own USB port and ports without USB ids are not checked
	const own = new FakeCable(0);
	own.getInfo = () => ({ usbVendorId: 0x1FC9, usbProductId: 0x0094 });
	await (await SerialStream.open(own)).close();
	assert.deepEqual(own.buffers, [65536]);
});

// Field: a Prolific adapter on macOS whose port was closed with data still going out sent nothing more, through every
// later open, until it was unplugged. The boot ROM wait keeps answers going out, so leaving it did exactly that
test("a port closes only after what was written had its time on the line; a stalled one closes at once", async () => {
	const port = new FakePort();
	const s = await SerialStream.open(port);
	port.writable = { getWriter: () => ({ write: async () => {}, releaseLock() {} }) };
	await s.write(new Uint8Array(4608));	// 400 ms at 115200
	const t0 = Date.now();
	await s.close();
	const waited = Date.now() - t0;
	assert.ok(waited >= 400 && waited < 1500, `waited ${waited} ms`);
	assert.equal(port.isOpen, false);
	const quiet = await SerialStream.open(new FakePort());
	const t1 = Date.now();
	await quiet.close();
	assert.ok(Date.now() - t1 < 300, "nothing written: no wait");
	const stalled = await SerialStream.open(new FakePort());
	stalled.outUntil = Date.now() + 10000;
	stalled.fault = new CableError("stalled", "x");
	const t2 = Date.now();
	await stalled.close();
	assert.ok(Date.now() - t2 < 300, "a cable that sends nothing is not waited for");
});

test("a cable picked before that sends nothing is reported, not passed over as a busy port", async (t) => {
	const was = { ...limits };
	Object.assign(limits, { sendCheck: 50 });
	const nav = Object.getOwnPropertyDescriptor(globalThis, "navigator");
	t.after(() => { Object.assign(limits, was); Object.defineProperty(globalThis, "navigator", nav); });
	const dead = new FakeCable(3);
	Object.defineProperty(globalThis, "navigator", { value: { serial: { getPorts: async () => [dead] } }, configurable: true });
	await assert.rejects(knownRadioPort([{ usbVendorId: 0x067B }]), (e) => e instanceof CableError && e.code === "stalled");
	assert.equal(dead.opens, 1, "checked once");
});

test("a port that never opens ends as a cable fault", async (t) => {
	const was = { ...limits };
	Object.assign(limits, { open: 60 });
	t.after(() => Object.assign(limits, was));
	await assert.rejects(SerialStream.open({ open: () => new Promise(() => {}) }), (e) => e instanceof CableError && e.code === "stalled");
});

test("the line errors of a port are counted, for the page's verdict on a cable that never carried the radio's greeting", async () => {
	const port = new FakePort();
	const s = await SerialStream.open(port);
	for (const name of ["BreakError", "FramingError", "BreakError"]) {
		await new Promise((r) => setTimeout(r, 5));
		lineError(port, name);
	}
	await new Promise((r) => setTimeout(r, 5));
	assert.equal(s.lineErrors, 3);
	assert.equal(s.received, 0);
	await s.close();
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

// A port the site was granted before: its USB ID, and a cable with no radio answering on it
class GrantedPort extends FakePort {
	constructor(usbVendorId, usbProductId) {
		super();
		this.info = { usbVendorId, usbProductId };
		this.writable = { getWriter: () => ({ write: async () => {}, releaseLock() {} }) };
	}

	getInfo() { return this.info; }
}

test("the ports granted before are tried as the chooser would list them; a silent one is closed and passed over", async (t) => {
	const cable = new GrantedPort(0x0403, 0x6001), other = new GrantedPort(0x05AC, 0x1234);
	// Node's navigator is a getter without a setter: replaced for this test and put back
	const was = Object.getOwnPropertyDescriptor(globalThis, "navigator");
	Object.defineProperty(globalThis, "navigator", { value: { serial: { getPorts: async () => [other, cable] } }, configurable: true });
	t.after(() => Object.defineProperty(globalThis, "navigator", was));
	const filters = [{ usbVendorId: 0x0403 }];
	assert.deepEqual(await grantedPorts(filters), [cable]);
	assert.equal(await knownRadioPort(filters), null, "nothing answered: the chooser opens");
	assert.equal(cable.opens, 2, "the send check, then the probe");
	assert.equal(cable.isOpen, false, "closed again for the chooser to hand out");
	assert.equal(other.opens, 0);
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

test("the memory chip's size from the radio info's flash part id", () => {
	assert.equal(flashBytes(0x4018), 0x1000000, "RadioFoundry's id, 16 MB");
	assert.equal(flashBytes(0x4014), 0x100000, "1 MB");
	assert.equal(flashBytes(0xEF4018), 0x1000000, "a full JEDEC id");
	assert.equal(flashBytes(0), null, "no chip");
	assert.equal(flashBytes(null), null);
	assert.equal(flashBytes(0x40FF), null, "no capacity code");
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
// its feature list says it is that build, or its revision when the features aren't known
test("a RadioFoundry build without the debug link is told apart right after it was written", async () => {
	const info = { git: "1a4901ae-dirty" };
	const full = "1a4901ae7c0ffee0000000000000000000000000";
	assert.equal(writtenWithoutLink(info, { git: full, features: "" }), true, "the same commit");
	assert.equal(writtenWithoutLink({ git: "1A4901AE" }, { git: full, features: "" }), true);
	assert.equal(writtenWithoutLink(info, { git: "ffff" + full.slice(4), features: "FM,DMR" }), true, "no DEBUGLINK in its features");
	assert.equal(writtenWithoutLink(info, { git: "ffff" + full.slice(4), features: "FM,DMR,DEBUGLINK" }), false);
	assert.equal(writtenWithoutLink(info, { git: full, features: "FM,DMR,DEBUGLINK" }), false,
		"the same commit, but a build with the debug link would have answered over it");
	assert.equal(writtenWithoutLink(info, { git: full, features: "FM,DMR" }), true);
	assert.equal(writtenWithoutLink(info, { git: "unknown", features: "" }), false, "a local build: not known");
	assert.equal(writtenWithoutLink({ git: "v1.2" }, { git: full, features: "" }), false);
	assert.equal(writtenWithoutLink(info, null), false);
	const link = new Link(openFw(radioInfo({ git: "1a4901ae" })), { timeout: 30 });
	await probe(link);
	assert.equal((await whyNoAnswer(link, { written: { git: full, features: "FM,DMR" } })).kind, "rfoundry-nodebug");
	const withLink = new Link(openFw(radioInfo({ git: "1a4901ae" })), { timeout: 30 });
	await probe(withLink);
	assert.equal((await whyNoAnswer(withLink, { written: { git: full, features: "FM,DMR,DEBUGLINK" } })).kind, "openfw",
		"a build with the debug link that doesn't answer over it is not that build");
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
	// Windows without the WinUSB driver, or another program holding the radio: not the page's origin
	assert.equal(browserErrorKey(err("SecurityError", "Failed to execute 'open' on 'USBDevice': Access denied.")), "flash.err.usb_busy");
	assert.equal(browserErrorKey(err("SecurityError", "Access to the feature \"usb\" is disallowed by permissions policy.")), "flash.err.security");
	// a chooser asked for after the click's activation ran out: click again, not an HTTPS problem (OPENDIGITALRADIO-1Q)
	assert.equal(browserErrorKey(err("SecurityError", "Failed to execute 'requestPort' on 'Serial': Must be handling a user gesture to show a permission request.")), "flash.err.gesture");
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
