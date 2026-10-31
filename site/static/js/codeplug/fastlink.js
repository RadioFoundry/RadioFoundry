// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The fast codeplug protocol (docs/spec/cps-fast.md): the debug link's CODEPLUG command, 4 KB blocks compared by
// CRC-32 and moved as LZ4 streams. transport.js uses it when the radio lists CPS_FAST; the open-firmware CPS stays the
// way for everything else. Addresses are image addresses, as in transport.js: the radio adds its base.

import { frame, parseFrames } from "../flasher/link.js";
import { decodeBlock, encodeBlock } from "../flasher/lz4.js";
import { concat, crc32, le32, sleep, u16le, u32le } from "../flasher/bytes.js";
import { note } from "../diag.js";

export const BLOCK = 4096;
const CMD = 0x1C;
const OP = { INFO: 0, SUMS: 1, READ: 2, BEGIN: 3, BLOCK: 4, DATA: 5, END: 6, COMMIT: 7, STATUS: 8, ABORT: 9 };
export const REASON = { RANGE: 1, STORAGE: 2, NO_STAGING: 3, STAGE_FULL: 4, STAGE_BUSY: 5, STATE: 6, CRC: 7, STREAM: 8, FLASH: 9 };
const WINDOW = 8;		// DATA requests in flight (3.6)
const FORMAT_LZ4 = 1, FORMAT_BLANK = 2;
export const BLANK_CRC = 0xF154670A;

export class FastError extends Error {
	constructor(what, status, reason = 0) {
		super(`${what}: status ${status}${reason ? ` reason ${reason}` : ""}`);
		this.name = "FastError";
		Object.assign(this, { what, status, reason });
	}
}

const u16 = (n) => [n & 0xFF, (n >> 8) & 0xFF];
const isBlank = (b) => b.every((x) => x === 0xFF);

export class FastLink {
	// link: the debug link client (flasher/link.js Link): its stream, sequence counter and wait
	constructor(link) {
		this.link = link;
		this.s = link.s;
		this.wire = 0;		// bytes sent and received, for the log and the tests
		this.requests = 0;
	}

	next() {
		this.link.seq = (this.link.seq + 1) & 0xFF;
		return this.link.seq;
	}

	// Answers to the frames sent with seqs, in order: [{status, reason, data}], undefined for one that didn't come
	async answers(seqs, timeout) {
		const got = await this.link.waitFor((buf) => {
			const fs = parseFrames(buf);
			const all = seqs.map((q) => fs.find((f) => f.seq === q && (f.cmd === (CMD | 0x80) || f.cmd === 0xFF)));
			if (all.every(Boolean)) return all;
			return undefined;
		}, timeout);
		const fs = got || parseFrames(this.s.buf);
		const out = seqs.map((q) => fs.find((f) => f.seq === q && (f.cmd === (CMD | 0x80) || f.cmd === 0xFF)));
		const end = out.reduce((n, f) => Math.max(n, f?.end || 0), 0);
		if (end) this.s.take(end);
		for (const f of out) if (f) this.wire += f.payload.length + 8;
		return out.map((f) => f && { status: f.payload[0], reason: f.payload[0] === 7 ? f.payload[1] || 0 : 0, data: f.payload.subarray(1) });
	}

	// One request and its answer; throws FastError unless the status is OK. Busy (the storage sector buffer taken, a
	// commit running) is waited out for a few seconds
	async op(what, payload, { timeout } = {}) {
		const until = Date.now() + 5000;
		for (;;) {
			this.s.clear();
			const seq = this.next();
			const f = frame(CMD, seq, Uint8Array.from(payload));
			this.wire += f.length;
			this.requests++;
			await this.s.write(f);
			const [r] = await this.answers([seq], timeout);
			if (!r) throw new FastError(what, "no answer");
			if (r.status === 3 && Date.now() < until) {
				await sleep(20);
				continue;
			}
			if (r.status !== 0) throw new FastError(what, r.status, r.reason);
			return r.data;
		}
	}

	// INFO (3.1), or null from a radio that doesn't speak the protocol
	async info() {
		let d;
		try {
			d = await this.op("info", [OP.INFO]);
		} catch (e) {
			if (e instanceof FastError) return null;
			throw e;
		}
		if (d.length < 26 || d[0] !== 1) return null;
		this.limits = {
			version: d[0], staging: !!(d[1] & 1), writable: !!(d[1] & 2), block: u16le(d, 2), sums: d[4], data: d[5],
			read: u16le(d, 6), eeSize: u32le(d, 8), flashStart: u32le(d, 12), end: u32le(d, 16), base: u32le(d, 20),
			stageMax: u16le(d, 24),
		};
		return this.limits;
	}

	// The CRC and blank flag of each block from addr: [{addr, crc, blank}]
	async sums(addr, count) {
		const out = [];
		for (let i = 0; i < count;) {
			const n = Math.min(this.limits.sums, count - i), a = addr + i * BLOCK;
			const d = await this.op("sums", [OP.SUMS, ...le32(a), n]);
			const mask = u16le(d, 5);
			for (let k = 0; k < n; k++) out.push({ addr: a + k * BLOCK, crc: u32le(d, 7 + 4 * k), blank: !!(mask & (1 << k)) });
			i += n;
		}
		return out;
	}

	// One block's 4096 bytes. A block that changes while it is read (its length or CRC moves) is read again
	async read(addr) {
		for (let tries = 0; tries < 3; tries++) {
			let stream = new Uint8Array(0), len = -1, crc = 0, changed = false;
			while (len < 0 || stream.length < len) {
				const d = await this.op("read", [OP.READ, ...le32(addr), ...u16(stream.length)]);
				if (d[4] === FORMAT_BLANK) return new Uint8Array(BLOCK).fill(0xFF);
				const l = u16le(d, 5), c = u32le(d, 7);
				if (len >= 0 && (l !== len || c !== crc)) {
					changed = true;
					break;
				}
				len = l;
				crc = c;
				stream = concat(stream, d.subarray(14));	// after the offset, a reserved byte (3.3)
			}
			if (changed) continue;
			const out = decodeBlock(stream, BLOCK);
			if (crc32(out) === crc) return out;
		}
		throw new FastError("read", "changing", 0);
	}

	async begin(staged) {
		const d = await this.op("begin", [OP.BEGIN, staged ? 1 : 0]);
		return u16le(d, 0);
	}

	// One block: BLOCK, its stream in DATA windows of 8, then END. A window that goes wrong sends the block again,
	// one DATA at a time (3.6)
	async write(addr, bytes) {
		const crc = crc32(bytes);
		if (isBlank(bytes)) {
			await this.op("block", [OP.BLOCK, ...le32(addr), FORMAT_BLANK, 0, 0, ...le32(crc)]);
			await this.op("end", [OP.END], { timeout: 5000 });
			return 0;
		}
		const stream = encodeBlock(bytes);
		for (let window = WINDOW; ; window = 1) {
			await this.op("block", [OP.BLOCK, ...le32(addr), FORMAT_LZ4, ...u16(stream.length), ...le32(crc)]);
			if (await this.data(stream, window)) break;
			if (window === 1) throw new FastError("data", "refused", REASON.STREAM);
			note("fast write: window failed, block sent again one DATA at a time", { addr });
		}
		// END writes the flash: a sector erase can take 400 ms, two of them in direct mode
		await this.op("end", [OP.END], { timeout: 5000 });
		return stream.length;
	}

	// The stream's DATA requests, window at a time; false when an answer is missing or refused
	async data(stream, window) {
		const per = this.limits.data;
		for (let off = 0; off < stream.length;) {
			const seqs = [], frames = [];
			for (let k = 0; k < window && off < stream.length; k++) {
				const n = Math.min(per, stream.length - off), seq = this.next();
				frames.push(frame(CMD, seq, Uint8Array.from([OP.DATA, ...u16(off), ...stream.subarray(off, off + n)])));
				seqs.push(seq);
				off += n;
			}
			this.s.clear();
			const all = concat(...frames);
			this.wire += all.length;
			this.requests += frames.length;
			await this.s.write(all);
			const got = await this.answers(seqs);
			if (got.some((r) => !r || r.status !== 0)) return false;
		}
		return true;
	}

	async commit() {
		await this.op("commit", [OP.COMMIT], { timeout: 5000 });
	}

	async status() {
		const d = await this.op("status", [OP.STATUS]);
		return { state: d[0], mode: d[1], blocks: u16le(d, 2), copied: u16le(d, 4), reason: d[6] };
	}

	async abort() {
		try {
			await this.op("abort", [OP.ABORT]);
		} catch {
			// nothing open, or the radio gone: the session times out by itself
		}
	}
}
