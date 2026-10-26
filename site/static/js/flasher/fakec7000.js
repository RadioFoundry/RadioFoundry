// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A simulated HR_C7000 radio on its programming cable, for the tests and the flasher's simulated mode
// (#sim=<board>-stock): the serial boot ROM's short window after power-on or a soft reset, the firmware it then starts
// (stock, or RadioFoundry answering the debug link VERSION and STUB commands), and the stub's protocol over a model of the two
// flash chips (erase sets a sector to 0xFF, programming only clears bits). The ROM side and the stub side follow
// stub/stubtool.py's FakeRadio. Bytes reach the host no faster than a 115200 baud cable would carry them (times
// `speed`).
//
// A stock firmware with a USMART console (usmart: its build) also answers the debug console the page loads the stub
// through (usmart.js): framed read_addr / write_addr over a model of its RAM, and the hijack word that starts what was
// written.
//
// A radio with the Baofeng updater (loaderId: the ID its loader answers) enters it when switched on with its keys
// held (powerOn({ keys: true })): FakeBfLoader, the radio side of bfloader.js, erases the application area and writes
// the blocks it takes into the program flash.
//
// The stock firmware and codec here are random bytes shaped like the real ones: no vendor code is in RadioFoundry.

import { concat, crc32, le32, sha256, sleep, u32le } from "./bytes.js";
import { frame, parseFrames } from "./link.js";
import { ACK, NAK, CRC_REQ, STX, EOT, BLOCK, HEADER_BLOCK, xmodem } from "./bfloader.js";
import { encodeBlock } from "./lz4.js";
import { CRASH_BOOT_SOFT, CRASH_SOFT_MARK, STUB_MAGIC, SSI_PROG, SYS_SOFT_RSTN, TIMER2_VALUE, WDT_EN } from "./stub.js";
import { HIJACK, STUB_WINDOW, BUILDS, FLASH_BASE } from "./usmart.js";
import { rng, randomBytes } from "./fake.js";
import { sigBytes } from "./c7000.js";

const SECTOR = 4096, OK = 0x05, BADSUM = 0x0A, BADOP = 0x0B;
const sum8 = (b) => b.reduce((a, x) => (a + x) & 0xFF, 0);
const be32 = (n) => new Uint8Array([(n >>> 24) & 0xFF, (n >>> 16) & 0xFF, (n >>> 8) & 0xFF, n & 0xFF]);
const u32be = (b, o) => ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0;
const RFOUNDRY_MARK = new TextEncoder().encode("RadioFoundry");
const LEGACY_MARK = new TextEncoder().encode("OpenDigitalRadio");

// A stock console's build from the board's first stock version ("909E.DBB.EARSAB.018" -> "018"), or null
const consoleBuild = (radio) => (radio?.flash.stock_console ? radio.flash.stock_versions[0]?.slice(-3) || null : null);

// What FakeC7000 needs to play a board, from its flasher data (radio: its radios.json entry): the name it reports, its
// codec, a small data memory when the stub reads none, its stock console and its Baofeng updater's ID
export function fakeC7000Options(radio, fb) {
	const b = fb.boards[radio.id];
	return { board: b.debuglink_names[0], codecId: b.codec.id,
		...(fb.c7000.boards[radio.id].data_flash === false ? { dataSize: 0x200000 } : {}), usmart: consoleBuild(radio),
		loaderId: radio.flash.update_mode === "baofeng" ? fb.c7000.boards[radio.id].bootloader_id : null };
}

// The radio side of the Baofeng updater (docs/spec/hardware-dm32.md 13.2), a port of tools/serial_loader.py's
// FakeBootloader: checks every step, the header CRC, each block's number (with the 0xFF -> 0x00 wrap), complement and
// CRC. send(bytes, delayMs) carries its answers to the host. Faults: corrupt, block numbers whose first transfer
// arrives damaged (the radio NAKs); silent, never answers; badStep2, a wrong answer at step 2; eraseMs, how long the
// erase takes before step 7's answer. onErase() and onBlock(k, data) (k from 0) let a radio model keep what is written.
export class FakeBfLoader {
	constructor({ id, send, corrupt = [], silent = false, badStep2 = false, eraseMs = 0, onErase = () => {}, onBlock = () => {} }) {
		this.magic = new TextEncoder().encode(id);
		this.send = send;
		this.corrupt = new Set(corrupt);
		this.silent = silent;
		this.badStep2 = badStep2;
		this.eraseMs = eraseMs;
		this.onErase = onErase;
		this.onBlock = onBlock;
		this.inbuf = new Uint8Array(0);
		this.state = 1;
		this.expectN = 1;
		this.blocks = [];
		this.header = null;
		this.naks = 0;
		this.done = false;
		this.errors = [];
	}

	feed(b) {
		if (this.silent) return;
		this.inbuf = concat(this.inbuf, b);
		while (this.step()) { /* each step takes what it can */ }
	}

	take(want) {
		if (this.inbuf.length < want.length || !want.every((x, i) => this.inbuf[i] === x)) return false;
		this.inbuf = this.inbuf.slice(want.length);
		return true;
	}

	step() {
		const s = this.state, ib = this.inbuf;
		if (!ib.length) return false;
		const fixed = { 1: [[0x52], [0x06]], 2: [[0x4D, 0, 0, 0, 1], [0x4D, 0x01, this.magic.length]], 3: [[0x06], [...this.magic]],
			4: [[0x45, 0, 0, 0, 0], [0x06]], 5: [[0x31], [0x43]], 6: [[0x01], [0x43]] };
		if (fixed[s]) {
			const [want, answer] = fixed[s];
			if (ib.length < want.length) return false;
			if (!this.take(want)) {
				this.errors.push(`step ${s}: got ${[...ib.subarray(0, want.length)].map((x) => x.toString(16)).join(" ")}`);
				this.inbuf = new Uint8Array(0);
				return false;
			}
			this.send(Uint8Array.from(s === 2 && this.badStep2 ? [0x4D, 0x01, 0x08] : answer));
			this.state++;
			return true;
		}
		if (s === 7) {
			if (ib.length < 2 + HEADER_BLOCK + 2) return false;
			const f = ib.slice(0, 2 + HEADER_BLOCK + 2);
			this.inbuf = ib.slice(f.length);
			const blk = f.subarray(2, 2 + HEADER_BLOCK);
			if (f[0] !== 0x00 || f[1] !== 0xFF || ((f.at(-2) << 8) | f.at(-1)) !== xmodem(blk)) {
				this.errors.push("header block");
				this.send(Uint8Array.of(NAK));
				return true;
			}
			const nul = blk.indexOf(0);
			const rest = blk.subarray(nul + 1);
			const end = rest.indexOf(0);
			this.header = [String.fromCharCode(...blk.subarray(0, nul)), Number(String.fromCharCode(...rest.subarray(0, end < 0 ? rest.length : end)))];
			this.onErase();
			this.send(Uint8Array.of(ACK, CRC_REQ), this.eraseMs);
			this.state = 8;
			return true;
		}
		if (s === 8) {		// waiting for STX or EOT
			const c = ib[0];
			this.inbuf = ib.slice(1);
			if (c === STX) {
				this.send(Uint8Array.of(CRC_REQ));
				this.state = 9;
			} else if (c === EOT) {
				this.send(Uint8Array.of(ACK));
				this.done = true;
				this.state = 10;
			} else this.errors.push(`expected 02 or 04, got ${c.toString(16)}`);
			return true;
		}
		if (s === 9) {		// one data frame
			if (ib.length < 2 + BLOCK + 2) return false;
			const f = ib.slice(0, 2 + BLOCK + 2);
			this.inbuf = ib.slice(f.length);
			const n = f[0];
			if (this.corrupt.has(n)) {
				this.corrupt.delete(n);
				f[10] ^= 0x40;		// line noise on the first transfer
			}
			const data = f.subarray(2, 2 + BLOCK);
			let ok = f[1] === 0xFF - n && ((f.at(-2) << 8) | f.at(-1)) === xmodem(data);
			if (ok && n !== (this.expectN & 0xFF)) {
				this.errors.push(`block number ${n}, expected ${this.expectN & 0xFF}`);
				ok = false;
			}
			if (ok) {
				this.onBlock(this.blocks.length, data.slice());
				this.blocks.push(data.slice());
				this.expectN++;
				this.send(Uint8Array.of(ACK));
			} else {
				this.naks++;
				this.send(Uint8Array.of(NAK));
			}
			this.state = 8;
			return true;
		}
		this.inbuf = new Uint8Array(0);
		return false;
	}

	image() {
		return this.header ? concat(...this.blocks).subarray(0, this.header[1]) : new Uint8Array(0);
	}
}

// A synthetic stock program flash for a board: bootloader, firmware, the codec at made-up addresses, and the c7000
// config changed so its profile matches them. radio (its radios.json entry, optional) adds what its stock firmware
// starts with: the first stage signature the vendor loader checks, and a stock console's version string. Returns
// {c, progmem, codec: {code, tables}}.
export async function c7000Simulation(cReal, boardId, seed = 7000, radio = null) {
	const c = structuredClone(cReal);
	const r = rng(seed);
	const progmem = new Uint8Array(c.boards[boardId].progmem_size || c.progmem_size).fill(0xFF);
	progmem.set(randomBytes(r, 0xA000), 0);					// bootloader, with its ID
	progmem.set(new TextEncoder().encode(c.boards[boardId].bootloader_id || ""), 0x5000);
	const fwLen = 0xC0000;
	progmem.set(randomBytes(r, fwLen).map((b) => (b < 120 ? b : 0)), c.app_start);	// firmware-like
	const real = c.profiles.find((p) => p.board === boardId);
	const code = randomBytes(r, real.code_len);
	code.set(sigBytes(c), 0);
	code.set(le32(real.run + 0x100), 0x40);		// one address word inside the code, which the map lists
	const tables = randomBytes(r, real.tables_len);
	const codeAt = 0xA4000, tabAt = 0x6A000;
	progmem.set(code, codeAt);
	progmem.set(tables, tabAt);
	const fs = radio?.flash.first_stage;
	if (fs) progmem.set(fs.head.match(/../g).map((x) => parseInt(x, 16)), c.app_start);	// the entry the loader checks
	const build = BUILDS.find(([n]) => n === consoleBuild(radio));
	if (build) {	// the version string the console's build check reads: "EARSAB.<build>" just after the build's word
		const version = radio.flash.stock_versions[0];
		progmem.set(new TextEncoder().encode(`${version}.Jan 21 2025`), build[1] - FLASH_BASE + 1 - version.lastIndexOf("EARSAB"));
	}
	c.profiles = c.profiles.filter((p) => p.board !== boardId);
	c.profiles.push({ ...real, name: `simulated ${real.name}`, code_sha256: await sha256(code), tables_sha256: await sha256(tables),
		tables_head_crc: crc32(tables.subarray(0, 64)), stock_file: null });
	// its codec map (docs/spec/codec-placement.md), as a real build has one
	const id = `sim-${boardId}`;
	c.codec_maps = { ...(c.codec_maps || {}), [id]: { format: "rfoundry-codec-map/1", profile: id, board: boardId, isa: "csky",
		code: { flash: 0, len: code.length, run: real.run, sha256: await sha256(code) }, entries: {},
		self_refs: [{ off: 0x40, value: real.run + 0x100 }], table_refs: [], objects: [], groups: [], outside_refs: [] } };
	return { c, progmem, codec: { code, tables, codeAt, tabAt } };
}

export class FakeC7000 {
	// firmware: "stock" | "rfoundry" (with stub entry) | "rfoundry-nostub"; speed: how many times faster than a real cable
	constructor({ progmem, dataSize = 0x1000000, board = "DM-1801 Pro", codecId = "dm1801pro-049", stubEntry = true, speed = 1,
		romWindow = 400, bootTime = 1500, faults = {}, stubSpan = [0x17000, 0x18000], usmart = null, loaderId = null } = {}) {
		this.usmart = usmart;			// a stock firmware build with the USMART console ("018")
		this.loaderId = loaderId;		// the Baofeng updater's ID ("BFUV32-V2"), or null: no such loader
		this.ram = new Map();			// the console's view of RAM: words written since power-on
		this.frames = [];			// every console frame received, as text
		this.stubSpan = stubSpan;		// the RAM span RadioFoundry offers in STUB INFO (the DB25-D's DMR builds: 0x1D000, 0x12000)
		this.stubCfg = { board: 0, flags: 0 };	// what the universal stub was configured with (CONFIG, op 0x0D)
		this.configs = [];
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
		// sector's data (as a record left over from an earlier request would); silentAfterReset: after the stub's soft
		// reset RadioFoundry runs but its host link stays silent until a power cycle, as on the real radios (dm32-bringup.md);
		// romEcho: the cable sends every byte the host writes to the boot ROM back to it, ahead of the ROM's answer;
		// romNak: the boot ROM answers the load's frames with something other than its acknowledgement
		this.faults = { flip: 0, misplace: [], silentAfterReset: false, romEcho: false, romNak: false, ...faults };
		this.rand = rng(11);
		this.state = "off";		// off | rom | booting | firmware | stub | bfloader
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
		else for (let i = 0; i + RFOUNDRY_MARK.length < app.length; i++)
			if ([RFOUNDRY_MARK, LEGACY_MARK].some((m) => app[i] === m[0] && m.every((b, k) => app[i + k] === b))) { kind = "rfoundry"; break; }
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
	// keys: the update mode keys held while switching on (a radio with the Baofeng updater enters it after the boot ROM)
	powerOn({ keys = false } = {}) {
		if (this.state !== "off") return;
		this.keysHeld = keys;
		this.boot();
	}

	powerOff() {
		this.state = "off";
		this.softReset = false;
		this.led = "off";
		this.gen = (this.gen || 0) + 1;
		this.events.push("off");
	}

	boot() {
		this.state = "rom";
		this.ram = new Map();
		this.fwKind = null;
		this.inb = new Uint8Array(0);
		this.loaded = new Uint8Array(0);
		this.events.push("rom");
		const gen = (this.gen = (this.gen || 0) + 1);
		this.send(new Uint8Array([0x02, 0x24, 0x00, 0x03]));
		setTimeout(() => {
			if (this.gen !== gen || this.state !== "rom") return;
			if (this.keysHeld && this.loaderId && !this.softReset) return this.updateMode();
			this.state = "booting";
			setTimeout(() => {
				if (this.gen !== gen || this.state !== "booting") return;
				this.state = "firmware";
				this.events.push(`firmware ${this.firmware()}`);
			}, this.bootTime / this.speed);
		}, this.romWindow);
	}

	// The Baofeng updater: its erase and writes go to the program flash model (the loader below 0xC000 is never written)
	updateMode() {
		this.state = "bfloader";
		this.inb = new Uint8Array(0);
		this.events.push("update mode");
		const app = 0xC000, end = this.progmem.length;
		this.bf = new FakeBfLoader({ id: this.loaderId, eraseMs: 12600 / this.speed, send: (b, ms = 0) => (ms ? setTimeout(() => this.send(b), ms) : this.send(b)),
			onErase: () => {
				this.progmem.fill(0xFF, app, end);
				this.fwKind = null;
				this.events.push("erase");
			},
			onBlock: (k, data) => {
				const at = app + k * BLOCK;
				if (at < end) this.progmem.set(data.subarray(0, Math.min(BLOCK, end - at)), at);
				this.writes++;
			} });
	}

	// ---- what arrives from the host
	receive(data) {
		if (this.state === "off" || this.state === "booting") return;
		if (this.state === "bfloader") return this.bf.feed(data);
		if (this.state === "firmware" && this.softReset && this.faults.silentAfterReset) return;
		if (this.faults.romEcho && (this.state === "rom" || this.state === "romload")) this.send(data.slice());
		this.inb = concat(this.inb, data);
		for (;;) {
			const used = this.state === "rom" || this.state === "romload" ? this.rom() : this.state === "stub" ? this.stub()
				: this.state === "firmware" ? (this.usmart && this.firmware() === "stock" ? this.console() : this.rfoundry()) : 0;
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
		this.send(new Uint8Array(this.faults.romNak ? [0x02, 0x0A, 0x00, 0x03] : [0x02, 0x05, 0x00, 0x03]));
		return 1;
	}

	rfoundry() {
		const fw = this.firmware();
		const f = parseFrames(this.inb)[0];
		if (!f) {
			if (this.inb.length > 4096) this.inb = new Uint8Array(0);
			return 0;
		}
		this.inb = this.inb.slice(f.end);
		if (fw !== "rfoundry") return 1;			// stock firmware doesn't speak the debug link
		const reply = (st, p = new Uint8Array(0)) => this.send(frame(f.cmd | 0x80, f.seq, concat(new Uint8Array([st]), p)));
		if (f.cmd === 0x02) {
			reply(0, new TextEncoder().encode(`proto=1\ngit=sim1234\nboard=${this.boardName}\nfeatures=FM,DMR\ncodec=${this.codecReport()}\n`));
		} else if (f.cmd === 0x1A) {
			if (!this.stubEntry) return reply(3), 1;
			const p = f.payload, op = p[0];
			if (op === 0) reply(0, concat(le32(STUB_MAGIC), le32(this.stubSpan[0]), le32(this.stubSpan[1]), new Uint8Array([59, 0, 0]), le32(this.loaded.length)));
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
					this.events.push("stub (from RadioFoundry)");
				}, 100);
			}
		} else reply(3);
		return 1;
	}

	// The original firmware's debug console: "@AT", a filler byte, the text, CR LF. The reply, then the application's
	// own log line. With the hijack word pointing at the window, any frame but "01" and "DMRCALL" starts what is there.
	console() {
		const text = String.fromCharCode(...this.inb);
		const at = text.indexOf("@AT");
		if (at < 0) {
			this.inb = this.inb.slice(Math.max(0, this.inb.length - 2));
			return 0;
		}
		const end = text.indexOf("\r\n", at + 4);
		if (end < 0) return 0;
		const cmd = text.slice(at + 4, end);
		this.inb = this.inb.slice(end + 2);
		this.frames.push(cmd);
		const H = (n) => n.toString(16).toUpperCase();
		if (cmd !== "01" && cmd !== "DMRCALL" && this.ram.get(HIJACK) === STUB_WINDOW[0]) {
			let top = 0;
			for (const a of this.ram.keys()) if (a >= STUB_WINDOW[0] && a < STUB_WINDOW[0] + STUB_WINDOW[1]) top = Math.max(top, a + 4 - STUB_WINDOW[0]);
			this.loaded = new Uint8Array(top);
			for (let o = 0; o < top; o += 4) this.loaded.set(le32(this.ram.get(STUB_WINDOW[0] + o) || 0), o);
			this.state = "stub";
			this.led = "slow";
			this.events.push("stub (debug console)");
			return 1;
		}
		let m = /^read_addr\((0x[0-9a-f]+)\)$/.exec(cmd);
		let out = null;
		if (m) {
			const a = Number(m[1]);
			const off = a - 0x03000000;
			const v = off >= 0 && off + 4 <= this.progmem.length ? u32le(this.progmem, off) : this.ram.get(a) || 0;
			out = `\r\nread_addr(0X${H(a)})=0X${H(v)};`;
		} else if ((m = /^write_addr\((0x[0-9a-f]+),(0x[0-9a-f]+)\)$/.exec(cmd))) {
			const a = Number(m[1]), v = Number(m[2]) >>> 0;
			if (a >= 0x03000000 && a < 0x04000000) throw new Error("simulated radio: a console write reached the program flash");
			this.ram.set(a, v);
			out = `\r\nwrite_addr(0X${H(a)},0X${H(v)});`;
		}
		if (out) this.send(new TextEncoder().encode(`${out}\r\nclose slot.\r\nu8PassWord 0 u8EncLevel 0 u8Enc0\r\n`));
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
		const univ = this.loaded.length >= 0xC0 && String.fromCharCode(...this.loaded.subarray(0x80, 0x84)) === "UCFG";
		const be = (v) => new Uint8Array([v >>> 24, (v >>> 16) & 0xFF, (v >>> 8) & 0xFF, v & 0xFF]);
		if (op === 0x01 && univ) {		// the universal stub: protocol 6, board and flags echoed, its RAM footprint
			const run = u32le(this.loaded, 0xB8), end = u32le(this.loaded, 0xBC);
			this.reply(OK, concat(new TextEncoder().encode("C7STUB"), new Uint8Array([6, 0x04, 0x08]), be(0x10000), be(0x0808),
				new Uint8Array([this.stubCfg.board, this.stubCfg.flags]), be(run), be(this.loaded.length), be(end - 0x1004),
				be(end - 0x1000), be(end), be(200)));
		} else if (op === 0x0D && univ) {
			if (p.length !== 16) return this.reply(0x0B, new Uint8Array(0)), 1;
			this.configs.push(p);
			this.stubCfg = { board: p[12], flags: p[13] };
			this.reply(OK, new Uint8Array(0));
		} else if (op === 0x01) this.reply(OK, concat(new TextEncoder().encode("C7STUB"), new Uint8Array([1, 0x04, 0x08]), le32(0x10000)));
		else if (op === 0x02) {
			const a = u32be(p, 0), n = (p[4] << 8) | p[5];
			const now = (-Math.floor(performance.now() * 24000)) >>> 0;
			this.reply(OK, a === TIMER2_VALUE ? le32(now) : new Uint8Array(n));
		} else if (op === 0x03) {
			if (u32be(p, 0) === WDT_EN && u32le(p, 4) === 0) this.events.push("wdt-off");
			if (u32be(p, 0) === CRASH_SOFT_MARK && u32le(p, 4) === CRASH_BOOT_SOFT) this.events.push("soft-mark");
			if (u32be(p, 0) === SYS_SOFT_RSTN) {
				this.events.push("reset");
				this.led = "off";
				this.softReset = true;
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
