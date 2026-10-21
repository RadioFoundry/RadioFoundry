// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// A simulated GD-77 in update mode, for the tests and the page's practice mode (#sim=gd77): the loader as
// docs/spec/update-gd77.md reads it (a port of gd77boot.FakeBootloader), a WebHID model to reach it through, and a
// synthetic stock update file with a made-up codec in the shape of a real build (a port of the odrflash tests'
// synthetic image and update file). No vendor data.

import { UpdateFile, forward, PERIOD, BODY_AT, INFO_LEN, REPORT, parse } from "./gd77.js";
import { encodeBranch } from "./gd77codec.js";
import { sha256, u32le } from "./bytes.js";
import { rng, randomBytes } from "./fake.js";

const bytesOf = (s) => Uint8Array.from(s, (c) => c.charCodeAt(0));
const same = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
const sum = (b) => b.reduce((a, x) => a + x, 0);
const N = bytesOf("N");

// The loader: checks every step in order and the running sums, keeps what it received. reject: a payload it refuses.
export class FakeGd77Loader {
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

// navigator.hid as the page uses it, with one radio in update mode behind it (a FakeGd77Loader for the file it will
// be sent). requestDevice() hands out the radio while `present`; replies come as "inputreport" events.
export class FakeHid {
	constructor(loader, { present = true, delay = 0 } = {}) {
		this.loader = loader;
		this.present = present;
		this.delay = delay;
		this.listeners = new Set();
		const self = this;
		this.device = {
			vendorId: 0x15A2, productId: 0x0073, productName: "simulated GD-77 update mode", opened: false,
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

// A valid update file around `plain` (the flat image), with the header fields a GD-77 file has
export function makeUpdateFile(plain, { total = 0x78801 + 0x400, tag = "DV01test", seed = 5, model = "GD-77" } = {}) {
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
export async function gd77Simulation(G) {
	const { image, spec } = await synthetic();
	return { gd77: { ...G, specs: { ...G.specs, synthetic: spec }, placed: { ...G.placed, synthetic: { tag: 0x1234 } } },
		donor: makeUpdateFile(image) };
}

export const loaderFor = (file, opts) => new FakeGd77Loader(new UpdateFile(file), opts);
