// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The C7 stub over WebSerial: flash access for the HR_C7000 radios (DM-32UV, DM-1801 Pro, DB25-D) through the RAM
// monitor stub/stubtool.py loads, ported from that tool (its protocol, its retries, its checks). One image, the universal
// stub, serves every radio: it copies itself to its run address wherever it was loaded, and the page sends it the
// board's profile (CONFIG) after HELLO and checks the echo before any write. Two ways to get the stub running:
//
//   boot ROM   radio off, cable in, then switched on: the SoC's serial boot ROM greets the host and takes the stub
//   from RadioFoundry   a running RadioFoundry built with CONFIG_STUB_ENTRY=y takes it over its debug link (docs/spec/debuglink.md 3.18)
//
// Then the stub reads both flash chips (every 4 KB sector checked against the radio's own CRC-32) and writes the
// program flash's application area, 0xC000 up; the bootloader below it is never written, and the boot ROM is in the
// chip, so a radio can always be loaded again whatever is in its flash.

import { be32, concat, crc32, le32, sleep, u32le } from "./bytes.js";
import { decodeBlock } from "./lz4.js";
import { Link, LinkError } from "./link.js";

export class StubError extends Error {}
// A damaged, short or missing frame, or a sector that failed its CRC: worth asking again
export class FrameError extends StubError {}

export const SECTOR = 4096, PAGE = 256, WINDOW = 16, PACK = 16, CHUNK = 1024, ROM_BLOCK = 256;
export const OK = 0x05, BADSUM = 0x0A, BADOP = 0x0B, TOOLONG = 0x0E;
export const SSI_PROG = 0x04000000, SSI_DATA = 0x140A0000;
export const QUAD_ENABLE = 0x1100005C, LCSFC_BAUDR = 0x11000030, SYS_SOFT_RSTN = 0x11000000, TIMER2_VALUE = 0x14000018;
export const WDT_LOCK = 0x14010000, WDT_EN = 0x14010010, WDT_KEY = 0x5ADA7200;
// RadioFoundry's restart-cause mark (docs/spec/crash-log.md 1.3; stubtool.py CRASH_SOFT_MARK): the reset was asked for
export const CRASH_SOFT_MARK = 0x18007EF4, CRASH_BOOT_SOFT = 0x54464F53;
export const STUB_MAGIC = 0x42533743, DL_STUB = 0x1A;
const ROM_HELLO = [0x02, 0x24, 0x00, 0x03], ROM_ACK = [0x02, 0x05, 0x00, 0x03];
// memory: SSI base, the GPIO chip select (the data flash's is PTA18), size
export const MEM = {
	progmem: { base: SSI_PROG, cs: null, size: 0x100000, name: "program memory" },
	q128: { base: SSI_DATA, cs: 18, size: 0x1000000, name: "data memory" },
};
// played when a run finishes: three green flashes, then the slow orange blink resumes
export const DONE_LED = [[0, 255, 150], [0, 0, 150], [0, 255, 150], [0, 0, 150], [0, 255, 150], [0, 0, 150]];

const sum8 = (b) => b.reduce((a, x) => (a + x) & 0xFF, 0);
const be16 = (n) => new Uint8Array([(n >> 8) & 0xFF, n & 0xFF]);
const u32be = (b, o) => ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0;
const romCmd = (op, payload = new Uint8Array(0)) => concat(new Uint8Array([0x02, op, sum8(payload), 0x03]), payload);
const isBlank = (b) => b.every((x) => x === 0xFF);
const memArgs = (m) => new Uint8Array([1 | (m.cs !== null ? 2 : 0), m.cs || 0]);

function find(buf, pat) {
	outer: for (let i = 0; i + pat.length <= buf.length; i++) {
		for (let k = 0; k < pat.length; k++) if (buf[i + k] !== pat[k]) continue outer;
		return i;
	}
	return -1;
}

export const SRAM = [0x10000, 0x58000];	// the HR_C7000's main SRAM (hardware-dm32.md 2)

// The universal stub from data/c7000-stub.json: the image, its power-on load address, and the RAM it runs in
export function stubImages(json) {
	const image = Uint8Array.from(atob(json.univ.data), (c) => c.charCodeAt(0));
	return { image, sha: json.univ.sha256, load: json.univ.load, run: json.univ.run, end: json.univ.end };
}

// [run, end) from a universal stub's parameter block ('UCFG' at 0x80, layout 2 or later), or null
export function univLayout(image) {
	if (image.length < 0xC0 || String.fromCharCode(...image.subarray(0x80, 0x84)) !== "UCFG") return null;
	if ((image[0x84] | (image[0x85] << 8)) < 2) return null;
	return [u32le(image, 0xB8), u32le(image, 0xBC)];
}

// The checks stubtool's check_univ_load makes before the stub is sent: its run area inside SRAM, the image 4-byte
// aligned inside SRAM and inside the window the firmware offers, and no overlap between the loaded copy and the copy
// the entry code makes at the run address (the stub would stop at its entry instead)
export function univFits(image, load, window = null) {
	const lay = univLayout(image);
	if (!lay) throw new StubError("this does not look like the universal stub image");
	const [run, end] = lay, n = image.length, hex = (x) => "0x" + x.toString(16);
	if (!(SRAM[0] <= run && run + n <= end && end <= SRAM[1])) throw new StubError(`the stub's RAM ${hex(run)}-${hex(end)} is outside SRAM`);
	if (load % 4 || load < SRAM[0] || load + n > SRAM[1]) throw new StubError(`the stub can't be loaded at ${hex(load)}`);
	if (window && (load < window[0] || load + n > window[0] + window[1])) throw new StubError(`the stub does not fit the RAM the firmware offers at ${hex(window[0])}`);
	if (load !== run && load < run + n && run < load + n) throw new StubError(`loading the stub at ${hex(load)} overlaps its run address ${hex(run)}`);
	return lay;
}

export class Stub {
	// stream: link.js SerialStream or a simulated one (buf, write, clear, take)
	constructor(stream, { log = () => {}, retries = 6 } = {}) {
		this.s = stream;
		this.log = log;
		this.retries = retries;
		this.retried = 0;
		this.info = null;
		this.size = {};		// per-radio memory sizes where they differ from MEM (the DB25-D's program flash is 2 MB)
		this.v6 = null;		// the universal stub's HELLO fields (protocol 6); null for an older stub
		this.configured = null;	// the board profile the stub confirmed after CONFIG
	}

	async readExact(n, timeout) {
		const until = Date.now() + timeout;
		while (this.s.buf.length < n && Date.now() < until) await sleep(1);
		return this.s.take(Math.min(n, this.s.buf.length));
	}

	// ---- boot ROM

	// Resolves once the boot ROM greets the host (the radio was just switched on, or soft-reset). signal stops it.
	async waitForRom(timeout, { signal } = {}) {
		const until = Date.now() + timeout;
		this.s.clear();
		while (Date.now() < until) {
			if (signal?.aborted) throw new StubError("stopped");
			const at = find(this.s.buf, ROM_HELLO);
			if (at >= 0) {
				this.s.take(at + ROM_HELLO.length);
				return;
			}
			if (this.s.buf.length > 64) this.s.take(this.s.buf.length - 3);
			await sleep(1);
		}
		throw new StubError("the radio's boot ROM did not answer");
	}

	// Answer the ROM's greeting at once (it waits only briefly), load the stub at `load` and start it
	async loadThroughRom(image, load, progress = () => {}) {
		univFits(image, load);
		await this.s.write(new Uint8Array([0x02, 0x14, 0x00, 0x03]));
		await sleep(50);
		const padded = new Uint8Array(Math.ceil(image.length / ROM_BLOCK) * ROM_BLOCK).fill(0xFF);
		padded.set(image);
		const blocks = padded.length / ROM_BLOCK;
		this.log(`boot ROM answered; loading the ${padded.length}-byte stub (${blocks} blocks)`);
		for (let i = 0; i < blocks; i++) {
			const payload = concat(be32(load + i * ROM_BLOCK), new Uint8Array([0, 0, 0, 0x40]), padded.subarray(i * ROM_BLOCK, (i + 1) * ROM_BLOCK));
			let ok = false;
			for (let attempt = 0; attempt < 3 && !ok; attempt++) {	// a write to a fixed address: sending it again is safe
				this.s.clear();
				await this.s.write(romCmd(0x18, payload));
				const r = await this.readExact(4, 1000);
				ok = r.length === 4 && ROM_ACK.every((x, k) => r[k] === x);
				if (!ok) this.log(`stub block ${i} not acknowledged, sending it again`);
			}
			if (!ok) throw new StubError(`stub block ${i} was not acknowledged`);
			progress(i + 1, blocks);
		}
		this.s.clear();
		await this.s.write(romCmd(0x1B, be32(load)));
		const r = await this.readExact(4, 1000);
		if (!(r.length === 4 && ROM_ACK.every((x, k) => r[k] === x))) throw new StubError("the start command was not acknowledged");
		await this.helloWithin(3000, "the stub was started but does not answer");
	}

	// ---- entry from RadioFoundry (debug link STUB, docs/spec/debuglink.md 3.18)

	// {load, size, chunk} when the running RadioFoundry offers stub entry, null when it doesn't (built without CONFIG_STUB_ENTRY)
	static async rfoundryOffer(link) {
		let r;
		try {
			r = await link.request(DL_STUB, new Uint8Array([0]));
		} catch (e) {
			return null;
		}
		if (r.length < 14 || u32le(r, 0) !== STUB_MAGIC) return null;
		return { load: u32le(r, 4), size: u32le(r, 8), chunk: Math.min(r[12] | (r[13] << 8), 59) };
	}

	// entry: where RadioFoundry jumps (the offered load address unless the stub runs elsewhere, as the STM32F4 one does)
	async enterFromRFoundry(image, progress = () => {}, { entry = null } = {}) {
		const link = new Link(this.s);
		const dl = async (payload, what) => {
			let last;
			for (let i = 0; i < 4; i++) {	// every request names its own offset or state: a resend is safe
				try {
					return await link.request(DL_STUB, payload);
				} catch (e) {
					last = e;
					if (!(e instanceof LinkError) || /status/.test(e.message)) break;
				}
			}
			throw new StubError(`RadioFoundry refused ${what} (${last?.message})`);
		};
		const offer = await Stub.rfoundryOffer(link);
		if (!offer) throw new StubError("this RadioFoundry build cannot hand over to the loader (built without CONFIG_STUB_ENTRY)");
		if (image.length > offer.size) throw new StubError(`the stub (${image.length} bytes) does not fit RadioFoundry's free RAM (${offer.size} bytes)`);
		if (entry === null) univFits(image, offer.load, [offer.load, offer.size]);
		this.log(`RadioFoundry offers ${offer.size} bytes of RAM at 0x${offer.load.toString(16)}; sending the ${image.length}-byte stub`);
		await dl(concat(new Uint8Array([1]), le32(STUB_MAGIC), le32(image.length), le32(crc32(image)), le32(entry ?? offer.load)), "the stub transfer");
		for (let off = 0; off < image.length; off += offer.chunk) {
			await dl(concat(new Uint8Array([2]), le32(off), image.subarray(off, off + offer.chunk)), `the chunk at ${off}`);
			progress(Math.min(off + offer.chunk, image.length), image.length);
		}
		// Refused (7) with reason 1: the radio's watchdog runs and this loader doesn't feed it (an older copy of the site)
		await dl(concat(new Uint8Array([3]), le32(STUB_MAGIC)), "to start the loader (image incomplete or CRC mismatch; status 7: this firmware needs a newer loader, reload the page)");
		this.log("RadioFoundry is jumping to the stub");
		this.jumped = true;
		await this.afterJump();
	}

	async afterJump() {
		await sleep(300);
		await this.drain(300);
		await this.helloWithin(3000, "RadioFoundry jumped to the stub but it does not answer: switch the radio off and on, then connect again");
	}

	// ---- stub frames: 02 op len(2) chk 03 payload -> 02 status len(2) chk 03 payload

	async request(op, payload = new Uint8Array(0), { retries = this.retries, timeout = 1000 } = {}) {
		const frame = concat(new Uint8Array([0x02, op, payload.length >> 8, payload.length & 0xFF, sum8(payload), 0x03]), payload);
		let why = "";
		for (let attempt = 0; attempt <= retries; attempt++) {
			this.s.clear();
			await this.s.write(frame);
			const hdr = await this.readExact(6, timeout);
			if (hdr.length !== 6 || hdr[0] !== 0x02 || hdr[5] !== 0x03) why = "no or malformed reply header";
			else {
				const n = (hdr[2] << 8) | hdr[3];
				const body = await this.readExact(n, 500 + n * 0.2);
				if (body.length !== n) why = `reply short (${body.length} of ${n} bytes)`;
				else if (sum8(body) !== hdr[4]) why = "reply checksum";
				else if (hdr[1] === OK) return body;
				else if (hdr[1] === BADSUM) why = "the radio saw a bad request checksum";
				else throw Object.assign(new StubError(`stub op 0x${op.toString(16)} refused: status 0x${hdr[1].toString(16)}`), { status: hdr[1] });
			}
			this.retried++;
			if (attempt < retries) {
				this.log(`op 0x${op.toString(16)}: ${why}, again (${attempt + 1} of ${retries})`);
				await sleep(20);
			}
		}
		throw new StubError(`op 0x${op.toString(16)}: no clean reply after ${retries + 1} tries (${why})`);
	}

	async sendNoReply(op, payload) {
		this.s.clear();
		await this.s.write(concat(new Uint8Array([0x02, op, payload.length >> 8, payload.length & 0xFF, sum8(payload), 0x03]), payload));
	}

	async hello({ retries = 0 } = {}) {
		const r = await this.request(0x01, new Uint8Array(0), { retries });
		if (String.fromCharCode(...r.subarray(0, 6)) !== "C7STUB") throw new StubError("unexpected hello");
		this.info = { version: r[6], maxPayload: (r[7] << 8) | r[8] };
		const g = (o) => ((r[o] << 24) | (r[o + 1] << 16) | (r[o + 2] << 8) | r[o + 3]) >>> 0;
		this.v6 = r.length >= 43 && r[6] >= 6 ? { load: g(9), board: r[17], flags: r[18], run: g(19), image: g(23), bssEnd: g(27),
			stackLo: g(31), stackTop: g(35), stackUsed: g(39) } : null;
		return this.info;
	}

	// Send the board's profile to the universal stub (CONFIG, op 0x0D: clock, power latch and key pins, LEDs, board id,
	// flags, the UART this request came in on) and check the echo in the next HELLO. Writes wait for this. The clock is
	// the APB clock, measured (24 MHz under the boot ROM, 36 MHz on the DB25-D, 84 MHz under RadioFoundry on the DM-32UV and
	// DM-1801 Pro). An older stub has no CONFIG.
	async configure(profile, led = { red: 33, green: 32 }) {
		if (!this.v6) return;
		if (this.v6.board && this.v6.board === profile.id && this.v6.flags === (profile.nand ? 1 : 0)) {
			this.configured = profile;
			return;
		}
		let hz = await this.timerHz(300);
		if (!(hz > 1e6 && hz < 4e8)) throw new StubError("the radio's timer rate can't be right: not configuring the loader");
		hz = Math.round(hz / 1e5) * 1e5;
		await this.request(0x0D, concat(be32(hz), be32(hz), new Uint8Array([profile.latch, profile.key, led.red, led.green, profile.id,
			profile.nand ? 1 : 0, 3, 0])));
		await this.hello({ retries: this.retries });	// a request like any other here: a damaged reply is asked for again
		if (!this.v6 || this.v6.board !== profile.id || this.v6.flags !== (profile.nand ? 1 : 0))
			throw new StubError("the loader did not take the radio's configuration: nothing was written");
		this.configured = profile;
		this.log(`loader configured (board ${profile.id}, clock ${(hz / 1e6).toFixed(1)} MHz); runs at 0x${this.v6.run.toString(16)}-0x${this.v6.stackTop.toString(16)}, stack ${this.v6.stackUsed} of ${this.v6.stackTop - this.v6.stackLo} bytes used`);
	}

	async helloWithin(ms, message) {
		const until = Date.now() + ms;
		for (;;) {
			try {
				const i = await this.hello();
				this.log(`stub v${i.version} running (max payload ${i.maxPayload})`);
				return i;
			} catch (e) {
				if (Date.now() > until) throw new StubError(message);
			}
		}
	}

	// A stub left running by an earlier run (the radio still in loader mode)
	async attach() {
		await this.drain(300);
		try {
			return await this.hello();
		} catch (e) {
			return null;
		}
	}

	// Drop what the stub still streams until the line has been quiet for `quiet` ms
	async drain(quiet = 400) {
		for (;;) {
			this.s.clear();
			await sleep(quiet);
			if (!this.s.buf.length) return;
		}
	}

	peek(addr, n) { return this.request(0x02, concat(be32(addr), be16(n))); }
	poke(addr, data) { return this.request(0x03, concat(be32(addr), data)); }

	async spi(mem, tx, rxLen, opts = {}) {
		const m = MEM[mem];
		const r = await this.request(0x05, concat(be32(m.base), memArgs(m), be16(tx.length), be16(rxLen), tx), opts);
		if (r.length !== rxLen) throw new StubError(`SPI reply is ${r.length} bytes, wanted ${rxLen}`);
		return r;
	}

	// The JEDEC ID; refuses a chip that doesn't answer (chip select or cable wrong)
	async checkId(mem) {
		const id = await this.spi(mem, new Uint8Array([0x9F]), 3);
		const hexId = [...id].map((b) => b.toString(16).padStart(2, "0")).join("");
		if (hexId === "ffffff" || hexId === "000000") throw new StubError(`the ${MEM[mem].name} chip does not answer (${hexId})`);
		return hexId;
	}

	// [{crc, blank}] for `count` sectors from addr, computed by the radio: no flash data crosses the cable
	async sums(mem, addr, count) {
		const m = MEM[mem];
		const r = await this.request(0x07, concat(be32(m.base), memArgs(m), be32(addr), be16(count)), { timeout: 1000 + count * 150 });
		if (r.length !== count * 5) throw new StubError(`sector sums reply is ${r.length} bytes, wanted ${count * 5}`);
		return Array.from({ length: count }, (_, i) => ({ crc: u32be(r, i * 5), blank: r[i * 5 + 4] === 1 }));
	}

	// Sector addresses in [start, start + data.length) where the flash differs from data, by CRC
	async verify(mem, data, start = 0, progress = () => {}) {
		if (start % SECTOR || data.length % SECTOR) throw new StubError("verify needs whole 4 KB sectors");
		const bad = [];
		const end = start + data.length;
		for (let w = start; w < end; w += WINDOW * SECTOR) {
			const count = Math.min(WINDOW, (end - w) / SECTOR);
			(await this.sums(mem, w, count)).forEach(({ crc }, i) => {
				const off = w + i * SECTOR - start;
				if (crc32(data.subarray(off, off + SECTOR)) !== crc) bad.push(w + i * SECTOR);
			});
			progress(Math.min(w + WINDOW * SECTOR, end) - start, data.length);
		}
		return bad;
	}

	async readFrame(timeout) {
		const hdr = await this.readExact(6, timeout);
		if (hdr.length !== 6 || hdr[0] !== 0x02 || hdr[5] !== 0x03) throw new FrameError("no or malformed frame header");
		const n = (hdr[2] << 8) | hdr[3];
		const body = await this.readExact(n, 500 + n * 0.2);
		if (body.length !== n) throw new FrameError(`frame short (${body.length} of ${n} bytes)`);
		if (sum8(body) !== hdr[4]) throw new FrameError("frame checksum");
		return [hdr[1], body];
	}

	// Read [start, end) of a memory. The radio sends each sector blank (a flag), LZ4-compressed or raw with its CRC-32;
	// a sector that doesn't match is asked for again, uncompressed from the second try. A record carries no address, so
	// one left over from a request cut short could pass for another sector: at the end every sector is checked against
	// the radio's own CRCs (op 07) and read again until it matches. progress(done, total, stats)
	async dump(mem, start, end, { progress = () => {}, signal, compress = true } = {}) {
		const m = MEM[mem];
		const size = this.size[mem] || m.size;
		if (!(start >= 0 && start < end && end <= size)) throw new StubError(`${m.name} range must lie within 0..0x${size.toString(16)}`);
		await this.checkId(mem);
		const lo = Math.floor(start / SECTOR) * SECTOR, hi = Math.ceil(end / SECTOR) * SECTOR;
		const full = new Uint8Array(hi - lo);
		const stats = { blank: 0, packed: 0, wire: 0 };
		await this.readSectors(m, full, lo, lo, hi, { compress, signal, stats,
			progress: (sa) => progress(Math.min(sa, end) - start, end - start, stats) });
		for (let pass = 0; ; pass++) {
			const bad = await this.verify(mem, full, lo);
			if (!bad.length) break;
			if (pass >= this.retries) throw new StubError(`0x${bad[0].toString(16)}: the read does not match the radio's checksum after ${pass + 1} tries`);
			this.log(`${bad.length} sector(s) read wrong (first 0x${bad[0].toString(16)}), reading them again`);
			this.retried += bad.length;
			for (const sa of bad) await this.readSectors(m, full, lo, sa, sa + SECTOR, { compress: false, signal, stats });
		}
		this.stats = stats;
		return full.slice(start - lo, end - lo);
	}

	// Sectors [from, to) into full (which starts at base), resuming where a damaged frame stopped
	async readSectors(m, full, base, from, to, { compress, signal, stats, progress = () => {} }) {
		let sa = from, fails = 0, want = compress;
		while (sa < to) {
			if (signal?.aborted) throw new StubError("stopped");
			const count = Math.min(PACK, (to - sa) / SECTOR);
			try {
				this.s.clear();
				const payload = concat(be32(m.base), memArgs(m), be32(sa), new Uint8Array([count, want ? 1 : 0]));
				await this.s.write(concat(new Uint8Array([0x02, 0x09, 0, payload.length, sum8(payload), 0x03]), payload));
				for (let i = 0; i < count; i++) {
					const [status, r] = await this.readFrame(i === 0 ? 3000 : 1500);
					if (status !== OK) {
						if (status === BADOP || status === TOOLONG) throw new StubError(`stub op 0x09 refused: status 0x${status.toString(16)}`);
						throw new FrameError(`stub op 0x09: status 0x${status.toString(16)}`);
					}
					if (r.length < 7 || r.length !== 7 + ((r[5] << 8) | r[6])) throw new FrameError("sector record malformed");
					const sec = unpack(u32be(r, 0), r[4], r.subarray(7));
					if (!sec) throw new FrameError(`0x${sa.toString(16)}: sector does not match the radio's CRC`);
					stats.blank += r[4] === 2;
					stats.packed += r[4] === 1;
					stats.wire += r.length - 7;
					full.set(sec, sa - base);
					sa += SECTOR;
					fails = 0;
					want = compress;
					progress(sa);
				}
			} catch (e) {
				if (!(e instanceof FrameError)) throw e;
				fails++;
				this.retried++;
				if (fails > this.retries) throw new StubError(`0x${sa.toString(16)}: no clean read after ${fails} tries (${e.message})`);
				this.log(`0x${sa.toString(16)}: ${e.message}, resuming here (${fails} of ${this.retries})`);
				await this.drain();
				want = compress && fails < 2;
			}
		}
	}

	// ---- writes (generic SPI flash ops: write enable 06, status 05, sector erase 20, page program 02)

	async status(mem) { return (await this.spi(mem, new Uint8Array([0x05]), 1))[0]; }

	async waitReady(mem, what, timeout) {
		const until = Date.now() + timeout;
		for (;;) {
			const st = await this.status(mem);
			if (!(st & 1)) return st;
			if (Date.now() > until) throw new StubError(`${what}: flash still busy after ${timeout / 1000} s`);
		}
	}

	async writeCmd(mem, tx, what, timeout) {
		await this.spi(mem, new Uint8Array([0x06]), 0);
		const st = await this.status(mem);
		if (!(st & 2)) throw new StubError(`${what}: write enable did not stick (status 0x${st.toString(16)}): chip write-protected?`);
		await this.spi(mem, tx, 0);
		await this.waitReady(mem, what, timeout);
	}

	async writeSector(addr, data) {
		const a3 = (a) => new Uint8Array([(a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF]);
		await this.writeCmd("progmem", concat(new Uint8Array([0x20]), a3(addr)), `erase 0x${addr.toString(16)}`, 3000);
		for (let p = 0; p < SECTOR; p += PAGE) {
			const page = data.subarray(p, p + PAGE);
			if (!isBlank(page)) await this.writeCmd("progmem", concat(new Uint8Array([0x02]), a3(addr + p), page), `program 0x${(addr + p).toString(16)}`, 1000);
		}
	}

	// Program `data` (whole sectors) into the program flash at `start`, which must lie in the application area
	// [appStart, appEnd). Only sectors whose CRC differs are written; each is read back by CRC at once and written
	// again (up to 3 times) when it doesn't match, the first sector of the application last, so the radio only has a
	// start address when everything else is in place. A final CRC pass over the whole range ends it.
	// onSector(addr, state): "todo" | "same" | "writing" | "retry" | "written"
	async writeProgram(data, start, { appStart, appEnd, onSector = () => {}, signal } = {}) {
		if (start % SECTOR || data.length % SECTOR || !data.length) throw new StubError("writes are whole 4 KB sectors");
		if (start < appStart || start + data.length > appEnd) throw new StubError(`refusing to write outside the application area 0x${appStart.toString(16)}..0x${appEnd.toString(16)}`);
		if (this.v6 && !this.configured) throw new StubError("the loader's radio configuration is not confirmed: refusing to write");
		await this.checkId("progmem");
		const todo = await this.verify("progmem", data, start);
		for (let a = start; a < start + data.length; a += SECTOR) onSector(a, todo.includes(a) ? "todo" : "same");
		const order = todo.filter((a) => a !== appStart).concat(todo.includes(appStart) ? [appStart] : []);
		for (const a of order) {
			if (signal?.aborted) throw new StubError("stopped");
			const sec = data.subarray(a - start, a - start + SECTOR);
			for (let attempt = 1; ; attempt++) {
				onSector(a, attempt === 1 ? "writing" : "retry");
				await this.writeSector(a, sec);
				const [{ crc }] = await this.sums("progmem", a, 1);
				if (crc === crc32(sec)) break;
				this.log(`0x${a.toString(16)}: read back differs, writing it again (${attempt} of 3)`);
				if (attempt === 3) throw new StubError(`0x${a.toString(16)}: the sector reads back wrong after 3 writes`);
			}
			onSector(a, "written");
		}
		const bad = todo.length ? await this.verify("progmem", data, start) : [];
		if (bad.length) throw new StubError(`verify after write: sectors differ at ${bad.map((x) => "0x" + x.toString(16)).join(", ")}`);
		return { written: todo.length, same: data.length / SECTOR - todo.length };
	}

	// ---- the LED on top of the radio, and leaving the loader

	// Tick rate of timer 2 (it counts down), measured against this computer's clock
	async timerHz(span = 400) {
		const t0 = performance.now();
		const v0 = u32le(await this.peek(TIMER2_VALUE, 4), 0);
		await sleep(span);
		const v1 = u32le(await this.peek(TIMER2_VALUE, 4), 0);
		return (((v0 - v1) >>> 0) / (performance.now() - t0)) * 1000;
	}

	// Status blink: slow while waiting (500 ms phases), fast while data moves (125 ms). Both LEDs make orange.
	async setupLed(red, green) {
		let hz = await this.timerHz();
		if (!(hz > 1e6 && hz < 4e8)) hz = 24e6;
		this.ledPerMs = Math.round(hz / 1000);
		await this.request(0x08, concat(new Uint8Array([red & 0xFF, green & 0xFF, 3]), be32(Math.round(hz * 0.5)),
			be32(Math.round(hz * 0.125)), be32(this.ledPerMs)));
	}

	// [[red, green, ms]] played by the radio itself; the status blink resumes after unless repeat or hold
	async ledSequence(steps, { repeat = false, hold = false } = {}) {
		const payload = [repeat | (hold << 1), steps.length];
		for (const [r, g, ms] of steps) payload.push((ms >> 8) & 0xFF, ms & 0xFF, r, g);
		await this.request(0x0A, Uint8Array.from(payload));
	}

	// Soft-reset the SoC: XIP quad off and the reset divider (the soft reset keeps it, and the ROM can't read flash at
	// RadioFoundry's), then SYS_SOFT_RSTN = 0. The boot ROM runs again and starts the firmware: a power cycle without the cycle.
	// The watchdog goes off first: RadioFoundry leaves it running while the stub feeds it, a soft reset may keep it, and the
	// bootloader never feeds it. RadioFoundry starts it again. The restart-cause mark goes first, so RadioFoundry's next start reports a
	// software restart, not the watchdog
	async resetChip() {
		await this.poke(CRASH_SOFT_MARK, le32(CRASH_BOOT_SOFT));
		await this.poke(WDT_LOCK, le32(WDT_KEY));
		await this.poke(WDT_EN, le32(0));
		await this.poke(WDT_LOCK, le32(0));
		await this.poke(QUAD_ENABLE, le32(0));
		await this.poke(LCSFC_BAUDR, le32(4));
		await this.sendNoReply(0x03, concat(be32(SYS_SOFT_RSTN), le32(0)));
	}
}

// The sector's 4096 bytes if they match the radio's CRC-32, else null
export function unpack(crc, method, data) {
	let sec;
	try {
		sec = method === 2 ? new Uint8Array(SECTOR).fill(0xFF) : method === 1 ? decodeBlock(data, SECTOR) : data;
	} catch (e) {
		return null;
	}
	return sec.length === SECTOR && crc32(sec) === crc ? sec : null;
}
