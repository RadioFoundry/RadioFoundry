// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Web Serial's port API over WebUSB, for browsers whose navigator.serial lists no USB ports: Chrome on Android offers
// Bluetooth serial ports only (bench 2026-11-01, Chrome 156), while its WebUSB reaches the same devices. The page
// drives the device itself: CDC-ACM (USB CDC PSTN subclass 1.2: SET_LINE_CODING, SET_CONTROL_LINE_STATE) and FTDI
// (FTDI AN232B-05 baud divisors, a 2-byte modem status at the start of every bulk IN packet). serialApi() is what
// pages call in place of navigator.serial; SerialStream (link.js) takes either kind of port.

export const FTDI = 0x0403;

// CDC class requests (PSTN 1.2, 6.3)
const SET_LINE_CODING = 0x20;
const SET_CONTROL_LINE_STATE = 0x22;
const DTR_RTS = 0x03;

// FTDI vendor requests (host to device, vendor type, device recipient)
const SIO_RESET = 0x00;
const SIO_MODEM_CTRL = 0x01;
const SIO_SET_FLOW_CTRL = 0x02;
const SIO_SET_BAUD_RATE = 0x03;
const SIO_SET_DATA = 0x04;
const FTDI_DTR_RTS_ON = 0x0303;		// high byte: which lines to change, low byte: their levels
const FTDI_8N1 = 0x0008;

const READ_SIZE = 4096;			// bytes asked per bulk IN transfer
const READS_IN_FLIGHT = 4;		// bulk IN transfers kept queued on the device
const READ_AHEAD = 64;			// chunks read ahead of a page that is slow to take them

// Web Serial on Android can't reach a USB device; WebUSB can. Desktop Chrome keeps its own Web Serial (the OS driver,
// vendor chips this file doesn't know).
export function usbSerialNeeded(nav = globalThis.navigator) {
	return !!nav?.usb && /Android/i.test(nav.userAgentData?.platform || nav.userAgent || "");
}

// The screen stays on while a port is open: a phone that turns it off puts the page in the background. A wake lock
// ends when the page is hidden, so it is taken again when the page comes back.
let wakeLock = null, openPorts = 0;
async function holdScreen() {
	if (!openPorts || wakeLock || globalThis.document?.visibilityState !== "visible") return;
	try {
		wakeLock = await navigator.wakeLock?.request("screen");
		wakeLock?.addEventListener("release", () => { wakeLock = null; });
	} catch { /* refused (battery saver): the notice still says it */ }
}
let watching = false;
function portOpened() {
	if (!watching && typeof globalThis.document?.addEventListener === "function") {
		document.addEventListener("visibilitychange", holdScreen);
		watching = true;
	}
	openPorts++;
	holdScreen();
}
function portClosed() {
	if (--openPorts > 0) return;
	openPorts = 0;
	wakeLock?.release().catch(() => {});
	wakeLock = null;
}

// FTDI's baud divisor (AN232B-05): 3 MHz over the rate in eighths, the fraction coded in 3 bits split over wValue
// bits 14-15 and wIndex bit 0. 3 and 2 Mbaud have their own codes.
export function ftdiDivisor(baud) {
	const eighths = Math.round(24000000 / baud);
	if (eighths <= 8) return { value: 0, index: 0 };
	if (eighths <= 12) return { value: 1, index: 0 };
	const frac = [0, 3, 2, 4, 1, 5, 6, 7][eighths & 7];
	return { value: ((eighths >> 3) & 0x3FFF) | ((frac & 3) << 14), index: frac >> 2 };
}

export function lineCoding(baud) {
	const b = new Uint8Array(7);
	new DataView(b.buffer).setUint32(0, baud, true);
	b[6] = 8;			// 1 stop bit, no parity, 8 data bits
	return b;
}

// FTDI bulk IN: each max-packet-sized piece starts with 2 modem status bytes
export function ftdiPayload(data, packetSize) {
	const out = [];
	for (let i = 0; i < data.length; i += packetSize) out.push(data.subarray(i + 2, Math.min(i + packetSize, data.length)));
	const n = out.reduce((a, p) => a + p.length, 0);
	const r = new Uint8Array(n);
	let o = 0;
	for (const p of out) { r.set(p, o); o += p.length; }
	return r;
}

const bulk = (alt, dir) => alt.endpoints.find((e) => e.type === "bulk" && e.direction === dir);

// The interfaces and endpoints a device's serial function uses, or null when it has none this file drives
export function serialLayout(device) {
	const ifaces = device.configuration?.interfaces || [];
	if (device.vendorId === FTDI) {
		const alt = ifaces[0]?.alternates[0];
		if (!alt || !bulk(alt, "in") || !bulk(alt, "out")) return null;
		return { kind: "ftdi", claim: [ifaces[0].interfaceNumber], data: ifaces[0].interfaceNumber, alt: alt.alternateSetting,
			in: bulk(alt, "in"), out: bulk(alt, "out"), control: 0 };
	}
	// CDC-ACM: a communication interface (class 2, subclass 2) and the data interface (class 10) with both bulk ends
	const comm = ifaces.find((i) => i.alternates.some((a) => a.interfaceClass === 2 && a.interfaceSubclass === 2));
	for (const i of ifaces) {
		const alt = i.alternates.find((a) => a.interfaceClass === 10 && bulk(a, "in") && bulk(a, "out"));
		if (comm && alt) return { kind: "acm", claim: [comm.interfaceNumber, i.interfaceNumber], data: i.interfaceNumber,
			alt: alt.alternateSetting, in: bulk(alt, "in"), out: bulk(alt, "out"), control: comm.interfaceNumber };
	}
	return null;
}

export class UsbSerialPort {
	constructor(device) {
		this.device = device;
		this.readable = null;
		this.writable = null;
	}

	getInfo() { return { usbVendorId: this.device.vendorId, usbProductId: this.device.productId }; }

	async open({ baudRate = 115200 } = {}) {
		if (this.readable) throw new DOMException("The port is already open.", "InvalidStateError");
		const d = this.device;
		if (!d.opened) await d.open();
		if (!d.configuration) await d.selectConfiguration(1);
		const l = serialLayout(d);
		if (!l) throw new DOMException("Not a serial device this page can drive.", "NotSupportedError");
		// Android's kernel driver (cdc_acm) holds the interfaces: claiming takes them over
		for (const n of l.claim) await d.claimInterface(n);
		if (l.alt) await d.selectAlternateInterface(l.data, l.alt);
		if (l.kind === "ftdi") await this.ftdiSetup(baudRate);
		else await this.acmSetup(l, baudRate);
		this.layout = l;
		this.closing = false;
		this.readable = this.makeReadable(l);
		this.writable = new WritableStream({
			write: async (chunk) => {
				const r = await d.transferOut(l.out.endpointNumber, chunk);
				if (r.status !== "ok") throw new DOMException(`USB write ${r.status}`, "NetworkError");
			},
		});
		portOpened();
	}

	async acmSetup(l, baud) {
		const setup = (request, value) => ({ requestType: "class", recipient: "interface", request, value, index: l.control });
		await this.device.controlTransferOut(setup(SET_LINE_CODING, 0), lineCoding(baud));
		await this.device.controlTransferOut(setup(SET_CONTROL_LINE_STATE, DTR_RTS));
	}

	async ftdiSetup(baud) {
		const out = (request, value, index = 0) =>
			this.device.controlTransferOut({ requestType: "vendor", recipient: "device", request, value, index });
		const div = ftdiDivisor(baud);
		await out(SIO_RESET, 0);
		await out(SIO_SET_BAUD_RATE, div.value, div.index);
		await out(SIO_SET_DATA, FTDI_8N1);
		await out(SIO_SET_FLOW_CTRL, 0);
		await out(SIO_MODEM_CTRL, FTDI_DTR_RTS_ON);
	}

	// Bulk IN transfers stay queued on the device whether or not the page is reading: an FTDI chip keeps 256 bytes,
	// 22 ms at 115200 baud, and drops the rest when no transfer is pending (frame checksum errors in a backup over a cable
	// from a phone whose tab went to the background, 2026-11-01). Transfers on one endpoint complete in order. An empty
	// result (an FTDI status-only packet) is skipped; a stall is cleared and read on, anything else ends the stream.
	makeReadable(l) {
		const d = this.device;
		const pending = [];
		const submit = () => pending.push(d.transferIn(l.in.endpointNumber, READ_SIZE).then((r) => ({ r }), (e) => ({ e })));
		return new ReadableStream({
			start: () => { for (let i = 0; i < READS_IN_FLIGHT; i++) submit(); },
			pull: async (ctl) => {
				for (;;) {
					const { r, e } = await pending.shift();
					if (e) {
						if (this.closing) return ctl.close();
						return ctl.error(new DOMException(String(e?.message || e), "NetworkError"));
					}
					if (r.status === "stall") await d.clearHalt("in", l.in.endpointNumber);
					submit();
					if (r.status !== "ok" || !r.data) continue;
					let data = new Uint8Array(r.data.buffer, r.data.byteOffset, r.data.byteLength);
					if (l.kind === "ftdi") data = ftdiPayload(data, l.in.packetSize);
					if (data.length) return ctl.enqueue(data.slice());
				}
			},
		}, { highWaterMark: READ_AHEAD });
	}

	// Releasing the interfaces ends a pending transferIn, which closes the readable (closing set first)
	async close() {
		if (this.readable) portClosed();
		this.closing = true;
		const d = this.device;
		for (const n of this.layout?.claim || []) { try { await d.releaseInterface(n); } catch { /* gone */ } }
		try { await d.close(); } catch { /* gone */ }
		this.readable = null;
		this.writable = null;
	}
}

// One port object per device, so a page comparing ports (granted before, picked again) sees the same one
const ports = new WeakMap();
const portOf = (device) => {
	if (!ports.has(device)) ports.set(device, new UsbSerialPort(device));
	return ports.get(device);
};

// Web Serial filters ({usbVendorId, usbProductId}) as WebUSB ones ({vendorId, productId})
export const usbFilters = (filters = []) =>
	filters.filter((f) => f.usbVendorId !== undefined).map((f) => (f.usbProductId === undefined
		? { vendorId: f.usbVendorId } : { vendorId: f.usbVendorId, productId: f.usbProductId }));

const usbSerial = {
	async requestPort({ filters } = {}) {
		return portOf(await navigator.usb.requestDevice({ filters: usbFilters(filters) }));
	},
	async getPorts() {
		return (await navigator.usb.getDevices()).map(portOf);
	},
};

// navigator.serial, or the WebUSB stand-in where Web Serial can't reach USB devices
export function serialApi() {
	return usbSerialNeeded() ? usbSerial : navigator.serial;
}
