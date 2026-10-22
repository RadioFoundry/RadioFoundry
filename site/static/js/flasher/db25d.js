// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The Radioddity DB25-D PRO's two stock-side protocols, for a radio that does not run ODR yet (docs/spec/hardware-db25d.md
// 7). Both run on the K1 programming cable (UART1, 115200 8N1):
//
//   USMART   the debug console the stock application carries. It reads and writes single words of memory, which is
//            enough to put the universal stub (stub.js) into a free stretch of RAM and start it. The stub then reads and
//            writes the program flash as on every other HR_C7000 radio. The console writes RAM only, never flash.
//   IAP      the vendor loader's update mode, entered with P1 held at power-on: writes an image to the application
//            area. The recovery route when neither the stock console, ODR nor the first stage's power-on window answers.
//
// Once ODR runs, the radio is reached the way the other HR_C7000 radios are (stub.js, c7000.js).

import { be32, concat, sleep } from "./bytes.js";
import { univFits } from "./stub.js";

export class DB25DError extends Error {}

export const FLASH_BASE = 0x03000000, FLASH_SIZE = 0x200000;

// ---------------------------------------------------------------------------------------------------------------
// USMART (hardware-db25d.md 7, Stub from stock). A command is one frame: "@AT", one filler byte, the text, CR LF. The
// console times every byte at 20 ms, so a frame goes out in a single write; anything unframed is ignored. Replies:
//   read_addr(0x0300c000)        -> "read_addr(0X300C000)=0XC001112C;"
//   write_addr(0x00054000,0x1)   -> "write_addr(0X54000,0X1);" (the function returns nothing; the echo is the reply)
// The stock application's own log text follows on the same line, so a reply is matched by its address.
//
// The stub goes into 0x54000-0x58000, free RAM above the heap in both known builds (018, 020). The console's command
// table lives in RAM at 0x18000840; its cmd_rec entry (+8) is called through the table for every ordinary frame, so
// pointing it at the stub makes the next frame start the stub.

const FRAME = "@AT ";
export const STUB_WINDOW = [0x54000, 0x4000];		// [load address, size]
export const HIJACK = 0x18000848;
export const APP_SIG = [0x0300C000, 0xC001112C];		// the application's first word: 2C 11 01 C0
// The version string "EARSAB.0NN" of each known build, and a word just below it: four words cover it
export const BUILDS = [["018", 0x03079F3C], ["020", 0x0307DCCC]];

// The stock application's version string ("909E.D6x.EARSAB.020") in a read of the program flash, wherever the build
// put it, or null. Only for messages: what a read holds is decided by its contents (c7000.js analyze)
export function stockVersion(progmem) {
	const app = progmem.subarray(APP_SIG[0] - FLASH_BASE, Math.min(progmem.length, APP_MAX + APP_SIG[0] - FLASH_BASE));
	const m = /909E\.[0-9A-Za-z]{2,4}\.EARSAB\.\d{3}/.exec(latin(app));
	return m ? m[0] : null;
}

const READ = /^read_addr\(0x[0-9a-f]{8}\)$/;
const WRITE = /^write_addr\(0x([0-9a-f]{8}),0x[0-9a-f]{8}\)$/;
const NUM = "(0[xX][0-9A-Fa-f]+|\\d+)";
const READ_REPLY = new RegExp(`read_addr\\(\\s*${NUM}\\s*\\)\\s*=\\s*${NUM}\\s*;`, "g");
const WRITE_REPLY = new RegExp(`write_addr\\(\\s*${NUM}\\s*,\\s*${NUM}\\s*\\)\\s*;`, "g");

// one char per byte (TextDecoder("latin1") is windows-1252 and would change the checksum bytes)
const latin = (b) => {
	let s = "";
	for (let i = 0; i < b.length; i++) s += String.fromCharCode(b[i]);
	return s;
};

export const hex8 = (n) => (n >>> 0).toString(16).padStart(8, "0");

// Writes may go to the stub window and to the one hijack word; nothing else, and never to flash
const writable = (a) => a === HIJACK || (a >= STUB_WINDOW[0] && a + 4 <= STUB_WINDOW[0] + STUB_WINDOW[1]);

export class Usmart {
	constructor(stream, { log = () => {}, quiet = 80 } = {}) {
		this.s = stream;
		this.log = log;
		this.quiet = quiet;	// after a write's echo, how long the line must stay quiet before the next frame
	}

	// The only things this class can send: framed read_addr lines, and write_addr lines to the stub window or the
	// hijack word. Anything else is refused before the port.
	async send(line) {
		const w = WRITE.exec(line);
		if (!(READ.test(line) || (w && writable(parseInt(w[1], 16))))) throw new DB25DError(`refused to send ${JSON.stringify(line)}`);
		await this.s.write(new TextEncoder().encode(`${FRAME}${line}\r\n`));
	}

	// Wait until a reply matching re with first number `addr` came in; returns its numbers, or null after `timeout`
	async reply(re, addr, timeout) {
		const until = Date.now() + timeout;
		while (Date.now() < until) {
			const text = latin(this.s.buf);
			if (text.endsWith("\n")) {
				for (const m of text.matchAll(re)) {
					if (Number(m[1]) === addr) return [Number(m[1]) >>> 0, Number(m[2]) >>> 0];
				}
			}
			await sleep(1);
		}
		return null;
	}

	// One read_addr: the 32-bit value, or null when no matching reply came in time
	async readWord(addr, timeout = 1000) {
		this.s.clear();
		await this.send(`read_addr(0x${hex8(addr)})`);
		const r = await this.reply(READ_REPLY, addr, timeout);
		return r ? r[1] : null;
	}

	// One write_addr: true once the radio echoed the address and the value, false when it did not. A write to a fixed
	// address can be sent again.
	async writeWord(addr, value, timeout = 1000) {
		this.s.clear();
		await this.send(`write_addr(0x${hex8(addr)},0x${hex8(value)})`);
		const r = await this.reply(WRITE_REPLY, addr, timeout);
		if (!r) return false;
		// the stock application prints its own lines after the echo: let them pass before the next frame
		let n = this.s.buf.length, last = Date.now();
		const until = Date.now() + 400;
		while (Date.now() < until && Date.now() - last < this.quiet) {
			if (this.s.buf.length !== n) { n = this.s.buf.length; last = Date.now(); }
			await sleep(2);
		}
		return r[1] === value >>> 0;
	}

	async retry(what, addr, fn, tries, signal) {
		for (let i = 0; i < tries; i++) {
			if (signal?.aborted) throw new DB25DError("stopped");
			const v = await fn(1000 + i * 500);
			if (v !== null && v !== false) return v;
			await sleep(100);
		}
		throw new DB25DError(`the console did not answer the ${what} of 0x${hex8(addr)} after ${tries} tries`);
	}

	readWordRetry(addr, tries = 6, signal) {
		return this.retry("read", addr, (t) => this.readWord(addr, t), tries, signal);
	}

	writeWordRetry(addr, value, tries = 6, signal) {
		return this.retry("write", addr, (t) => this.writeWord(addr, value, t), tries, signal);
	}

	// Is a radio with the stock firmware on the cable? true when the console answers a read of the application's
	// first word
	async probe(timeout = 1500) {
		return (await this.readWord(APP_SIG[0], timeout)) !== null;
	}

	// Which stock build runs: "018", "020", or null for one this page does not know (its RAM may differ)
	async build(signal) {
		for (const [name, addr] of BUILDS) {
			const words = [];
			for (let i = 0; i < 4; i++) words.push(await this.readWordRetry(addr + 4 * i, 6, signal));
			const bytes = new Uint8Array(16);
			words.forEach((w, i) => new DataView(bytes.buffer).setUint32(4 * i, w, true));
			const text = latin(bytes);
			if (text.includes(`EARSAB.${name}`)) return name;
		}
		return null;
	}

	// Write the universal stub into the window, read every word back, then point the hijack word at it and send one
	// more frame, which starts it. The console stops answering from then on: the stub owns the cable.
	// onProgress(phase, done, total), phase "write" | "check" | "start".
	async loadStub(image, { onProgress = () => {}, signal } = {}) {
		const [load, size] = STUB_WINDOW;
		univFits(image, load, [load, size]);
		const pad = new Uint8Array(Math.ceil(image.length / 4) * 4);
		pad.set(image);
		const words = Array.from({ length: pad.length / 4 }, (_, i) => new DataView(pad.buffer).getUint32(4 * i, true));
		const sig = await this.readWordRetry(APP_SIG[0], 6, signal);
		if (sig !== APP_SIG[1]) throw new DB25DError(`the application does not start as the original firmware does (0x${hex8(sig)})`);
		for (let i = 0; i < words.length; i++) {
			await this.writeWordRetry(load + 4 * i, words[i], 6, signal);
			if (i % 16 === 15 || i === words.length - 1) onProgress("write", i + 1, words.length);
		}
		for (let i = 0; i < words.length; i++) {
			const v = await this.readWordRetry(load + 4 * i, 6, signal);
			if (v !== words[i]) throw new DB25DError(`RAM 0x${hex8(load + 4 * i)} reads 0x${hex8(v)} instead of 0x${hex8(words[i])}`);
			if (i % 16 === 15 || i === words.length - 1) onProgress("check", i + 1, words.length);
		}
		onProgress("start", 0, 1);
		this.log(`stub written to 0x${hex8(load)} and checked (${words.length} words); starting it`);
		// Sent once, never again and never read back: with the word in place every frame starts the stub, a resend
		// included. A lost echo is not an error here; whether the stub came up is what the caller checks.
		if (!(await this.writeWord(HIJACK, load))) this.log("no echo for the hijack word; starting anyway");
		this.s.clear();
		await this.send(`read_addr(0x${hex8(APP_SIG[0])})`);
		onProgress("start", 1, 1);
	}
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

// The loader's read request: "Flash Read " + u32 BE address + u32 BE page count. It answers the version string, the pages
// and ChecksumR. Read only; the page flasher uses one page of it to tell that the loader is the one answering.
export const readPacket = (addr, pages) => concat(new TextEncoder().encode("Flash Read "), be32(addr), be32(pages));

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

// A program memory backup's application area put back through the loader (a backup of the original firmware, or of
// ODR): it must start with the entry the loader checks at power-on, or the radio would stay in update mode
export function checkRestoreImage(image) {
	if (image.length <= 0x1000 || ![0x2C, 0x11, 0x01, 0xC0].every((b, i) => image[i] === b))
		throw new DB25DError("the backup's application area does not start with an application the loader would start");
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

	// Is the loader's update mode on the cable now (a radio whose application is missing or incomplete stays in it at
	// every power-on)? Asks for one page of a read, which changes nothing, and never sends Erase. Returns {version} when
	// the loader answers (version is null when only the banner or the checksum was seen), else null.
	async probe(timeout = 1500) {
		this.s.clear();
		await this.s.write(readPacket(0, 1));
		const seen = (t) => /Read_\w+_V\d+\.\d+[A-Z]?|ChecksumR|IAPReady/.test(t);
		const text = await this.until(seen, timeout);
		if (!seen(text)) return null;
		const v = /Read_\w+_V\d+\.\d+[A-Z]?/.exec(text);
		this.s.clear();
		return { version: v ? v[0] : null };
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
	// restore: the image is a backup's application area (checkRestoreImage), not an install image
	async write(image, { onPage = () => {}, onPhase = () => {}, signal, eraseTimeout = 60000, pageTimeout = 10000, restore = false } = {}) {
		(restore ? checkRestoreImage : checkInstallImage)(image);
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
