// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A simulated AnyTone update mode (anytone.js) for the tests and the practice run: a port of rfoundry's
// anytone.FakeRadio. It answers the handshake and the identity, checks and keeps every frame, and can refuse a frame
// now and then (nakEvery) or go silent (silentAfter frames). stream: what anytone.js reads and writes, as a
// SerialStream's buffer and write().

import { concat } from "./bytes.js";
import { HELLO, HELLO_REPLY, FINISH, ACK, NAK, DATA, CHUNK, FRAME_LEN } from "./anytone.js";

export class FakeAnytone {
	constructor({ model = "MODEL1", nakEvery = 0, silentAfter = null, delay = 0 } = {}) {
		Object.assign(this, { model, nakEvery, silentAfter, delay });
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

	feed(b) {
		this.inbuf = concat(this.inbuf, b);
		const starts = (p) => p.every((x, i) => this.inbuf[i] === x);
		while (this.inbuf.length) {
			if (this.inbuf.length >= HELLO.length && starts(HELLO)) {
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
			} else if (this.inbuf.length < HELLO.length && starts(HELLO.subarray(0, this.inbuf.length))) {
				return;	// the start of UPDATE: wait for the rest
			} else {
				this.inbuf = this.inbuf.slice(1);	// noise: ignored
			}
		}
	}
}
