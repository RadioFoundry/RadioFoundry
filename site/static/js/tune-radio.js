// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Power tuning on the debug link (docs/spec/debuglink.md 3.31, power-tune.md) and the test transmission the tuning page
// keys: the VFO on the tuning frequency, FM, no tone, the test TX limit on that frequency alone, the level, a preview
// of the adjustment, PTT for a few seconds. `link` is a flasher Link (request(cmd, payload) -> reply after the status,
// throws with .status).

export const TUNE = 0x27;
const PTT = 0x07, GET_VIEW = 0x09, RADIO = 0x0A;
const OP = Object.freeze({ INFO: 0, READ: 1, LIMITS: 2, PREVIEW: 3, PUT_POINT: 4, DEL_POINT: 5, PUT_LABEL: 6, CLEAR: 7 });
const R = Object.freeze({ VFO: 4, VFO_SET: 5, POWER: 7, TONE: 10, TX_LIMIT: 35, DMR_FIELDS: 31 });
export const STATES = ["applied", "stale", "gone"];
export const ST = Object.freeze({ BAD_ARG: 2, BUSY: 3, UNAVAILABLE: 4, REFUSED: 7 });
export const NAME_MAX = 6;

const dec = new TextDecoder();
const enc = new TextEncoder();
const view = (b) => new DataView(b.buffer, b.byteOffset, b.byteLength);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const le32 = (n) => [n & 0xFF, (n >>> 8) & 0xFF, (n >>> 16) & 0xFF, n >>> 24];

function names(b, off, count) {
	const out = [];
	for (let i = 0; i < count; i++) {
		const n = b[off];
		out.push(dec.decode(b.subarray(off + 1, off + 1 + n)));
		off += 1 + n;
	}
	return [out, off];
}

export function parseInfo(b) {
	const v = view(b);
	const info = { version: b[0], store: !!(b[1] & 1), floor: !!(b[1] & 2), preview: !!(b[1] & 4), copies: b[2],
		seq: v.getUint32(3, true), maxPoints: b[7], maxLabels: b[8], nameLen: b[9], levels: b[10], bands: [] };
	let off = 12;
	for (let i = 0; i < b[11]; i++, off += 8)
		info.bands.push({ lo: v.getUint32(off, true), hi: v.getUint32(off + 4, true) });
	for (const band of info.bands) [band.names, off] = names(b, off, info.levels);
	return info;
}

// [{factory, tuned, lo, hi, name}] per level, in drive units
export function parseLimits(b) {
	const v = view(b);
	const n = b[0];
	const out = [];
	for (let i = 0; i < n; i++) {
		const o = 1 + 8 * i;
		out.push({ factory: v.getUint16(o, true), tuned: v.getUint16(o + 2, true), lo: v.getUint16(o + 4, true),
			hi: v.getUint16(o + 6, true) });
	}
	names(b, 1 + 8 * n, n)[0].forEach((name, i) => { out[i].name = name; });
	return out;
}

// The record in force, or null: {seq, levels, points: [{khz, level, state, factory, delta, mw}], labels: [{khz, level,
// state, factory, mw, name}]}
export function parseRecord(b) {
	if (b.length < 20) return null;
	const v = view(b);
	const rec = { seq: v.getUint32(4, true), levels: b[11], points: [], labels: [] };
	let off = 20;
	for (let i = 0; i < b[9]; i++, off += 16)
		rec.points.push({ khz: v.getUint32(off, true), level: b[off + 4], state: STATES[Math.min(b[off + 5], 2)],
			factory: v.getUint16(off + 6, true), delta: v.getInt16(off + 8, true), mw: v.getUint32(off + 12, true) });
	for (let i = 0; i < b[10]; i++, off += 20) {
		const raw = b.subarray(off + 12, off + 20);
		const end = raw.indexOf(0);
		rec.labels.push({ khz: v.getUint32(off, true), level: b[off + 4], state: STATES[Math.min(b[off + 5], 2)],
			factory: v.getUint16(off + 6, true), mw: v.getUint32(off + 8, true),
			name: dec.decode(end < 0 ? raw : raw.subarray(0, end)) });
	}
	return rec;
}

// A name the radio takes: printable ASCII (the radio's fonts), at most 6 bytes
export function cleanName(s) {
	return String(s ?? "").replace(/[^\x20-\x7E]/g, "").trim().slice(0, NAME_MAX);
}

function payload(op, build = () => {}, size = 1) {
	const b = new Uint8Array(size);
	b[0] = op;
	build(view(b), b);
	return b;
}

export function encodePoint(khz, level, delta, mw = 0) {
	return payload(OP.PUT_POINT, (v) => {
		v.setUint32(1, khz, true); v.setUint8(5, level); v.setInt16(6, delta, true); v.setUint32(8, Math.round(mw), true);
	}, 12);
}

export function encodeLabel(khz, level, name, mw = 0) {
	const n = enc.encode(cleanName(name));
	const b = payload(OP.PUT_LABEL, (v) => { v.setUint32(1, khz, true); v.setUint8(5, level); v.setUint32(6, Math.round(mw), true); },
		10 + n.length);
	b.set(n, 10);
	return b;
}

// A key=value view section as an object
function kv(text) {
	const out = {};
	for (const line of text.split("\n")) {
		const i = line.indexOf("=");
		if (i > 0) out[line.slice(0, i)] = line.slice(i + 1);
	}
	return out;
}

export class TuneRadio {
	constructor(link) {
		this.link = link;
		this.saved = null;	// the radio's state before the first test transmission
	}

	tune(op, ...rest) { return this.link.request(TUNE, rest.length ? rest[0] : new Uint8Array([op])); }
	async info() { return parseInfo(await this.tune(OP.INFO)); }
	async read() { return parseRecord(await this.tune(OP.READ)); }
	async limits(khz) { return parseLimits(await this.tune(OP.LIMITS, payload(OP.LIMITS, (v) => v.setUint32(1, khz, true), 5))); }
	async preview(level, delta = 0) {
		await this.tune(OP.PREVIEW, payload(OP.PREVIEW, (v) => { v.setUint8(1, level ?? 0xFF); v.setInt16(2, level == null ? 0 : delta, true); }, 4));
	}
	async putPoint(khz, level, delta, mw) { await this.tune(OP.PUT_POINT, encodePoint(khz, level, delta, mw)); }
	async delPoint(khz, level) {
		await this.tune(OP.DEL_POINT, payload(OP.DEL_POINT, (v) => { v.setUint32(1, khz, true); v.setUint8(5, level); }, 6));
	}
	async putLabel(khz, level, name, mw) { await this.tune(OP.PUT_LABEL, encodeLabel(khz, level, name, mw)); }
	async clear() { await this.tune(OP.CLEAR); }

	// ---- the test transmission

	async view(section = 0) {
		const r = await this.link.request(GET_VIEW, new Uint8Array([section]));
		return kv(dec.decode(r.subarray(5)));
	}
	radio(op, bytes = []) { return this.link.request(RADIO, new Uint8Array([op, ...bytes])); }
	async ptt(down, maxMs = 0) {
		const b = payload(down ? 1 : 0, (v) => v.setUint16(1, maxMs, true), 3);
		await this.link.request(PTT, b);
	}
	async txLimit(khz) {
		const b = khz ? payload(0, (v) => { v.setUint8(1, 1); v.setUint32(2, khz * 1000, true); }, 6) : new Uint8Array([0, 0]);
		await this.link.request(RADIO, new Uint8Array([R.TX_LIMIT, ...b]));
	}

	async waitView(test, what, ms = 6000) {
		const until = Date.now() + ms;
		for (;;) {
			const v = await this.view();
			if (test(v)) return v;
			if (Date.now() > until) throw Object.assign(new Error(what), { tuneStep: what });
			await sleep(200);
		}
	}

	// The VFO on khz in FM without a tone, transmissions limited to khz alone. The first call keeps the VFO's own state
	// (and whether the radio was on a channel) for restore()
	async setup(khz) {
		let v = await this.view();
		const onChannel = v.vfo !== "1";
		if (onChannel) {
			await this.radio(R.VFO, [1]);
			v = await this.waitView((x) => x.vfo === "1", "vfo");
		}
		if (!this.saved) {
			const ch = await this.view(1);
			this.saved = { channel: onChannel, hz: +v.rx_hz, mode: v.mode, power: +v.power,
				tones: [[1, ch["ch.tx_tone"]], [2, ch["ch.rx_tone"]]] };
		}
		if (v.mode !== "fm") {
			try { await this.radio(R.DMR_FIELDS, [0, 0xFF, 0]); } catch (e) { /* a build without DMR: FM already */ }
			await this.waitView((x) => x.mode === "fm", "fm");
		}
		await this.radio(R.TONE, [3, 0, 0, 0]);
		await this.radio(R.VFO_SET, le32(khz * 1000));
		await this.waitView((x) => +x.rx_hz === khz * 1000, "frequency");
		await this.txLimit(khz);
	}

	async setLevel(n) {
		for (let i = 0; i < 16; i++) {
			const p = +(await this.view()).power;
			if (p === n) return;
			await this.radio(R.POWER, [n > p ? 1 : 0xFF]);
			await sleep(250);
		}
		throw Object.assign(new Error("level"), { tuneStep: "level" });
	}

	// One test transmission: level with the offset delta (a preview: the radio clamps it), holdMs keyed, then the preview
	// and the TX limit off again. onTick(msLeft)
	// while keyed. Returns the core view seen while keyed; throws {tuneStep: "tx", deny} when the radio didn't transmit
	async key(khz, level, delta, holdMs, onTick = () => {}) {
		await this.setup(khz);
		await this.setLevel(level);
		await this.preview(level, delta);
		const start = Date.now();
		let seen = null;
		await this.ptt(true, Math.min(10000, holdMs + 2500));
		try {
			await sleep(400);
			seen = await this.view();
			for (let left = holdMs - (Date.now() - start); left > 0; left = holdMs - (Date.now() - start)) {
				onTick(left);
				await sleep(Math.min(250, left));
			}
		} finally {
			await this.ptt(false);
			// Nothing left behind between transmissions: a closed page leaves no preview and no TX limit
			try { await this.preview(null); } catch (e) { /* the radio may be gone */ }
			try { await this.txLimit(0); } catch (e) { /* idem */ }
		}
		onTick(0);
		if (seen?.state !== "tx")
			throw Object.assign(new Error("tx"), { tuneStep: "tx", deny: seen?.tx_deny || "", state: seen?.state || "" });
		return seen;
	}

	// The radio as it was: no PTT, no preview, no TX limit; the VFO's level, tones, mode and frequency; the channel
	async restore() {
		const s = this.saved;
		this.saved = null;
		const quiet = async (f) => { try { await f(); } catch (e) { /* the radio may be gone */ } };
		await quiet(() => this.ptt(false));
		await quiet(() => this.preview(null));
		await quiet(() => this.waitView((x) => x.state !== "tx" && x.state !== "tx_tail", "end", 4000));
		await quiet(() => this.txLimit(0));
		if (!s) return;
		await quiet(() => this.setLevel(s.power));
		const types = { none: 0, ctcss: 1, dcs: 2, dcs_inv: 3 };
		for (const [which, tone] of s.tones) {
			const [type, value] = String(tone || "none:0").split(":");
			if (type in types) await quiet(() => this.radio(R.TONE, [which, types[type], +value & 0xFF, (+value >> 8) & 0xFF]));
		}
		const modes = { dmr: 1, p25: 2, nxdn: 3 };
		if (s.mode in modes) await quiet(() => this.radio(R.DMR_FIELDS, [modes[s.mode], 0xFF, 0]));
		if (s.hz) await quiet(() => this.radio(R.VFO_SET, le32(s.hz)));
		if (s.channel) await quiet(() => this.radio(R.VFO, [0]));
	}
}
