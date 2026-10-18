// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// A simulated radio for the tests and the flasher's "simulated radio" mode: the AnyRoad bootloader with a model of
// the STM32F405 flash (a port of tools/odrflash/odrflash/fakedfu.py: erase sets a sector to 0xFF, programming only
// clears bits), and ODR's USB serial port answering the debug link and the CPS reads from the same flash and an SPI
// flash. A synthetic stock firmware file and key stand in for the real ones, which are never part of ODR. A radio
// running ODR also hands over to the STM32F4 stub (debug link STUB, then the stub's protocol and interlock as
// stub/f4stub.py's FakeF4 has them).

import { BLOCK, FIRST_BLOCK } from "./plan.js";
import { concat, crc32, le32, sha256, u16be, u32le } from "./bytes.js";
import { frame, parseFrames } from "./link.js";
import { STUB_MAGIC } from "./stub.js";
import { APP_BASE, F4_MAGIC, f405Sectors } from "./f4stub.js";

const FLASH_BASE = 0x08000000, FLASH_SIZE = 0x100000;
const sum8 = (b) => b.reduce((a, x) => (a + x) & 0xFF, 0);
const be32 = (n) => new Uint8Array([(n >>> 24) & 0xFF, (n >>> 16) & 0xFF, (n >>> 8) & 0xFF, n & 0xFF]);
const u32be = (b, o) => ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0;

export function rng(seed) {
	let s = seed >>> 0 || 1;
	return () => {
		s ^= s << 13; s >>>= 0;
		s ^= s >>> 17;
		s ^= s << 5; s >>>= 0;
		return s & 0xFF;
	};
}

export function randomBytes(r, n) {
	const out = new Uint8Array(n);
	for (let i = 0; i < n; i++) out[i] = r();
	return out;
}

// A synthetic stock file shaped like a real profile, the board and profiles made to accept it. Returns
// {fb, board, key, stock, codecSegments}: fb is a copy of flash-boards.json whose board and profile match the key.
export async function simulation(fbReal, boardId = "dm1701", seed = 1701) {
	const fb = structuredClone(fbReal);
	const board = fb.boards[boardId];
	const r = rng(seed);
	const key = randomBytes(r, fb.key_len);
	board.key_sha256 = await sha256(key);
	const realSha = Object.keys(fb.profiles).find((h) => fb.profiles[h].board === boardId);
	const real = fb.profiles[realSha];
	const payloadLen = real.segments.reduce((a, s) => a + s[1], 0);
	const plain = randomBytes(r, payloadLen).map((b) => (b < 100 ? b : 0));	// firmware-like: mostly zero
	const appOff = real.segments.filter(([base]) => base < board.app_base).reduce((a, s) => a + s[1], 0);
	const windows = [], codecSegments = [];
	for (const [i, [addr, length]] of real.windows.entries()) {
		const data = randomBytes(r, length);
		if (i === 0) data.set(le32(fb.codec_sig), 0);
		plain.set(data, appOff + addr - board.app_base);
		windows.push([addr, length, await sha256(data)]);
		codecSegments.push([addr, data]);
	}
	const header = new Uint8Array(fb.header_len);
	for (let i = 0; i < fb.magic.length; i++) header[i] = fb.magic.charCodeAt(i);
	header.set(le32(real.segments.length > 1 ? real.segments.length : fb.single_segment), 0x7C);
	real.segments.forEach(([a, n], i) => { header.set(le32(a), 0x80 + 8 * i); header.set(le32(n), 0x84 + 8 * i); });
	const enc = plain.map((b, i) => b ^ key[i % fb.key_len]);
	const stock = concat(header, enc, randomBytes(r, 0x100));
	const sha = await sha256(stock);
	delete fb.profiles[realSha];
	fb.profiles[sha] = { ...real, windows, size: stock.length, key_sha256: board.key_sha256, name: real.name + " (simulated)" };
	fb.known_files[sha] = real.version + " (simulated)";
	return { fb, board, key, stock, codecSegments };
}

// ---------------------------------------------------------------------------------------------------------------

export class FakeRadio {
	// state: "stock" (stock firmware: codec in place, no serial port), "odr", "odr-nocodec", "bootloader".
	// stubEntry: ODR built with CONFIG_STUB_ENTRY; adopt: the stub keeps ODR's USB device (else the port goes away
	// and serialStream() gives the new one); cutAfter: stub programs before the "cable" is pulled
	constructor(sim, { state = "stock", spiSize = 0x1000000, stubEntry = true, adopt = true, cutAfter = null } = {}) {
		this.sim = sim;
		this.board = sim.board;
		this.key = sim.key;
		this.flash = new Uint8Array(FLASH_SIZE);
		const r = rng(380);
		for (let i = 0; i < FLASH_SIZE; i += 4096) this.flash.set(randomBytes(r, 64), i);	// "stock code"
		for (const [a, d] of sim.codecSegments) this.flash.set(d, a - FLASH_BASE);
		this.spi = new Uint8Array(spiSize).fill(0xFF);
		this.spi.set(randomBytes(r, 0x40000), 0);		// a stock codeplug where ODR's settings go
		this.spi.set(randomBytes(r, 0x5D400), 0x60000);		// stock resources
		this.flash.set(this.key, 0x2340);		// the bootloader holds its key (anywhere: found by its hash)
		this.security = randomBytes(r, 0x300);
		this.uid = randomBytes(r, 12);
		this.running = state === "bootloader" ? "stock" : state;
		this.mode = state === "bootloader" ? "bootloader" : "normal";
		this.git = "5f034440";
		this.stubEntry = stubEntry;
		this.adopt = adopt;
		this.cutAfter = cutAfter;
		this.programs = 0;
		this.events = [];
		this.port = 0;
		this.resetDfu();
		this.log = [];
	}

	resetDfu() {
		this.state = 2;
		this.busy = 0;
		this.addr = null;
		this.vendor = [];
		this.erased = [];
		this.blocks = 0;
		this.violations = [];
		this.exited = false;
	}

	enterBootloader() { this.mode = "bootloader"; this.resetDfu(); }

	codecReport() {
		if (this.running === "stock") return null;
		const ok = this.sim.codecSegments.every(([a, d]) => {
			const f = this.flash.subarray(a - FLASH_BASE, a - FLASH_BASE + d.length);
			return crc32(f) === crc32(d);
		});
		return ok ? this.board.codec.id : "absent";
	}

	// ---- the bootloader, as a dfu.js transport

	dfuTransport() {
		const radio = this;
		return {
			async out(req, value, data) { return radio.dfuOut(req, value, data || new Uint8Array(0)); },
			async in(req, length) { return radio.dfuIn(req, length); },
			async fingerprint() {
				return { string1: radio.board.dfu_string1, manufacturer: "STMicroelectronics", product: "Digital Radio in USB mode",
					serial: "SIMULATED", version: "2.0.0", interfaces: ["@Internal Flash  /0x08000000/03*016Kg,01*016Kg,01*064Kg,07*128Kg"] };
			},
			async close() {},
		};
	}

	dfuIn(req) {
		if (this.mode !== "bootloader") throw new Error("the radio is not in its bootloader");
		let st = this.state;
		if (this.busy) { this.busy--; st = 4; }
		return new Uint8Array([0, 1, 0, 0, st, 0]);
	}

	dfuOut(req, value, data) {
		if (this.mode !== "bootloader") throw new Error("the radio is not in its bootloader");
		if (req === 4 || req === 6) { this.state = 2; return 0; }
		if (req !== 1) throw new Error(`unexpected request ${req}`);
		this.state = 5;
		this.busy = 1;
		const sectorAt = (a) => this.board.sectors.find((s) => s.addr <= a && a < s.addr + s.size);
		if (value === 0) {
			if (!data.length) {
				if (this.addr !== 0x08000000) throw new Error("exit without set address 0x08000000");
				this.exited = true;
				this.mode = "normal";
				// what boots next: ODR unless a test says it wrote stock firmware back
				this.running = this.afterExit || "odr";
				if (this.running === "odr" && this.codecReport() === "absent") this.running = "odr-nocodec";
			} else if (data[0] === 0x91) {
				this.vendor.push(data[1]);
				this.busy = 0;
			} else if (data[0] === 0x41) {
				const a = u32le(data, 1);
				const sec = sectorAt(a);
				if (!sec || sec.addr !== a) throw new Error(`erase of ${a.toString(16)}, not a sector start`);
				if (this.vendor.join() !== "1,49") throw new Error("erase before the vendor commands");
				this.flash.fill(0xFF, a - FLASH_BASE, a - FLASH_BASE + sec.size);
				this.erased.push(a);
			} else if (data[0] === 0x21) {
				this.addr = u32le(data, 1);
			} else throw new Error("unknown command");
		} else {
			if (value < FIRST_BLOCK || data.length !== BLOCK) throw new Error("bad block");
			const dest = this.addr + (value - FIRST_BLOCK) * BLOCK;
			const sec = sectorAt(this.addr);
			if (!sec || sec.addr !== this.addr || dest + BLOCK > sec.addr + sec.size) throw new Error("block outside its sector");
			const off = dest - this.board.app_base;
			for (let i = 0; i < BLOCK; i++) {
				const plain = data[i] ^ this.key[(off + i) % this.key.length];
				const o = dest - FLASH_BASE + i, old = this.flash[o];
				if (old !== 0xFF && plain !== 0xFF && old !== plain) this.violations.push(dest + i);
				this.flash[o] = old & plain;
			}
			this.blocks++;
		}
		return data.length;
	}

	// ---- ODR's serial port, as a link.js stream

	serialStream() {
		const radio = this;
		const port = this.port;
		const s = {
			buf: new Uint8Array(0),
			inb: new Uint8Array(0),
			clear() { s.buf = new Uint8Array(0); },
			take(n) { const o = s.buf.slice(0, n); s.buf = s.buf.slice(n); return o; },
			async write(data) {
				if (port !== radio.port) throw new Error("the device has been lost");
				if (radio.mode === "stub") {
					if (radio.cutAfter !== null && radio.programs >= radio.cutAfter) throw new Error("the device has been lost");
					s.inb = concat(s.inb, data);
					s.inb = radio.stubFeed(s.inb, (b) => { s.buf = concat(s.buf, b); });
					return;
				}
				if (radio.mode !== "normal" || !radio.running.startsWith("odr")) throw new Error("no serial port: the radio isn't running ODR");
				s.buf = concat(s.buf, radio.serialAnswer(data));
			},
			async close() {},
		};
		return s;
	}

	// ---- the STM32F4 stub (stub/stm32f4/f4stub_proto.c's rules)

	word(addr) { return u32le(this.flash, addr - FLASH_BASE); }

	stubFeed(inb, send) {
		while (inb.length >= 6) {
			if (inb[0] !== 0x02) { inb = inb.slice(1); continue; }
			const n = (inb[2] << 8) | inb[3];
			if (inb.length < 6 + n) break;
			const op = inb[1], p = inb.slice(6, 6 + n);
			const bad = sum8(p) !== inb[4];
			inb = inb.slice(6 + n);
			const reply = (st, body = new Uint8Array(0)) => send(concat(new Uint8Array([0x02, st, body.length >> 8, body.length & 0xFF, sum8(body), 0x03]), body));
			if (bad) reply(0x0A);
			else this.stubOp(op, p, reply);
		}
		return inb;
	}

	stubOp(op, p, reply) {
		const OK = 0x05, REFUSED = 0x0F;
		const mem = (baseLen, addr) => (p.length > baseLen && p[baseLen] === 1 ? [this.spi, addr] : [this.flash, addr - FLASH_BASE]);
		const appBlank = () => this.word(APP_BASE) === 0xFFFFFFFF;
		if (op === 0x01) {
			reply(OK, concat(new TextEncoder().encode("F4STUB"), new Uint8Array([2, 0x10, 0x08, 0x04, 0x00, 0x55, this.adopt ? 1 : 0]), be32(APP_BASE),
				be32(this.spi.length), new Uint8Array([0xEF, 0x40, 0x18])));
		} else if (op === 0x07) {
			const a = u32be(p, 0), k = (p[4] << 8) | p[5];
			const [m, o] = mem(6, a);
			const body = new Uint8Array(5 * k);
			for (let i = 0; i < k; i++) {
				const blk = m.subarray(o + i * 4096, o + (i + 1) * 4096);
				body.set(be32(crc32(blk)), i * 5);
				body[i * 5 + 4] = blk.every((x) => x === 0xFF) ? 1 : 0;
			}
			reply(OK, body);
		} else if (op === 0x09) {
			const a = u32be(p, 0), k = p[4];
			const [m, o0] = mem(6, a);
			for (let i = 0; i < k; i++) {
				const blk = m.subarray(o0 + i * 4096, o0 + (i + 1) * 4096);
				const blank = blk.every((x) => x === 0xFF);
				const data = blank ? new Uint8Array(0) : blk;
				reply(OK, concat(be32(crc32(blk)), new Uint8Array([blank ? 2 : 0, data.length >> 8, data.length & 0xFF]), data));
			}
		} else if (op === 0x10) {
			const sec = f405Sectors()[p[0]];
			if (u32be(p, 1) !== F4_MAGIC || p[0] < 3 || !sec || (p[0] !== 3 && !appBlank())) return reply(REFUSED);
			this.flash.fill(0xFF, sec.addr - FLASH_BASE, sec.addr - FLASH_BASE + sec.size);
			this.events.push(`erase ${p[0]}`);
			reply(OK);
		} else if (op === 0x11) {
			const a = u32be(p, 0), data = p.subarray(4), o = a - FLASH_BASE;
			if (a < APP_BASE || (a + data.length > 0x08010000 && !appBlank())) return reply(REFUSED);
			if (!this.flash.subarray(o, o + data.length).every((x) => x === 0xFF)) return reply(REFUSED);
			this.flash.set(data, o);
			this.programs++;
			reply(OK);
		} else if (op === 0x12) {
			const a = u32be(p, 0);
			if (u32be(p, 4) !== F4_MAGIC || a % 4096 || a >= this.spi.length) return reply(REFUSED);
			this.spi.fill(0xFF, a, a + 4096);
			reply(OK);
		} else if (op === 0x13) {
			const a = u32be(p, 0), data = p.subarray(4);
			if (!this.spi.subarray(a, a + data.length).every((x) => x === 0xFF)) return reply(REFUSED);
			this.spi.set(data, a);
			reply(OK);
		} else if (op === 0x0D) {
			if (u32be(p, 0) !== F4_MAGIC) return reply(REFUSED);
			reply(OK);
			this.events.push("reset");
			this.powerOn();
		} else reply(0x0B);
	}

	// A reset or power cycle: the bootloader stays in DFU while the application's first word is erased
	powerOn() {
		this.port++;
		if (this.word(APP_BASE) === 0xFFFFFFFF) { this.mode = "bootloader"; this.resetDfu(); return; }
		this.mode = "normal";
		this.running = this.codecReport() === "absent" ? "odr-nocodec" : "odr";
	}

	serialAnswer(req) {
		if (req[0] === 0xA5) {
			const f = parseFrames(req)[0];
			const ok = (p) => frame(f.cmd | 0x80, f.seq, concat(new Uint8Array([0]), p));
			if (f.cmd === 0x02) {
				const text = `proto=1\ngit=${this.git}\nboard=${this.board.debuglink_names[0]}\nfeatures=FM,DMR\ncodec=${this.codecReport()}\ndisplay=160x120x16\n`;
				return ok(new TextEncoder().encode(text));
			}
			if (f.cmd === 0x0B) {
				const p = new Uint8Array(28);
				p.set(le32(0x0009A413), 0); p[4] = 0x00; p[5] = 0x04; p.set(this.uid, 6); p.set(le32(0x0FFF55ED), 18); p[22] = 1; p[23] = 0x55;
				return ok(p);
			}
			if (f.cmd === 0x1A && this.stubEntry) {
				const op = f.payload[0], q = f.payload;
				if (op === 0) return ok(concat(le32(STUB_MAGIC), le32(0x2001E000), le32(8192), new Uint8Array([59, 0, 0]), le32(0)));
				if (op === 1) { this.stubLen = u32le(q, 5); this.stubCrc = u32le(q, 9); this.stubEntryAt = u32le(q, 13); this.loaded = new Uint8Array(this.stubLen); return ok(le32(0)); }
				if (op === 2) { this.loaded.set(q.subarray(5), u32le(q, 1)); return ok(le32(u32le(q, 1) + q.length - 5)); }
				if (op === 3) {
					if (!this.loaded || crc32(this.loaded) !== this.stubCrc || this.stubEntryAt !== 0x20000000) return frame(f.cmd | 0x80, f.seq, new Uint8Array([4]));
					this.mode = "stub";
					this.events.push("stub (from ODR)");
					if (!this.adopt) this.port++;	// the old port is gone; serialStream() is the new one
					return ok(new Uint8Array(0));
				}
				return frame(f.cmd | 0x80, f.seq, new Uint8Array([3]));
			}
			if (f.cmd === 0x0C) {
				const addr = u32le(f.payload, 0), n = f.payload[4] | (f.payload[5] << 8);
				if (n < 1 || n > 1000 || addr < FLASH_BASE || addr + n > FLASH_BASE + FLASH_SIZE) return frame(f.cmd | 0x80, f.seq, new Uint8Array([7]));
				return ok(concat(le32(addr), this.flash.subarray(addr - FLASH_BASE, addr - FLASH_BASE + n)));
			}
			return frame(f.cmd | 0x80, f.seq, new Uint8Array([1]));
		}
		if (req[0] === 0x52) {
			// ODR answers up to 32 KB per read, older builds 2045 bytes (open-firmware-cps.md 2.1)
			const area = req[1], addr = (req[2] << 24 | req[3] << 16 | req[4] << 8 | req[5]) >>> 0, len = Math.min(u16be(req, 6), this.readMax || 32768);
			let src = null, a = addr;
			if (area === 1) src = this.spi;
			else if (area === 5) src = this.flash;
			else if (area === 10) { src = this.security; a = ((addr >> 12) - 1) * 0x100 + (addr & 0xFFF); }
			if (!src || a + len > src.length || len === 0) return new Uint8Array([0x2D]);
			return concat(new Uint8Array([0x52, len >> 8, len & 0xFF]), src.subarray(a, a + len));
		}
		if (req[0] === 0x58) {
			if (req[1] === 1) { this.pending = ((req[2] << 16 | req[3] << 8 | req[4]) >>> 0) * 4096; this.sector = this.spi.slice(this.pending, this.pending + 4096); return req.subarray(0, 2); }
			if (req[1] === 2 && this.sector) {
				const addr = (req[2] << 24 | req[3] << 16 | req[4] << 8 | req[5]) >>> 0, len = u16be(req, 6);
				this.sector.set(req.subarray(8, 8 + len), addr - this.pending);
				return req.subarray(0, 2);
			}
			if (req[1] === 3 && this.sector) { this.spi.set(this.sector, this.pending); this.sector = null; return req.subarray(0, 2); }
			return new Uint8Array([0x2D]);
		}
		if (req[0] === 0x43) return new Uint8Array([0x2D]);
		return new Uint8Array([0x2D]);
	}
}

