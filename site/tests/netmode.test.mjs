// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A radio in USB network mode asked for its serial port (netmode.js): the endpoint 0 request over WebUSB, the radio's
// own page where WebUSB can't open it, and the wait for the serial port.

import { test } from "node:test";
import assert from "node:assert/strict";

import { SERIAL_MODE_REQUEST, SERIAL_MODE_URL, grantedNetworkRadios, requestOverUsb, serialPortPresent, toSerial } from "../static/js/flasher/netmode.js";

const NET = { vendorId: 0x1FC9, productId: 0x0096 };

function device({ opens = true } = {}) {
	const d = { ...NET, sent: [], closed: 0 };
	d.open = async () => { if (!opens) throw new DOMException("Access denied.", "SecurityError"); };
	d.controlTransferOut = async (setup) => { d.sent.push(setup); };
	d.close = async () => { d.closed++; };
	return d;
}
const usbWith = (...devices) => ({ getDevices: async () => devices });
// a serial API whose radio port shows up after `after` calls
function serialAfter(after) {
	let calls = 0;
	return { getPorts: async () => (++calls > after ? [{ getInfo: () => ({ usbVendorId: 0x1FC9, usbProductId: 0x0094 }) }] : []) };
}

test("only the network personality counts as a radio in network mode", async () => {
	const other = { vendorId: 0x1FC9, productId: 0x0094 };
	assert.deepEqual(await grantedNetworkRadios(usbWith(other, NET)), [NET]);
	assert.deepEqual(await grantedNetworkRadios(undefined), []);
	assert.deepEqual(await grantedNetworkRadios({ getDevices: async () => { throw new Error("blocked"); } }), []);
});

test("the WebUSB request is the vendor request to the device, with no data", async () => {
	const d = device();
	assert.equal(await requestOverUsb([d]), true);
	assert.deepEqual(d.sent, [{ requestType: "vendor", recipient: "device", request: SERIAL_MODE_REQUEST, value: 0, index: 0 }]);
	assert.equal(SERIAL_MODE_REQUEST, 0x53);	// drivers/usb/usb_device.h USB_REQ_SERIAL_MODE
});

test("a device the OS driver holds is skipped, and the next one tried", async () => {
	const held = device({ opens: false }), free = device();
	assert.equal(await requestOverUsb([held, free]), true);
	assert.equal(held.sent.length, 0);
	assert.equal(free.sent.length, 1);
	assert.equal(await requestOverUsb([held]), false);
	assert.equal(await requestOverUsb([]), false);
});

test("over WebUSB no tab opens, and the port coming back ends the wait", async () => {
	const opened = [];
	const r = await toSerial({ usb: usbWith(device()), serial: serialAfter(2), open: (...a) => opened.push(a), step: 1, ms: 50 });
	assert.deepEqual(r, { how: "usb", back: true });
	assert.equal(opened.length, 0);
});

test("where WebUSB can't open the radio, its own page opens in a tab that has no handle on this page", async () => {
	const opened = [];
	const r = await toSerial({ usb: usbWith(device({ opens: false })), serial: serialAfter(0), open: (...a) => opened.push(a), step: 1, ms: 50 });
	assert.deepEqual(r, { how: "http", back: true });
	assert.deepEqual(opened, [[SERIAL_MODE_URL, "_blank", "noopener"]]);
	assert.equal(SERIAL_MODE_URL, "http://12.255.255.254/api/usb?mode=serial");	// IPGW_ANY_RADIO_IP
});

test("outside a click nothing opens: no way to ask, no wait", async () => {
	const opened = [];
	const r = await toSerial({ usb: usbWith(), serial: serialAfter(0), open: (...a) => opened.push(a), http: false, step: 1, ms: 50 });
	assert.deepEqual(r, { how: null, back: false });
	assert.equal(opened.length, 0);
});

test("a port this page never had can't be seen coming back", async () => {
	const r = await toSerial({ usb: usbWith(), serial: { getPorts: async () => [] }, open: () => {}, step: 1, ms: 10 });
	assert.deepEqual(r, { how: "http", back: false });
	assert.equal(await serialPortPresent(null), false);
});
