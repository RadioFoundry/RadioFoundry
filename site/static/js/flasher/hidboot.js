// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The SGL update file (.sgl) and the vendor bootloader's USB HID update protocol (docs/spec/update-gd77.md). A port of
// tools/rfoundry/rfoundry/updatefile.py and hidboot.py; site/tests/hidboot.test.mjs checks the two agree byte for byte.
//
// An update file is a 1 KB header, then the flash image and one zero byte, byte-scrambled. A new image keeps the header
// of the file it is written into (the donor, a stock update file the user supplies): same names, same tag, same stream.
// The donor's own tail, zero before scrambling, gives the stream, so no key is needed.
//
// Unverified on a radio: that the loader accepts a body written this way, its timing and its HID enumeration. Every
// loader behavior here is as the spec reads it, and the tests run it against a model loader (fakehid.js).

import { ascii, startsWith, u16le, u32le, sleep as wait } from "./bytes.js";

export const PERIOD = 32767;
export const INFO_LEN = 103;
export const BODY_AT = 0x400;
export const FLASH_BASE = 0x4000;		// flat image offset 0 is this flash address
export const VID = 0x15A2, PID = 0x0073;
export const REPORT = 42;			// report id + 41 data bytes
export const STEP_TIMEOUT = 2000;		// ms
export const ERASE_TIMEOUT = 10000;
const KEY = [0x53, 0x47, 0x4C, 0x21];		// "SGL!"

// code: a stable name for the page's message (hidui.js); message: what rfoundry says
export class UpdateFileError extends Error {
	constructor(code, message) {
		super(message);
		this.code = code;
	}
}

export class BootError extends Error {
	constructor(code, message) {
		super(message);
		this.code = code;
	}
}

const ror3 = (b) => ((b >> 3) | (b << 5)) & 255;
const rol3 = (b) => ((b << 3) | (b >> 5)) & 255;
const mod = (x) => ((x % PERIOD) + PERIOD) % PERIOD;
const bytesOf = (s) => Uint8Array.from(s, (c) => c.charCodeAt(0));
const printable = (b) => [...b].filter((c) => c >= 32 && c < 127).map((c) => String.fromCharCode(c)).join("");

function sum(bytes) {
	let s = 0;
	for (let i = 0; i < bytes.length; i++) s += bytes[i];
	return s;
}

export class UpdateFile {
	constructor(data) {
		const d = (this.data = data instanceof Uint8Array ? data : new Uint8Array(data));
		if (d.length < BODY_AT + 64 || !startsWith(d, "SGL!"))
			throw new UpdateFileError("not_sgl", 'not an SGL update file (no "SGL!" header)');
		const h = d.slice(0, 16);
		for (let i = 4; i < 16; i++) h[i] ^= KEY[i % 4];
		if (ascii(h.subarray(4, 11)) !== "ENCV001")
			throw new UpdateFileError("version", `an update file of another version (header ${JSON.stringify(ascii(h.subarray(4, 11)))})`);
		this.pad = h[11];
		this.infoAt = h[12] | (h[13] << 8);
		this.k0 = h[14];
		this.k1 = h[15];
		this.bodyAt = BODY_AT + this.pad;
		if (this.infoAt + INFO_LEN > BODY_AT || this.bodyAt >= d.length)
			throw new UpdateFileError("damaged", "damaged update file (header fields out of range)");
		this.info = new Uint8Array(INFO_LEN);
		for (let i = 0; i < INFO_LEN; i++) this.info[i] = d[this.infoAt + i] ^ (i % 2 === 0 ? this.k0 : this.k1);
		this.body = d.subarray(this.bodyAt);
		if (u32le(this.info, 10) !== ~sum(this.body) >>> 0)
			throw new UpdateFileError("sum", "the file's sum check does not match its contents");
	}

	get start() { return u32le(this.info, 2); }
	get length() { return u32le(this.info, 6); }	// bytes the loader writes, the zero byte included
	get company() { return this.info.slice(50, 66); }
	get model() { return this.info.slice(66, 74); }
	get version() { return this.info.slice(74, 82); }
	get tag() { return this.info.slice(95, 103); }
	get addrWidth() { return this.info[91] >> 4; }
	get lenWidth() { return (this.info[92] >> 3) & 3; }
	get blockSize() { return u16le(this.info, 93); }
	get useFprog() { return !!(this.info[91] & 1); }
	get sendTag() { return !!(this.info[92] & 0x20); }

	// Bytes of flash image this file holds; the last 32 body bytes are zero (they show the stream), so an image longer
	// than room - 32 must end in zeros
	get room() { return this.length - 1 - this.start; }

	label() {
		return `${printable(this.company)} ${printable(this.model)} ${printable(this.version)}`;
	}
}

export function forward(seed, n) {
	const out = new Uint8Array(n);
	out.set(seed.subarray(0, Math.min(seed.length, n)));
	for (let j = seed.length; j < n; j++) out[j] = out[j - 1] ^ out[j - 8] ^ out[j - 9] ^ out[j - 15] ^ out[j - 16];
	return out;
}

// {anchor, k}: the scrambling bytes, from the file's own zero tail
function stream(u) {
	const tail = u.data.subarray(u.data.length - 32);
	const f = forward(tail.subarray(0, 16), 32);
	for (let i = 16; i < 32; i++)
		if (f[i] !== tail[i]) throw new UpdateFileError("not_donor", "the end of this file is not a clean byte stream: it cannot be used as a donor");
	return { anchor: u.data.length - 32, k: forward(tail.subarray(0, 16), PERIOD) };
}

const asFile = (x) => (x instanceof UpdateFile ? x : new UpdateFile(x));

// The flat flash image (flash address = offset + FLASH_BASE) of an update file
export function unpack(data) {
	const u = asFile(data);
	const { anchor, k } = stream(u);
	const d = u.data;
	const out = new Uint8Array(d.length - u.bodyAt);
	for (let i = u.bodyAt; i < d.length; i++) out[i - u.bodyAt] = rol3(d[i] ^ k[mod(i - anchor)]);
	return out.length && out[out.length - 1] === 0 ? out.subarray(0, out.length - 1) : out;
}

// An update file with `image` (flat, from FLASH_BASE) as its body, in the header of `donor`
export function pack(image, donor) {
	const u = asFile(donor);
	const { anchor, k } = stream(u);
	const total = u.data.length - u.bodyAt;
	if (image.length > total - 1)
		throw new UpdateFileError("too_large", `the image is ${image.length} bytes, the donor file has room for ${total - 1}`);
	for (let i = total - 32; i < image.length; i++)
		if (image[i]) throw new UpdateFileError("tail", "the image reaches into the last 32 bytes of the donor file, which must stay zero");
	const plain = new Uint8Array(total);
	plain.fill(0xFF, image.length, Math.max(image.length, total - 32));
	plain.set(image);
	const body = new Uint8Array(total);
	for (let j = 0; j < total; j++) body[j] = ror3(plain[j]) ^ k[mod(u.bodyAt + j - anchor)];
	const info = u.info.slice();
	new DataView(info.buffer).setUint32(10, ~sum(body) >>> 0, true);
	const out = new Uint8Array(u.data.length);
	out.set(u.data.subarray(0, u.bodyAt));
	for (let i = 0; i < INFO_LEN; i++) out[u.infoAt + i] = info[i] ^ (i % 2 === 0 ? u.k0 : u.k1);
	out.set(body, u.bodyAt);
	return out;
}

// ---------------------------------------------------------------------------------------------------------------
// the loader's protocol

export function frame(payload) {
	if (payload.length > REPORT - 4) throw new Error("payload longer than one report");
	const out = new Uint8Array(REPORT);
	out[0] = 1;
	out[2] = payload.length & 255;
	out[3] = payload.length >> 8;
	out.set(payload, 4);
	return out;
}

export function parse(report) {
	if (report.length < 4) throw new BootError("short", "short report from the radio");
	const n = report[2] | (report[3] << 8);
	return report.slice(4, 4 + n);
}

const be = (n, width) => {
	const out = new Uint8Array(width);
	for (let i = width - 1; i >= 0; i--, n = Math.floor(n / 256)) out[i] = n & 255;
	return out;
};
const show = (b) => JSON.stringify(printable(b) || [...b].map((c) => c.toString(16).padStart(2, "0")).join(" "));
const same = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
const A = bytesOf("A");

// link: {write(report), read(timeoutMs) -> report or null}, as HidLink or the model loader. sleep(ms) is injectable for
// the tests.
export class Updater {
	constructor(link, { sleep = wait } = {}) {
		this.link = link;
		this.sleep = sleep;
	}

	async xfer(payload, timeout = STEP_TIMEOUT) {
		await this.link.write(frame(payload));
		const r = await this.link.read(timeout);
		if (!r) throw new BootError("no_answer", `the radio did not answer within ${Math.round(timeout / 1000)} s`);
		return parse(r);
	}

	async step(payload, what, timeout = STEP_TIMEOUT, code = "nak") {
		const r = await this.xfer(payload, timeout);
		if (r[0] !== 0x41) throw Object.assign(new BootError(code, `${what}: the radio answered ${show(r)}, not an acknowledgement`), { what });
	}

	// Send the update file u (UpdateFile) to the radio. body: the scrambled body to send (default u.body).
	// progress(done, total) after every block.
	async flash(u, { body = u.body, progress = () => {} } = {}) {
		let ready = false;
		for (let i = 0; i < 5 && !ready; i++) {
			const r = await this.xfer(bytesOf("DOWNLOAD"));
			ready = ["#UPDATE?", "#UPDATA?"].includes(ascii(r.subarray(0, 8)));
			if (!ready) await this.sleep(200);
		}
		if (!ready) throw new BootError("no_update_mode", "the radio is not in update mode (it did not answer DOWNLOAD)");
		await this.step(A, "start");
		if (u.sendTag) {
			const r = await this.xfer(u.tag);
			if (!same(r.subarray(0, 8), u.tag)) throw new BootError("tag", `the radio did not echo the file's tag (${show(r)})`);
		}
		if (u.useFprog) {
			await this.sleep(5);
			await this.step(bytesOf("F-PROG\xff\xff"), "program mode");
		}
		for (const [part, what] of [[u.company, "company"], [u.model, "model"], [u.version, "version"]]) {
			await this.sleep(5);
			await this.step(part, `the file's ${what} was refused`, STEP_TIMEOUT, "refused");
		}
		await this.sleep(100);
		await this.step(bytesOf(u.useFprog ? "F-ERASE\xff" : "ERASEROM"), "erase", ERASE_TIMEOUT);
		await this.sleep(5);
		await this.step(A, "erase done");
		await this.sleep(5);
		await this.step(bytesOf("PROGRAM\xff"), "program");
		const total = u.length, bs = u.blockSize;
		let addr = u.start, since = 0, acc = 0;
		while (addr < total) {
			const n = Math.min(bs - (addr % bs), total - addr);
			const data = body.subarray(addr, addr + n);
			const p = new Uint8Array(u.addrWidth + u.lenWidth + n);
			p.set(be(addr, u.addrWidth));
			p.set(be(n, u.lenWidth), u.addrWidth);
			p.set(data, u.addrWidth + u.lenWidth);
			await this.step(p, `block at 0x${addr.toString(16).toUpperCase()}`);
			acc = (acc + sum(data)) >>> 0;
			addr += n;
			since += n;
			progress(addr, total);
			if (since % 1024 === 0 || addr >= total) {
				const end = new Uint8Array(8);
				end.set(bytesOf("END\xff"));
				new DataView(end.buffer).setUint32(4, acc, true);
				await this.step(end, `checksum after 0x${addr.toString(16).toUpperCase()}`);
				acc = 0;
			}
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------
// WebHID (Chrome, Edge and other Chromium browsers on a computer)

export const hidSupported = () => !!globalThis.navigator?.hid;
export const HID_FILTERS = [{ vendorId: VID, productId: PID }];

// The radio in update mode over WebHID: write(report) sends report[0] as the report id and the rest as its data;
// read(timeoutMs) gives the next input report with its id in front, or null.
export class HidLink {
	// hid: navigator.hid, or a model of it (fakehid.js). Asks the user to choose the radio.
	static async request(hid = globalThis.navigator?.hid) {
		if (!hid) throw new BootError("no_hid", "this browser has no WebHID");
		const [dev] = await hid.requestDevice({ filters: HID_FILTERS });
		if (!dev) throw new BootError("no_device", "no radio in update mode was chosen");
		if (!dev.opened) await dev.open();
		return new HidLink(dev);
	}

	constructor(dev) {
		this.dev = dev;
		this.q = [];
		this.waiter = null;
		this.onReport = (e) => {
			const r = new Uint8Array(1 + e.data.byteLength);
			r[0] = e.reportId;
			r.set(new Uint8Array(e.data.buffer, e.data.byteOffset, e.data.byteLength), 1);
			this.q.push(r);
			this.waiter?.();
		};
		dev.addEventListener("inputreport", this.onReport);
	}

	async write(report) {
		await this.dev.sendReport(report[0], report.slice(1));
	}

	read(timeoutMs) {
		if (this.q.length) return Promise.resolve(this.q.shift());
		return new Promise((done) => {
			const timer = setTimeout(() => { this.waiter = null; done(null); }, timeoutMs);
			this.waiter = () => {
				clearTimeout(timer);
				this.waiter = null;
				done(this.q.shift());
			};
		});
	}

	async close() {
		this.dev.removeEventListener("inputreport", this.onReport);
		try { await this.dev.close(); } catch (e) { /* gone */ }
	}
}
