// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The WebUSB serial port (usbserial.js) against fake USB devices shaped like the bench ones (2026-11-01): a radio's
// CDC-ACM function and an FTDI FT232R cable. Run with site/tests/run.sh.

import { test } from "node:test";
import assert from "node:assert/strict";
import { ftdiDivisor, lineCoding, ftdiPayload, serialLayout, usbFilters, usbSerialNeeded, showPhoneNotes, UsbSerialPort, FTDI } from "../static/js/flasher/usbserial.js";
import { SerialStream } from "../static/js/flasher/link.js";

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

test("an FTDI cable: baud, 8N1, DTR and RTS set; status bytes dropped; reopen after close", async () => {
	const dev = new FakeDevice(FTDI, 0x6001, FTDI_CONFIG, [[0x31, 0x60], [0x31, 0x60, 0xAA, 0x55]]);
	const port = new UsbSerialPort(dev);
	const s = await SerialStream.open(port);
	assert.deepEqual(dev.control.map((c) => [c.requestType, c.request, c.value]),
		[["vendor", 0, 0], ["vendor", 3, 26], ["vendor", 4, 8], ["vendor", 2, 0], ["vendor", 1, 0x0303]]);
	for (let i = 0; i < 50 && s.buf.length < 2; i++) await new Promise((r) => setTimeout(r, 1));
	assert.deepEqual([...s.buf], [0xAA, 0x55]);
	await s.close();
	await assert.doesNotReject(SerialStream.open(port).then((s2) => s2.close()));
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

test("the keep-in-front notice shows where the WebUSB port is used, only there", () => {
	const notes = [{ hidden: true }, { hidden: true }];
	const doc = { querySelectorAll: (sel) => (sel === ".phone-note" ? notes : []) };
	showPhoneNotes(doc, { usb: {}, userAgent: "Mozilla/5.0 (X11; Linux x86_64) Chrome/156" });
	assert.deepEqual(notes.map((n) => n.hidden), [true, true]);
	showPhoneNotes(doc, { usb: {}, userAgent: "Mozilla/5.0 (Linux; Android 16) Chrome/156 Mobile" });
	assert.deepEqual(notes.map((n) => n.hidden), [false, false]);
});
