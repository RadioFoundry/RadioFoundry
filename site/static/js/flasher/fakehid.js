// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A simulated radio in a vendor loader's USB HID update mode, for the tests and the page's practice mode (#sim=<board>
// on a board of the sgl-hid backend): the loader as docs/spec/update-gd77.md reads it (a port of
// hidboot.FakeBootloader), a WebHID model to reach it through, and a synthetic stock update file with a made-up codec in
// the shape of a real build (a port of the rfoundry tests' synthetic image and update file). No vendor data. Also such
// a radio running RadioFoundry with stub entry: its debug link, the hand-over and the K22F stub's protocol and interlock as stub/f4stub.py's FakeF4 has them.

import { UpdateFile, forward, PERIOD, BODY_AT, INFO_LEN, REPORT, parse } from "./hidboot.js";
import { encodeBranch } from "./k22fcodec.js";
import { concat, crc32, fromHex, le32, sha256, u32le } from "./bytes.js";
import { rng, randomBytes } from "./fake.js";
import { frame as dlFrame, parseFrames } from "./link.js";
import { STUB_MAGIC } from "./stub.js";

const bytesOf = (s) => Uint8Array.from(s, (c) => c.charCodeAt(0));
const same = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
const sum = (b) => b.reduce((a, x) => a + x, 0);
const N = bytesOf("N");

// The loader: checks every step in order and the running sums, keeps what it received. reject: a payload it refuses.
export class FakeHidLoader {
	constructor(u, { reject = null } = {}) {
		this.u = u;
		this.q = [];
		this.state = "idle";
		this.mem = new Map();
		this.sum = 0;
		this.reject = reject;
		this.frames = 0;
	}

	write(report) {
		this.frames++;
		const r = this.respond(parse(report));
		const out = new Uint8Array(REPORT);
		out[0] = 3;
		out[2] = r.length & 255;
		out[3] = r.length >> 8;
		out.set(r, 4);
		this.q.push(out);
	}

	read() {
		return this.q.length ? this.q.shift() : null;
	}

	respond(p) {
		const u = this.u, st = this.state;
		if (this.reject && same(p, this.reject)) return N;
		if (st === "idle" && same(p, bytesOf("DOWNLOAD"))) {
			this.state = "start";
			return bytesOf("#UPDATE?");
		}
		const order = [["start", bytesOf("A"), u.sendTag ? "tag" : "fprog"], ["tag", u.tag, "fprog"],
			["fprog", bytesOf("F-PROG\xff\xff"), "company"], ["company", u.company, "model"], ["model", u.model, "version"],
			["version", u.version, "erase"], ["erase", bytesOf("F-ERASE\xff"), "erased"], ["erased", bytesOf("A"), "program"],
			["program", bytesOf("PROGRAM\xff"), "data"]];
		for (const [s, want, nxt] of order) {
			if (st === s && same(p, want)) {
				this.state = nxt;
				return s === "tag" ? p : bytesOf("A");
			}
		}
		if (st === "data") {
			if (same(p.subarray(0, 4), bytesOf("END\xff"))) {
				if (u32le(p, 4) !== this.sum) return N;
				this.sum = 0;
				return bytesOf("A");
			}
			const aw = u.addrWidth, lw = u.lenWidth;
			let addr = 0, n = 0;
			for (let i = 0; i < aw; i++) addr = addr * 256 + p[i];
			for (let i = aw; i < aw + lw; i++) n = n * 256 + p[i];
			const data = p.slice(aw + lw);
			if (data.length !== n || (addr % u.blockSize) + n > u.blockSize) return N;
			this.mem.set(addr, data);
			this.sum = (this.sum + sum(data)) >>> 0;
			return bytesOf("A");
		}
		return N;
	}

	received() {
		const out = new Uint8Array(this.u.length);
		for (const [a, d] of this.mem) out.set(d, a);
		return out;
	}
}

// navigator.hid as the page uses it, with one radio in update mode behind it (a FakeHidLoader for the file it will
// be sent). requestDevice() hands out the radio while `present`; replies come as "inputreport" events.
export class FakeHid {
	constructor(loader, { present = true, delay = 0 } = {}) {
		this.loader = loader;
		this.present = present;
		this.delay = delay;
		this.listeners = new Set();
		const self = this;
		this.device = {
			vendorId: 0x15A2, productId: 0x0073, productName: "simulated update mode", opened: false,
			async open() { this.opened = true; },
			async close() { this.opened = false; },
			addEventListener: (type, fn) => { if (type === "inputreport") self.listeners.add(fn); },
			removeEventListener: (type, fn) => { if (type === "inputreport") self.listeners.delete(fn); },
			async sendReport(id, data) {
				if (!this.opened) throw new Error("device not open");
				self.loader.write(Uint8Array.of(id, ...data));
				const r = self.loader.read();
				if (!r) return;
				const fire = () => { for (const fn of self.listeners) fn({ reportId: r[0], data: new DataView(r.buffer, 1) }); };
				if (self.delay) setTimeout(fire, self.delay);
				else queueMicrotask(fire);
			},
		};
	}

	async requestDevice({ filters }) {
		const ok = filters.some((f) => f.vendorId === this.device.vendorId && f.productId === this.device.productId);
		return ok && this.present ? [this.device] : [];
	}
}

// ---------------------------------------------------------------------------------------------------------------
// a synthetic stock update file

const FLASH_BASE = 0x4000;
const bl = (at, target, condB = false) => {
	const [h1, h2] = encodeBranch(target - (at + 4), condB ? 0x9000 : 0xD000);
	return Uint8Array.of(h1 & 255, h1 >> 8, h2 & 255, h2 >> 8);
};

// A flat stock image (from FLASH_BASE) and a spec for it, in the shape of a real build: main 0x54000 (0x2000 bytes),
// low 0x4400 (0x400), table window 0x68000 (0x1000); a code pointer, branches between the parts and one inside main
export async function synthetic(seed = 77) {
	const size = 0x7B800 - FLASH_BASE;
	const img = new Uint8Array(size);
	const block = randomBytes(rng(seed), 0x1000);
	for (let o = 0; o < size; o += 0x1000) img.set(block.subarray(0, Math.min(0x1000, size - o)), o);
	const put = (addr, data) => img.set(data, addr - FLASH_BASE);
	put(0x54100, Uint8Array.of(0x01, 0x45, 0x05, 0x00));	// a function pointer into main (Thumb)
	put(0x54200, bl(0x54200, 0x4500));
	put(0x4410, bl(0x4410, 0x54300, true));
	put(0x54210, bl(0x54210, 0x68040));
	put(0x68040, bl(0x68040, 0x54180));
	put(0x54220, bl(0x54220, 0x54010));
	const names = ["encode", "decode", "enc_init", "fec_init", "dec_init", "enc_params", "enc_set", "dec_set"];
	const spec = { profile: "synthetic", stock: { name: "Practice build" }, entries: Object.fromEntries(names.map((k, i) => [k, 0x100 + 4 * i])),
		main: { flash: 0x54000, len: 0x2000 }, low: { flash: 0x4400, len: 0x400 }, tab: { flash: 0x68000, len: 0x1000 },
		words: [[0x54100, 0x54501]], branches: [[0x54200, 0x4500], [0x4410, 0x54300], [0x54210, 0x68040], [0x68040, 0x54180]] };
	for (const p of ["main", "low", "tab"]) spec[p].sha256 = await sha256(img.subarray(spec[p].flash - FLASH_BASE, spec[p].flash - FLASH_BASE + spec[p].len));
	return { image: img, spec };
}

// A valid update file around `plain` (the flat image), with the header fields a vendor's file has, for radio model
export function makeUpdateFile(plain, { total = 0x78801 + 0x400, tag = "DV01test", seed = 5, model = "XR-100" } = {}) {
	const k = forward(randomBytes(rng(seed), 16), PERIOD);
	const pad = 3, infoAt = 0x100, k0 = 0x5A, k1 = 0xA5;
	const bodyAt = BODY_AT + pad;
	const n = total - bodyAt;
	const image = new Uint8Array(n).fill(0xFF);
	image.set(plain.subarray(0, n - 32));
	image.fill(0, n - 32);
	const anchor = total - 32;
	const body = new Uint8Array(n);
	for (let j = 0; j < n; j++) body[j] = (((image[j] >> 3) | (image[j] << 5)) & 255) ^ k[(((bodyAt + j - anchor) % PERIOD) + PERIOD) % PERIOD];
	const info = new Uint8Array(INFO_LEN);
	const dv = new DataView(info.buffer);
	dv.setUint32(6, n, true);
	dv.setUint32(10, ~sum(body) >>> 0, true);
	const field = (at, len, s) => { info.fill(0xFF, at, at + len); info.set(bytesOf(s).subarray(0, len), at); };
	field(50, 16, "SG-" + model);
	field(66, 8, model);
	field(74, 8, "V1.00.01");
	info[91] = 0x35;
	info[92] = 0xF4;
	dv.setUint16(93, 0x20, true);
	info.set(bytesOf(tag).subarray(0, 8), 95);
	const out = new Uint8Array(total);
	out.set(bytesOf("SGL!"));
	const h = [...bytesOf("ENCV001"), pad, infoAt & 255, infoAt >> 8, k0, k1];
	const KEY = bytesOf("SGL!");
	h.forEach((c, i) => { out[4 + i] = c ^ KEY[(4 + i) % 4]; });
	for (let i = 0; i < INFO_LEN; i++) out[infoAt + i] = info[i] ^ (i % 2 === 0 ? k0 : k1);
	out.set(body, bodyAt);
	return out;
}

// Practice mode: the data the page reads with the synthetic build added as a supported one, and its update file
// (named for the radio model)
export async function hidSimulation(G, model) {
	const { image, spec } = await synthetic();
	return { hid: { ...G, specs: { ...G.specs, synthetic: spec }, placed: { ...G.placed, synthetic: { tag: 0x1234 } } },
		donor: makeUpdateFile(image, model ? { model } : {}) };
}

export const loaderFor = (file, opts) => new FakeHidLoader(new UpdateFile(file), opts);

// ---------------------------------------------------------------------------------------------------------------
// a radio running RadioFoundry with stub entry, and the K22F stub it hands over to (#sim=<board>)

const be32 = (n) => new Uint8Array([(n >>> 24) & 0xFF, (n >>> 16) & 0xFF, (n >>> 8) & 0xFF, n & 0xFF]);
const u32be = (b, o) => ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0;
const blank = (b) => b.every((x) => x === 0xFF);
const OK = 0x05, BADSUM = 0x0A, BADOP = 0x0B, BADARG = 0x0D, REFUSED = 0x0F, NOCONFIG = 0x11;

// The radio: its loader below the application base, an RadioFoundry already installed, a codec in its area, radio data in the
// last sector, the SPI flash's first megabyte the radio's own. st: flash-boards.json hid.stub; board: one of its boards (the first by default).
//   codecId    what the debug link reports as the codec (the board's id by default)
//   stubEntry  RadioFoundry built with CONFIG_STUB_ENTRY        adopt     the stub keeps RadioFoundry's USB device (else a new port)
//   cutAfter   programs before the cable is pulled     badAt     a flash address that programs wrong (verify fails)
export class FakeHidRFoundry {
	constructor(st, { board = Object.keys(st.boards)[0], spiSize = 0x200000, stubEntry = true, adopt = true, cutAfter = null, badAt = null, codecId = board, seed = 77 } = {}) {
		this.st = st;
		this.b = st.boards[board];
		this.flash = new Uint8Array(st.flash_kb * 1024).fill(0xFF);
		const r = rng(seed);
		this.flash.set(randomBytes(r, st.app_base), 0);
		const app = randomBytes(r, 0x30000).map((x) => (x < 90 ? x : 0));
		app.set(le32(0x2000FEF0), 0);
		this.flash.set(app, st.app_base);
		this.flash.set(randomBytes(r, this.b.codec_image_end - this.b.codec[0]), this.b.codec[0]);
		this.flash.set(randomBytes(r, st.sector), this.flash.length - st.sector);
		this.spi = new Uint8Array(spiSize).fill(0xFF);
		this.spi.set(randomBytes(r, 0x100000), 0);
		this.mode = "rfoundry";		// "rfoundry", "stub", or "loader" (no application committed: no serial port)
		this.record = new Uint8Array(0);
		this.git = "1c2d3e4f";
		Object.assign(this, { board, stubEntry, adopt, cutAfter, badAt, codecId });
		this.programs = 0;
		this.events = [];
		this.port = 0;
	}

	word(addr) { return u32le(this.flash, addr - this.st.flash_base); }

	// ---- the serial port, as a link.js stream (the same USB identity for RadioFoundry and the stub)

	serialStream() {
		const radio = this, port = this.port;
		const s = {
			buf: new Uint8Array(0), inb: new Uint8Array(0),
			clear() { s.buf = new Uint8Array(0); },
			take(n) { const o = s.buf.slice(0, n); s.buf = s.buf.slice(n); return o; },
			async write(data) {
				if (port !== radio.port) throw new Error("the device has been lost");
				if (radio.mode === "stub") {
					if (radio.cutAfter !== null && radio.programs >= radio.cutAfter) throw new Error("the device has been lost");
					s.inb = radio.stubFeed(concat(s.inb, data), (b) => { s.buf = concat(s.buf, b); });
					return;
				}
				if (radio.mode !== "rfoundry") throw new Error("no serial port: the radio isn't running RadioFoundry");
				s.buf = concat(s.buf, radio.rfoundryAnswer(data));
			},
			async close() {},
		};
		return s;
	}

	// RadioFoundry's debug link: VERSION, and STUB (0x1A) with the K22F's run address as the load address
	rfoundryAnswer(req) {
		const f = parseFrames(req)[0];
		if (!f) return new Uint8Array(0);
		const ok = (p) => dlFrame(f.cmd | 0x80, f.seq, concat(new Uint8Array([0]), p));
		const fail = (st) => dlFrame(f.cmd | 0x80, f.seq, new Uint8Array([st]));
		if (f.cmd === 0x02) {
			const name = this.b.debuglink_names[0];
			return ok(new TextEncoder().encode(`proto=1\ngit=${this.git}\nboard=${name}\nfeatures=FM,DMR\ncodec=${this.codecId}\ndisplay=128x64x1\n`));
		}
		if (f.cmd === 0x1A && this.stubEntry) {
			const op = f.payload[0], q = f.payload;
			if (op === 0) return ok(concat(le32(STUB_MAGIC), le32(this.st.run), le32(8192), new Uint8Array([59, 0, 0]), le32(0)));
			if (op === 1) { this.stubLen = u32le(q, 5); this.stubCrc = u32le(q, 9); this.stubEntryAt = u32le(q, 13); this.loaded = new Uint8Array(this.stubLen); return ok(le32(0)); }
			if (op === 2) { this.loaded.set(q.subarray(5), u32le(q, 1)); return ok(le32(u32le(q, 1) + q.length - 5)); }
			if (op === 3) {
				if (!this.loaded || crc32(this.loaded) !== this.stubCrc || this.stubEntryAt !== this.st.run) return fail(4);
				this.mode = "stub";
				this.record = new Uint8Array(0);
				this.events.push("stub (from RadioFoundry)");
				if (!this.adopt) this.port++;	// the old port is gone; serialStream() is the new one
				return ok(new Uint8Array(0));
			}
			return fail(3);
		}
		return fail(1);
	}

	// ---- the stub (stub/common/stubeng.c's rules, as FakeF4 with k22stub.K22)

	stubFeed(inb, send) {
		while (inb.length >= 6) {
			if (inb[0] !== 0x02) { inb = inb.slice(1); continue; }
			const n = (inb[2] << 8) | inb[3];
			if (inb.length < 6 + n) break;
			const op = inb[1], p = inb.slice(6, 6 + n);
			const bad = inb.subarray(6, 6 + n).reduce((a, x) => (a + x) & 0xFF, 0) !== inb[4];
			inb = inb.slice(6 + n);
			const reply = (st, body = new Uint8Array(0)) => send(concat(new Uint8Array([0x02, st, body.length >> 8, body.length & 0xFF,
				body.reduce((a, x) => (a + x) & 0xFF, 0), 0x03]), body));
			if (bad) reply(BADSUM);
			else this.stubOp(op, p, reply);
		}
		return inb;
	}

	// {appBase, appEnd, xfloor} of a board record the stub takes (k22stub.py K22Chip.parse_record), or null
	parse(rec) {
		if (rec.length !== this.st.record_len || rec[0] === 0 || rec[1] !== 1 || rec.subarray(20, 24).some((x) => x)) return null;
		return { appBase: u32be(rec, 4), appEnd: u32be(rec, 8) || this.flash.length, xfloor: u32be(rec, 12) };
	}

	// HELLO v4 (stub-k22f.md 1): name, protocol, max payload, flash KB, the security byte, flags, application base, SPI
	// size and JEDEC ID, then the board id, the record length and the record
	hello() {
		const rec = this.record, on = rec.length > 0;
		return concat(new TextEncoder().encode(this.st.name), new Uint8Array([this.st.protocol, 4104 >> 8, 4104 & 0xFF, this.st.flash_kb >> 8,
			this.st.flash_kb & 0xFF, 0x55, (this.adopt ? 1 : 0) | (on ? 0x80 : 0)]), be32(on ? this.parse(rec).appBase : 0),
		be32(on ? this.spi.length : 0), new Uint8Array([0xEF, 0x40, 0x18, on ? rec[0] : 0, rec.length]), rec);
	}

	stubOp(op, p, reply) {
		const st = this.st, BLOCK = 4096;
		if ([0x10, 0x11, 0x12, 0x13].includes(op) && !this.record.length) return reply(NOCONFIG);
		const c = this.record.length ? this.parse(this.record) : null;
		const appBlank = () => c && this.word(c.appBase) === 0xFFFFFFFF;
		const commitEnd = () => c.appBase - (c.appBase % st.sector) + st.sector;
		const mem = (addr) => (p.length > 6 && p[6] === 1 ? [this.spi, addr] : [this.flash, addr - st.flash_base]);
		if (op === 0x01) return reply(OK, this.hello());
		if (op === 0x14) {
			const got = this.parse(p);
			const ok = got && got.appBase === st.app_base;
			this.record = ok ? p.slice() : new Uint8Array(0);
			this.events.push(ok ? "config" : "config refused");
			return reply(ok ? OK : BADARG);
		}
		if (op === 0x07) {
			const a = u32be(p, 0), k = (p[4] << 8) | p[5];
			const [m, o] = mem(a);
			const body = new Uint8Array(5 * k);
			for (let i = 0; i < k; i++) {
				const blk = m.subarray(o + i * BLOCK, o + (i + 1) * BLOCK);
				body.set(be32(crc32(blk)), i * 5);
				body[i * 5 + 4] = blank(blk) ? 1 : 0;
			}
			return reply(OK, body);
		}
		if (op === 0x09) {
			const a = u32be(p, 0), k = p[4];
			const [m, o0] = mem(a);
			for (let i = 0; i < k; i++) {
				const blk = m.subarray(o0 + i * BLOCK, o0 + (i + 1) * BLOCK);
				const data = blank(blk) ? new Uint8Array(0) : blk;
				reply(OK, concat(be32(crc32(blk)), new Uint8Array([blank(blk) ? 2 : 0, data.length >> 8, data.length & 0xFF]), data));
			}
			return;
		}
		if (op === 0x02) {
			const a = u32be(p, 0), n = (p[4] << 8) | p[5];
			return reply(OK, this.flash.slice(a - st.flash_base, a - st.flash_base + n));
		}
		if (op === 0x10) {
			const s = p[0], a = st.flash_base + s * st.sector;
			const commit = a <= c.appBase && c.appBase < a + st.sector;
			if (u32be(p, 1) !== st.magic || a < c.appBase || a >= c.appEnd || (!commit && !appBlank())) return reply(REFUSED);
			this.flash.fill(0xFF, a - st.flash_base, a - st.flash_base + st.sector);
			this.events.push(`erase ${s}`);
			return reply(OK);
		}
		if (op === 0x11) {
			const a = u32be(p, 0), data = p.subarray(4), u = st.prog_unit;
			if (a % u || data.length % u || a < c.appBase || a + data.length > c.appEnd || (a + data.length > commitEnd() && !appBlank())) return reply(REFUSED);
			const o = a - st.flash_base;
			if (!blank(this.flash.subarray(o, o + data.length))) return reply(REFUSED);
			this.flash.set(data, o);
			if (this.badAt !== null && this.badAt >= a && this.badAt < a + data.length) this.flash[this.badAt - st.flash_base] ^= 0x01;
			this.programs++;
			this.events.push(`program 0x${a.toString(16)}`);
			return reply(OK);
		}
		if (op === 0x12) {
			const a = u32be(p, 0);
			if (u32be(p, 4) !== st.magic || a % BLOCK || a < c.xfloor || a >= this.spi.length) return reply(REFUSED);
			this.spi.fill(0xFF, a, a + BLOCK);
			return reply(OK);
		}
		if (op === 0x13) {
			const a = u32be(p, 0), data = p.subarray(4);
			if (a < c.xfloor || !blank(this.spi.subarray(a, a + data.length))) return reply(REFUSED);
			this.spi.set(data, a);
			return reply(OK);
		}
		if (op === 0x0D) {
			if (u32be(p, 0) !== st.magic) return reply(REFUSED);
			reply(OK);
			this.events.push("reset");
			return this.powerOn();
		}
		return reply(BADOP);
	}

	// A reset or power cycle: RadioFoundry when the application's first word is there; else the loader finds no application
	powerOn() {
		this.port++;
		this.record = new Uint8Array(0);
		this.mode = this.word(this.st.app_base) === 0xFFFFFFFF ? "loader" : "rfoundry";
	}
}

// The board record of a board of hid.stub as bytes (the first by default)
export const recordOf = (st, board = Object.keys(st.boards)[0]) => fromHex(st.boards[board].record);
