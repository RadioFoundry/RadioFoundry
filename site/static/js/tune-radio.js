// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Radio tuning on the debug link (docs/spec/radio-tune.md): the record the radio checks and loads (built and checked
// here, so the radio's side stays small), its save through STORAGE TUNE_WRITE, the live words of RADIO TUNE_SET, the
// live drive of RADIO TX_DRIVE, the oscillator's SETTING 35, the battery reading, and the test transmission: the VFO
// on the tuning frequency, a mode, no tone, the test TX limit on that frequency alone, a power level, PTT with a
// time-out. `link` is a flasher Link (request(cmd, payload) -> the reply after the status; throws with .status).

export const KIND = Object.freeze({ DRIVE: 0, DEV: 1, FREQ: 2, NAME: 3, BATT: 4 });
export const MODE = Object.freeze({ FM: 0, DMR: 1 });
export const BATT = Object.freeze({ GAIN: 0, EMPTY: 1, FULL: 2 });	// a battery entry's sel
const W_BATT = 3;			// a battery entry's kind in a RAM word (names are never words)
export const MAGIC = 0x4E544652;	// "RFTN"
export const VERSION = 1;
export const HDR = 16, ENTRY = 8, MAX_ENTRIES = 32, NAME_LEN = 6;
export const RECORD_MAX = HDR + MAX_ENTRIES * ENTRY + 8;
export const DEV_MAX = 250;		// permille either way
export const FREQ_MAX = 2000;		// Hz either way
export const VALUE_MAX = 4095;		// what a RAM word holds
export const REF_TRIM_MAX = 10000;	// ppb either way (SETTING 35)
export const GAIN_MAX = 150;		// the battery reading's gain, permille either way
export const BATT_MV_MAX = 1000;	// the empty or full voltage, mV either way
export const BATT_SPAN_MV = 400;	// empty and full at least this far apart, else the radio keeps its own
export const CHUNK = 60;		// TUNE_WRITE data bytes per request: the 64-byte request less op, copy, offset
const WORDS_PER_SET = 15;		// TUNE_SET words per request: the 64-byte request less op, first, total
export const ST = Object.freeze({ BAD_ARG: 2, BUSY: 3, UNAVAILABLE: 4, REFUSED: 7 });
const REFUSED_FOREIGN = 2;		// TUNE_WRITE's reason: the tune area holds data that isn't a tune copy

const PTT = 0x07, GET_VIEW = 0x09, RADIO = 0x0A, STORAGE = 0x0F;
const S_OP = Object.freeze({ INFO: 0, READ: 1, TUNE_WRITE: 3, TUNE_LOAD: 4 });
const R = Object.freeze({ VFO: 4, VFO_SET: 5, POWER: 7, TONE: 10, SETTING: 24, MODE: 31, TX_LIMIT: 35, TX_DRIVE: 36,
	TUNE_SET: 44 });
const SETTING_REF_TRIM = 35;
const PTT_MAX_MS = 10000;

const dec = new TextDecoder();
const enc = new TextEncoder();
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const dv = (b) => new DataView(b.buffer, b.byteOffset, b.byteLength);
const le32 = (n) => [n & 0xFF, (n >>> 8) & 0xFF, (n >>> 16) & 0xFF, (n >>> 24) & 0xFF];
const le16 = (n) => [n & 0xFF, (n >> 8) & 0xFF];

// ---- the record

// The storage CRC-32 (storage_crc32): reflected 0xEDB88320, start and end inverted; crc32(b, crc32(a)) = crc32(a + b)
export function crc32(bytes, crc = 0) {
	let c = ~crc >>> 0;
	for (const x of bytes) {
		c ^= x;
		for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xEDB88320 & -(c & 1));
	}
	return ~c >>> 0;
}

export function hexBytes(hex) {
	const s = String(hex || "").replace(/[^0-9a-f]/gi, "");
	return Uint8Array.from(s.match(/../g) || [], (h) => parseInt(h, 16));
}

// Whose record: the board's name, then the chip's unique ID as INFO_CHIP gives it (a radio without one: the name alone)
export function owner(board, uidHex) {
	const crc = crc32(enc.encode(board || ""));
	const uid = hexBytes(uidHex);
	return uid.length ? crc32(uid, crc) : crc;
}

// A frequency as the record keeps it (25 kHz units) and as a RAM word does (100 kHz): trims sit on a 100 kHz grid
export const gridKhz = (khz) => Math.round(khz / 100) * 100;

// entries: [{kind, sel, khz, value, factory}] (drive, deviation, frequency; battery with khz 0) and
// [{kind: NAME, sel, band, name}]
export function encodeRecord(seq, own, entries) {
	const n = Math.min(entries.length, MAX_ENTRIES);
	const b = new Uint8Array(HDR + n * ENTRY + 8);
	const v = dv(b);
	v.setUint32(0, MAGIC, true);
	v.setUint32(4, seq >>> 0, true);
	b[8] = VERSION;
	b[9] = n;
	v.setUint32(12, own >>> 0, true);
	entries.slice(0, n).forEach((e, i) => {
		const o = HDR + i * ENTRY;
		b[o] = (e.kind << 4) | (e.sel & 0xF);
		if (e.kind === KIND.NAME) {
			b[o + 1] = e.band;
			b.set(enc.encode(cleanName(e.name)).subarray(0, NAME_LEN), o + 2);
		} else {
			v.setUint16(o + 2, Math.round(e.khz / 25), true);
			v.setInt16(o + 4, e.value, true);
			v.setUint16(o + 6, e.kind === KIND.DRIVE ? e.factory : 0, true);
		}
	});
	const end = HDR + n * ENTRY;
	v.setUint32(end, crc32(b.subarray(0, end)), true);
	return b;	// the commit word (0) is the last 4 bytes
}

// A copy as the radio checks it: {seq, entries} when whole and this radio's, else null
export function parseRecord(b, own) {
	if (!b || b.length < HDR + 8) return null;
	const v = dv(b);
	const n = b[9];
	if (v.getUint32(0, true) !== MAGIC || b[8] !== VERSION || n > MAX_ENTRIES || v.getUint32(4, true) === 0 ||
	    v.getUint32(12, true) !== own >>> 0 || b.length < HDR + n * ENTRY + 8) return null;
	const end = HDR + n * ENTRY;
	if (crc32(b.subarray(0, end)) !== v.getUint32(end, true) || v.getUint32(end + 4, true) !== 0) return null;
	const entries = [];
	for (let i = 0; i < n; i++) {
		const o = HDR + i * ENTRY;
		const kind = b[o] >> 4, sel = b[o] & 0xF;
		if (kind === KIND.NAME) {
			const raw = b.subarray(o + 2, o + 2 + NAME_LEN);
			const z = raw.indexOf(0);
			entries.push({ kind, sel, band: b[o + 1], name: dec.decode(z < 0 ? raw : raw.subarray(0, z)) });
		} else if (kind <= KIND.BATT) {
			entries.push({ kind, sel, khz: v.getUint16(o + 2, true) * 25, value: v.getInt16(o + 4, true),
				factory: v.getUint16(o + 6, true) });
		}
	}
	return { seq: v.getUint32(4, true), entries };
}

// Which copy is in force, as the radio picks it: the newer valid one (sequence as a signed difference); -1 none
export function inForce(a, b) {
	if (a && b) return ((b.seq - a.seq) | 0) > 0 ? 1 : 0;
	return a ? 0 : b ? 1 : -1;
}

// A RAM word (radio-tune.md 5): kHz/100 bits 0-13, kind 14-15, level or mode 16-18, value 19-31 (signed)
export function word(khz, kind, sel, value) {
	return ((Math.round(khz / 100) & 0x3FFF) | (kind << 14) | ((sel & 7) << 16) | (value << 19)) >>> 0;
}

// The words the radio would load from entries: names left out, drive trims only where the factory drive is still the
// one they were tuned against (factoryAt(khz, level): the drive now, undefined when not known), battery trims as
// word kind 3 at frequency 0
export function toWords(entries, factoryAt = () => undefined) {
	return entries.filter((e) => e.kind !== KIND.NAME && Math.abs(e.value) <= VALUE_MAX && e.sel < 8 &&
		(e.kind !== KIND.BATT || e.sel <= BATT.FULL) &&
		(e.kind !== KIND.DRIVE || (e.factory > 0 && (factoryAt(e.khz, e.sel) ?? e.factory) === e.factory)))
		.map((e) => (e.kind === KIND.BATT ? word(0, W_BATT, e.sel, e.value) : word(e.khz, e.kind, e.sel, e.value)));
}

// ---- the battery, as the radio works it out (core/radio/battery.c)

const CURVE = [0, 2, 4, 10, 22, 42, 60, 75, 88, 100];	// charge at nine even steps from empty to full

// A reading with the gain trim (permille), truncated toward zero as the radio does
export const battScale = (mv, gain) => mv + Math.trunc(mv * gain / 1000);

// Empty and full with the owner's offsets, or the board's own when those bring them closer than BATT_SPAN_MV
export function battRange(empty, full, dEmpty = 0, dFull = 0) {
	const e = empty + dEmpty, f = full + dFull;
	if (e > 0 && f >= e + BATT_SPAN_MV) return { empty: e, full: f, ok: true };
	return { empty, full, ok: dEmpty === 0 && dFull === 0 };
}

export function battPct(mv, empty, full) {
	if (mv >= full) return 100;
	if (mv <= empty) return 0;
	const span = full - empty, x = (mv - empty) * (CURVE.length - 1), i = Math.floor(x / span);
	return CURVE[i] + Math.floor((CURVE[i + 1] - CURVE[i]) * (x % span) / span);
}

// The value of kind/sel at khz from its trims in the band [lo, hi]: linear between the two around khz, flat past the
// outer ones, 0 with none (the radio's own rule, radio-tune.md 2)
export function valueAt(entries, kind, sel, khz, lo = 0, hi = Infinity) {
	const ps = entries.filter((e) => e.kind === kind && e.sel === sel && e.khz >= lo && e.khz <= hi)
		.sort((a, b) => a.khz - b.khz);
	if (!ps.length) return 0;
	if (khz <= ps[0].khz) return ps[0].value;
	if (khz >= ps[ps.length - 1].khz) return ps[ps.length - 1].value;
	const i = ps.findIndex((p) => p.khz >= khz);
	const a = ps[i - 1], b = ps[i];
	return Math.round(a.value + (b.value - a.value) * (khz - a.khz) / (b.khz - a.khz));
}

// A name the radio takes: printable ASCII (the radio's fonts), at most 6 characters
export function cleanName(s) {
	return String(s ?? "").replace(/[^\x20-\x7E]/g, "").trim().slice(0, NAME_LEN);
}

// ---- the radio's views

export function kv(text) {
	const out = {};
	for (const line of text.split("\n")) {
		const i = line.indexOf("=");
		if (i > 0) out[line.slice(0, i)] = line.slice(i + 1);
	}
	return out;
}

// The core view's facts the page needs: bands (kHz, board order, tx), levels, names and drives here, tuning's room
export function parseCore(v) {
	const tune = (v.tune || "").split("/");
	const txMask = parseInt(tune[5] || "0", 16) || 0;
	const bands = (v.bands || "").split(",").filter(Boolean).map((s, i) => {
		const [lo, hi] = s.split("-").map((x) => Math.round(+x / 1000));
		return { index: i, lo, hi, tx: !!(txMask & (1 << i)) };
	});
	const list = (s) => (s ? s.split("/") : []);
	return {
		built: tune.length >= 6,
		words: +tune[0] || 0, maxWords: +tune[1] || 0, names: +tune[2] || 0, maxNames: +tune[3] || 0,
		devModes: parseInt(tune[4] || "0", 16) || 0, liveDrive: tune[6] === "1",
		bands, levels: +v.power_count || 0, power: +v.power || 0,
		levelNames: list(v.power_names), drives: list(v.tx_drive).map((x) => +x || 0),
		rxHz: +v.rx_hz || 0, txHz: +v.tx_hz || 0, mode: v.mode || "", vfo: v.vfo === "1", state: v.state || "",
		deny: v.tx_deny || "",
		// The battery as read before the gain trim, the board's empty and full voltages; null without a battery
		battery: +tune[8] > 0 ? { raw: +tune[7] || 0, empty: +tune[8], full: +tune[9] || 0 } : null,
	};
}

// ---- the radio

export class TuneRadio {
	constructor(link) {
		this.link = link;
		this.saved = null;	// the radio as it was before the page moved its VFO (restore)
	}

	async view(section = 0) {
		const r = await this.link.request(GET_VIEW, new Uint8Array([section]));
		return kv(dec.decode(r.subarray(5)));
	}
	async core() { return parseCore(await this.view(0)); }

	// The battery now: the core view's reading before the gain trim, and the radio's own voltage and charge
	async battery() {
		const c = await this.core();
		if (!c.battery) return null;
		const f = await this.view(7);
		return { ...c.battery, mv: +f.battery_mv || 0, pct: +f.battery_pct || 0, level: f.battery || "" };
	}

	// The trim in effect (ppb), or null on a radio without one (`ref_trim=-`) or a firmware without the key
	async refTrim() {
		const v = await this.view(9);
		return v.ref_trim === undefined || v.ref_trim === "-" ? null : +v.ref_trim;
	}

	// A radio request; the radio's request rate limit answers busy, so a busy is tried again
	async radio(op, bytes = []) {
		for (let i = 0; ; i++) {
			try {
				return await this.link.request(RADIO, new Uint8Array([op, ...bytes]));
			} catch (e) {
				if (e?.status !== ST.BUSY || i >= 8) throw e;
				await sleep(40);
			}
		}
	}

	// ---- the record

	// The tune area {addr, unit}, or null in a build without one
	async area() {
		const r = await this.link.request(STORAGE, new Uint8Array([S_OP.INFO]));
		if (r.length < 34) return null;
		const v = dv(r);
		const addr = v.getUint32(26, true), unit = v.getUint32(30, true);
		return addr ? { addr, unit } : null;
	}

	async read(addr, n) {
		const r = await this.link.request(STORAGE, new Uint8Array([S_OP.READ, ...le32(addr), ...le16(n)]));
		return r.subarray(4);
	}

	// Both copies parsed (null where invalid), the one in force and the next sequence number
	async copies(area, own) {
		const raw = [await this.read(area.addr, RECORD_MAX), await this.read(area.addr + area.unit, RECORD_MAX)];
		const recs = raw.map((b) => parseRecord(b, own));
		const at = inForce(recs[0], recs[1]);
		const seq = at < 0 ? 0 : recs[at].seq;
		return { recs, at, record: at < 0 ? null : recs[at], next: ((seq + 1) >>> 0) || 1 };
	}

	// Throws with tuneStep "foreign" when the radio finds other data in the tune area: nothing is erased then
	async writeCopy(copy, rec) {
		const body = rec.subarray(0, rec.length - 4);
		const put = (off, data) => this.link.request(STORAGE, new Uint8Array([S_OP.TUNE_WRITE, copy, ...le16(off), ...data]))
			.catch((e) => {
				if (e?.status === ST.REFUSED && e.data?.[0] === REFUSED_FOREIGN)
					throw Object.assign(new Error("foreign"), { tuneStep: "foreign", status: e.status });
				throw e;
			});
		for (let off = 0; off < body.length; off += CHUNK) await put(off, body.subarray(off, off + CHUNK));
		await put(body.length, rec.subarray(rec.length - 4));	// the commit word last: a copy cut short stays invalid
	}

	// Save (radio-tune.md 4): the copy not in force first, read back, then the other one, then load. Throws with
	// tuneStep "verify" when a copy reads back wrong; the copy in force before is then still in force
	async save(area, at, rec) {
		const first = at === 0 ? 1 : 0;
		for (const copy of [first, 1 - first]) {
			await this.writeCopy(copy, rec);
			const back = await this.read(area.addr + copy * area.unit, rec.length);
			if (back.length !== rec.length || back.some((x, i) => x !== rec[i]))
				throw Object.assign(new Error("verify"), { tuneStep: "verify" });
		}
		return this.load();
	}

	// The copies the radio found valid; busy while a save or a CPS write holds the radio's storage buffer
	async load() {
		for (let i = 0; ; i++) {
			try {
				return (await this.link.request(STORAGE, new Uint8Array([S_OP.TUNE_LOAD])))[0];
			} catch (e) {
				if (e?.status !== ST.BUSY || i >= 20) throw e;
				await sleep(100);
			}
		}
	}

	// The RAM table, live from the next use; each request's total is only what was written so far, so a reader on the
	// radio never sees a word left from the table before
	async setWords(words) {
		if (!words.length) {
			await this.radio(R.TUNE_SET, [0, 0]);
			return;
		}
		for (let first = 0; first < words.length; first += WORDS_PER_SET) {
			const part = words.slice(first, first + WORDS_PER_SET);
			await this.radio(R.TUNE_SET, [first, first + part.length, ...part.flatMap(le32)]);
		}
	}

	// The drive of this transmission (the radio's own units); 0 the level's again. On a radio with the live drive it
	// moves an FM transmission at once
	async txDrive(drive) { await this.radio(R.TX_DRIVE, le16(Math.max(0, Math.round(drive)))); }

	// The oscillator trim: in effect at once and stored as the menu stores it
	async setRefTrim(ppb) {
		const v = Math.max(-REF_TRIM_MAX, Math.min(REF_TRIM_MAX, Math.round(ppb / 100) * 100));
		await this.radio(R.SETTING, [SETTING_REF_TRIM, ...le16(v & 0xFFFF)]);
		return v;
	}

	// ---- the test transmission

	async waitCore(test, what, ms = 6000) {
		const until = Date.now() + ms;
		for (;;) {
			const c = await this.core();
			if (test(c)) return c;
			if (Date.now() > until) throw Object.assign(new Error(what), { tuneStep: what });
			await sleep(150);
		}
	}

	// The VFO on khz in mode without a tone, transmissions limited to khz alone. The first call keeps the VFO's own state
	// (and whether the radio was on a channel) for restore(). Returns the core view there
	async setup(khz, mode = MODE.FM) {
		let c = await this.core();
		const onChannel = !c.vfo;
		if (onChannel) {
			await this.radio(R.VFO, [1]);
			c = await this.waitCore((x) => x.vfo, "vfo");
		}
		if (!this.saved) {
			const ch = await this.view(1);
			this.saved = { channel: onChannel, hz: c.rxHz, mode: c.mode, power: c.power,
				tones: [[1, ch["ch.tx_tone"]], [2, ch["ch.rx_tone"]]] };
		}
		const want = mode === MODE.DMR ? "dmr" : "fm";
		if (c.mode !== want) {
			try { await this.radio(R.MODE, [mode, 0xFF, 0]); } catch (e) { if (mode !== MODE.FM) throw e; }
			c = await this.waitCore((x) => x.mode === want, mode === MODE.DMR ? "dmr" : "fm");
		}
		if (mode === MODE.FM) await this.radio(R.TONE, [3, 0, 0, 0]);
		const hz = khz * 1000;
		if (c.rxHz !== hz || c.txHz !== hz) {
			await this.radio(R.VFO_SET, [...le32(hz), ...le32(0)]);	// TX 0: simplex, whatever offset the VFO had
			c = await this.waitCore((x) => x.rxHz === hz && x.txHz === hz, "frequency");
		}
		await this.txLimit(khz);
		return c;
	}

	async txLimit(khz) {
		await this.radio(R.TX_LIMIT, khz ? [0, 1, ...le32(khz * 1000)] : [0, 0]);
	}

	async setLevel(n) {
		for (let i = 0; i < 16; i++) {
			const p = (await this.core()).power;
			if (p === n) return;
			await this.radio(R.POWER, [n > p ? 1 : 0xFF]);
			await sleep(150);
		}
		throw Object.assign(new Error("level"), { tuneStep: "level" });
	}

	// PTT down with the radio's own time-out, so a lost link never leaves the radio keyed
	async keyUp(maxMs) {
		await this.link.request(PTT, new Uint8Array([1, ...le16(Math.min(PTT_MAX_MS, Math.max(500, Math.round(maxMs))))]));
	}
	async keyDown() { await this.link.request(PTT, new Uint8Array([0, 0, 0])); }

	// The radio as it was: no PTT, no live drive, no TX limit; the VFO's level, tones, mode and frequency; the channel
	async restore() {
		const s = this.saved;
		this.saved = null;
		const quiet = async (f) => { try { await f(); } catch (e) { /* the radio may be gone */ } };
		await quiet(() => this.keyDown());
		await quiet(() => this.waitCore((x) => x.state !== "tx" && x.state !== "tx_tail", "end", 4000));
		await quiet(() => this.txDrive(0));
		await quiet(() => this.txLimit(0));
		if (!s) return;
		await quiet(() => this.setLevel(s.power));
		const types = { none: 0, ctcss: 1, dcs: 2, dcs_inv: 3 };
		for (const [which, tone] of s.tones) {
			const [type, value] = String(tone || "none:0").split(":");
			if (type in types) await quiet(() => this.radio(R.TONE, [which, types[type], ...le16(+value || 0)]));
		}
		const modes = { fm: 0, dmr: 1, p25: 2, nxdn: 3 };
		if (s.mode in modes) await quiet(() => this.radio(R.MODE, [modes[s.mode], 0xFF, 0]));
		if (s.hz) await quiet(() => this.radio(R.VFO_SET, le32(s.hz)));
		if (s.channel) await quiet(() => this.radio(R.VFO, [0]));
	}
}
