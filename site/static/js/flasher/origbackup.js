// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A backup before the install, through the DFU device of the radio's original firmware or of its bootloader (board.toml
// [flash.original_backup], flash-boards.json original_backup; docs/dev/web-flasher.md "Backup before the install").
// What a board's devices answer is in its hardware page (docs/spec/hardware-md9600.md 15.1, 15.2). No DOM here.
//
// Every request goes through Guard, an allowlist: GETSTATUS, CLRSTATUS, ABORT, UPLOAD, and DNLOAD block 0 with exactly
// `A2 ss`, `21` + a 32-bit address, `91 01`, and `91 05` (the original firmware only, where it restarts the radio).
// Anything else throws before it reaches the device: no erase, no write (DNLOAD block 2 and up), no zero-length DNLOAD
// (the original firmware's manifest writes its SPI flash and restarts), no other block 0 command.
//
// The order matters. The internal flash is readable only before program mode (`91 01`): an UPLOAD of block 2 or above
// then answers from address 0, the flash from 0x08000000 (bootloader first), the block number ignored, so one request
// is one snapshot as long as the host allows. The SPI flash is readable only in program mode. The original firmware
// leaves program mode only by restarting (`91 05`); the bootloader stays in it, and its 91 01 works once, so a write in
// the same session starts at 91 31 (dfu.js run programMode).

import { hex, toHex } from "./bytes.js";
import { runsOriginal, parseVersion } from "./origversion.js";

const DNLOAD = 1, UPLOAD = 2, GETSTATUS = 3, CLRSTATUS = 4, ABORT = 6;
const ST_IDLE = 2, ST_DNBUSY = 4, ST_DNIDLE = 5, ST_ERROR = 10;
const BLOCK = 1024;
// One snapshot of the internal flash, largest first: WebUSB on Windows and macOS takes 65535; Linux caps a control
// transfer at 4096
const MIRROR_SIZES = [65535, 32768, 16384, 8192, 4096];
const BYTES_PER_S = 47000;	// the SPI read, device paced (measured)

export class BackupRefused extends Error {}

const same = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);

// The allowlist. original: the device is the original firmware (91 05 allowed); a bootloader never gets 91 05
export class Guard {
	constructor(transport, { original }) {
		this.t = transport;
		this.original = original;
		this.programMode = false;
	}

	allowedCommand(d) {
		if (d.length === 2 && d[0] === 0xA2) return true;
		if (d.length === 5 && d[0] === 0x21) return true;
		if (same(d, [0x91, 0x01])) return true;
		return this.original && same(d, [0x91, 0x05]);
	}

	async out(request, value, data = new Uint8Array(0)) {
		const ok = (request === CLRSTATUS || request === ABORT) ? value === 0 && !data.length
			: request === DNLOAD && value === 0 && data.length > 0 && this.allowedCommand(data);
		if (!ok) throw new BackupRefused(`refused: request ${request}, block ${value}, ${data.length} bytes [${toHex(data.subarray(0, 2))}]`);
		if (request === DNLOAD && same(data, [0x91, 0x01])) this.programMode = true;
		return this.t.out(request, value, data);
	}

	async in(request, length, value = 0) {
		const ok = request === GETSTATUS ? length === 6 && value === 0
			: request === UPLOAD && value !== 1 && (length <= BLOCK || (value === 2 && !this.programMode && length <= 65535));
		if (!ok) throw new BackupRefused(`refused: IN request ${request}, block ${value}, ${length} bytes`);
		return this.t.in(request, length, value);
	}
}

// Which device a fingerprint shows: "original", "bootloader" (backup.loader_interface), or null
export function deviceKind(board, fingerprint) {
	const ifs = fingerprint?.interfaces || [];
	if (runsOriginal(board.original_version, ifs)) return "original";
	if (runsOriginal({ interface: board.original_backup?.loader_interface }, ifs)) return "bootloader";
	return null;
}

// How long a read of n SPI bytes takes, in seconds
export const spiSeconds = (n) => Math.ceil(n / BYTES_PER_S);

// The SPI bytes a backup reads: "full", "quick" or "none", capped where the device can't read on
export function spiSize(spec, kind, choice) {
	if (choice === "none") return 0;
	const want = choice === "quick" ? spec.quick : spec.spi;
	return kind === "bootloader" ? Math.min(want, spec.loader_spi) : want;
}

class Session {
	constructor(guard) { this.g = guard; }

	async status() {
		const r = await this.g.in(GETSTATUS, 6);
		return { status: r[0], state: r[4] };
	}

	async idle() {
		const { state } = await this.status();
		if (state === ST_ERROR) await this.g.out(CLRSTATUS, 0);
		else if (state !== ST_IDLE) await this.g.out(ABORT, 0);
	}

	// a block 0 command: DNLOAD, GETSTATUS (busy: it runs after this reply), GETSTATUS (done), ABORT
	async command(bytes) {
		await this.g.out(DNLOAD, 0, new Uint8Array(bytes));
		const a = await this.status(), b = await this.status();
		if (a.status || a.state !== ST_DNBUSY || b.status || b.state !== ST_DNIDLE)
			throw new Error(`command ${toHex(bytes)}: states ${a.state}, ${b.state}`);
		await this.g.out(ABORT, 0);
	}

	async info(ss) {
		await this.command([0xA2, ss]);
		return await this.g.in(UPLOAD, BLOCK, 0);
	}

	// One snapshot of the internal flash from 0x08000000, before program mode; the largest the host passes
	async mirror(max, log) {
		for (const n of MIRROR_SIZES.filter((x) => x <= max)) {
			try {
				const d = await this.g.in(UPLOAD, n, 2);
				await this.g.out(ABORT, 0);
				if (d.length) return d;
			} catch (e) {
				if (e instanceof BackupRefused) throw e;
				log(`internal flash read of ${n} bytes: ${e?.message || e}`);
				try { await this.idle(); } catch (e2) { /* the next size tries again */ }
			}
		}
		return null;
	}

	async spi(size, { progress, signal }) {
		await this.command([0x91, 0x01]);
		await this.command([0x21, 0, 0, 0, 0]);
		const out = new Uint8Array(size);
		let done = 0;
		for (let b = 2; done < size; b++) {
			if (signal?.aborted) break;
			const d = await this.g.in(UPLOAD, BLOCK, b);
			if (d.length !== BLOCK) throw new Error(`SPI block ${b}: ${d.length} bytes`);
			out.set(d.subarray(0, Math.min(BLOCK, size - done)), done);
			done += BLOCK;
			progress(Math.min(done, size), size);
		}
		await this.g.out(ABORT, 0);
		return { data: out.subarray(0, Math.min(done, size)), complete: done >= size };
	}

	// 91 05: the original firmware restarts before it answers, so a failed GETSTATUS is the success
	async restart() {
		await this.g.out(DNLOAD, 0, new Uint8Array([0x91, 0x05]));
		try { await this.status(); } catch (e) { return true; }
		try { await this.status(); } catch (e) { return true; }
		return false;
	}
}

// Read what the device gives. kind: "original" or "bootloader" (deviceKind). spi: "full", "quick" or "none".
// Returns {parts (backup.js part objects), version, loaderVersion, uid, programMode, restarted}. progress(what, done, total)
export async function readOriginal(transport, board, kind, { spi = "full", progress = () => {}, signal, log = () => {} } = {}) {
	const spec = board.original_backup;
	if (!spec || !["original", "bootloader"].includes(kind)) throw new BackupRefused("this device is not one this board backs up");
	const g = new Guard(transport, { original: kind === "original" });
	const s = new Session(g);
	const parts = [];
	await s.idle();
	const infos = {};
	for (const code of spec.info) {
		const ss = parseInt(code, 16);
		try {
			infos[code] = await s.info(ss);
			parts.push({ file: `info/info-${code}.bin`, what: `info-${code}`, addr: ss, data: infos[code], complete: true });
		} catch (e) {
			if (e instanceof BackupRefused) throw e;
			log(`info ${code}: ${e?.message || e}`);
			await s.idle().catch(() => {});
		}
		progress("info", Object.keys(infos).length, spec.info.length);
	}
	const v = infos["02"];
	const version = parseVersion(v);
	const loaderVersion = v?.length === 4 && v[0] === 0x54 ? toHex(v.subarray(1)) : null;
	const u = infos["07"];
	const uid = u?.length >= 16 ? toHex(u.subarray(4, 16)) : null;
	const mirror = await s.mirror(spec.mirror, log);
	if (mirror) parts.push({ file: `bootloader-${mirror.length}.bin`, what: "internal-flash-mirror", addr: 0x08000000, data: mirror, complete: true });
	progress("mirror", 1, 1);
	const size = spiSize(spec, kind, spi);
	let restarted = false;
	if (size) {
		const r = await s.spi(size, { progress: (d, n) => progress("spi", d, n), signal });
		const mb = size >= 0x100000 && size % 0x100000 === 0 ? `${size / 0x100000}MB` : hex(size);
		parts.push({ file: size === spec.spi ? "spi-flash.bin" : `spi-flash-0-${mb}.bin`, what: "spi-flash", addr: 0, data: r.data, complete: r.complete && size === spec.spi });
		if (kind === "original") restarted = await s.restart();
	}
	return { parts, version, loaderVersion, uid, programMode: kind === "bootloader" && g.programMode, restarted };
}
