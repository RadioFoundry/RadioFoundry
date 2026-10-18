// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The Radioddity DB25-D PRO's two stock-side protocols, for a radio that does not run ODR yet (docs/spec/hardware-db25d.md
// 7). Both run on the K1 programming cable (UART1, 115200 8N1):
//
//   USMART   read the whole program flash of a radio running the stock firmware, through the debug console the stock
//            application carries. Read only.
//   IAP      write an install image (ODR's first stage and application, plus the codec slot) through the vendor IAP
//            loader the radio enters with P1 held at power-on. The image is the first stage followed by ODR.
//
// Once ODR runs, the radio is reached the way the other HR_C7000 radios are (stub.js, c7000.js).

import { be32, concat, sleep } from "./bytes.js";

export class DB25DError extends Error {}

export const FLASH_BASE = 0x03000000, FLASH_SIZE = 0x200000;
export const APP_HEAD = [0x0300C000, 0x0300D000];	// the application's first 4 KB: read first, twice

// ---------------------------------------------------------------------------------------------------------------
// USMART: the stock firmware treats a '@' byte as "the next line is a console command"; read_addr(addr) returns one
// 32-bit word of memory, and the program flash is memory-mapped at 0x03000000. One word per round trip.

const LINE = /^read_addr\(0x[0-9a-f]{8}\)$/;
const REPLY = /read_addr\(\s*(0[xX][0-9A-Fa-f]+|\d+)\s*\)\s*=\s*(0[xX][0-9A-Fa-f]+|\d+)\s*;/g;

// one char per byte (TextDecoder("latin1") is windows-1252 and would change the checksum bytes)
const latin = (b) => {
	let s = "";
	for (let i = 0; i < b.length; i++) s += String.fromCharCode(b[i]);
	return s;
};

export const hex8 = (n) => (n >>> 0).toString(16).padStart(8, "0");

export class Usmart {
	constructor(stream, { log = () => {} } = {}) {
		this.s = stream;
		this.log = log;
	}

	// The only things this class can send: '@' and read_addr lines. Anything else is refused before the port.
	async send(line) {
		if (!LINE.test(line)) throw new DB25DError(`refused to send ${JSON.stringify(line)}`);
		await this.s.write(new TextEncoder().encode(`@${line}\r\n`));
	}

	// One read_addr call: the 32-bit value, or null when no matching reply came in time
	async readWord(addr, timeout = 1000) {
		this.s.clear();
		await this.send(`read_addr(0x${hex8(addr)})`);
		const until = Date.now() + timeout;
		while (Date.now() < until) {
			const text = latin(this.s.buf);
			if (text.endsWith("\n")) {
				for (const m of text.matchAll(REPLY)) {
					if (Number(m[1]) === addr) return Number(m[2]) >>> 0;
				}
			}
			await sleep(1);
		}
		return null;
	}

	async readWordRetry(addr, tries = 6, signal) {
		for (let i = 0; i < tries; i++) {
			if (signal?.aborted) throw new DB25DError("stopped");
			const v = await this.readWord(addr, 1000 + i * 1000);
			if (v !== null) return v;
			await sleep(200);
		}
		throw new DB25DError(`no answer for 0x${hex8(addr)} after ${tries} tries. Run the read again to continue where it stopped`);
	}

	// Is a radio with the stock firmware on the cable? true when the console answers a read of the application's first word
	async probe(timeout = 1500) {
		return (await this.readWord(APP_HEAD[0], timeout)) !== null;
	}
}

// Word addresses in reading order: the application's first 4 KB, then the rest ascending
export function usmartOrder(limit = FLASH_SIZE) {
	const order = [];
	for (let a = APP_HEAD[0]; a < APP_HEAD[1]; a += 4) order.push(a);
	for (let a = FLASH_BASE; a < FLASH_BASE + limit; a += 4) if (a < APP_HEAD[0] || a >= APP_HEAD[1]) order.push(a);
	return order;
}

// Read the program flash (first `limit` bytes of it). state: {done, image} from an earlier, stopped run (words done in
// reading order, and the image so far), or null. onProgress(done, total), onCheckpoint(done, image) now and then, so a
// stopped read can resume. The first 4 KB are read twice and must agree. Returns the image (limit bytes).
export async function usmartDump(con, { limit = FLASH_SIZE, state = null, onProgress = () => {}, onCheckpoint = () => {}, signal, log = () => {} } = {}) {
	const order = usmartOrder(limit);
	const image = state?.image && state.image.length >= limit ? state.image : new Uint8Array(limit).fill(0xFF);
	const view = new DataView(image.buffer, image.byteOffset, image.byteLength);
	let done = state?.done || 0;
	if (done) log(`continuing at word ${done} of ${order.length}`);
	try {
		for (let i = done; i < order.length; i++) {
			if (signal?.aborted) throw new DB25DError("stopped");
			const addr = order[i];
			let v = await con.readWordRetry(addr, 6, signal);
			if (i < 1024) {
				let v2 = await con.readWordRetry(addr, 6, signal);
				let again = 0;
				while (v2 !== v) {
					if (++again > 20) throw new DB25DError(`0x${hex8(addr)} reads differently every time`);
					[v, v2] = [v2, await con.readWordRetry(addr, 6, signal)];
				}
			}
			view.setUint32(addr - FLASH_BASE, v, true);
			done = i + 1;
			if (done % 64 === 0 || done === order.length) onProgress(done, order.length);
			if (done % 4096 === 0) onCheckpoint(done, image);
		}
	} finally {
		onCheckpoint(done, image);
	}
	onProgress(done, order.length);
	return image;
}

// ---------------------------------------------------------------------------------------------------------------
// IAP (notes: db25d-iap-protocol.md):
//   banner       the loader prints its state at entry, ending "IAPReady"
//   Erase        "Erase      " + u32 BE address (0) + u32 BE N, N = length / 2048 (integer division)
//                the loader answers its version string at once and "Erase ok" about 1.5 s later
//   pages        N + 1 raw 2048-byte pages (the last one padded with 0xFF), each answered "kyd"
//   ChecksumI    + u32 BE additive sum of every byte sent, about 1.4 s after the last page; the loader then starts
//                the application at once

export const PAGE = 2048;
export const APP_MAX = 0x0310C000 - 0x0300C000;	// the application region ends where the stock data area starts

// The install image as pages: {n, pages, sum}. The pad byte fills the last page.
export function iapPlan(image, pad = 0xFF) {
	if (!image.length || image.length > APP_MAX) throw new DB25DError(`the image is ${image.length} bytes; it must be 1 to ${APP_MAX}`);
	const n = Math.floor(image.length / PAGE);
	const pages = [];
	let sum = 0;
	for (let i = 0; i <= n; i++) {
		const p = new Uint8Array(PAGE).fill(pad);
		p.set(image.subarray(i * PAGE, (i + 1) * PAGE));
		for (const b of p) sum = (sum + b) >>> 0;
		pages.push(p);
	}
	return { n, pages, sum };
}

export const erasePacket = (n) => concat(new TextEncoder().encode("Erase      "), be32(0), be32(n));

// What the loader takes: the first stage (exactly 4096 bytes, starting 2C 11 01 C0 so the loader boots it at a normal
// power-on) and ODR's application (starting with its reset handler, psrclr ie), as the image builder checks
export function checkInstallImage(image) {
	if (image.length <= 0x1000) throw new DB25DError("this file holds only the first stage; the install image is the first stage and the application");
	const head = [0x2C, 0x11, 0x01, 0xC0];
	if (!head.every((b, i) => image[i] === b)) throw new DB25DError("this is not a DB25-D install image (it does not start with the first stage's 2C 11 01 C0)");
	const app = [0x80, 0xC0, 0x20, 0x70];
	if (!app.every((b, i) => image[0x1000 + i] === b)) throw new DB25DError("the application after the first stage does not start with ODR's reset handler");
}

// The loader writes as far as the image reaches, so the 0xFF tail target() pads on is dropped (rounded up to whole words)
export function trimTail(data) {
	let end = data.length;
	while (end > PAGE && data[end - 1] === 0xFF) end--;
	return data.subarray(0, Math.min(data.length, (end + 3) & ~3));
}

const isBanner = (t) => t.includes("IAPReady");

export class Iap {
	constructor(stream, { log = () => {} } = {}) {
		this.s = stream;
		this.log = log;
	}

	text() { return latin(this.s.buf); }

	// Everything received until done(text) holds and `quiet` ms pass with nothing more, or `timeout` ms in all
	async until(done, timeout, quiet = 150) {
		const t0 = Date.now();
		let last = Date.now(), n = this.s.buf.length;
		for (;;) {
			const now = Date.now();
			if (this.s.buf.length !== n) {
				n = this.s.buf.length;
				last = now;
			} else if ((done(this.text()) && now - last > quiet) || now - t0 > timeout) return this.text();
			await sleep(2);
		}
	}

	// Listen for the banner the loader prints when it starts in update mode
	async waitBanner(timeout, { signal } = {}) {
		this.s.clear();
		const until = Date.now() + timeout;
		while (Date.now() < until) {
			if (signal?.aborted) throw new DB25DError("stopped");
			if (isBanner(this.text())) {
				await this.until(() => false, 400);	// the rest of the banner
				this.s.clear();
				return;
			}
			if (this.s.buf.length > 4096) this.s.take(this.s.buf.length - 64);
			await sleep(20);
		}
		throw new DB25DError("the radio did not report update mode");
	}

	// Erase and write `image`. onPage(i, total). Returns the checksum the radio reported (it matches ours or this throws).
	async write(image, { onPage = () => {}, onPhase = () => {}, signal, eraseTimeout = 60000, pageTimeout = 10000 } = {}) {
		checkInstallImage(image);
		const { n, pages, sum } = iapPlan(image);
		onPhase("erase");
		this.s.clear();
		await this.s.write(erasePacket(n));
		const er = await this.until((t) => t.includes("ok"), eraseTimeout);
		if (!er.includes("ok")) throw new DB25DError("the radio did not confirm the erase. The application area may be erased: enter update mode again and repeat the install");
		onPhase("write");
		let sent = 0, reply = "";
		for (let i = 0; i < pages.length; i++) {
			if (signal?.aborted) throw new DB25DError("stopped");
			this.s.clear();
			await this.s.write(pages[i]);
			for (const b of pages[i]) sent = (sent + b) >>> 0;
			const last = i === pages.length - 1;
			// after the last page wait for the checksum itself: the new application starts right after it and may talk
			reply = await this.until(last ? (t) => /ChecksumI[\s\S]{4}/.test(t) : (t) => (t.includes("Write") || t.includes("d")) && !t.includes("ok"),
				pageTimeout, 50);
			onPage(i + 1, pages.length);
			if (last) break;
			if (!(reply.includes("Write") || reply.includes("d")) || reply.includes("ok"))
				throw new DB25DError(`page ${i}: unexpected reply, stopped. Enter update mode again and repeat the install`);
		}
		const k = reply.indexOf("ChecksumI");
		if (k < 0 || reply.length < k + 13) throw new DB25DError("the radio did not report a checksum after the last page");
		const theirs = ((reply.charCodeAt(k + 9) << 24) | (reply.charCodeAt(k + 10) << 16) | (reply.charCodeAt(k + 11) << 8) | reply.charCodeAt(k + 12)) >>> 0;
		if (theirs !== sent) throw new DB25DError(`the radio's checksum 0x${hex8(theirs)} does not match the file's 0x${hex8(sent)}: install again`);
		if (sent !== sum) throw new DB25DError("internal checksum error");
		return theirs;
	}
}
