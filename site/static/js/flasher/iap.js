// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// IAP, a vendor loader's update mode on the programming cable (a board's radios.json flash.update_mode "iap"; the
// DB25-D PRO's, entered with P1 held at power-on, docs/spec/hardware-db25d.md 7). It writes an image to the
// application area: the recovery route when neither a stock console, RadioFoundry nor the first stage's power-on window answers.
// The loader starts what it wrote at a power-on when it begins with the board's first stage signature
// (flash.first_stage, from board.mk BOOT_SIZE and BOOT_SIGNATURE).

import { be32, concat, sleep } from "./bytes.js";

export class IapError extends Error {}

// one char per byte (TextDecoder("latin1") is windows-1252 and would change the checksum bytes)
const latin = (b) => {
	let s = "";
	for (let i = 0; i < b.length; i++) s += String.fromCharCode(b[i]);
	return s;
};

const hex8 = (n) => (n >>> 0).toString(16).padStart(8, "0");
// RadioFoundry's application starts with its reset handler (psrclr ie), right after the first stage
export const APP_ENTRY = [0x80, 0xC0, 0x20, 0x70];
const headBytes = (fs) => fs.head.match(/../g).map((x) => parseInt(x, 16));

// The protocol (notes: db25d-iap-protocol.md):
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
	if (!image.length || image.length > APP_MAX) throw new IapError(`the image is ${image.length} bytes; it must be 1 to ${APP_MAX}`);
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

// What the loader takes: the first stage (firstStage.size bytes, starting with its signature so the loader boots it at
// a normal power-on) and RadioFoundry's application (starting with its reset handler), as the image builder checks
export function checkInstallImage(image, firstStage) {
	const { size } = firstStage, head = headBytes(firstStage);
	if (image.length <= size) throw new IapError("this file holds only the first stage; the install image is the first stage and the application");
	if (!head.every((b, i) => image[i] === b)) throw new IapError(`this is not an install image for this radio (it does not start with the first stage's ${firstStage.head.toUpperCase()})`);
	if (!APP_ENTRY.every((b, i) => image[size + i] === b)) throw new IapError("the application after the first stage does not start with RadioFoundry's reset handler");
}

// A program memory backup's application area put back through the loader (a backup of the original firmware, or of
// RadioFoundry): it must start with the entry the loader checks at power-on, or the radio would stay in update mode
export function checkRestoreImage(image, firstStage) {
	if (image.length <= firstStage.size || !headBytes(firstStage).every((b, i) => image[i] === b))
		throw new IapError("the backup's application area does not start with an application the loader would start");
}

// The loader writes as far as the image reaches, so the 0xFF tail target() pads on is dropped (rounded up to whole words)
export function trimTail(data) {
	let end = data.length;
	while (end > PAGE && data[end - 1] === 0xFF) end--;
	return data.subarray(0, Math.min(data.length, (end + 3) & ~3));
}

const isBanner = (t) => t.includes("IAPReady");

export class Iap {
	// firstStage: the board's flash.first_stage, which every image the loader takes starts with
	constructor(stream, { log = () => {}, firstStage = null } = {}) {
		this.s = stream;
		this.log = log;
		this.firstStage = firstStage;
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
			if (signal?.aborted) throw new IapError("stopped");
			if (isBanner(this.text())) {
				await this.until(() => false, 400);	// the rest of the banner
				this.s.clear();
				return;
			}
			if (this.s.buf.length > 4096) this.s.take(this.s.buf.length - 64);
			await sleep(20);
		}
		throw new IapError("the radio did not report update mode");
	}

	// Erase and write `image`. onPage(i, total). Returns the checksum the radio reported (it matches ours or this throws).
	// restore: the image is a backup's application area (checkRestoreImage), not an install image
	async write(image, { onPage = () => {}, onPhase = () => {}, signal, eraseTimeout = 60000, pageTimeout = 10000, restore = false } = {}) {
		if (!this.firstStage) throw new IapError("this radio has no first stage: the loader would not start the image");
		(restore ? checkRestoreImage : checkInstallImage)(image, this.firstStage);
		const { n, pages, sum } = iapPlan(image);
		onPhase("erase");
		this.s.clear();
		await this.s.write(erasePacket(n));
		const er = await this.until((t) => t.includes("ok"), eraseTimeout);
		if (!er.includes("ok")) throw new IapError("the radio did not confirm the erase. The application area may be erased: enter update mode again and repeat the install");
		onPhase("write");
		let sent = 0, reply = "";
		for (let i = 0; i < pages.length; i++) {
			if (signal?.aborted) throw new IapError("stopped");
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
				throw new IapError(`page ${i}: unexpected reply, stopped. Enter update mode again and repeat the install`);
		}
		const k = reply.indexOf("ChecksumI");
		if (k < 0 || reply.length < k + 13) throw new IapError("the radio did not report a checksum after the last page");
		const theirs = ((reply.charCodeAt(k + 9) << 24) | (reply.charCodeAt(k + 10) << 16) | (reply.charCodeAt(k + 11) << 8) | reply.charCodeAt(k + 12)) >>> 0;
		if (theirs !== sent) throw new IapError(`the radio's checksum 0x${hex8(theirs)} does not match the file's 0x${hex8(sent)}: install again`);
		if (sent !== sum) throw new IapError("internal checksum error");
		return theirs;
	}
}
