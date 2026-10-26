// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The Baofeng serial updater, a radio's own update mode on the programming cable (a board's radios.json
// flash.update_mode "baofeng"; entered by holding the board's flash.bootloader keys at power-on). The protocol is
// docs/spec/hardware-dm32.md 13.2; this is a port of tools/serial_loader.py's Flasher, byte for byte the same on the
// wire (site/tests/bfloader.test.mjs compares the two). The loader erases the whole application area, the codec slot
// included, before it takes the first block, and never writes itself, so update mode stays whatever is written.

import { sleep } from "./bytes.js";
import { crc16 } from "./link.js";

export class BfError extends Error {}

export const ACK = 0x06, NAK = 0x15, CRC_REQ = 0x43, SOH = 0x01, STX = 0x02, EOT = 0x04;
export const BLOCK = 1024;
export const HEADER_BLOCK = 128;
export const STEP_TIMEOUT = 2000;	// steps 1-6 and every 1 KB block
export const ERASE_TIMEOUT = 30000;	// step 7: the radio erases the application area (up to about 15 s)
export const RETRIES = 4;		// resends of a block the radio refuses (NAK) or doesn't answer

// CRC-16/XMODEM: link.js's CRC-16 from an initial value of 0
export const xmodem = (data) => crc16(data, 0);

const hexs = (b) => (b?.length ? [...b].map((x) => x.toString(16).toUpperCase().padStart(2, "0")).join(" ") : "nothing");
const text = (b) => String.fromCharCode(...b);

export const blocksNeeded = (size) => Math.ceil(size / BLOCK);

// Step 7's 128-byte block: name, NUL, decimal size in ASCII, zero padded (the radio uses neither)
export function headerBlock(name, size) {
	const base = String(name).split(/[\\/]/).pop();
	const n = [...base].slice(0, HEADER_BLOCK - 16).map((ch) => (ch.charCodeAt(0) < 128 ? ch.charCodeAt(0) : 0x3F));
	const body = [...n, 0, ...[...String(size)].map((ch) => ch.charCodeAt(0))];
	const out = new Uint8Array(HEADER_BLOCK);
	out.set(body);
	return out;
}

// Step 8's frame after the 0x02 / 0x43 exchange: block number, its complement, 1024 bytes (zero padded), CRC big endian
export function dataFrame(n, chunk) {
	const data = new Uint8Array(BLOCK);
	data.set(chunk);
	const crc = xmodem(data);
	const out = new Uint8Array(2 + BLOCK + 2);
	out[0] = n & 0xFF;
	out[1] = 0xFF - (n & 0xFF);
	out.set(data, 2);
	out[2 + BLOCK] = crc >> 8;
	out[3 + BLOCK] = crc & 0xFF;
	return out;
}

// Problems that stop a write before anything is sent (serial_loader.check_image). appMax: everything the loader may
// write, from the application start to the end of the program flash
export function checkImage(raw, appMax) {
	if (!raw?.length) throw new BfError("the image is empty");
	if (raw.length > appMax) throw new BfError(`the image is ${raw.length} bytes: it does not fit the application area (${appMax} bytes)`);
	const head = raw.subarray(0, 16);
	if (head.every((b) => b === 0xFF) || head.every((b) => b === 0)) throw new BfError("the image starts with 16 blank bytes: not an application");
}

export class BfLoader {
	// id: the string the board's loader answers at step 3 (c7000.boards.<id>.bootloader_id); appMax as checkImage's
	constructor(stream, { id, appMax, log = () => {}, stepTimeout = STEP_TIMEOUT, eraseTimeout = ERASE_TIMEOUT, retries = RETRIES, poll = 1000 } = {}) {
		this.s = stream;
		this.id = new TextEncoder().encode(id || "");
		this.appMax = appMax;
		this.log = log;
		this.stepTimeout = stepTimeout;
		this.eraseTimeout = eraseTimeout;
		this.retries = retries;
		this.poll = poll;
		this.ready = false;	// steps 1-3 done: the loader waits for step 4
	}

	// What arrived, up to n bytes, once n are there or the time is up (serial_loader read_exact)
	async readExact(n, timeout) {
		const end = Date.now() + timeout;
		while (this.s.buf.length < n && Date.now() < end) await sleep(2);
		return this.s.take(n);
	}

	async exchange(step, send, want, { timeout = this.stepTimeout, what = "" } = {}) {
		if (send) await this.s.write(Uint8Array.from(send));
		const got = await this.readExact(want.length, timeout);
		if (got.length === want.length && got.every((b, i) => b === want[i])) return;
		if (!got.length) throw new BfError(`step ${step}${what}: no answer within ${Math.round(timeout / 1000)} s. Is the radio in its update mode? Switch it off and on again with the keys held, and check the cable is pushed fully into the radio`);
		throw new BfError(`step ${step}${what}: the radio answered ${hexs(got)}, expected ${hexs(want)}`);
	}

	// Steps 2 and 3, after step 1 was acknowledged: the loader's ID must be this board's
	async identify() {
		await this.exchange(2, [0x4D, 0, 0, 0, 1], [0x4D, 0x01, this.id.length]);
		const got = await this.readExactAfter([ACK], this.id.length);
		if (got.length === this.id.length && got.every((b, i) => b === this.id[i])) {
			this.ready = true;
			return { id: text(this.id) };
		}
		if (!got.length) throw new BfError(`step 3 (model): no answer within ${Math.round(this.stepTimeout / 1000)} s`);
		throw new BfError(`step 3 (model): the radio's loader is ${JSON.stringify(text(got))}, not ${text(this.id)}: this is another radio`);
	}

	async readExactAfter(send, n) {
		await this.s.write(Uint8Array.from(send));
		return this.readExact(n, this.stepTimeout);
	}

	// Steps 1-3 as serial_loader's handshake: step 1 a few times (the radio may still be starting), and an answer other
	// than the acknowledgement means the radio runs its firmware, not the loader
	async hello() {
		this.s.clear();
		for (let attempt = 0; attempt < 3; attempt++) {
			await this.s.write(Uint8Array.of(0x52));
			const got = await this.readExact(1, this.stepTimeout);
			if (got.length === 1 && got[0] === ACK) return this.identify();
			if (got.length) throw new BfError(`step 1: the radio answered ${hexs(got)}, expected 06. If the screen is on, the radio runs its firmware, not its update mode`);
		}
		throw new BfError("step 1: no answer. Is the radio in its update mode?");
	}

	// Listen for update mode while the user switches the radio on with the keys held: step 1 again and again until the
	// loader acknowledges, then steps 2 and 3. Anything else that answers (the boot ROM's greeting, a firmware) is
	// passed over. Nothing in the radio changes before step 7's erase.
	async listen(timeout, { signal } = {}) {
		const until = Date.now() + timeout;
		let other = false;
		while (Date.now() < until) {
			if (signal?.aborted) throw new BfError("stopped");
			this.s.clear();
			await this.s.write(Uint8Array.of(0x52));
			const got = await this.readExact(1, this.poll);
			if (got.length === 1 && got[0] === ACK) return this.identify();
			if (got.length && !other) {
				other = true;
				this.log(`something answers that is not the update mode (${hexs(got)}); listening on`);
			}
		}
		throw new BfError("the radio did not report update mode");
	}

	// Is the loader on the cable now? Steps 1-3 for a short time; null when nothing answers as the loader does
	async probe(timeout = 1500) {
		try {
			return await this.listen(timeout);
		} catch (e) {
			if (e instanceof BfError && /did not report/.test(e.message)) return null;
			throw e;
		}
	}

	// Steps 4-9: the erase, every block, the end. onPage(i, total); onPhase("erase" | "write"). The loader acknowledges
	// each block by its CRC; there is no checksum over the whole image to compare. Returns the blocks written.
	async write(raw, { name = "rfoundry.bin", onPage = () => {}, onPhase = () => {}, signal } = {}) {
		checkImage(raw, this.appMax);
		if (signal?.aborted) throw new BfError("stopped");
		if (!this.ready) await this.hello();
		this.ready = false;
		this.log(`   bootloader found (${text(this.id)})`);
		await this.exchange(4, [0x45, 0, 0, 0, 0], [ACK]);
		await this.exchange(5, [0x31], [CRC_REQ]);
		await this.exchange(6, [SOH], [CRC_REQ]);
		onPhase("erase");
		const blk = headerBlock(name, raw.length);
		const crc = xmodem(blk);
		this.log("   step 7: header sent, the radio erases the application area (up to about 15 s) ...");
		const t0 = Date.now();
		await this.exchange(7, [0x00, 0xFF, ...blk, crc >> 8, crc & 0xFF], [ACK, CRC_REQ], { timeout: this.eraseTimeout, what: " (erase)" });
		this.log(`   erased in ${((Date.now() - t0) / 1000).toFixed(1)} s`);
		onPhase("write");
		const total = blocksNeeded(raw.length);
		for (let i = 0; i < total; i++) {
			if (signal?.aborted) throw new BfError("stopped");
			await this.sendBlock((i + 1) & 0xFF, raw.subarray(i * BLOCK, (i + 1) * BLOCK));
			onPage(i + 1, total);
		}
		await this.s.write(Uint8Array.of(EOT));
		await this.readExact(1, 500);	// the radio may acknowledge before it restarts; not required
		this.log("   done");
		return total;
	}

	async sendBlock(n, chunk) {
		const frame = dataFrame(n, chunk);
		let last = null;
		for (let attempt = 0; attempt < 1 + this.retries; attempt++) {
			if (attempt) this.log(`   block ${n}: ${last}, sending it again (${attempt} of ${this.retries})`);
			await this.s.write(Uint8Array.of(STX));
			let got = await this.readExact(1, this.stepTimeout);
			if (!(got.length === 1 && got[0] === CRC_REQ)) {
				last = `answered ${hexs(got)} to 02 instead of 43`;
				this.s.clear();
				continue;
			}
			await this.s.write(frame);
			got = await this.readExact(1, this.stepTimeout);
			if (got.length === 1 && got[0] === ACK) return;
			last = got.length === 1 && got[0] === NAK ? "refused (NAK)" : `answered ${hexs(got)}`;
			this.s.clear();
		}
		throw new BfError(`step 8, block ${n}: ${last} after ${1 + this.retries} tries. The application area is erased now: the radio won't start until an installation completes. Enter update mode again and repeat it`);
	}
}
