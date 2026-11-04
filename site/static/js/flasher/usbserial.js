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
const LINES_OFF = 0x00;

// FTDI vendor requests (host to device, vendor type, device recipient)
const SIO_RESET = 0x00;
const SIO_MODEM_CTRL = 0x01;
const SIO_SET_FLOW_CTRL = 0x02;
const SIO_SET_BAUD_RATE = 0x03;
const SIO_SET_DATA = 0x04;
const FTDI_DTR_RTS_ON = 0x0303;		// high byte: which lines to change, low byte: their levels
const FTDI_DTR_RTS_OFF = 0x0300;
const FTDI_8N1 = 0x0008;

const READ_SIZE = 4096;			// bytes asked per bulk IN transfer
const READS_IN_FLIGHT = 4;		// bulk IN transfers kept queued on the device
const READ_AHEAD = 64;			// chunks read ahead of a page that is slow to take them
const WRITE_STALLS = 3;			// bulk OUT stalls cleared and retried before a write fails

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
	// CDC-ACM: a communication interface (class 2, subclass 2) and the data interface (class 10) with both bulk ends.
	// WebUSB doesn't show the union descriptor that pairs them; the data interface right after the communication one
	// (as an interface association groups them) comes first, so a composite device's other data interface isn't taken
	const comm = ifaces.find((i) => i.alternates.some((a) => a.interfaceClass === 2 && a.interfaceSubclass === 2));
	const next = comm && ifaces.find((i) => i.interfaceNumber === comm.interfaceNumber + 1);
	for (const i of next ? [next, ...ifaces.filter((x) => x !== next)] : ifaces) {
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
		this.isOpen = false;
		this.held = false;	// counted in openPorts (the wake lock)
	}

	getInfo() { return { usbVendorId: this.device.vendorId, usbProductId: this.device.productId }; }

	// As Web Serial: open() on an open port is an InvalidStateError (SerialStream.open closes it and opens again). A
	// failure part way leaves the device closed, so trying again starts clean.
	async open({ baudRate = 115200 } = {}) {
		if (this.isOpen) throw new DOMException("The port is already open.", "InvalidStateError");
		const d = this.device;
		let l = null;
		try {
			if (!d.opened) await d.open();
			if (!d.configuration) await d.selectConfiguration(1);
			l = serialLayout(d);
			if (!l) throw new DOMException("Not a serial device this page can drive.", "NotSupportedError");
			// Android's kernel driver (cdc_acm) holds the interfaces: claiming takes them over
			for (const n of l.claim) await d.claimInterface(n);
			if (l.alt) await d.selectAlternateInterface(l.data, l.alt);
			if (l.kind === "ftdi") await this.ftdiSetup(baudRate);
			else await this.acmSetup(l, baudRate);
		} catch (e) {
			for (const n of l?.claim || []) { try { await d.releaseInterface(n); } catch { /* not claimed */ } }
			try { await d.close(); } catch { /* gone */ }
			throw e;
		}
		this.layout = l;
		this.closing = false;
		this.isOpen = true;
		this.readable = this.makeReadable(l);
		this.writable = new WritableStream({ write: (chunk) => this.send(l, chunk) });
		this.held = true;
		portOpened();
	}

	// A bulk OUT transfer can end short or stall; the rest is sent again, never skipped: a dropped byte in a codeplug or
	// firmware write is a corrupt radio
	async send(l, chunk) {
		const d = this.device;
		let data = chunk instanceof Uint8Array ? chunk : new Uint8Array(chunk.buffer ?? chunk, chunk.byteOffset ?? 0, chunk.byteLength);
		let stalls = 0;
		while (data.length) {
			const r = await d.transferOut(l.out.endpointNumber, data);
			const n = r.bytesWritten ?? (r.status === "ok" ? data.length : 0);
			data = data.subarray(n);
			if (r.status === "stall" && stalls++ < WRITE_STALLS) { await d.clearHalt("out", l.out.endpointNumber); continue; }
			if (r.status !== "ok") throw new DOMException(`USB write ${r.status}`, "NetworkError");
			if (!n && data.length) throw new DOMException("USB write sent nothing", "NetworkError");
		}
	}

	acmSetup(l, baud) {
		return this.acmLines(l, DTR_RTS, lineCoding(baud));
	}

	async acmLines(l, lines, coding = null) {
		const setup = (request, value) => ({ requestType: "class", recipient: "interface", request, value, index: l.control });
		if (coding) await this.device.controlTransferOut(setup(SET_LINE_CODING, 0), coding);
		await this.device.controlTransferOut(setup(SET_CONTROL_LINE_STATE, lines));
	}

	ftdiOut(request, value, index = 0) {
		return this.device.controlTransferOut({ requestType: "vendor", recipient: "device", request, value, index });
	}

	ftdiModem(lines) { return this.ftdiOut(SIO_MODEM_CTRL, lines); }

	async ftdiSetup(baud) {
		const div = ftdiDivisor(baud);
		await this.ftdiOut(SIO_RESET, 0);
		await this.ftdiOut(SIO_SET_BAUD_RATE, div.value, div.index);
		await this.ftdiOut(SIO_SET_DATA, FTDI_8N1);
		await this.ftdiOut(SIO_SET_FLOW_CTRL, 0);
		await this.ftdiModem(FTDI_DTR_RTS_ON);
	}

	// Bulk IN transfers stay queued on the device whether or not the page is reading: an FTDI chip keeps 256 bytes,
	// 22 ms at 115200 baud, and drops the rest when no transfer is pending (frame checksum errors in a backup over a cable
	// from a phone whose tab went to the background, 2026-11-01). Transfers on one endpoint complete in order. An empty
	// result (an FTDI status-only packet) is skipped; a stall is cleared and read on, anything else ends the stream.
	makeReadable(l) {
		const d = this.device;
		const pending = [];
		const submit = () => pending.push(d.transferIn(l.in.endpointNumber, READ_SIZE).then((r) => ({ r }), (e) => ({ e })));
		const stream = new ReadableStream({
			start: () => { for (let i = 0; i < READS_IN_FLIGHT; i++) submit(); },
			pull: async (ctl) => {
				for (;;) {
					const { r, e } = await pending.shift();
					if (e) {
						if (this.closing) return ctl.close();
						// as Web Serial after a fatal error: no readable, so a reader loop ends instead of reading on
						if (this.readable === stream) this.readable = null;
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
		return stream;
	}

	// As Web Serial: a port whose readable or writable is locked (a reader or writer still holds it) refuses to close,
	// so another SerialStream reading it isn't cut off. DTR and RTS drop as when an OS driver closes the port: the
	// radio sees the host go. Releasing the interfaces ends a pending transferIn, which closes the readable (closing
	// set first).
	async close() {
		if (this.readable?.locked || this.writable?.locked) throw new DOMException("A stream of the port is locked.", "InvalidStateError");
		if (!this.isOpen) return;
		const l = this.layout, d = this.device;
		this.closing = true;
		this.isOpen = false;
		this.readable = null;
		this.writable = null;
		if (this.held) { this.held = false; portClosed(); }
		try {
			if (l.kind === "ftdi") await this.ftdiModem(FTDI_DTR_RTS_OFF);
			else await this.acmLines(l, LINES_OFF);
		} catch { /* gone */ }
		for (const n of l.claim) { try { await d.releaseInterface(n); } catch { /* gone */ } }
		try { await d.close(); } catch { /* gone */ }
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
