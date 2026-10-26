// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The USB device the page talks to, for error reports and the flash log: a Web Serial port gives only its USB vendor
// and product ids (getInfo()), from which the chip is named; a WebUSB device (the bootloader) also its product string.
// No serial numbers. No DOM here, so the Node tests can use it.

export const CH340_VID = 0x1A86;

// USB vendor id -> the USB-serial chip family the programming cables use
const CHIPS = new Map([[0x0403, "FTDI"], [CH340_VID, "CH340"], [0x10C4, "CP210x"], [0x067B, "PL2303"]]);
// the radio's own USB serial port (RadioFoundry, the stub and the open-firmware CPS all enumerate with it)
const RADIO_USB = { vid: 0x1FC9, pid: 0x0094 };

const id16 = (n) => (Number.isInteger(n) && n >= 0 && n <= 0xFFFF ? n : null);

function chipOf(vid, pid) {
	if (vid === RADIO_USB.vid && pid === RADIO_USB.pid) return "radio USB";
	return CHIPS.get(vid) || (vid === null ? "" : "other");
}

// A Web Serial port's getInfo(): {vid, pid, chip, product: ""}, or null without ids (a Bluetooth or built-in port)
export function fromSerialInfo(info) {
	const vid = id16(info?.usbVendorId), pid = id16(info?.usbProductId);
	if (vid === null) return null;
	return { vid, pid, chip: chipOf(vid, pid), product: "" };
}

// A WebUSB device: {vid, pid, chip, product}
export function fromUsbDevice(dev) {
	const vid = id16(dev?.vendorId), pid = id16(dev?.productId);
	if (vid === null) return null;
	return { vid, pid, chip: vid === 0x0483 && pid === 0xDF11 ? "DFU bootloader" : chipOf(vid, pid), product: String(dev.productName || "").slice(0, 64) };
}

export const isCh340 = (a) => a?.vid === CH340_VID;

// One short line: "1a86:7523 CH340"
export function describe(a) {
	if (!a) return "";
	const x = (n) => (n === null || n === undefined ? "????" : n.toString(16).padStart(4, "0"));
	return `${x(a.vid)}:${x(a.pid)} ${a.chip}${a.product ? ` '${a.product}'` : ""}`.trim();
}
