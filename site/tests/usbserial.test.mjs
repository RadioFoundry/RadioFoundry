// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The WebUSB serial port (usbserial.js) against fake USB devices shaped like the bench ones (2026-11-01): a radio's
// CDC-ACM function and an FTDI FT232R cable. Run with site/tests/run.sh.

import { test } from "node:test";
import assert from "node:assert/strict";
import { ftdiDivisor, lineCoding, ftdiPayload, serialLayout, usbFilters, usbSerialNeeded, UsbSerialPort, FTDI, FTDI_LATENCY_MS } from "../static/js/flasher/usbserial.js";
import { SerialStream, browserErrorKey } from "../static/js/flasher/link.js";

const ep = (endpointNumber, direction, type = "bulk", packetSize = 64) => ({ endpointNumber, direction, type, packetSize });
const iface = (interfaceNumber, alternates) => ({ interfaceNumber, alternates });
const alt = (alternateSetting, interfaceClass, interfaceSubclass, endpoints) => ({ alternateSetting, interfaceClass, interfaceSubclass, endpoints });

// The radio in serial mode as Android listed it: CDC comm interface 0, data interface 1 with bulk 0x81 / 0x01
const ACM_CONFIG = { interfaces: [
	iface(0, [alt(0, 2, 2, [ep(2, "in", "interrupt", 16)])]),
	iface(1, [alt(0, 10, 0, [ep(1, "in"), ep(1, "out")])]),
] };
const FTDI_CONFIG = { interfaces: [iface(0, [alt(0, 255, 255, [ep(1, "in"), ep(2, "out")])])] };
// The radio in network mode: CDC-NCM, whose data interface has endpoints only in alternate 1. Not a serial port.
const NCM_CONFIG = { interfaces: [
	iface(0, [alt(0, 2, 13, [ep(2, "in", "interrupt", 16)])]),
	iface(1, [alt(0, 10, 0, []), alt(1, 10, 0, [ep(1, "in"), ep(1, "out")])]),
] };

class FakeDevice {
	constructor(vendorId, productId, config, replies = []) {
		Object.assign(this, { vendorId, productId, opened: false, configuration: null, config, replies, claimed: [], control: [], sent: [], waiting: [], reads: 0 });
	}
	async open() { this.opened = true; }
	async close() { this.opened = false; this.cancelReads(); }
	async selectConfiguration() { this.configuration = this.config; }
	async claimInterface(n) { this.claimed.push(n); }
	async releaseInterface(n) { this.claimed = this.claimed.filter((c) => c !== n); this.cancelReads(); }
	cancelReads() { for (const w of this.waiting.splice(0)) w(); }
	// data from the device while transfers wait: the oldest waiting transfer gets it, as on the bus
	arrive(bytes) { const w = this.waiting.shift(); if (w) w(bytes); else this.replies.push(bytes); }
	async selectAlternateInterface() {}
	async controlTransferOut(setup, data) { this.control.push({ ...setup, data: data && [...data] }); return { status: "ok" }; }
	async transferOut(epn, data) { this.sent.push([...data]); return { status: "ok", bytesWritten: data.length }; }
	async clearHalt() {}
	// queued replies first, then a read that waits for arrive() or until the interface is released (as a real one does)
	async transferIn() {
		this.reads++;
		const got = this.replies.length ? this.replies.shift() : await new Promise((r) => this.waiting.push(r));
		if (!got) throw new Error("transfer cancelled");
		const b = Uint8Array.from(got);
		return { status: "ok", data: new DataView(b.buffer) };
	}
}

test("FTDI divisors match AN232B-05", () => {
	assert.deepEqual(ftdiDivisor(115200), { value: 26, index: 0 });
	assert.deepEqual(ftdiDivisor(9600), { value: 0x4138, index: 0 });
	assert.deepEqual(ftdiDivisor(38400), { value: 0xC04E, index: 0 });
	assert.deepEqual(ftdiDivisor(3000000), { value: 0, index: 0 });
	assert.deepEqual(ftdiDivisor(2000000), { value: 1, index: 0 });
});

test("CDC line coding is 8N1 at the rate, little endian", () => {
	assert.deepEqual([...lineCoding(115200)], [0x00, 0xC2, 0x01, 0x00, 0, 0, 8]);
});

test("FTDI packets lose their 2 status bytes, a status-only packet gives nothing", () => {
	const two = [0x31, 0x60, ...Array(62).fill(7), 0x31, 0x60, 1, 2];
	assert.deepEqual([...ftdiPayload(Uint8Array.from(two), 64)], [...Array(62).fill(7), 1, 2]);
	assert.equal(ftdiPayload(Uint8Array.from([0x31, 0x60]), 64).length, 0);
});

test("layout: CDC-ACM and FTDI are serial ports, CDC-NCM is not", () => {
	const acm = serialLayout({ vendorId: 0x1FC9, configuration: ACM_CONFIG });
	assert.equal(acm.kind, "acm");
	assert.deepEqual(acm.claim, [0, 1]);
	assert.equal(acm.control, 0);
	const ftdi = serialLayout({ vendorId: FTDI, configuration: FTDI_CONFIG });
	assert.equal(ftdi.kind, "ftdi");
	assert.equal(ftdi.out.endpointNumber, 2);
	assert.equal(serialLayout({ vendorId: 0x1FC9, configuration: NCM_CONFIG }), null);
});

test("Web Serial filters become WebUSB filters", () => {
	assert.deepEqual(usbFilters([{ usbVendorId: 0x1FC9, usbProductId: 0x94 }, { usbVendorId: FTDI }, { bluetoothServiceClassId: "x" }]),
		[{ vendorId: 0x1FC9, productId: 0x94 }, { vendorId: FTDI }]);
	assert.deepEqual(usbFilters(undefined), []);
});

test("WebUSB stands in on Android only", () => {
	const android = "Mozilla/5.0 (Linux; Android 16; SM-S906U) AppleWebKit/537.36 Chrome/156.0.0.0 Mobile Safari/537.36";
	assert.equal(usbSerialNeeded({ usb: {}, userAgent: android }), true);
	assert.equal(usbSerialNeeded({ userAgent: android }), false);
	assert.equal(usbSerialNeeded({ usb: {}, userAgent: "Mozilla/5.0 (X11; Linux x86_64) Chrome/156" }), false);
});

test("a CDC-ACM radio through SerialStream: line set up, bytes both ways, closes clean", async () => {
	const dev = new FakeDevice(0x1FC9, 0x94, ACM_CONFIG, [[1, 2, 3], [4]]);
	const s = await SerialStream.open(new UsbSerialPort(dev));
	assert.deepEqual(dev.claimed, [0, 1]);
	assert.deepEqual(dev.control.map((c) => [c.requestType, c.request, c.value, c.index]),
		[["class", 0x20, 0, 0], ["class", 0x22, 3, 0]]);
	await s.write(Uint8Array.from([9, 9]));
	assert.deepEqual(dev.sent, [[9, 9]]);
	for (let i = 0; i < 50 && s.buf.length < 4; i++) await new Promise((r) => setTimeout(r, 1));
	assert.deepEqual([...s.buf], [1, 2, 3, 4]);
	await s.close();
	assert.deepEqual(dev.claimed, []);
	assert.equal(dev.opened, false);
	assert.equal(s.lost, false);
});

test("an FTDI cable: baud, 8N1, DTR and RTS set, the latency timer lowered; status bytes dropped; reopen after close", async () => {
	const dev = new FakeDevice(FTDI, 0x6001, FTDI_CONFIG, [[0x31, 0x60], [0x31, 0x60, 0xAA, 0x55]]);
	const port = new UsbSerialPort(dev);
	const s = await SerialStream.open(port);
	assert.deepEqual(dev.control.map((c) => [c.requestType, c.request, c.value]),
		[["vendor", 0, 0], ["vendor", 3, 26], ["vendor", 4, 8], ["vendor", 2, 0], ["vendor", 1, 0x0303], ["vendor", 9, FTDI_LATENCY_MS]]);
	assert.ok(FTDI_LATENCY_MS >= 1 && FTDI_LATENCY_MS < 16);
	for (let i = 0; i < 50 && s.buf.length < 2; i++) await new Promise((r) => setTimeout(r, 1));
	assert.deepEqual([...s.buf], [0xAA, 0x55]);
	await s.close();
	await assert.doesNotReject(SerialStream.open(port).then((s2) => s2.close()));
});

test("an FTDI cable that refuses the latency timer still opens and reads", async () => {
	const dev = new FakeDevice(FTDI, 0x6001, FTDI_CONFIG, [[0x31, 0x60, 0x42]]);
	const plain = dev.controlTransferOut.bind(dev);
	dev.controlTransferOut = async (setup, data) => {
		if (setup.requestType === "vendor" && setup.request === 9) throw new DOMException("refused", "NetworkError");
		return plain(setup, data);
	};
	const s = await SerialStream.open(new UsbSerialPort(dev));
	for (let i = 0; i < 50 && s.buf.length < 1; i++) await new Promise((r) => setTimeout(r, 1));
	assert.deepEqual([...s.buf], [0x42]);
	await s.close();
});

test("reads stay queued on the device while the page isn't reading, and nothing is lost", async () => {
	const dev = new FakeDevice(FTDI, 0x6001, FTDI_CONFIG);
	const port = new UsbSerialPort(dev);
	await port.open({ baudRate: 115200 });
	await new Promise((r) => setTimeout(r, 5));
	assert.ok(dev.waiting.length >= 2, `transfers waiting on the device: ${dev.waiting.length}`);
	// the page takes nothing while 20 packets come in
	for (let i = 0; i < 20; i++) dev.arrive([0x31, 0x60, i]);
	await new Promise((r) => setTimeout(r, 5));
	assert.ok(dev.waiting.length >= 2, "still waiting after the burst");
	const reader = port.readable.getReader();
	const got = [];
	while (got.length < 20) got.push(...(await reader.read()).value);
	assert.deepEqual(got, [...Array(20).keys()]);
	reader.releaseLock();
	await port.close();
});


const tick = (ms = 1) => new Promise((r) => setTimeout(r, ms));
const lines = (dev) => dev.control.filter((c) => c.request === 0x22 || (c.requestType === "vendor" && c.request === 1)).map((c) => c.value);

test("FTDI divisors: every fraction code (AN232B-05), the high bit in wIndex", () => {
	// eighths of the 3 MHz clock: 8 * whole + fraction; codes 0, 3, 2, 4, 1, 5, 6, 7 for 0/8 .. 7/8
	const at = (eighths) => ftdiDivisor(24000000 / eighths);
	assert.deepEqual(at(26 * 8 + 0), { value: 26, index: 0 });
	assert.deepEqual(at(26 * 8 + 1), { value: 26 | (3 << 14), index: 0 });
	assert.deepEqual(at(26 * 8 + 2), { value: 26 | (2 << 14), index: 0 });
	assert.deepEqual(at(26 * 8 + 3), { value: 26 | (0 << 14), index: 1 });
	assert.deepEqual(at(26 * 8 + 4), { value: 26 | (1 << 14), index: 0 });
	assert.deepEqual(at(26 * 8 + 5), { value: 26 | (1 << 14), index: 1 });
	assert.deepEqual(at(26 * 8 + 6), { value: 26 | (2 << 14), index: 1 });
	assert.deepEqual(at(26 * 8 + 7), { value: 26 | (3 << 14), index: 1 });
	// common rates: 57600 is 52 1/8 (417 eighths), 19200 is 156 2/8, 230400 is 13, 921600 is 3 2/8
	assert.deepEqual(ftdiDivisor(57600), { value: 0xC034, index: 0 });
	assert.deepEqual(ftdiDivisor(19200), { value: 0x809C, index: 0 });
	assert.deepEqual(ftdiDivisor(230400), { value: 13, index: 0 });
	assert.deepEqual(ftdiDivisor(921600), { value: 0x8003, index: 0 });
});

test("FTDI status bytes go at every packet boundary of a long transfer, full and high speed", () => {
	for (const size of [64, 512]) {
		const payload = Array.from({ length: 3 * (size - 2) + 5 }, (_, i) => i & 0xFF);
		const raw = [];
		for (let i = 0; i < payload.length; i += size - 2) raw.push(0x01, 0x60, ...payload.slice(i, i + size - 2));
		assert.deepEqual([...ftdiPayload(Uint8Array.from(raw), size)], payload, `packet size ${size}`);
	}
	// a transfer that ends on a packet boundary, and one of status packets only
	assert.deepEqual([...ftdiPayload(Uint8Array.from([1, 0x60, ...Array(62).fill(5)]), 64)], Array(62).fill(5));
	assert.equal(ftdiPayload(Uint8Array.from([1, 0x60, ...Array(62).fill(5), 1, 0x60]).subarray(64), 64).length, 0);
});

test("an FTDI cable through the port: status bytes dropped across packets, nothing reordered", async () => {
	const payload = Array.from({ length: 200 }, (_, i) => i);
	const raw = [];
	for (let i = 0; i < payload.length; i += 62) raw.push(0x31, 0x60, ...payload.slice(i, i + 62));
	const dev = new FakeDevice(FTDI, 0x6001, FTDI_CONFIG, [raw.slice(0, 128), raw.slice(128)]);
	const s = await SerialStream.open(new UsbSerialPort(dev));
	for (let i = 0; i < 50 && s.buf.length < 200; i++) await tick();
	assert.deepEqual([...s.buf], payload);
	await s.close();
});

test("CDC-ACM in a composite device: the data interface next to the communication one", () => {
	const config = { interfaces: [
		iface(0, [alt(0, 2, 13, [ep(3, "in", "interrupt", 16)])]),
		iface(1, [alt(0, 10, 0, []), alt(1, 10, 0, [ep(4, "in"), ep(4, "out")])]),
		iface(2, [alt(0, 2, 2, [ep(2, "in", "interrupt", 16)])]),
		iface(3, [alt(0, 10, 0, [ep(1, "in"), ep(1, "out")])]),
	] };
	const l = serialLayout({ vendorId: 0x1FC9, configuration: config });
	assert.deepEqual([l.control, l.data, l.alt, l.in.endpointNumber], [2, 3, 0, 1]);
	assert.deepEqual(l.claim, [2, 3]);
});

test("a short or stalled bulk OUT is sent on from where it stopped, no byte lost or doubled", async () => {
	const dev = new FakeDevice(0x1FC9, 0x94, ACM_CONFIG);
	const plan = [{ status: "ok", n: 3 }, { status: "stall", n: 2 }, { status: "ok" }];
	const halts = [];
	dev.transferOut = async (epn, data) => {
		const step = plan.shift() || { status: "ok" };
		const n = step.n ?? data.length;
		dev.sent.push(...data.subarray(0, n));
		return { status: step.status, bytesWritten: n };
	};
	dev.clearHalt = async (dir, n) => { halts.push([dir, n]); };
	const port = new UsbSerialPort(dev);
	await port.open({ baudRate: 115200 });
	const w = port.writable.getWriter();
	await w.write(Uint8Array.from([0, 1, 2, 3, 4, 5, 6, 7, 8, 9]));
	w.releaseLock();
	assert.deepEqual(dev.sent, [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]);
	assert.deepEqual(halts, [["out", 1]]);
	await port.close();
});

test("a bulk OUT that keeps stalling fails the write instead of hanging", async () => {
	const dev = new FakeDevice(0x1FC9, 0x94, ACM_CONFIG);
	dev.transferOut = async () => ({ status: "stall", bytesWritten: 0 });
	const s = await SerialStream.open(new UsbSerialPort(dev));
	await assert.rejects(s.write(Uint8Array.from([1, 2])), (e) => e.name === "NetworkError" && browserErrorKey(e) === "flash.err.connection_lost");
	await s.close();
});

test("close: refused while another stream reads the port; DTR and RTS dropped; the wake count settles", async () => {
	const dev = new FakeDevice(0x1FC9, 0x94, ACM_CONFIG);
	const port = new UsbSerialPort(dev);
	const s = await SerialStream.open(port);
	await tick();
	await assert.rejects(port.close(), (e) => e.name === "InvalidStateError");
	assert.ok(port.readable && dev.opened, "the reading stream keeps its port");
	// the same port picked again: Web Serial's answer stands, the first stream is not cut off
	await assert.rejects(SerialStream.open(port), (e) => e.name === "InvalidStateError");
	dev.arrive([7]);
	for (let i = 0; i < 50 && !s.buf.length; i++) await tick();
	assert.deepEqual([...s.buf], [7]);
	await s.close();
	assert.equal(dev.opened, false);
	assert.equal(port.readable, null);
	assert.deepEqual(lines(dev), [3, 0]);
});

test("FTDI close drops DTR and RTS", async () => {
	const dev = new FakeDevice(FTDI, 0x6001, FTDI_CONFIG);
	const s = await SerialStream.open(new UsbSerialPort(dev));
	await s.close();
	assert.deepEqual(lines(dev), [0x0303, 0x0300]);
});

test("a device unplugged mid-read: the stream is lost, the pump ends, a write says so", async () => {
	const dev = new FakeDevice(0x1FC9, 0x94, ACM_CONFIG);
	const port = new UsbSerialPort(dev);
	const s = await SerialStream.open(port);
	await tick();
	dev.transferOut = async () => { throw new DOMException("The device was disconnected.", "NotFoundError"); };
	dev.cancelReads();	// every waiting transferIn fails, as when the device goes
	await Promise.race([s.pumping, tick(500)]);
	assert.equal(s.lost, true);
	assert.equal(port.readable, null);
	await assert.rejects(s.write(Uint8Array.from([1])), (e) => browserErrorKey(e) === "flash.err.disconnected");
	await s.close();
});

test("a failed open leaves the device closed and released; a cable chip without a driver says what to do", async () => {
	const dev = new FakeDevice(0x1FC9, 0x94, ACM_CONFIG);
	dev.controlTransferOut = async () => { throw new DOMException("A transfer error has occurred.", "NetworkError"); };
	const port = new UsbSerialPort(dev);
	await assert.rejects(port.open({ baudRate: 115200 }));
	assert.equal(dev.opened, false);
	assert.deepEqual(dev.claimed, []);
	assert.equal(port.readable, null);
	const ch340 = new FakeDevice(0x1A86, 0x7523, { interfaces: [iface(0, [alt(0, 255, 1, [ep(2, "in"), ep(2, "out")])])] });
	const err = await new UsbSerialPort(ch340).open().catch((e) => e);
	assert.equal(browserErrorKey(err), "flash.err.usb_chip");
	assert.equal(ch340.opened, false);
	assert.equal(browserErrorKey(new DOMException("other", "NotSupportedError")), null);
});
