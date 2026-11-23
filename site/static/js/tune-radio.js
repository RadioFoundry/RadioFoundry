// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Radio tuning on the debug link (docs/spec/radio-tune.md): the entries the radio keeps in its settings ring, put,
// saved and read back through the STORAGE tuning operations, the live words of RADIO TUNE_SET, the
// live drive of RADIO TX_DRIVE, the oscillator's SETTING 35, the battery reading, and the test transmission: the VFO
// on the tuning frequency, a mode, no tone, the test TX limit on that frequency alone, a power level, PTT with a
// time-out. `link` is a flasher Link (request(cmd, payload) -> the reply after the status; throws with .status).

export const KIND = Object.freeze({ DRIVE: 0, DEV: 1, FREQ: 2, NAME: 3, BATT: 4 });
export const MODE = Object.freeze({ FM: 0, DMR: 1 });
export const BATT = Object.freeze({ GAIN: 0, EMPTY: 1, FULL: 2 });	// a battery entry's sel
const W_BATT = 3;			// a battery entry's kind in a RAM word (names are never words)
export const ENTRY = 8, MAX_ENTRIES = 26, NAME_LEN = 6;	// MAX_ENTRIES: the radio's table (RADIO_TUNE_ENTRIES)
export const DEV_MAX = 250;		// permille either way
export const FREQ_MAX = 2000;		// Hz either way
export const VALUE_MAX = 4095;		// what a RAM word holds
export const REF_TRIM_MAX = 10000;	// ppb either way (SETTING 35)
export const GAIN_MAX = 150;		// the battery reading's gain, permille either way
export const BATT_MV_MAX = 1000;	// the empty or full voltage, mV either way
export const BATT_SPAN_MV = 400;	// empty and full at least this far apart, else the radio keeps its own
const PER_REQUEST = 7;			// entries a TUNE_PUT or TUNE_GET carries
const WORDS_PER_SET = 15;		// TUNE_SET words per request: the 64-byte request less op, first, total
export const ST = Object.freeze({ BAD_ARG: 2, BUSY: 3, UNAVAILABLE: 4, REFUSED: 7 });

const PTT = 0x07, GET_VIEW = 0x09, RADIO = 0x0A, STORAGE = 0x0F;
const S_OP = Object.freeze({ PUT: 3, LOAD: 4, SAVE: 5, CLEAR: 6, GET: 7 });
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

// ---- the entries

// A frequency as an entry keeps it (25 kHz units) and as a RAM word does (100 kHz): trims sit on a 100 kHz grid
export const gridKhz = (khz) => Math.round(khz / 100) * 100;

// An entry (radio-tune.md 3), 8 bytes: {kind, sel, khz, value, factory} (drive, deviation, frequency; battery with
// khz 0) or {kind: NAME, sel, band, name}
export function encodeEntry(e) {
	const b = new Uint8Array(ENTRY);
	const v = dv(b);
	b[0] = (e.kind << 4) | (e.sel & 0xF);
	if (e.kind === KIND.NAME) {
		b[1] = e.band;
		b.set(enc.encode(cleanName(e.name)).subarray(0, NAME_LEN), 2);
	} else {
		v.setUint16(2, Math.round(e.khz / 25), true);
		v.setInt16(4, e.value, true);
		v.setUint16(6, e.kind === KIND.DRIVE ? e.factory : 0, true);
	}
	return b;
}

// An entry back from its 8 bytes; null for a kind the page doesn't know
export function parseEntry(b) {
	const v = dv(b);
	const kind = b[0] >> 4, sel = b[0] & 0xF;
	if (kind === KIND.NAME) {
		const raw = b.subarray(2, 2 + NAME_LEN);
		const z = raw.indexOf(0);
		return { kind, sel, band: b[1], name: dec.decode(z < 0 ? raw : raw.subarray(0, z)) };
	}
	if (kind > KIND.BATT) return null;
	return { kind, sel, khz: v.getUint16(2, true) * 25, value: v.getInt16(4, true), factory: v.getUint16(6, true) };
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

	// ---- the saved entries (radio-tune.md 4)

	// A STORAGE tuning operation: the entries in the radio's table after it. Busy while a save or a CPS write holds
	// the radio's storage, so a busy is tried again; unavailable on a radio without a settings ring of its own
	async tune(op, bytes = []) {
		for (let i = 0; ; i++) {
			try {
				return (await this.link.request(STORAGE, new Uint8Array([op, ...bytes])))[0];
			} catch (e) {
				if (e?.status !== ST.BUSY || i >= 20) throw e;
				await sleep(100);
			}
		}
	}

	// Whether the radio saves trims: false on a radio without a settings ring of its own (live trims only)
	async canSave() {
		try {
			await this.link.request(STORAGE, new Uint8Array([S_OP.GET, 0]));
			return true;
		} catch (e) {
			if (e?.status === ST.UNAVAILABLE) return false;
			throw e;
		}
	}

	// The radio's table as entries, drive entries with the factory drive at their frequency now
	async entries() {
		const out = [];
		for (;;) {
			const r = await this.link.request(STORAGE, new Uint8Array([S_OP.GET, out.length]));
			const n = r[0], got = Math.floor((r.length - 1) / ENTRY);
			for (let k = 0; k < got; k++) out.push(parseEntry(r.subarray(1 + k * ENTRY, 1 + (k + 1) * ENTRY)));
			if (!got || out.length >= n) return out.filter(Boolean);
		}
	}

	// The saved entries: the table loaded from the saved record (live words give way to it), then read
	async stored() {
		await this.load();
		return this.entries();
	}

	// Save: the entries into the table in place of what it holds, the table saved as the record, then read back.
	// Throws with tuneStep "verify" when the table reads back different (an entry the radio can't use left out). A
	// save that fails leaves the record before in force
	async save(entries, same = (a, b) => JSON.stringify(a) === JSON.stringify(b)) {
		const bytes = entries.slice(0, MAX_ENTRIES).map(encodeEntry);
		await this.tune(S_OP.PUT, [1]);
		for (let i = 0; i < bytes.length; i += PER_REQUEST)
			await this.tune(S_OP.PUT, [0, ...bytes.slice(i, i + PER_REQUEST).flatMap((b) => [...b])]);
		await this.tune(S_OP.SAVE);
		const back = await this.entries();
		if (!same(back, entries)) throw Object.assign(new Error("verify"), { tuneStep: "verify" });
		return back;
	}

	// The saved record into the table again: live tries undone
	async load() { return this.tune(S_OP.LOAD); }

	// No trims: the table emptied and an empty record saved
	async clear() { return this.tune(S_OP.CLEAR); }

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
