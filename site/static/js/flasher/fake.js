// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A simulated radio for the tests and the flasher's "simulated radio" mode: the AnyRoad bootloader with a model of
// the STM32F405 flash (a port of tools/rfoundry/rfoundry/fakedfu.py: erase sets a sector to 0xFF, programming only
// clears bits), and RadioFoundry's USB serial port answering the debug link and the CPS reads from the same flash and an SPI
// flash. A synthetic stock firmware file and key stand in for the real ones, which are never part of RadioFoundry. A radio
// running RadioFoundry also hands over to the STM32F4 stub (debug link STUB, then the stub's protocol, its board record
// (CONFIG) and its interlock as stub/f4stub.py's FakeF4 and docs/spec/stub-stm32f4.md 5.2 have them).

import { BLOCK, FIRST_BLOCK } from "./plan.js";
import { concat, crc32, equal, fromHex, le32, sha256, u16be, u32le } from "./bytes.js";
import { frame, parseFrames } from "./link.js";
import { STUB_MAGIC } from "./stub.js";
import { APP_BASE, F4_MAGIC, f405Sectors } from "./f4stub.js";
import { moveCode, moveBranches, putBl } from "./codecmap.js";
import { place } from "./stockfw.js";

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
// {fb, board, key, stock, codecSegments, stockSegments}: fb is a copy of flash-boards.json whose board and profile
// match the key; codecSegments: the codec as RadioFoundry's merge places it (the profile's placement), stockSegments: where
// the stock firmware keeps it. Where the profile moves the code, the synthetic codec gets what stockfw.py's
// build_synthetic_stock plants (a self reference, an island's call into the code) and a codec map for them.
const SYN_SELF_REF = 0x40, SYN_BL = 0x110;
export async function simulation(fbReal, boardId, seed = 1701) {
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
	const windows = [], stockSegments = [];
	const run = real.windows[0][0];
	for (const [i, [addr, length]] of real.windows.entries()) {
		const data = randomBytes(r, length);
		if (i === 0) data.set(le32(fb.codec_sig), 0);
		if (i === 0 && real.placement) data.set(le32(run + 0x201), SYN_SELF_REF);
		if (i === 1 && real.placement) putBl(data, SYN_BL, addr + SYN_BL, run + 0x200);
		plain.set(data, appOff + addr - board.app_base);
		windows.push([addr, length, await sha256(data)]);
		stockSegments.push([addr, data]);
	}
	const header = new Uint8Array(fb.header_len);
	for (let i = 0; i < fb.magic.length; i++) header[i] = fb.magic.charCodeAt(i);
	header.set(le32(real.segments.length > 1 ? real.segments.length : fb.single_segment), 0x7C);
	real.segments.forEach(([a, n], i) => { header.set(le32(a), 0x80 + 8 * i); header.set(le32(n), 0x84 + 8 * i); });
	const enc = plain.map((b, i) => b ^ key[i % fb.key_len]);
	const stock = concat(header, enc, randomBytes(r, 0x100));
	const sha = await sha256(stock);
	delete fb.profiles[realSha];
	const prof = { ...real, windows, size: stock.length, key_sha256: board.key_sha256, name: real.name + " (simulated)" };
	if (real.placement) {
		const [[, code], [tabAddr, tables]] = stockSegments;
		const pl = real.placement;
		const cut = tables.slice(0, pl.tables_len ?? tables.length);	// the codec's own data, the map's cut
		prof.map = { format: "rfoundry-codec-map/1", profile: "sim-" + boardId, board: boardId, isa: "thumb2",
			code: { flash: run, len: code.length, run, sha256: windows[0][2] }, entries: {},
			self_refs: [{ kind: "word", off: SYN_SELF_REF, value: run + 0x201 }], island_ptrs: [], table_refs: [], island_refs: [],
			islands: [{ start: tabAddr + SYN_BL - 0x10, len: 0x40 }], branches: [{ at: tabAddr + SYN_BL, target: run + 0x200, kind: "bl" }],
			objects: [], groups: [], outside_refs: [],
			table_windows: [{ id: "tables", flash: tabAddr, run: tabAddr, len: tables.length, min_start: tabAddr, min_len: cut.length,
				min_sha256: await sha256(cut) }] };
		const mc = await moveCode(code, prof.map, pl.code);
		const mt = moveBranches(tabAddr, tables.slice(0, pl.tables_len ?? tables.length), prof.map, pl.code);
		prof.placed = { code: [crc32(mc), await sha256(mc)], tables: [crc32(mt), await sha256(mt)] };
	}
	fb.profiles[sha] = prof;
	fb.known_files[sha] = real.version + " (simulated)";
	const codecSegments = await place(prof, stockSegments, fb);
	return { fb, board, key, stock, codecSegments, stockSegments, profile: prof };
}

// ---------------------------------------------------------------------------------------------------------------

export class FakeRadio {
	// state: "stock" (stock firmware: codec in place, no serial port), "rfoundry", "rfoundry-nocodec", "bootloader".
	// stubEntry: RadioFoundry built with CONFIG_STUB_ENTRY; adopt: the stub keeps RadioFoundry's USB device (else the port goes away
	// and serialStream() gives the new one); cutAfter: stub programs before the "cable" is pulled. stubVersion: the
	// STM32F4 stub's protocol (5: the security register ops, which this fake and the page leave out; 4: CONFIG and the
	// record echo; 2 and 3: the DM-1701's wiring built in); stubConfig: what the stub does with a record it would
	// accept ("ok", "refuse": answers bad argument, "garble": accepts it but HELLO echoes different bytes).
	// state "openfw": another open firmware runs (installOpenFirmware; await radio.ready before using it), options in openfw
	constructor(sim, { state = "stock", spiSize = 0x1000000, stubEntry = true, adopt = true, cutAfter = null, stubVersion = 5, stubConfig = "ok", openfw = {} } = {}) {
		this.sim = sim;
		this.board = sim.board;
		this.key = sim.key;
		this.flash = new Uint8Array(FLASH_SIZE);
		const r = rng(380);
		for (let i = 0; i < FLASH_SIZE; i += 4096) this.flash.set(randomBytes(r, 64), i);	// "stock code"
		// stock keeps its codec in its stock place; RadioFoundry (with its codec) where the merge put it
		for (const [a, d] of state === "stock" || state === "bootloader" ? sim.stockSegments : sim.codecSegments) this.flash.set(d, a - FLASH_BASE);
		this.spi = new Uint8Array(spiSize).fill(0xFF);
		this.spi.set(randomBytes(r, 0x40000), 0);		// a stock codeplug where RadioFoundry's settings go
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
		this.stubVersion = stubVersion;
		this.stubConfig = stubConfig;
		this.stubRecord = null;		// the board record in force; a fresh stub has none
		this.configs = 0;
		this.programs = 0;
		this.events = [];
		this.port = 0;
		this.resetDfu();
		this.log = [];
		this.ready = state === "openfw" ? this.installOpenFirmware(openfw) : Promise.resolve();
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
				// what boots next: RadioFoundry unless a test says it wrote stock firmware back
				this.running = this.afterExit || "rfoundry";
				if (this.running === "rfoundry" && this.codecReport() === "absent") this.running = "rfoundry-nocodec";
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

	// ---- RadioFoundry's serial port, as a link.js stream

	serialStream() {
		const radio = this;
		const port = this.port;
		const s = {
			buf: new Uint8Array(0),
			inb: new Uint8Array(0),
			received: 0,
			first: new Uint8Array(0),
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
				if (radio.mode === "normal" && radio.running === "openfw") {
					// what came in, as SerialStream counts it: link.js silence() tells this firmware by its first byte
					const a = radio.openfwAnswer(data);
					s.buf = concat(s.buf, a);
					s.received += a.length;
					if (s.first.length < 16) s.first = concat(s.first, a.subarray(0, 16 - s.first.length));
					return;
				}
				if (radio.mode !== "normal" || !radio.running.startsWith("rfoundry")) throw new Error("no serial port: the radio isn't running RadioFoundry");
				s.buf = concat(s.buf, radio.serialAnswer(data));
			},
			async close() {},
		};
		return s;
	}

	// ---- the STM32F4 stub (stub/common/stubeng.c's and stub/stm32f4/f4stub_board.c's rules)

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

	// The board record the stub would accept (stub-stm32f4.md 5.2 "Checked by the stub"): null if it refuses it
	static recordOk(p) {
		const port = (b) => b >> 4 <= 8;
		const base = u32be(p, 8), hse = u32be(p, 4);
		const pins = [p[16], p[17], p[18]];
		return p.length === 24 && p[0] !== 0 && p[1] === 1 && p[2] <= 3 && p[3] <= 3 && hse >= 4e6 && hse <= 26e6
			&& base >= 0x0800C000 && f405Sectors().some((sec) => sec.index > 0 && sec.addr === base) && u32be(p, 12) >= 100000
			&& pins.every(port) && p[19] <= 15 && port(p[20]) && !pins.includes(p[20])
			&& [p[21], p[22]].every((x) => x === 0xFF || port(x)) && p[23] === 0;
	}

	// This radio's own wiring (its profile's record): a record with other pins finds no SPI flash chip
	ownRecord() {
		const r = this.sim.fb.f4stub?.boards[this.board.id]?.record;
		return r ? fromHex(r) : null;
	}

	stubSpiSize() {
		const own = this.ownRecord(), rec = this.stubRecord;
		if (!rec || !own) return 0;
		return equal(rec.subarray(2, 23), own.subarray(2, 23)) ? this.spi.length : 0;
	}

	stubOp(op, p, reply) {
		const OK = 0x05, BADARG = 0x0D, REFUSED = 0x0F, NOCONFIG = 0x11;
		const cfg = this.stubVersion >= 4;	// CONFIG (protocol 4)
		if (!cfg && !this.stubRecord) this.stubRecord = this.ownRecord();	// before protocol 4: the wiring built in
		const rec = this.stubRecord;
		const appBase = rec ? u32be(rec, 8) : 0;
		const spiSize = this.stubSpiSize();
		const isSpi = (baseLen) => p.length > baseLen && p[baseLen] === 1;
		const mem = (baseLen, addr) => (isSpi(baseLen) ? [this.spi, addr] : [this.flash, addr - FLASH_BASE]);
		const appBlank = () => this.word(APP_BASE) === 0xFFFFFFFF;
		if ([0x10, 0x11, 0x12, 0x13].includes(op) && !rec) return reply(NOCONFIG);
		if (((op === 0x07 || op === 0x09) && isSpi(6)) || op === 0x12 || op === 0x13) {
			if (!spiSize) return reply(BADARG);	// no chip answered, or not configured
		}
		if (op === 0x01) {
			let body = concat(new TextEncoder().encode("F4STUB"), new Uint8Array([this.stubVersion, 0x10, 0x08, 0x04, 0x00, 0x55,
				(this.adopt ? 1 : 0) | (cfg && rec ? 0x80 : 0)]), be32(cfg ? appBase : APP_BASE), be32(spiSize),
				new Uint8Array([0xEF, 0x40, 0x18]));
			if (cfg) {
				let echo = rec || new Uint8Array(0);
				if (rec && this.stubConfig === "garble") { echo = echo.slice(); echo[20] ^= 0x01; }
				body = concat(body, new Uint8Array([rec ? rec[0] : 0, echo.length]), echo);
			}
			reply(OK, body);
		} else if (op === 0x14 && cfg) {
			this.configs++;
			this.stubRecord = null;		// a refused record leaves the stub unconfigured
			if (!FakeRadio.recordOk(p) || this.stubConfig === "refuse") return reply(BADARG);
			this.stubRecord = p.slice();
			reply(OK);
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
			if (u32be(p, 1) !== F4_MAGIC || !sec || sec.addr < appBase || (sec.addr !== appBase && !appBlank())) return reply(REFUSED);
			this.flash.fill(0xFF, sec.addr - FLASH_BASE, sec.addr - FLASH_BASE + sec.size);
			this.events.push(`erase ${p[0]}`);
			reply(OK);
		} else if (op === 0x11) {
			const a = u32be(p, 0), data = p.subarray(4), o = a - FLASH_BASE;
			if (a < appBase || (a + data.length > 0x08010000 && !appBlank())) return reply(REFUSED);
			if (!this.flash.subarray(o, o + data.length).every((x) => x === 0xFF)) return reply(REFUSED);
			this.flash.set(data, o);
			this.programs++;
			reply(OK);
		} else if (op === 0x12) {
			const a = u32be(p, 0);
			if (u32be(p, 4) !== F4_MAGIC || a % 4096 || a >= spiSize) return reply(REFUSED);
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
		this.running = this.codecReport() === "absent" ? "rfoundry-nocodec" : "rfoundry";
	}

	// Another open firmware on the radio, as far as docs/spec/open-firmware-cps.md says and as a radio was seen to behave:
	// its own code where the stock firmware was, the stock codec moved to codecAt (its self references and, with tables
	// "patched", the islands' calls into it rewritten by the codec map; "stock": the tables as stock left them), or no
	// codec at all (codec false). Its CPS port: R 1 / R 2 clipped to clip bytes with the VFO overlay (0xEE here) on a read
	// that spans it; R 5 the MCU flash with no bounds check (a read past the end faults the radio: silent until
	// powerCycle()); R 9 the radio info; R 10 no answer; C and anything else "-". hangAfter N: the Nth read goes
	// unanswered, then hangFor more requests (0: until powerCycle()). refuse5: area 5 refused ("-") from this offset.
	// Every request is logged in openfwLog
	async installOpenFirmware({ codecAt = null, tables = "patched", codec = true, radioType = null, git = "sim-open1", clip = 2045,
		hangAfter = 0, hangFor = 0, refuse5 = null } = {}) {
		const r = rng(77);
		for (const [a, d] of [...this.sim.codecSegments, ...this.sim.stockSegments]) this.flash.set(randomBytes(r, d.length), a - FLASH_BASE);
		this.openfw = { radioType: radioType ?? this.board.cps_radio_types?.[0] ?? 6, git, clip, hangAfter, hangFor, refuse5, reads: 0, silent: 0, dead: false, controls: 0 };
		this.openfwLog = [];
		if (!codec) return;
		const m = this.sim.profile.map;
		if (!m) {
			for (const [a, d] of this.sim.stockSegments) this.flash.set(d, a - FLASH_BASE);
			return;
		}
		const [[run, code], [tabAddr, tabs], ...extra] = this.sim.stockSegments;
		const at = codecAt ?? this.board.app_base + 0x1000 + (run & 3);
		this.openfw.codecAt = at;
		this.flash.set(await moveCode(code, m, at), at - FLASH_BASE);
		this.flash.set(tables === "patched" ? moveBranches(tabAddr, tabs, m, at) : tabs, tabAddr - FLASH_BASE);
		for (const [a, d] of extra) this.flash.set(d, a - FLASH_BASE);
	}

	powerCycle() {
		if (this.openfw) Object.assign(this.openfw, { dead: false, silent: 0, hangAfter: 0 });
	}

	openfwAnswer(req) {
		const o = this.openfw, none = new Uint8Array(0), refuse = new Uint8Array([0x2D]);
		if (req[0] !== 0x52) {
			if (req[0] === 0x43) o.controls++;
			this.openfwLog.push(req[0] === 0x43 ? `C ${req[1]}` : `other 0x${req[0].toString(16)}`);
			return o.dead ? none : refuse;
		}
		const area = req[1], addr = u32be(req, 2), len = u16be(req, 6);
		this.openfwLog.push(`R ${area} 0x${addr.toString(16)} ${len}`);
		o.reads++;
		if (o.dead) return none;
		if (o.silent) { o.silent--; return none; }
		if (o.hangAfter && o.reads === o.hangAfter) {
			if (o.hangFor) o.silent = o.hangFor;
			else o.dead = true;
			return none;
		}
		const n = Math.min(len, o.clip);
		const reply = (d) => concat(new Uint8Array([0x52, d.length >> 8, d.length & 0xFF]), d);
		if (area === 1 || area === 2) {
			if (area === 2 && addr + n > 0x10000) return refuse;
			if (addr + n > this.spi.length) return refuse;
			const d = this.spi.slice(addr, addr + n);
			if (addr <= 0x7590 && addr + n > 0x7600 + 8) d.fill(0xEE, 0x7590 - addr, 0x7600 - addr);
			return reply(d);
		}
		if (area === 5) {
			if (addr + n > this.flash.length) { o.dead = true; return none; }
			if (o.refuse5 !== null && addr + n > o.refuse5) return refuse;
			return reply(this.flash.slice(addr, addr + n));
		}
		if (area === 9) {
			const d = new Uint8Array(46);
			d.set(le32(3), 0); d.set(le32(o.radioType), 4);
			d.set(new TextEncoder().encode(o.git.slice(0, 16)), 8);
			d.set(new TextEncoder().encode("20261025120000"), 24);
			d.set(le32(0x4018), 40);
			return reply(d);
		}
		if (area === 10) return none;
		return refuse;
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
					this.stubRecord = null;		// a fresh stub: no board record yet
					this.events.push("stub (from RadioFoundry)");
					if (!this.adopt) this.port++;	// the old port is gone; serialStream() is the new one
					return ok(new Uint8Array(0));
				}
				return frame(f.cmd | 0x80, f.seq, new Uint8Array([3]));
			}
			if (f.cmd === 0x0F) {
				// STORAGE (debuglink.md 3.13): the settings ring and journal hold the stock codeplug until claimed
				const foreign = !this.spi.subarray(0x11000, 0x1B000).every((x) => x === 0xFF) && !this.storageOurs;
				if (f.payload[0] === 0) return ok(concat(new Uint8Array([foreign ? 2 : 1, 0]), le32(0x11000), le32(0x8000), le32(0x19000), le32(0x2000), le32(0x10000), le32(0x200)));
				if (f.payload[0] === 2) {
					if (!foreign) return frame(f.cmd | 0x80, f.seq, new Uint8Array([4]));
					if (u32le(f.payload, 1) !== u32le(this.uid, 0)) return frame(f.cmd | 0x80, f.seq, new Uint8Array([7, 1]));
					this.spi.fill(0xFF, 0x11000, 0x1B000);
					this.storageOurs = true;
					this.events.push("storage claimed");
					return ok(new Uint8Array(0));
				}
			}
			if (f.cmd === 0x0C) {
				const addr = u32le(f.payload, 0), n = f.payload[4] | (f.payload[5] << 8);
				if (n < 1 || n > 1000 || addr < FLASH_BASE || addr + n > FLASH_BASE + FLASH_SIZE) return frame(f.cmd | 0x80, f.seq, new Uint8Array([7]));
				return ok(concat(le32(addr), this.flash.subarray(addr - FLASH_BASE, addr - FLASH_BASE + n)));
			}
			return frame(f.cmd | 0x80, f.seq, new Uint8Array([1]));
		}
		if (req[0] === 0x52) {
			// RadioFoundry answers up to 32 KB per read, older builds 2045 bytes (open-firmware-cps.md 2.1)
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

