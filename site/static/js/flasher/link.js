// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Talking to a radio that runs ODR, over its USB serial port (WebSerial): the debug link (docs/spec/debuglink.md:
// VERSION, INFO_CHIP, READ_MEM) and the open-firmware CPS reads (docs/spec/open-firmware-cps.md 2.1 and 3: area 1 the
// SPI flash, 5 the MCU flash, 9 the radio info, 10 the flash security registers). Read only, apart from the CPS
// codeplug writes restore uses (X 1-3), which the radio limits to the codeplug areas.

import { be32, concat, le32, sleep, u16be, u16le, u32le, toHex } from "./bytes.js";

export const ODR_USB = { usbVendorId: 0x1FC9, usbProductId: 0x0094 };
// The same radio as a USB network adapter (CDC-NCM, docs/spec/usb-network.md 1): no serial port, and the OS network
// driver owns the interfaces, so a page can only see that it is there (WebUSB lists it without claiming it)
export const ODR_NET = { vendorId: 0x1FC9, productId: 0x0096 };
const SOF1 = 0xA5, SOF2 = 0x5A;
export const CMD = { PING: 0x01, VERSION: 0x02, INFO_CHIP: 0x0B, READ_MEM: 0x0C, STORAGE: 0x0F };

export class LinkError extends Error {}

export const serialSupported = () => typeof navigator !== "undefined" && "serial" in navigator;

// CRC-16/CCITT-FALSE
export function crc16(data, crc = 0xFFFF) {
	for (const b of data) {
		crc ^= b << 8;
		for (let i = 0; i < 8; i++) crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xFFFF : (crc << 1) & 0xFFFF;
	}
	return crc;
}

export function frame(cmd, seq, payload = new Uint8Array(0)) {
	const body = concat(new Uint8Array([cmd, seq, payload.length & 0xFF, payload.length >> 8]), payload);
	const c = crc16(body);
	return concat(new Uint8Array([SOF1, SOF2]), body, new Uint8Array([c & 0xFF, c >> 8]));
}

// [{cmd, seq, payload, end}] for each complete, CRC-valid frame in buf
export function parseFrames(buf) {
	const out = [];
	let i = 0;
	while (i + 8 <= buf.length) {
		if (buf[i] !== SOF1 || buf[i + 1] !== SOF2) { i++; continue; }
		const n = u16le(buf, i + 4);
		const e = i + 8 + n;
		if (e > buf.length) break;
		const body = buf.subarray(i + 2, i + 6 + n);
		if (u16le(buf, i + 6 + n) === crc16(body)) {
			out.push({ cmd: buf[i + 2], seq: buf[i + 3], payload: buf.slice(i + 6, i + 6 + n), end: e });
			i = e;
		} else i++;
	}
	return out;
}

export function parseKv(text) {
	const out = {};
	for (const line of text.split("\n")) {
		const k = line.indexOf("=");
		if (k > 0) out[line.slice(0, k).trim()] = line.slice(k + 1).trim();
	}
	return out;
}

// A byte stream with a receive buffer: WebSerial in the browser, fake.js in tests and the simulator.
export class SerialStream {
	constructor(port) {
		this.port = port;
		this.buf = new Uint8Array(0);
		this.closed = false;
	}

	static async request() {
		const port = await navigator.serial.requestPort({ filters: [ODR_USB] });
		return SerialStream.open(port);
	}

	// Any serial port (USB-serial programming cables on radios without USB)
	static async requestAny() {
		return SerialStream.open(await navigator.serial.requestPort());
	}

	// Chrome's default 255 byte buffer stops the USB reads whenever the page is busy for a moment; the radio gives up
	// on a reply the host doesn't take for a while (drivers/usb/usb_device.c), so a 2 KB read could lose its tail
	static async open(port, baudRate = 115200) {
		await port.open({ baudRate, bufferSize: 65536 });
		const s = new SerialStream(port);
		s.pump();
		return s;
	}

	async pump() {
		while (!this.closed && this.port.readable) {
			this.reader = this.port.readable.getReader();
			try {
				for (;;) {
					const { value, done } = await this.reader.read();
					if (done) break;
					this.buf = concat(this.buf, value);
				}
			} catch (e) {
				break;
			} finally {
				this.reader.releaseLock();
			}
		}
	}

	async write(data) {
		const w = this.port.writable.getWriter();
		try { await w.write(data); } finally { w.releaseLock(); }
	}

	clear() { this.buf = new Uint8Array(0); }

	take(n) {
		const out = this.buf.slice(0, n);
		this.buf = this.buf.slice(n);
		return out;
	}

	async close() {
		this.closed = true;
		try { await this.reader?.cancel(); } catch (e) { /* gone */ }
		try { await this.port.close(); } catch (e) { /* gone */ }
	}
}

export class Link {
	constructor(stream, { timeout = 1500 } = {}) {
		this.s = stream;
		this.timeout = timeout;
		this.seq = 0x40;
	}

	async waitFor(test, timeout = this.timeout) {
		const until = Date.now() + timeout;
		for (;;) {
			const r = test(this.s.buf);
			if (r !== undefined) return r;
			if (Date.now() > until) return undefined;
			await sleep(2);
		}
	}

	// ---- debug link

	async request(cmd, payload = new Uint8Array(0)) {
		this.seq = (this.seq + 1) & 0xFF;
		const seq = this.seq;
		this.s.clear();
		await this.s.write(frame(cmd, seq, payload));
		const reply = await this.waitFor((buf) => parseFrames(buf).find((f) => f.seq === seq && (f.cmd === (cmd | 0x80) || f.cmd === 0xFF)));
		if (!reply) throw new LinkError(`no answer to command 0x${cmd.toString(16)}`);
		this.s.take(reply.end);
		if (!reply.payload.length || reply.payload[0] !== 0) throw new LinkError(`command 0x${cmd.toString(16)}: status ${reply.payload[0]}`);
		return reply.payload.subarray(1);
	}

	async version() {
		return parseKv(new TextDecoder().decode(await this.request(CMD.VERSION)));
	}

	// INFO_CHIP (debuglink.md 3.9), offsets after the status byte
	async chip() {
		const r = await this.request(CMD.INFO_CHIP);
		if (r.length < 28) return null;
		return { idcode: u32le(r, 0), flashKb: u16le(r, 4), uid: toHex(r.subarray(6, 18)), optcr: u32le(r, 18),
			rdp: r[22], wrp: u16le(r, 24), bor: r[26], user: r[27] };
	}

	// READ_MEM (debuglink.md 3.10) in 1000-byte pieces: internal flash, the stock bootloader included
	async readMem(addr, size) {
		const out = new Uint8Array(size);
		for (let done = 0; done < size;) {
			const n = Math.min(1000, size - done);
			const r = await this.request(CMD.READ_MEM, concat(le32(addr + done), new Uint8Array([n & 0xFF, n >> 8])));
			if (r.length !== 4 + n || u32le(r, 0) !== addr + done) throw new LinkError(`READ_MEM answer doesn't match 0x${(addr + done).toString(16)}`);
			out.set(r.subarray(4), done);
			done += n;
		}
		return out;
	}

	// ---- open-firmware CPS

	// R area addr len: the reply is "R" + length (BE) + data, or "-" when refused. A read changes nothing, so one
	// that went unanswered or came back short is asked again, after the rest of a broken reply has drained.
	async cpsRead(area, addr, len, tries = 3) {
		for (let i = 0; i < tries; i++) {
			if (i) await sleep(300);
			this.s.clear();
			await this.s.write(concat(new Uint8Array([0x52, area]), be32(addr), new Uint8Array([len >> 8, len & 0xFF])));
			const got = await this.waitFor((buf) => {
				if (buf.length && buf[0] === 0x2D) return null;
				if (buf.length >= 3 && buf[0] === 0x52 && buf.length >= 3 + u16be(buf, 1)) return buf.slice(3, 3 + u16be(buf, 1));
				return undefined;
			});
			this.s.clear();
			if (got !== undefined) return got;	// null: refused (out of range, not offered)
		}
		throw new LinkError(`no answer to a codeplug read at 0x${addr.toString(16)}`);
	}

	// Read [addr, addr + size) of an area in pieces; stops early (and says so) when the radio refuses
	async cpsDump(area, addr, size, { chunk = 2045, progress = () => {}, signal } = {}) {
		const out = new Uint8Array(size);
		let done = 0;
		while (done < size) {
			if (signal?.aborted) throw new LinkError("stopped");
			const n = Math.min(chunk, size - done);
			const part = await this.cpsRead(area, addr + done, n);
			if (part === null || part.length === 0) return { data: out.subarray(0, done), complete: false };
			out.set(part.subarray(0, n), done);
			done += Math.min(part.length, n);
			progress(done, size);
		}
		return { data: out, complete: true };
	}

	// X 1 (prepare sector), X 2 (merge data), X 3 (commit): one 4 KB sector of the SPI flash
	async cpsWriteSector(addr, data) {
		const sector = addr >>> 12;
		const ok = async (req) => {
			this.s.clear();
			await this.s.write(req);
			const r = await this.waitFor((buf) => (buf.length >= 1 && buf[0] === 0x2D ? false : buf.length >= 2 ? true : undefined), 3000);
			this.s.clear();
			return r === true;
		};
		if (!await ok(new Uint8Array([0x58, 1, sector >> 16, (sector >> 8) & 0xFF, sector & 0xFF]))) return false;
		for (let o = 0; o < data.length; o += 1024) {
			const part = data.subarray(o, o + 1024);
			if (!await ok(concat(new Uint8Array([0x58, 2]), be32(addr + o), new Uint8Array([part.length >> 8, part.length & 0xFF]), part))) return false;
		}
		return ok(new Uint8Array([0x58, 3]));
	}

	async cpsClose() {
		this.s.clear();
		await this.s.write(new Uint8Array([0x43, 5]));
		await this.waitFor((buf) => (buf.length ? true : undefined), 500);
		this.s.clear();
	}
}

// Ask a running ODR radio what it is. Returns null when nothing answers.
export async function probe(link) {
	try {
		const v = await link.version();
		let chip = null;
		try { chip = await link.chip(); } catch (e) { /* older firmware */ }
		return { board: v.board, git: v.git, codec: v.codec, features: v.features || "", display: v.display, version: v, chip };
	} catch (e) {
		return null;
	}
}
