// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// A simulated HR_C7000 radio on its programming cable, for the tests and the flasher's simulated mode (#sim=c7000):
// the serial boot ROM's short window after power-on or a soft reset, the firmware it then starts (stock, or ODR
// answering the debug link VERSION and STUB commands), and the stub's protocol over a model of the two flash chips
// (erase sets a sector to 0xFF, programming only clears bits). The ROM side and the stub side follow stub/stubtool.py's
// FakeRadio. Bytes reach the host no faster than a 115200 baud cable would carry them (times `speed`).
//
// The stock firmware and codec here are random bytes shaped like the real ones: no vendor code is in ODR.

import { concat, crc32, le32, sha256, sleep, u32le } from "./bytes.js";
import { frame, parseFrames } from "./link.js";
import { encodeBlock } from "./lz4.js";
import { STUB_MAGIC, SSI_PROG, SYS_SOFT_RSTN, TIMER2_VALUE } from "./stub.js";
import { rng, randomBytes } from "./fake.js";
import { sigBytes } from "./c7000.js";

const SECTOR = 4096, OK = 0x05, BADSUM = 0x0A, BADOP = 0x0B;
const sum8 = (b) => b.reduce((a, x) => (a + x) & 0xFF, 0);
const be32 = (n) => new Uint8Array([(n >>> 24) & 0xFF, (n >>> 16) & 0xFF, (n >>> 8) & 0xFF, n & 0xFF]);
const u32be = (b, o) => ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0;
const ODR_MARK = new TextEncoder().encode("OpenDigitalRadio");

// A synthetic stock program flash for a board: bootloader, firmware, the codec at made-up addresses, and the c7000
// config changed so its profile matches them. Returns {c, progmem, codec: {code, tables}}.
export async function c7000Simulation(cReal, boardId, seed = 7000) {
	const c = structuredClone(cReal);
	const r = rng(seed);
	const progmem = new Uint8Array(c.progmem_size).fill(0xFF);
	progmem.set(randomBytes(r, 0xA000), 0);					// bootloader, with its ID
	progmem.set(new TextEncoder().encode(c.boards[boardId].bootloader_id || ""), 0x5000);
	const fwLen = 0xC0000;
	progmem.set(randomBytes(r, fwLen).map((b) => (b < 120 ? b : 0)), c.app_start);	// firmware-like
	const real = c.profiles.find((p) => p.board === boardId);
	const code = randomBytes(r, real.code_len);
	code.set(sigBytes(c), 0);
	const tables = randomBytes(r, real.tables_len);
	const codeAt = 0xA4000, tabAt = 0x6A000;
	progmem.set(code, codeAt);
	progmem.set(tables, tabAt);
	c.profiles = c.profiles.filter((p) => p.board !== boardId);
	c.profiles.push({ ...real, name: `simulated ${real.name}`, code_sha256: await sha256(code), tables_sha256: await sha256(tables),
		tables_head_crc: crc32(tables.subarray(0, 64)), stock_file: null });
	return { c, progmem, codec: { code, tables, codeAt, tabAt } };
}

export class FakeC7000 {
	// firmware: "stock" | "odr" (with stub entry) | "odr-nostub"; speed: how many times faster than a real cable
	constructor({ progmem, dataSize = 0x1000000, board = "DM-1801 Pro", codecId = "dm1801pro-049", stubEntry = true, speed = 1,
		romWindow = 400, bootTime = 1500, faults = {} } = {}) {
		this.progmem = progmem;
		this.q128 = new Uint8Array(dataSize).fill(0xFF);
		this.q128.set(randomBytes(rng(5), 0x40000), 0x1000);
		// the stock codeplug's pages carry their logical ID in the last byte; the calibration is ID 2, at 0x7C000 as on
		// the DM-1801 Pro examined (hardware-dm1801pro.md 6.4)
		for (let p = 0x1000; p < 0x41000; p += 0x1000) this.q128[p + 0xFFF] = 0x10 + (p >> 12);
		this.q128.set(randomBytes(rng(6), 0xFFF), 0x7C000);
		this.q128[0x7CFFF] = 2;
		this.boardName = board;
		this.codecId = codecId;
		this.stubEntry = stubEntry;
		this.speed = speed;
		this.romWindow = romWindow;
		this.bootTime = bootTime;
		// flip: chance per stub frame of a damaged byte; misplace: sector addresses whose record, once, carries the next
		// sector's data (as a record left over from an earlier request would)
		this.faults = { flip: 0, misplace: [], ...faults };
		this.rand = rng(11);
		this.state = "off";		// off | rom | booting | firmware | stub
		this.inb = new Uint8Array(0);
		this.loaded = new Uint8Array(0);
		this.wel = false;
		this.led = "off";
		this.events = [];
		this.writes = 0;
		this.stream = this.makeStream();
	}

	firmware() {
		if (this.fwKind) return this.fwKind;
		const app = this.progmem.subarray(0xC000, 0xE8000);
		let kind = "stock";
		if (app.subarray(0, 64).every((b) => b === 0xFF)) kind = "empty";
		else for (let i = 0; i + ODR_MARK.length < app.length; i++)
			if (app[i] === 0x4F && ODR_MARK.every((b, k) => app[i + k] === b)) { kind = "odr"; break; }
		return (this.fwKind = kind);
	}

	codecReport() {
		const s = this.progmem.subarray(0xE8000);
		return u32le(s, 0) === 0x43524444 ? this.codecId : "absent";
	}

	// ---- the cable: what the radio sends reaches the host at the cable's speed
	makeStream() {
		const radio = this;
		const s = {
			buf: new Uint8Array(0), closed: false,
			clear() { s.buf = new Uint8Array(0); },
			take(n) { const o = s.buf.slice(0, n); s.buf = s.buf.slice(n); return o; },
			async write(data) { radio.receive(data); },
			async close() { s.closed = true; },
		};
		return s;
	}

	// Each reply arrives when a cable at 115200 baud (times speed) would have carried it, in order
	send(bytes) {
		const s = this.stream, now = performance.now();
		this.wireFree = Math.max(now, this.wireFree || 0) + (bytes.length * 1000) / (11520 * this.speed);
		const gen = this.gen;
		setTimeout(() => {
			if (!s.closed && this.gen === gen && this.state !== "off") s.buf = concat(s.buf, bytes);
		}, Math.max(0, this.wireFree - now));
	}

	stop() {
		this.stream.closed = true;
	}

	// ---- power and resets
	powerOn() {
		if (this.state !== "off") return;
		this.boot();
	}

	powerOff() {
		this.state = "off";
		this.led = "off";
		this.gen = (this.gen || 0) + 1;
		this.events.push("off");
	}

	boot() {
		this.state = "rom";
		this.fwKind = null;
		this.inb = new Uint8Array(0);
		this.loaded = new Uint8Array(0);
		this.events.push("rom");
		const gen = (this.gen = (this.gen || 0) + 1);
		this.send(new Uint8Array([0x02, 0x24, 0x00, 0x03]));
		setTimeout(() => {
			if (this.gen !== gen || this.state !== "rom") return;
			this.state = "booting";
			setTimeout(() => {
				if (this.gen !== gen || this.state !== "booting") return;
				this.state = "firmware";
				this.events.push(`firmware ${this.firmware()}`);
			}, this.bootTime / this.speed);
		}, this.romWindow);
	}

	// ---- what arrives from the host
	receive(data) {
		if (this.state === "off" || this.state === "booting") return;
		this.inb = concat(this.inb, data);
		for (;;) {
			const used = this.state === "rom" || this.state === "romload" ? this.rom() : this.state === "stub" ? this.stub()
				: this.state === "firmware" ? this.odr() : 0;
			if (!used) break;
		}
	}

	rom() {
		const ib = this.inb;
		if (ib.length < 4) return 0;
		if (ib[0] === 0x02 && ib[1] === 0x14 && ib[2] === 0 && ib[3] === 3) {
			this.state = "romload";
			this.inb = ib.slice(4);
			return 1;
		}
		if (this.state !== "romload") {
			this.inb = ib.slice(1);
			return 1;
		}
		const op = ib[1], n = { 0x18: 4 + 8 + 256, 0x1B: 8 }[op];
		if (!n) {
			this.inb = new Uint8Array(0);
			return 0;
		}
		if (ib.length < n) return 0;
		const payload = ib.slice(4, n);
		this.inb = ib.slice(n);
		if (sum8(payload) !== ib[2]) return 1;
		if (op === 0x18) this.loaded = concat(this.loaded, payload.subarray(8));
		else {
			this.state = "stub";
			this.led = "slow";
			this.events.push("stub (boot ROM)");
		}
		this.send(new Uint8Array([0x02, 0x05, 0x00, 0x03]));
		return 1;
	}

	odr() {
		const fw = this.firmware();
		const f = parseFrames(this.inb)[0];
		if (!f) {
			if (this.inb.length > 4096) this.inb = new Uint8Array(0);
			return 0;
		}
		this.inb = this.inb.slice(f.end);
		if (fw !== "odr") return 1;			// stock firmware doesn't speak the debug link
		const reply = (st, p = new Uint8Array(0)) => this.send(frame(f.cmd | 0x80, f.seq, concat(new Uint8Array([st]), p)));
		if (f.cmd === 0x02) {
			reply(0, new TextEncoder().encode(`proto=1\ngit=sim1234\nboard=${this.boardName}\nfeatures=FM,DMR\ncodec=${this.codecReport()}\n`));
		} else if (f.cmd === 0x1A) {
			if (!this.stubEntry) return reply(3), 1;
			const p = f.payload, op = p[0];
			if (op === 0) reply(0, concat(le32(STUB_MAGIC), le32(0x17000), le32(0x18000), new Uint8Array([59, 0, 0]), le32(this.loaded.length)));
			else if (op === 1) {
				this.total = u32le(p, 5);
				this.crc = u32le(p, 9);
				this.loaded = new Uint8Array(this.total);
				reply(0, le32(0));
			} else if (op === 2) {
				this.loaded.set(p.subarray(5), u32le(p, 1));
				reply(0, le32(u32le(p, 1) + p.length - 5));
			} else if (op === 3) {
				if (crc32(this.loaded) !== this.crc) return reply(4), 1;
				reply(0);
				setTimeout(() => {
					this.state = "stub";
					this.led = "slow";
					this.events.push("stub (from ODR)");
				}, 100);
			}
		} else reply(3);
		return 1;
	}

	stub() {
		const ib = this.inb;
		if (ib.length < 6) return 0;
		if (ib[0] !== 0x02) {
			this.inb = ib.slice(1);
			return 1;
		}
		const op = ib[1], ln = (ib[2] << 8) | ib[3];
		if (ib.length < 6 + ln) return 0;
		const p = ib.slice(6, 6 + ln);
		this.inb = ib.slice(6 + ln);
		if (sum8(p) !== ib[4]) return this.reply(BADSUM, new Uint8Array(0)), 1;
		const mem = (base, flags, cs) => {
			const which = base === SSI_PROG ? this.progmem : this.q128;
			return (flags & 1) && (base === SSI_PROG || ((flags & 2) && cs === 18)) ? which : null;
		};
		if (op === 0x01) this.reply(OK, concat(new TextEncoder().encode("C7STUB"), new Uint8Array([1, 0x04, 0x08]), le32(0x10000)));
		else if (op === 0x02) {
			const a = u32be(p, 0), n = (p[4] << 8) | p[5];
			const now = (-Math.floor(performance.now() * 24000)) >>> 0;
			this.reply(OK, a === TIMER2_VALUE ? le32(now) : new Uint8Array(n));
		} else if (op === 0x03) {
			if (u32be(p, 0) === SYS_SOFT_RSTN) {
				this.events.push("reset");
				this.led = "off";
				this.boot();
				return 1;
			}
			this.reply(OK, new Uint8Array(0));
		} else if (op === 0x05) {
			const m = mem(u32be(p, 0), p[4], p[5]);
			const txn = (p[6] << 8) | p[7], rxn = (p[8] << 8) | p[9], tx = p.subarray(10, 10 + txn);
			let body = new Uint8Array(rxn);
			if (!m) body.fill(0xFF);
			else if (tx[0] === 0x9F) body = new Uint8Array([0xC8, 0x40, m === this.progmem ? 0x14 : 0x18]).subarray(0, rxn);
			else if (tx[0] === 0x03) body = m.slice((tx[1] << 16) | (tx[2] << 8) | tx[3]).subarray(0, rxn);
			else if (tx[0] === 0x06) this.wel = true;
			else if (tx[0] === 0x05) body[0] = this.wel ? 2 : 0;
			else if (tx[0] === 0x20 || tx[0] === 0x02) {
				const a = (tx[1] << 16) | (tx[2] << 8) | tx[3];
				if (this.wel && m === this.progmem && a < 0xC000) throw new Error("simulated radio: a write reached the bootloader");
				if (this.wel) {
					if (tx[0] === 0x20) m.fill(0xFF, a, a + SECTOR);
					else for (let i = 4; i < tx.length; i++) m[a + i - 4] &= tx[i];
					this.writes++;
				}
				this.wel = false;
			}
			this.reply(OK, body);
		} else if (op === 0x07) {
			const m = mem(u32be(p, 0), p[4], p[5]);
			const a = u32be(p, 6), n = (p[10] << 8) | p[11];
			const out = [];
			for (let i = 0; i < n; i++) {
				let at = a + i * SECTOR;
				const k = this.faults.misplace.indexOf(at);
				if (k >= 0) {
					this.faults.misplace.splice(k, 1);
					at += SECTOR;
				}
				const sec = m ? m.subarray(at, at + SECTOR) : new Uint8Array(SECTOR).fill(0xFF);
				out.push(be32(crc32(sec)), new Uint8Array([sec.every((b) => b === 0xFF) ? 1 : 0]));
			}
			this.reply(OK, concat(...out));
		} else if (op === 0x08 || op === 0x0A) {
			if (op === 0x0A) this.led = p[1] ? "sequence" : "slow";
			this.reply(OK, new Uint8Array(0));
		} else if (op === 0x09) {
			const m = mem(u32be(p, 0), p[4], p[5]);
			const a = u32be(p, 6), n = p[10], packed = p[11];
			this.led = "fast";
			for (let i = 0; i < n; i++) {
				let at = a + i * SECTOR;
				const k = this.faults.misplace.indexOf(at);
				if (k >= 0) {
					this.faults.misplace.splice(k, 1);
					at += SECTOR;
				}
				const sec = m ? m.subarray(at, at + SECTOR) : new Uint8Array(SECTOR).fill(0xFF);
				const crc = be32(crc32(sec));
				let rec;
				if (sec.every((b) => b === 0xFF)) rec = concat(crc, new Uint8Array([2, 0, 0]));
				else {
					const z = packed ? encodeBlock(sec) : sec;
					const use = packed && z.length < SECTOR - 16 ? z : sec;
					rec = concat(crc, new Uint8Array([use === z && packed ? 1 : 0, use.length >> 8, use.length & 0xFF]), use);
				}
				this.reply(OK, rec);
			}
		} else this.reply(BADOP, new Uint8Array(0));
		return 1;
	}

	reply(status, body) {
		const f = concat(new Uint8Array([0x02, status, body.length >> 8, body.length & 0xFF, sum8(body), 0x03]), body);
		if (this.faults.flip && body.length > 8 && this.rand() / 255 < this.faults.flip) f[6 + (body.length >> 1)] ^= 0x10;
		this.send(f);
	}
}

// Power the simulated radio on after `ms` (what the user does when the page asks)
export const later = (ms, fn) => sleep(ms).then(fn);
