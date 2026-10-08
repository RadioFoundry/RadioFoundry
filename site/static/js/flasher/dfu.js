// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The AnyRoad (TYT / Baofeng STM32) DFU bootloader over WebUSB. Protocol: docs/spec/hardware-dm1701.md 15.1-15.3, as
// tools/odrflash/odrflash/dfu.py drives it: only the plan's sectors are erased and written, each 1024-byte block
// XORed with the model key (every sector starts a multiple of 1024 bytes after the application base, so each block
// starts at key index 0). The bootloader can't read flash back (the radios are at read protection level 1).

import { BLOCK, FIRST_BLOCK } from "./plan.js";
import { hex, le32, sha256, sleep } from "./bytes.js";

export const KEY_LEN = 1024;
export const EXIT_ADDR = 0x08000000;
const DNLOAD = 1, GETSTATUS = 3, CLRSTATUS = 4, ABORT = 6;
const ST_IDLE = 2, ST_DNBUSY = 4, ST_DNIDLE = 5, ST_ERROR = 10;
const STATE_NAMES = ["appIDLE", "appDETACH", "dfuIDLE", "dfuDNLOAD-SYNC", "dfuDNBUSY", "dfuDNLOAD-IDLE",
	"dfuMANIFEST-SYNC", "dfuMANIFEST", "dfuMANIFEST-WAIT-RESET", "dfuUPLOAD-IDLE", "dfuERROR"];

export class DfuError extends Error {}

export async function checkKey(key, board) {
	if (key.length !== KEY_LEN) throw new DfuError(`the key is ${key.length} bytes, expected ${KEY_LEN}`);
	if (await sha256(key) !== board.key_sha256) throw new DfuError(`this is not the ${board.name} bootloader key`);
	return key;
}

export function xorBlock(block, key) {
	const out = new Uint8Array(block.length);
	for (let i = 0; i < block.length; i++) out[i] = block[i] ^ key[i];
	return out;
}

// A transport has: out(request, value, data?) and in(request, length) -> Uint8Array, both class requests on the DFU
// interface. WebUsbTransport is the real one; fake.js has a simulated bootloader.
export class Dfu {
	constructor(transport, { log = () => {}, wait = sleep } = {}) {
		this.t = transport;
		this.log = log;
		this.wait = wait;
	}

	async getStatus() {
		const r = await this.t.in(GETSTATUS, 6);
		if (r.length !== 6) throw new DfuError(`short GETSTATUS reply (${r.length} bytes)`);
		return { status: r[0], pollMs: r[1] | (r[2] << 8) | (r[3] << 16), state: r[4] };
	}

	async waitIdle() {
		let st;
		for (let attempt = 0; attempt < 20; attempt++) {
			st = await this.getStatus();
			if (st.state === ST_IDLE) return;
			await this.t.out(attempt < 10 ? CLRSTATUS : ABORT, 0);
		}
		throw new DfuError(`the bootloader stays in state ${STATE_NAMES[st.state] || st.state}, not dfuIDLE: power-cycle the radio into the bootloader and try again`);
	}

	async waitDone(what) {
		for (let i = 0; i < 4000; i++) {
			const st = await this.getStatus();
			if (st.state === ST_ERROR || st.status !== 0) throw new DfuError(`${what} failed: status ${st.status}, state ${STATE_NAMES[st.state] || st.state}`);
			if (st.state === ST_DNIDLE) return;
			if (st.state !== ST_DNBUSY) throw new DfuError(`${what}: unexpected state ${STATE_NAMES[st.state] || st.state}`);
			await this.wait(Math.max(st.pollMs, 1));
		}
		throw new DfuError(`${what}: the bootloader stayed busy`);
	}

	async vendor(b0, b1) {
		await this.t.out(DNLOAD, 0, new Uint8Array([b0, b1]));
		await this.getStatus();
		await this.wait(100);
		await this.getStatus();
		await this.waitIdle();
	}

	async command(cmd, addr, what) {
		const data = new Uint8Array(5);
		data[0] = cmd;
		data.set(le32(addr), 1);
		await this.t.out(DNLOAD, 0, data);
		await this.waitDone(what);
	}

	// Erase the plan's sectors in the bootloader's order, write each, exit (the radio restarts).
	async run(plan, key, progress = () => {}) {
		const base = plan.board.app_base;
		for (const st of plan.writes) if ((st.sector.addr - base) % KEY_LEN) throw new DfuError(`sector ${hex(st.sector.addr)} is not key-aligned`);
		await this.waitIdle();
		await this.vendor(0x91, 0x01);
		await this.vendor(0x91, 0x31);
		for (const s of plan.erase) {
			this.log(`erase sector ${s.index} at ${hex(s.addr)}`);
			await this.command(0x41, s.addr, `erase ${hex(s.addr)}`);
		}
		const total = plan.totalBlocks;
		let done = 0;
		for (const st of plan.writes) {
			const addr = st.sector.addr;
			this.log(`write sector ${st.sector.index}: ${st.blocks.length} blocks`);
			await this.command(0x21, addr, `set address ${hex(addr)}`);
			for (const [num, blk] of st.blocks) {
				if (blk.length !== BLOCK) throw new DfuError(`block of ${blk.length} bytes`);
				await this.t.out(DNLOAD, num, xorBlock(blk, key));
				await this.waitDone(`block ${num} at ${hex(addr + (num - FIRST_BLOCK) * BLOCK)}`);
				progress(++done, total);
			}
		}
		await this.command(0x21, EXIT_ADDR, `set address ${hex(EXIT_ADDR)}`);
		await this.t.out(DNLOAD, 0);
		try { await this.getStatus(); } catch (e) { /* the radio restarts; it may not answer */ }
	}
}

// ---------------------------------------------------------------------------------------------------------------
// WebUSB

export const usbSupported = () => typeof navigator !== "undefined" && "usb" in navigator;

export class WebUsbTransport {
	constructor(device, intf) {
		this.device = device;
		this.intf = intf;
	}

	static async request(board) {
		const [vendorId, productId] = board.dfu_ids;
		const device = await navigator.usb.requestDevice({ filters: [{ vendorId, productId }] });
		return WebUsbTransport.open(device);
	}

	static async open(device) {
		await device.open();
		if (device.configuration === null) await device.selectConfiguration(1);
		let intf = null;
		for (const i of device.configuration.interfaces)
			for (const alt of i.alternates)
				if (alt.interfaceClass === 0xFE && alt.interfaceSubclass === 0x01 && intf === null) intf = i.interfaceNumber;
		if (intf === null) throw new DfuError("the device has no DFU interface");
		try {
			await device.claimInterface(intf);
		} catch (e) {
			throw new DfuError(`cannot open the bootloader (${e.message}). Close other programs that use the radio. On Windows the bootloader needs the WinUSB driver (Zadig, once).`);
		}
		return new WebUsbTransport(device, intf);
	}

	async out(request, value, data) {
		const r = await this.device.controlTransferOut({ requestType: "class", recipient: "interface", request, value, index: this.intf }, data || new Uint8Array(0));
		if (r.status !== "ok") throw new DfuError(`USB transfer ${r.status}`);
		return r.bytesWritten;
	}

	async in(request, length) {
		const r = await this.device.controlTransferIn({ requestType: "class", recipient: "interface", request, value: 0, index: this.intf }, length);
		if (r.status !== "ok") throw new DfuError(`USB transfer ${r.status}`);
		return new Uint8Array(r.data.buffer, r.data.byteOffset, r.data.byteLength);
	}

	async stringDescriptor(index) {
		try {
			const r = await this.device.controlTransferIn({ requestType: "standard", recipient: "device", request: 6, value: 0x0300 | index, index: 0x0409 }, 255);
			const b = new Uint8Array(r.data.buffer, r.data.byteOffset, r.data.byteLength);
			return new TextDecoder("utf-16le").decode(b.subarray(2, b[0]));
		} catch (e) {
			return null;
		}
	}

	// What the bootloader shows: the strings the model fingerprint table will be built from
	async fingerprint() {
		const d = this.device;
		const names = [];
		for (const i of d.configuration.interfaces) for (const a of i.alternates) if (a.interfaceName) names.push(a.interfaceName);
		return { string1: await this.stringDescriptor(1), manufacturer: d.manufacturerName, product: d.productName,
			serial: d.serialNumber, version: `${d.deviceVersionMajor}.${d.deviceVersionMinor}.${d.deviceVersionSubminor}`, interfaces: names };
	}

	async close() {
		try { await this.device.close(); } catch (e) { /* already gone: the radio restarted */ }
	}
}
