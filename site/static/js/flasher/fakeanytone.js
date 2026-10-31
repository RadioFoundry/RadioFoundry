// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A simulated AnyTone update mode (anytone.js) for the tests and the practice run: a port of rfoundry's
// anytone.FakeRadio. It answers the handshake and the identity, checks and keeps every frame, and can refuse a frame
// now and then (nakEvery) or go silent (silentAfter frames). With memory ([[address, bytes], ...]) it also plays the
// stock firmware's PC mode: PROGRAM, reads of that memory (unset bytes 0xFF), END. With pairs, a read in a block
// that isn't current comes from the block after it, as the radio's does (codeplug/stock_anytone.js currentBlock). stream: what anytone.js reads and
// writes, as a SerialStream's buffer and write().

import { concat } from "./bytes.js";
import { AT, BLOCK, CTCSS, MAP, currentBlock } from "../codeplug/stock_anytone.js";
import { HELLO, HELLO_REPLY, FINISH, ACK, NAK, DATA, CHUNK, FRAME_LEN, PROGRAM, END, READ, readReply } from "./anytone.js";

export class FakeAnytone {
	constructor({ model = "MODEL1", nakEvery = 0, silentAfter = null, delay = 0, memory = [], pairs = false } = {}) {
		Object.assign(this, { model, nakEvery, silentAfter, delay, memory, pairs });
		this.program = false;
		this.ended = false;
		this.reads = 0;
		this.inbuf = new Uint8Array(0);
		this.mem = new Map();
		this.frames = 0;
		this.bad = 0;
		this.finished = false;
		const s = this.stream = {
			buf: new Uint8Array(0), waiters: [],
			clear() { s.buf = new Uint8Array(0); },
			take(n) { const o = s.buf.slice(0, n); s.buf = s.buf.slice(n); return o; },
			write: async (b) => this.feed(b),
			arrived(ms) {
				return new Promise((ok) => {
					const done = () => { clearTimeout(timer); s.waiters = s.waiters.filter((w) => w !== done); ok(); };
					const timer = setTimeout(done, ms);
					s.waiters.push(done);
				});
			},
			async close() {},
		};
	}

	send(b) {
		const go = () => { this.stream.buf = concat(this.stream.buf, b); for (const w of this.stream.waiters) w(); };
		if (this.delay) setTimeout(go, this.delay); else go();
	}

	// The bytes written, from the lowest address, gaps 0xFF
	image() {
		if (!this.mem.size) return new Uint8Array(0);
		const lo = Math.min(...this.mem.keys()), hi = Math.max(...this.mem.keys()) + CHUNK;
		const out = new Uint8Array(hi - lo).fill(0xFF);
		for (const [a, d] of this.mem) out.set(d, a - lo);
		return out;
	}

	peek(addr, n) {
		const out = new Uint8Array(n).fill(0xFF);
		for (const [a, d] of this.memory) {
			const lo = Math.max(a, addr), hi = Math.min(a + d.length, addr + n);
			if (lo < hi) out.set(d.subarray(lo - a, hi - a), lo - addr);
		}
		return out;
	}

	feed(b) {
		this.inbuf = concat(this.inbuf, b);
		const starts = (p) => p.every((x, i) => this.inbuf[i] === x);
		while (this.inbuf.length) {
			if (this.inbuf.length >= PROGRAM.length && starts(PROGRAM)) {
				this.inbuf = this.inbuf.slice(PROGRAM.length);
				this.program = true;
				this.send(HELLO_REPLY);
			} else if (this.program && this.inbuf.length >= END.length && starts(END)) {
				this.inbuf = this.inbuf.slice(END.length);
				this.ended = true;
				this.program = false;	// the radio restarts
				this.send(HELLO_REPLY);
			} else if (this.program && this.inbuf[0] === READ) {
				if (this.inbuf.length < 6) return;
				const addr = ((this.inbuf[1] << 24) | (this.inbuf[2] << 16) | (this.inbuf[3] << 8) | this.inbuf[4]) >>> 0, n = this.inbuf[5];
				this.inbuf = this.inbuf.slice(6);
				this.reads++;
				const b = addr - (addr % BLOCK);
				const at = !this.pairs || currentBlock(b, this.peek(b + BLOCK - 16, 16)) === b ? addr : addr + BLOCK;
				this.send(readReply(addr, this.peek(at, n)));
			} else if (this.inbuf.length >= HELLO.length && starts(HELLO)) {
				this.inbuf = this.inbuf.slice(HELLO.length);
				this.send(HELLO_REPLY);
			} else if (this.inbuf[0] === 0x02) {
				this.inbuf = this.inbuf.slice(1);
				const r = new Uint8Array(16);
				r[0] = 0x49;
				r.set(new TextEncoder().encode(this.model).subarray(0, 7), 1);
				r.set([0x01, 0x56, 0x31, 0x30, 0x30, 0, 0, ACK], 8);
				this.send(r);
			} else if (this.inbuf[0] === FINISH[0]) {
				this.inbuf = this.inbuf.slice(1);
				this.finished = true;
				this.send(Uint8Array.of(ACK));
			} else if (this.inbuf[0] === DATA) {
				if (this.inbuf.length < FRAME_LEN) return;
				const f = this.inbuf.slice(0, FRAME_LEN);
				this.inbuf = this.inbuf.slice(FRAME_LEN);
				this.frames++;
				if (this.silentAfter !== null && this.frames > this.silentAfter) continue;
				if (this.nakEvery && this.frames % this.nakEvery === 0) { this.send(Uint8Array.of(NAK)); continue; }
				let sum = 0;
				for (let i = 1; i < 37; i++) sum += f[i];
				const ok = (f[37] | (f[38] << 8)) === (sum & 0xFFFF) && f[39] === ACK;
				if (ok) this.mem.set((f[1] | (f[2] << 8) | (f[3] << 16) | (f[4] << 24)) >>> 0, f.slice(5, 37));
				else this.bad++;
				this.send(Uint8Array.of(ok ? ACK : NAK));
			} else if ([HELLO, PROGRAM, END].some((w) => this.inbuf.length < w.length && starts(w.subarray(0, this.inbuf.length)))) {
				return;	// the start of a handshake word: wait for the rest
			} else {
				this.inbuf = this.inbuf.slice(1);	// noise: ignored
			}
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------
// A stock codeplug in the PC mode's memory (codeplug/stock_anytone.js), for the tests and the practice run:
// [[address, bytes], ...] for FakeAnytone's memory, with pairs: true. Each table goes in the pair's second block, or its first with
// first: true (the first then carries the valid marker). The bitmaps name what is written and nothing else

const ascii = (r, off, s) => { for (let i = 0; i < s.length; i++) r[off + i] = s.charCodeAt(i); };
const bcdBe = (r, off, v) => { for (let i = 3; i >= 0; i--) { r[off + i] = (v % 10) | (Math.floor(v / 10) % 10) << 4; v = Math.floor(v / 100); } };
const set = (r, i, on) => { if (on) r[i >> 3] |= 1 << (i & 7); else r[i >> 3] &= ~(1 << (i & 7)); };
const le = (r, off, v, n) => { for (let i = 0; i < n; i++) r[off + i] = (v >>> (8 * i)) & 0xFF; };

export function stockAnytone({ zones = [], channels = {}, lists = [], contacts = [], radioId = null, first = false } = {}) {
	const mem = [];
	const marks = new Set();
	const at = (base, off, bytes) => {
		if (first && !marks.has(base)) {
			marks.add(base);
			const tail = new Uint8Array(16).fill(0xFF);
			tail.set([0x55, 0x55, 0xAA, 0xAA], 12);
			mem.push([base + BLOCK - 16, tail]);
		}
		mem.push([base + (first ? 0 : BLOCK) + off, bytes]);
	};
	const zoneSet = new Uint8Array(32), chSet = new Uint8Array(AT.nChannels / 8), listSet = new Uint8Array(32);
	const free = new Uint8Array(AT.nContacts / 8).fill(0xFF);
	zones.forEach((z, i) => {
		set(zoneSet, i, true);
		const n = new Uint8Array(32);
		ascii(n, 0, z.name);
		at(AT.zoneNames, i * AT.zoneName, n);
		const m = new Uint8Array(AT.zoneSlots * 2).fill(0xFF);
		z.channels.forEach((c, k) => le(m, 2 * k, c, 2));
		at(AT.zoneMembers, i * AT.zoneRec, m);
	});
	for (const [n, c] of Object.entries(channels)) {
		const r = new Uint8Array(AT.chHalf);
		bcdBe(r, 0, c.rx / 10);
		bcdBe(r, 4, (c.offset || 0) / 10);
		r[8] = (c.type || 0) | (c.power ?? 2) << 2 | (c.wide ? 1 : 0) << 4 | (c.dir || 0) << 6;
		const ct = (t) => (t ? CTCSS.indexOf(Math.round(t * 10)) : 0);
		r[9] = (c.rxTone ? 1 : 0) | (c.txTone ? 1 : 0) << 2 | (c.rxOnly ? 0x20 : 0);
		r[0xA] = ct(c.txTone);
		r[0xB] = ct(c.rxTone);
		le(r, 0x14, c.contact ?? 0, 4);
		r[0x18] = 0;
		r[0x1B] = 0xFF;
		r[0x1C] = c.rxList ?? 0xFF;
		r[0x20] = c.cc ?? 1;
		r[0x21] = c.ts === 2 ? 1 : 0;
		ascii(r, 0x23, c.name);
		const k = Number(n);
		set(chSet, k, true);
		at(AT.channels + Math.floor(k / AT.chPerPair) * 2 * BLOCK, (k % AT.chPerPair) * AT.chHalf, r);
	}
	lists.forEach((l) => {
		set(listSet, l.index, true);
		const r = new Uint8Array(AT.listName + 17).fill(0xFF);
		l.contacts.forEach((c, k) => le(r, 4 * k, c, 4));
		r.fill(0, AT.listName);
		ascii(r, AT.listName, l.name);
		at(AT.lists, l.index * AT.listRec, r);
	});
	contacts.forEach((c) => {
		set(free, c.index, false);
		const r = new Uint8Array(AT.contactRec);
		r[0] = { private: 0, group: 1, all: 2 }[c.type];
		ascii(r, 1, c.name);
		bcdBe(r, 0x23, c.id);
		at(AT.contacts + Math.floor(c.index / AT.perSlot) * 2 * BLOCK, (c.index % AT.perSlot) * AT.contactRec, r);
	});
	const pairOf = (a) => a - (a % (2 * BLOCK));
	for (const [a, b] of [[MAP.zones, zoneSet], [MAP.channels, chSet], [MAP.lists, listSet], [MAP.contacts, free]]) {
		at(pairOf(a), a - pairOf(a), b);
	}
	if (radioId) {
		const r = new Uint8Array(AT.idRec);
		bcdBe(r, 0, radioId.id);
		ascii(r, 5, radioId.name);
		at(AT.ids, 0, r);
	}
	return mem;
}

// The practice run's original codeplug: two zones, FM and DMR channels, a list and its contacts
export const SAMPLE_ANYTONE = {
	zones: [{ name: "Local", channels: [0, 1, 2] }, { name: "DMR", channels: [2, 130] }],
	channels: {
		0: { name: "Simplex", rx: 146520000, power: 2 },
		1: { name: "Repeater", rx: 147000000, offset: 600000, dir: 1, txTone: 100.0, wide: true },
		2: { name: "DMR Local", rx: 438500000, offset: 7600000, dir: 2, type: 1, contact: 1001, rxList: 0, cc: 1, ts: 2 },
		130: { name: "DMR TG9", rx: 439500000, type: 1, contact: 3, rxList: 0, power: 0 },
	},
	lists: [{ index: 0, name: "Local", contacts: [3, 1001] }],
	contacts: [{ index: 3, name: "Local 9", type: "group", id: 9 }, { index: 1001, name: "World", type: "group", id: 91 }],
	radioId: { id: 1234567, name: "N0CALL" },
};

