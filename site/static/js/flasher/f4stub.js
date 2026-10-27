// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The STM32F4 stub over WebSerial: updating an STM32F4 radio that already runs RadioFoundry without its
// bootloader, ported from stub/f4stub.py (its protocol, its write order, its checks; docs/spec/stub-stm32f4.md). A
// running RadioFoundry built with CONFIG_STUB_ENTRY=y takes the stub over its debug link and jumps to it; the stub then reads
// and writes the internal flash from the application base (0x0800C000) up and the SPI flash. The bootloader's sectors
// 0 to 2 are never written, so the bootloader route (dfu.js) always works afterwards, and a stock radio still needs it
// first.
//
// One stub image serves every radio (protocol 4). The radio's wiring (SPI flash bus, pins, chip select, crystal, the
// bootloader's end) goes to it as a 24-byte board record (CONFIG) made by stub/f4stub.py config_record() and carried in
// data/flash-boards.json (f4stub.boards.<id>.record), so the page and the Python tool send the same bytes. The stub
// writes nothing until it has accepted a record (status 0x11), and the page checks the echo in the next HELLO first.
//
// An install erases sector 3 first: from then until its first word is written at the very end, the bootloader stays
// in DFU at power-on, so an update cut short leaves a radio the bootloader route finishes.

import { be32, concat, crc32, equal, fromHex, sleep, toHex } from "./bytes.js";
import { FrameError, OK, Stub, StubError, unpack } from "./stub.js";

export const BLOCK = 4096, FLASH_BASE = 0x08000000, APP_BASE = 0x0800C000, STUB_RUN = 0x20000000;
export const F4_MAGIC = 0x46345342;	// 'F4SB': ERASE, XERASE and RESET carry it
export const MEM_INTERNAL = 0, MEM_SPI = 1;
export const OP = { HELLO: 0x01, PEEK: 0x02, SUMS: 0x07, READ: 0x09, RESET: 0x0D, ERASE: 0x10, PROGRAM: 0x11, XERASE: 0x12, XPROGRAM: 0x13, CONFIG: 0x14, SREAD: 0x15 };
export const BADARG = 0x0D, REFUSED = 0x0F, NOCONFIG = 0x11;
export const HELLO_CONFIGURED = 0x80, HELLO_LEN = 26, REC_LEN = 24;	// HELLO v4 without the record echo; the board record
export const CONFIG_VERSION = 4;	// the first protocol with CONFIG (3 added the image header and the watchdog feed)
export const SREG_VERSION = 5, SREG_LEN = 256, SREG_COUNT = 3;	// the SPI flash's security registers 1-3 (stub-stm32f4.md 6.2)
const SUM_MAX = 800, READ_PACK = 16, ERASE_TIMEOUT = 6000;
const be16 = (n) => new Uint8Array([(n >> 8) & 0xFF, n & 0xFF]);
const u16be = (b, o) => (b[o] << 8) | b[o + 1];
const u32be = (b, o) => ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0;
const isBlank = (b) => b.every((x) => x === 0xFF);
const memByte = (mem) => (mem === MEM_INTERNAL ? new Uint8Array(0) : new Uint8Array([mem]));
const hex8 = (n) => "0x" + n.toString(16).toUpperCase().padStart(8, "0");

export function f4Image(json) {
	return Uint8Array.from(atob(json.data), (c) => c.charCodeAt(0));
}

// The image header (stub/stm32f4/f4stub_header.h): 16 bytes at 0x10, little endian. A watchdog build of RadioFoundry enters
// only a stub whose header has the magic, a version of at least 3 and the feed flag. null: no header (before version 3)
export const HDR = { OFFSET: 0x10, MAGIC: 0x48533446, LAYOUT: 1, SIZE: 16, VERSION_FEEDS_IWDG: 3, FLAG_FEEDS_IWDG: 0x1 };
export function f4StubHeader(image) {
	if (image.length < HDR.OFFSET + HDR.SIZE) return null;
	const v = new DataView(image.buffer, image.byteOffset + HDR.OFFSET, HDR.SIZE);
	const magic = v.getUint32(0, true), layout = v.getUint16(4, true), size = v.getUint16(6, true);
	if (magic !== HDR.MAGIC || layout < HDR.LAYOUT || size < HDR.SIZE) return null;
	const version = v.getUint32(8, true), flags = v.getUint32(12, true);
	return { layout, size, version, flags, feedsIwdg: version >= HDR.VERSION_FEEDS_IWDG && (flags & HDR.FLAG_FEEDS_IWDG) !== 0 };
}

// [{index, addr, size}] of a single-bank STM32F4: 4 x 16 KB, 64 KB, then 128 KB each
export function f405Sectors(flashKb = 1024) {
	const out = [];
	for (let i = 0, addr = FLASH_BASE; addr < FLASH_BASE + flashKb * 1024 && i < 12; i++) {
		const size = i < 4 ? 0x4000 : i === 4 ? 0x10000 : 0x20000;
		out.push({ index: i, addr, size });
		addr += size;
	}
	return out;
}

export class F4Stub extends Stub {
	// reopen: async () => a new stream, for a stub that enumerated again (the old port is gone), or null.
	// board: the radio's entry of data/flash-boards.json f4stub.boards ({name, stub_id, record, v2_wiring, owned, codec}),
	// what configure() sends; without one the stub stays unconfigured and only the internal flash can be read
	constructor(stream, { log = () => {}, retries = 3, reopen = null, board = null } = {}) {
		super(stream, { log, retries });
		this.reopen = reopen;
		this.board = board;
		this.configured = false;	// the stub confirmed this.board's record (or, protocol 2, has its wiring built in)
	}

	async enterFromRFoundry(image, progress = () => {}, opts = {}) {
		const h = f4StubHeader(image);
		this.log(h ? `STM32F4 stub image v${h.version}${h.feedsIwdg ? ", feeds the watchdog" : ""}` : "STM32F4 stub image without a header (older than version 3)");
		return super.enterFromRFoundry(image, progress, opts);
	}

	// Status 0x11 is the stub's own refusal of a write before a board record; the rest stay the generic error
	async request(op, payload, opts) {
		try {
			return await super.request(op, payload, opts);
		} catch (e) {
			if (e.status === NOCONFIG) {
				this.configured = false;
				throw Object.assign(new StubError("the loader has no board configuration (status 0x11): nothing was written"), { status: NOCONFIG });
			}
			throw e;
		}
	}

	async hello({ retries = 0 } = {}) {
		const r = await this.request(OP.HELLO, new Uint8Array(0), { retries, timeout: 500 });
		if (String.fromCharCode(...r.subarray(0, 6)) !== "F4STUB" || r.length < 17) throw new StubError("unexpected hello: not the STM32F4 stub");
		const i = {
			version: r[6], maxPayload: u16be(r, 7), flashKb: u16be(r, 9), rdp: r[11], adopted: !!(r[12] & 1), appBase: u32be(r, 13),
			spiSize: r.length >= 24 ? u32be(r, 17) : 0, spiId: r.length >= 24 ? toHex(r.subarray(21, 24)) : "",
			boardId: 0, record: new Uint8Array(0),
		};
		if (i.version >= CONFIG_VERSION) {
			// version 4: board id, record length and the record echo after the JEDEC ID; app base 0 before a record
			if (r.length < HELLO_LEN || r.length !== HELLO_LEN + r[25]) throw new StubError(`HELLO v${i.version} is ${r.length} bytes: malformed`);
			i.boardId = r[12] & HELLO_CONFIGURED ? r[24] : 0;
			i.record = r.slice(HELLO_LEN);
		} else if (i.appBase !== APP_BASE) {
			throw new StubError(`the stub reports the application at ${hex8(i.appBase)}, this page knows ${hex8(APP_BASE)}`);
		}
		this.info = i;
		// a stub left configured by an earlier run keeps its record: the echo alone tells whether it is this radio's
		this.configured = i.version >= CONFIG_VERSION ? this.echoes() : this.configured;
		return i;
	}

	// The last HELLO echoes exactly this radio's record, with the application base the page writes from
	echoes() {
		const b = this.board, i = this.info;
		return !!(b && i && i.version >= CONFIG_VERSION && i.boardId === b.stub_id && i.appBase === APP_BASE && equal(i.record, fromHex(b.record)));
	}

	// Send the radio's board record (CONFIG) and check the echo in the next HELLO; the stub writes nothing before. A stub
	// already configured with exactly this record is left as it is. A stub before protocol 4 has no CONFIG: its wiring is the
	// DM-1701's and MD-380's, built in, so it serves only a radio with that wiring (v2_wiring).
	async configure() {
		const b = this.board;
		if (!b) throw new StubError("no board profile for this radio: the loader stays unconfigured and nothing is written");
		if (this.info.version < CONFIG_VERSION) {
			if (!b.v2_wiring) throw new StubError(`this loader (protocol ${this.info.version}) has its SPI flash wiring built in and can't serve the ${b.name}: switch the radio off and on, then start again`);
			this.configured = true;
			return;
		}
		if (this.configured) return;
		const rec = fromHex(b.record);
		try {
			await this.request(OP.CONFIG, rec, { retries: 0 });
		} catch (e) {
			this.configured = false;
			if (e.status === BADARG) throw new StubError(`the loader refused the ${b.name}'s board record: nothing was written`);
			throw e;
		}
		await this.hello({ retries: this.retries });	// a request like any other here: a damaged reply is asked for again
		if (!this.configured) {
			throw new StubError(`the loader did not take the ${b.name}'s board record (HELLO echoes ${toHex(this.info.record) || "none"}, application base ${hex8(this.info.appBase)}): nothing was written`);
		}
		const i = this.info;
		this.log(`loader configured for the ${b.name}: ${i.spiSize ? `SPI flash ${i.spiSize / 1024} KB (JEDEC ${i.spiId})` : "no SPI flash answered"}`);
	}

	needConfigured() {
		if (!this.configured) throw new StubError(`the loader is not configured for ${this.board ? `the ${this.board.name}` : "this radio"}: nothing was written`);
	}

	// After the jump the stub either adopted RadioFoundry's USB device (the same port answers at once) or enumerated again (the
	// port goes away and comes back with the same serial number)
	async afterJump() {
		const until = Date.now() + 2000;
		while (Date.now() < until) {
			try {
				await this.drain(50);
				await this.hello();
				return this.attached();
			} catch (e) {
				await sleep(100);
			}
		}
		if (!this.reopen) throw new StubError("the loader does not answer on this port");
		this.log("waiting for the loader's USB port to come back");
		try { await this.s.close?.(); } catch (e) { /* gone */ }
		const end = Date.now() + 20000;
		for (;;) {
			try {
				this.s = await this.reopen();
				await this.hello();
				return this.attached();
			} catch (e) {
				if (Date.now() > end) throw new StubError("the loader's USB port did not come back within 20 s: switch the radio off and on (RadioFoundry starts again) and check the cable");
				await sleep(300);
			}
		}
	}

	attached() {
		const i = this.info;
		const spi = i.spiSize ? `SPI flash ${i.spiSize / 1024} KB (JEDEC ${i.spiId})` : i.version >= CONFIG_VERSION && !this.configured ? "not configured yet" : "no SPI flash";
		this.log(`STM32F4 stub v${i.version}: ${i.flashKb} KB flash, ${spi}, USB ${i.adopted ? "adopted from RadioFoundry" : "enumerated again"}`);
		return i;
	}

	needSpi() {
		if (this.info?.version >= CONFIG_VERSION) this.needConfigured();
		if (!this.info?.spiSize) throw new StubError(`the loader found no SPI flash (stub version ${this.info?.version})`);
	}

	// [{crc, blank}] per 4 KB block from addr (internal flash, or the SPI flash from 0), computed by the radio
	async blockSums(addr, count, mem = MEM_INTERNAL) {
		const out = [];
		while (count > 0) {
			const k = Math.min(count, SUM_MAX);
			// internal flash reads at memory speed; the SPI flash at about 0.5 MB/s
			const r = await this.request(OP.SUMS, concat(be32(addr), be16(k), memByte(mem)), { timeout: 1000 + k * (mem === MEM_INTERNAL ? 10 : 20) });
			if (r.length !== 5 * k) throw new StubError(`SUMS reply is ${r.length} bytes, wanted ${5 * k}`);
			for (let i = 0; i < k; i++) out.push({ crc: u32be(r, i * 5), blank: r[i * 5 + 4] === 1 });
			addr += k * BLOCK;
			count -= k;
		}
		return out;
	}

	// Block addresses where the flash differs from data (whole 4 KB blocks from start)
	async diff(data, start, mem = MEM_INTERNAL) {
		const bad = [];
		const n = data.length / BLOCK;
		for (let off = 0; off < n; off += 256) {	// 1 MB per request: a slow SPI flash stays inside its time-out
			(await this.blockSums(start + off * BLOCK, Math.min(256, n - off), mem)).forEach(({ crc }, i) => {
				const o = (off + i) * BLOCK;
				if (crc32(data.subarray(o, o + BLOCK)) !== crc) bad.push(start + o);
			});
		}
		return bad;
	}

	// n bytes (whole 4 KB blocks) from addr, every block checked against the radio's CRC-32
	async readFlash(addr, n, { mem = MEM_INTERNAL, progress = () => {}, signal } = {}) {
		if (addr % BLOCK || n % BLOCK) throw new StubError("reads are whole 4 KB blocks");
		const out = new Uint8Array(n);
		let a = addr, fails = 0;
		while (a < addr + n) {
			if (signal?.aborted) throw new StubError("stopped");
			const count = Math.min(READ_PACK, (addr + n - a) / BLOCK);
			try {
				this.s.clear();
				const payload = concat(be32(a), new Uint8Array([count, 0]), memByte(mem));
				await this.s.write(concat(new Uint8Array([0x02, OP.READ, 0, payload.length, payload.reduce((x, y) => (x + y) & 0xFF, 0), 0x03]), payload));
				for (let i = 0; i < count; i++) {
					const [status, r] = await this.readFrame(2000);
					if (status !== OK) throw new StubError(`READ at ${hex8(a)} refused: status 0x${status.toString(16)}`);
					if (r.length < 7 || r.length !== 7 + u16be(r, 5)) throw new FrameError("READ record malformed");
					const blk = unpack(u32be(r, 0), r[4], r.subarray(7));
					if (!blk) throw new FrameError(`${hex8(a)}: block does not match the radio's CRC`);
					out.set(blk, a - addr);
					a += BLOCK;
					progress(a - addr, n);
				}
				fails = 0;
			} catch (e) {
				if (!(e instanceof FrameError)) throw e;
				if (++fails > this.retries) throw new StubError(`${hex8(a)}: no clean read after ${fails} tries (${e.message})`);
				this.log(`${hex8(a)}: ${e.message}, reading again`);
				await this.drain(200);
			}
		}
		return out;
	}

	erase(sector) {
		this.needConfigured();
		return this.request(OP.ERASE, concat(new Uint8Array([sector]), be32(F4_MAGIC)), { retries: 0, timeout: ERASE_TIMEOUT });
	}

	// data (whole words) at addr in pieces of up to 4 KB, all-0xFF pieces skipped (erased already)
	async program(addr, data) {
		this.needConfigured();
		if (addr % 4 || data.length % 4) throw new StubError(`program ${hex8(addr)}+${data.length}: words only`);
		for (let off = 0; off < data.length; off += BLOCK) {
			const piece = data.subarray(off, off + BLOCK);
			if (!isBlank(piece)) await this.request(OP.PROGRAM, concat(be32(addr + off), piece), { retries: 0, timeout: 2000 });
		}
	}

	// System reset: the bootloader runs, then the application (or stays in DFU if sector 3 holds none)
	resetChip() {
		return this.request(OP.RESET, be32(F4_MAGIC), { retries: 0 });
	}

	checkChip() {
		if (this.info.flashKb !== 1024) throw new StubError(`the chip reports ${this.info.flashKb} KB of flash; the STM32F405 radios have 1024`);
	}

	// {sectors: [{index, addr, size, data, kept}], changed: [index]} for every sector holding firmware-owned flash.
	// Owned bytes come from the segments (0xFF where they have none); the rest of a sector is read back from the radio
	// and written again unchanged (kept; no radio shares a sector since the codec moved). owned: [[lo, hi)] (data/f4-stub.json)
	async plan(owned, segs) {
		for (const [addr, data] of segs) {
			if (!owned.some(([lo, hi]) => lo <= addr && addr + data.length <= hi))
				throw new StubError(`image data at ${hex8(addr)} is outside the flash the firmware owns: a codec-merged image or the wrong radio?`);
		}
		if (!segs.length || segs[0][0] !== APP_BASE || segs[0][1].length < 8) throw new StubError(`the image must start at ${hex8(APP_BASE)} with the vector table`);
		const sp = (segs[0][1][0] | (segs[0][1][1] << 8) | (segs[0][1][2] << 16) | (segs[0][1][3] << 24)) >>> 0;
		if ((sp & 0x2FFE0000) >>> 0 !== 0x20000000) throw new StubError(`the image's first word ${hex8(sp)} is not a RAM stack pointer: the bootloader would not start it`);
		const sectors = [], changed = [];
		for (const sec of f405Sectors(this.info.flashKb)) {
			const end = sec.addr + sec.size;
			const parts = owned.filter(([lo, hi]) => lo < end && hi > sec.addr).map(([lo, hi]) => [Math.max(lo, sec.addr), Math.min(hi, end)]);
			if (!parts.length) continue;
			const whole = parts.length === 1 && parts[0][0] === sec.addr && parts[0][1] === end;
			const buf = whole ? new Uint8Array(sec.size).fill(0xFF) : await this.readFlash(sec.addr, sec.size);
			const kept = whole ? null : buf.slice();
			for (const [lo, hi] of parts) buf.fill(0xFF, lo - sec.addr, hi - sec.addr);
			for (const [addr, data] of segs) {
				const lo = Math.max(addr, sec.addr), hi = Math.min(addr + data.length, end);
				if (lo < hi) buf.set(data.subarray(lo - addr, hi - addr), lo - sec.addr);
			}
			sectors.push({ ...sec, data: buf, kept });
			if ((await this.diff(buf, sec.addr)).length) changed.push(sec.index);
		}
		return { sectors, changed };
	}

	// Install a build, in the order the stub's interlock enforces: erase sector 3 (the bootloader stays in DFU from
	// here to the end), erase and program every other changed sector, program sector 3 but its first word, verify all
	// by CRC, program the first word (the commit), verify it. onKept(sector) runs before anything is erased, for each
	// sector to be rewritten that holds bytes that are not the firmware's (keep a copy). progress(done, total, what).
	// Returns {written, same}.
	async writeImage(owned, segs, { onKept = async () => {}, progress = () => {}, plan = null } = {}) {
		this.checkChip();
		this.needConfigured();
		const p = plan || await this.plan(owned, segs);
		if (!p.changed.length) return { written: 0, same: p.sectors.length };
		const by = new Map(p.sectors.map((s) => [s.index, s]));
		for (const i of p.changed) if (by.get(i).kept) await onKept(by.get(i));
		const order = [3, ...p.changed.filter((i) => i !== 3)];
		const s3 = by.get(3);
		await this.erase(3);
		for (const [n, i] of order.slice(1).entries()) {
			progress(n + 1, order.length, i);
			await this.erase(i);
			await this.program(by.get(i).addr, by.get(i).data);
		}
		progress(order.length, order.length, 3);
		await this.program(APP_BASE + 4, s3.data.subarray(4));
		const bad = [];
		for (const i of order) {
			const want = i === 3 ? concat(new Uint8Array([0xFF, 0xFF, 0xFF, 0xFF]), s3.data.subarray(4)) : by.get(i).data;
			if ((await this.diff(want, by.get(i).addr)).length) bad.push(i);
		}
		if (bad.length) throw new StubError(`verify before the commit: sector(s) ${bad.join(", ")} differ. Nothing is committed: the radio starts in its bootloader until an install finishes`);
		await this.program(APP_BASE, s3.data.subarray(0, 4));
		if ((await this.diff(s3.data.subarray(0, BLOCK), APP_BASE)).length) throw new StubError("the commit word did not verify");
		return { written: order.length, same: p.sectors.length - order.length };
	}

	// Write data (whole 4 KB sectors) into the SPI flash from start: only the sectors whose CRC differs are erased and
	// programmed (blank pieces skipped), each read back by the stub, then all checked again. Returns {written, same}.
	// The SPI flash's three security registers (768 bytes), each checked against the stub's CRC-32 and read again when it
	// doesn't match. null: a stub before protocol 5, or a flash it doesn't send their commands to (not Winbond: BADARG)
	async sregReadAll({ tries = 3 } = {}) {
		if (!(this.info?.version >= SREG_VERSION)) return null;
		const out = new Uint8Array(SREG_LEN * SREG_COUNT);
		for (let reg = 1; reg <= SREG_COUNT; reg++) {
			for (let i = 0; ; i++) {
				let r;
				try {
					r = await this.request(OP.SREAD, new Uint8Array([reg]), { timeout: 2000 });
				} catch (e) {
					if (e.status === BADARG) return null;
					throw e;
				}
				if (r.length !== 5 + SREG_LEN) throw new StubError(`SREAD ${reg} reply is ${r.length} bytes, wanted ${5 + SREG_LEN}`);
				const data = r.subarray(5);
				if (crc32(data) === u32be(r, 1)) {
					out.set(data, (reg - 1) * SREG_LEN);
					break;
				}
				if (i + 1 >= tries) throw new StubError(`security register ${reg}: no read matching the stub's CRC after ${tries} tries`);
				this.log(`security register ${reg}: CRC mismatch, reading again`);
			}
		}
		return out;
	}

	async spiWrite(data, start = 0, { progress = () => {}, signal } = {}) {
		this.needSpi();
		if (start % BLOCK || data.length % BLOCK || start + data.length > this.info.spiSize) throw new StubError("SPI flash writes are whole 4 KB sectors inside the chip");
		const todo = await this.diff(data, start, MEM_SPI);
		for (const [n, a] of todo.entries()) {
			if (signal?.aborted) throw new StubError("stopped");
			const sec = data.subarray(a - start, a - start + BLOCK);
			await this.request(OP.XERASE, concat(be32(a), be32(F4_MAGIC)), { retries: 0, timeout: 2000 });
			for (let off = 0; off < BLOCK; off += 1024) {
				const piece = sec.subarray(off, off + 1024);
				if (!isBlank(piece)) await this.request(OP.XPROGRAM, concat(be32(a + off), piece), { retries: 0, timeout: 2000 });
			}
			progress(n + 1, todo.length);
		}
		const bad = todo.length ? await this.diff(data, start, MEM_SPI) : [];
		if (bad.length) throw new StubError(`SPI flash verify: ${bad.length} sector(s) differ, first at 0x${bad[0].toString(16)}`);
		return { written: todo.length, same: data.length / BLOCK - todo.length };
	}
}
