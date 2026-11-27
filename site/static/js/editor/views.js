// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// What the codeplug editor shows for each kind of record: table columns, the full form of a record, and the
// membership editor for zones and RX group lists. Setters parse what was typed and return an error text, or
// nothing when the value was taken.

import { h, button } from "../flasher/dom.js";
import { parseFreq, parseTone, mhz, TONES } from "../codeplug/fields.js";
import { LIMITS, POWER_NAMES } from "../codeplug/records.js";
import { refNumber, listenBand, followListen } from "../codeplug/model.js";
import { keyValid } from "../codeplug/kv.js";
import { kindRow, kindName, offeredKinds } from "../codeplug/modes.js";
import { setKind } from "../codeplug/channelmodes.js";
import { specField } from "./modefields.js";
import { combo, comboText } from "./combo.js";

// --- Parsers shared by the columns and the forms ---

const name = (max) => (t) => (r, k, v) => {
	v = String(v ?? "").trim();
	if (!v) return t("editor.err.no_name");
	if (v.length > max) return t("editor.err.too_long", { max });
	r[k] = v;
};

const int = (lo, hi) => (t) => (r, k, v) => {
	const n = Number(String(v).trim());
	if (!Number.isInteger(n) || n < lo || n > hi) return t("editor.err.range", { min: lo, max: hi });
	r[k] = n;
};

const freq = (t) => (r, k, v) => {
	const hz = parseFreq(v);
	if (hz === null) return t("editor.err.bad_freq");
	r[k] = hz;
};

// A TX frequency typed in a receive-only band is refused, and the channel keeps its TX frequency and RX Only. TX on an
// RX in such a band (TX follows RX there) and a TX frequency left unchanged (an import) are taken
const txFreq = (t) => (r, k, v) => {
	const hz = parseFreq(v);
	if (hz === null) return t("editor.err.bad_freq");
	if (listenBand(hz) && !listenBand(r.rxHz) && hz !== r[k]) return t("editor.err.tx_listen", { mhz: mhz(hz) });
	r[k] = hz;
};

// An RX frequency in a receive-only band (model.js listenBand) makes the channel RX only, with TX on RX (followListen)
const rxFreq = (t) => (r, k, v) => {
	const err = freq(t)(r, k, v);
	if (!err) followListen(r);
	return err;
};
const listening = (value) => !!listenBand(parseFreq(value("rxHz")));
// The form's transmit fields (TX frequency, TX tone, power, VOX, tone burst) don't apply while RX Only is ticked, or implied by the band
const noTx = (w, value, checked) => listening(value) || checked("rxOnly");

// The mode as the radio shows it: an FM channel in the air band is received in AM
// A channel of a kind (channel-modes.md): the kind's short name
// a kind's full name, or its short one where the text is missing
const kindText = (id, t) => {
	const key = `editor.mode_name.${kindRow(id)?.token}`, s = kindRow(id) ? t(key) : key;
	return s === key ? kindName(id) : s;
};
const modeText = (r, t) => (r.kind ? kindText(r.kind, t) : r.mode === "dmr" ? "DMR" : listenBand(r.rxHz) ? t(`editor.mode.${listenBand(r.rxHz)}`) : "FM");

// A channel's Mode, in its form and in the table: FM, DMR and the kinds offered (modes.js), the channel's own kind too.
// The value is "fm", "dmr" or "k<kind>"
function modeColumn(m, t, features) {
	const kinds = offeredKinds(m, features);
	return {
		options: (r) => {
			const out = [["fm", "FM"], ["dmr", "DMR"], ...kinds.map((k) => [`k${k.id}`, kindText(k.id, t)])];
			if (r?.kind && !kinds.some((k) => k.id === r.kind)) out.push([`k${r.kind}`, kindText(r.kind, t)]);
			return out;
		},
		get: (r) => (r.kind ? `k${r.kind}` : r.mode),
		set: (r, v) => {
			if (v === (r.kind ? `k${r.kind}` : r.mode)) return;
			if (v.startsWith("k")) setKind(m, r, Number(v.slice(1)));
			else setKind(m, r, 0, v);
		},
	};
}
const kindMod = (r) => (r.kind ? kindRow(r.kind)?.module ?? null : null);
// A channel of a kind keeps the record's fields its module lists (a talk target, a unit ID); the rest of FM and DMR
// stays as stored and is not shown
const recordField = (r, key) => !r.kind || !!kindMod(r)?.recordFields?.includes(key);
// Frequencies of its own: a trunked kind's are its system's
const ownFreqs = (r) => !r.kind || kindMod(r)?.ownFreqs !== false;
// Receive only: for a channel of a kind, its tag's transmit allowed turned round (the record is receive only)
const rxOnlyGet = (r) => (r.kind ? !r.tagTx : r.rxOnly);
// The TX timeouts the record can hold: the radio's default, then 15 s steps to 3825 s (the imports round up to them)
export function totOptions(t) {
	const out = [[0, t("editor.tot.default")]];
	for (let s = 15; s <= 3825; s += 15) out.push([s, s < 60 ? `${s} s` : `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")} (${s} s)`]);
	return out;
}

const rxOnlySet = (r, v) => (r.kind ? (r.tagTx = !v) : (r.rxOnly = !!v));

const tone = (t) => (r, k, v) => {
	const s = parseTone(v);
	if (s === null) return t("editor.err.bad_tone");
	r[k] = s;
};

const dmrId = (t) => (r, k, v) => {
	const s = String(v ?? "").trim();
	if (s === "" || s === "0") {
		r[k] = 0;
		return;
	}
	const n = Number(s);
	if (!Number.isInteger(n) || n < 1 || n > 0xFFFFFF) return t("editor.err.id_range");
	r[k] = n;
};

// A settings area key that is a number (kv.js KEYS: a uint, int, enum or id row; codeplug-settings.md 3) as typed on
// the Radio tab: {value} or {error}. Empty is an absent key (null: the radio keeps its own setting); for an ID, 0
// turns the setting off
export function parseAreaKey(row, v, t) {
	const s = String(v ?? "").trim();
	if (s === "") return { value: null };
	const n = Number(s);
	return keyValid(row, n) ? { value: n } : { error: t("editor.err.range", { min: row.min, max: row.max }) };
}

const pick = (r, k, v) => {
	r[k] = v;
};

const bool = (r, k, v) => {
	r[k] = !!v;
};

export const toneText = (v, t) => v || t("editor.none");

// The tone fields' searchable list (combo.js): every CTCSS tone and DCS code, and any other tone typed
export const toneCombo = (t) => () => ({
	values: TONES, accept: parseTone, none: t("editor.none"),
	use: (v) => t("editor.tone.use", { tone: v || t("editor.none") }), noMatch: t("editor.tone.no_match"),
});

export function powerOptions(t) {
	const opts = [[0, t("editor.power.default")]];
	for (let n = 1; n <= 10; n++) opts.push([n, POWER_NAMES[n] ? t(`editor.power.${POWER_NAMES[n].toLowerCase()}`) : t("editor.power.level", { n: n - 1 })]);
	return opts;
}

function refOptions(list, t, label = (x) => x.name) {
	return [[null, t("editor.none")], ...list.map((x) => [x, label(x)])];
}

const refLabel = (v, t) => (v == null ? "" : typeof v === "number" ? t("editor.missing", { n: v }) : v.name);

// --- Columns per table ---

// The columns Bulk Edit can set: every editable one but the name and number, which must differ per record. None on a
// table whose members are edited in its own dialog (zones, RX lists): Bulk Edit isn't offered there
export const bulkColumns = (table, m, t, opts) => columns(table, m, t, opts).filter((c) => c.readOnly !== true && c.key !== "name" && c.key !== "number");

export function columns(table, m, t, { features = null } = {}) {
	const col = (key, label, type, parse, extra = {}) => ({
		key, label: t(label), type,
		get: (r) => r[key],
		set: parse ? (r, v) => parse(r, key, v) : undefined,
		readOnly: !parse,
		...extra,
	});
	// a column of one mode only: blank and locked on a channel of the other mode (its value is kept), and on a
	// channel of a kind that doesn't use the record's field
	const only = (mode, c) => {
		const format = c.format;
		const off = (r) => r.mode !== mode || !recordField(r, c.key);
		return { ...c, readOnly: off, format: (v, r) => (off(r) ? "" : format ? format(v, r) : c.type === "select" ? c.options(r).find(([k]) => k === v)?.[1] ?? "" : v) };
	};
	const own = (c) => ({ ...c, readOnly: (r) => !ownFreqs(r) });
	// a transmit field on an RX only channel: blank, grayed and locked like a field that doesn't apply (the value is
	// kept, and shows again when RX Only is cleared). A TX frequency in a receive-only band (an import) is what made
	// the channel RX only (followListen): that cell stays shown and open, so the frequency can be corrected
	const tx = (c) => {
		const ro = c.readOnly, format = c.format;
		const off = (r) => !!rxOnlyGet(r) && !(c.key === "txHz" && !listenBand(r.rxHz) && listenBand(r.txHz));
		return {
			...c, readOnly: (r) => off(r) || (typeof ro === "function" ? ro(r) : !!ro),
			format: (v, r) => (off(r) ? "" : format ? format(v, r) : c.type === "select" ? c.options(r).find(([k]) => k === v)?.[1] ?? "" : v),
		};
	};
	const num = { key: "number", label: "#", type: "number", get: (r) => r.number, readOnly: true, width: "3.5rem", format: (v) => v ?? "–" };
	switch (table) {
	case "channels":
		return [
			num,
			col("name", "editor.col.name", "text", name(LIMITS.name)(t)),
			{ ...col("mode", "editor.col.mode", "select", pick), ...modeColumn(m, t, features), readOnly: false, format: (v, r) => modeText(r, t) },
			own(col("rxHz", "editor.col.rx", "text", (r, k, v) => {
				// a simplex channel stays simplex: TX follows RX when they were the same
				const was = r.rxHz;
				const err = rxFreq(t)(r, k, v);
				if (!err && r.txHz === was) r.txHz = r.rxHz;
				return err;
			}, { format: mhz, numeric: true })),
			tx(own(col("txHz", "editor.col.tx", "text", txFreq(t), { format: mhz, numeric: true }))),
			tx(col("power", "editor.col.power", "select", pick, { options: () => powerOptions(t) })),
			only("fm", col("bw", "editor.col.bw", "select", pick, { options: () => [[12.5, "12.5"], [25, "25"]] })),
			only("fm", col("rxTone", "editor.col.rx_tone", "text", tone(t), { format: (v) => toneText(v, t), editValue: (v) => v, combo: toneCombo(t) })),
			tx(only("fm", col("txTone", "editor.col.tx_tone", "text", tone(t), { format: (v) => toneText(v, t), editValue: (v) => v, combo: toneCombo(t) }))),
			only("dmr", col("colorCode", "editor.col.cc", "select", pick, { options: () => [...Array(16).keys()].map((n) => [n, String(n)]) })),
			only("dmr", col("timeslot", "editor.col.ts", "select", pick, { options: () => [[1, "1"], [2, "2"]] })),
			only("dmr", col("contact", "editor.col.contact", "select", pick, { options: () => refOptions(m.contacts, t), format: (v) => refLabel(v, t), sortValue: (r) => refLabel(r.contact, t) })),
			only("dmr", col("rxList", "editor.col.rx_list", "select", pick, { options: () => refOptions(m.rxLists, t), format: (v) => refLabel(v, t), sortValue: (r) => refLabel(r.rxList, t) })),
			{ ...col("rxOnly", "editor.col.rx_only", "bool", bool), get: rxOnlyGet, set: (r, v) => rxOnlySet(r, v) },
			col("zoneSkip", "editor.col.zone_skip", "bool", bool),
		];
	case "zones":
		return [
			col("name", "editor.col.name", "text", name(LIMITS.name)(t)),
			{ key: "count", label: t("editor.col.channels"), type: "number", readOnly: true, get: (r) => r.channels.length, width: "7rem" },
			{ key: "list", label: t("editor.col.members"), type: "text", readOnly: true, get: (r) => r.channels.map((c) => refLabel(c, t)).join(", ") },
		];
	case "contacts":
		return [
			num,
			col("name", "editor.col.name", "text", name(LIMITS.name)(t)),
			col("id", "editor.col.id", "text", int(1, 0xFFFFFF)(t), { numeric: true, readOnly: false }),
			col("type", "editor.col.call_type", "select", pick, { options: () => [["group", t("editor.call.group")], ["private", t("editor.call.private")], ["all", t("editor.call.all")]] }),
			col("timeslot", "editor.col.ts_override", "select", pick, { options: () => [[0, t("editor.none")], [1, "1"], [2, "2"]] }),
		];
	case "rxLists":
		return [
			num,
			col("name", "editor.col.name", "text", name(LIMITS.name)(t)),
			{ key: "count", label: t("editor.col.contacts"), type: "number", readOnly: true, get: (r) => r.contacts.length, width: "7rem" },
			{ key: "list", label: t("editor.col.members"), type: "text", readOnly: true, get: (r) => r.contacts.map((c) => refLabel(c, t)).join(", ") },
		];
	case "dtmfContacts":
		return [
			num,
			col("name", "editor.col.name", "text", name(LIMITS.name)(t)),
			col("code", "editor.col.dtmf_code", "text", (r, k, v) => {
				v = String(v ?? "").trim().toUpperCase();
				if (!/^[0-9A-D*#]{1,16}$/.test(v)) return t("editor.err.bad_dtmf");
				r[k] = v;
			}),
		];
	case "aprs":
		return [
			num,
			col("name", "editor.col.name", "text", name(LIMITS.aprsName)(t)),
			col("ssid", "editor.col.ssid", "select", pick, { options: () => [...Array(16).keys()].map((n) => [n, String(n)]) }),
			col("path1", "editor.col.path1", "text", (r, k, v) => setPath(r, k, v, t)),
			col("path2", "editor.col.path2", "text", (r, k, v) => setPath(r, k, v, t)),
			col("comment", "editor.col.comment", "text", (r, k, v) => {
				v = String(v ?? "");
				if (v.length > LIMITS.aprsComment) return t("editor.err.too_long", { max: LIMITS.aprsComment });
				r[k] = v;
			}),
			col("txHz", "editor.col.aprs_freq", "text", (r, k, v) => {
				if (!String(v ?? "").trim()) {
					r[k] = 0;
					return;
				}
				return freq(t)(r, k, v);
			}, { format: (v) => (v ? mhz(v) : t("editor.aprs.radio_freq")), numeric: true }),
		];
	}
	return [];
}

function setPath(r, k, v, t) {
	v = String(v ?? "").trim().toUpperCase();
	if (v && !/^[A-Z0-9]{1,6}(-([0-9]|1[0-5]))?$/.test(v)) return t("editor.err.bad_path");
	r[k] = v;
}

// --- The full form of a record ---

// SFR byte A (open-firmware-cps.md 7.3.3): role bits 0-2, framing 3-4, inbound slot 5, hang time 6-7; byte B: bit 7
// outbound color code set, bits 0-3 the color code
const sfrField = (key, shift, mask, byte = "sfrA") => ({
	get: (r) => (r[byte] >> shift) & mask,
	set: (r, v) => {
		r[byte] = (r[byte] & ~(mask << shift)) | ((Number(v) & mask) << shift);
	},
	key,
});

// The channels a fallback may name (channel-modes.md 7.3): plain ones, but the record being edited (self, or the
// form's copy of it); the current one stays listed when it isn't one of them
export function fallbackOptions(m, t, current, self = null, none = t("editor.modes.fallback_system")) {
	const list = m.channels.filter((c) => !c.kind && c !== self && !(self?.number && c.number === self.number)).sort((a, b) => a.number - b.number);
	const out = [[null, none], ...list.map((c) => [c, `${c.number} · ${c.name}`])];
	if (current != null && !list.includes(current)) out.push([current, typeof current === "number" ? t("editor.missing", { n: current }) : `${current.number} · ${current.name}`]);
	return out;
}

// The systems of a type a channel may name; a number as read stays listed
export function systemOptions(m, t, type, current, optional = false) {
	const out = (m.systems ?? []).filter((s) => s.type === type).sort((a, b) => a.number - b.number).map((s) => [s, s.name]);
	if (optional || current == null) out.unshift([null, t("editor.none")]);
	if (typeof current === "number") out.push([current, t("editor.missing", { n: current })]);
	return out;
}

// features: the connected radio's (null: none), which says the kinds a channel may be made
export function channelForm(m, t, { features = null } = {}) {
	const sel = (key, label, options, extra = {}) => ({ key, label: t(label), type: "select", options, ...extra });
	const chk = (key, label) => ({ key, label: t(label), type: "bool" });
	const txt = (key, label, parse, format) => ({ key, label: t(label), type: "text", parse, format });
	// the FM and DMR parts show only for a channel of that mode; the other mode's values are kept as they are. A
	// channel of a kind shows the DMR record fields its module uses
	const fm = (f) => ({ ...f, show: (r) => r.mode === "fm" && !r.kind });
	const dmr = (f) => ({ ...f, show: (r) => r.mode === "dmr" && recordField(r, f.key) && (!f.show || f.show(r)) });
	const sfr = (f, label, options) => dmr({ ...sfrField(...f), label: t(label), type: "select", options, show: (r) => r.sfr && !r.kind });
	const kinds = offeredKinds(m, features);
	const mode = modeColumn(m, t, features);
	const kindFields = [];
	for (const k of kinds) {
		const mod = k.module, on = (r) => r.kind === k.id;
		const holder = { on, get: (r) => r.params, put: (r, key, v) => (r.params = { ...r.params, [key]: v }) };
		if (mod.systemType) kindFields.push({ ...sel("system", "editor.modes.system", (r) => systemOptions(m, t, mod.systemType, r.system, mod.optionalSystem)), key: `k${k.id}.system`, get: (r) => r.system, set: (r, v) => on(r) && (r.system = v), show: on });
		if (mod.txKeyOptions) kindFields.push(sel(`k${k.id}.txKeyId`, "editor.nxdn.tx_key", (r) => {
			const ids = mod.txKeyOptions(r);
			const current = r.params?.txKeyId;
			return [[null, t("editor.nxdn.tx_clear")], ...ids.map((id) => [id, String(id)]),
				...(current != null && !ids.includes(current) ? [[current, t("editor.missing", { n: current })]] : [])];
		}, {
			dynamicOptions: true, show: on,
			get: (r) => r.params?.txKeyId ?? null,
			set: (r, id) => {
				if (!on(r)) return;
				const params = { ...r.params };
				if (id == null) delete params.txKeyId;
				else params.txKeyId = id;
				r.params = params;
			},
			help: t("editor.nxdn.tx_key_help"),
		}));
		for (const flag of Object.keys(mod.flags ?? {})) kindFields.push({ ...chk(`k${k.id}.${flag}`, `editor.modes.flag.${flag}`), get: (r) => r[flag], set: (r, v) => on(r) && (r[flag] = !!v), show: on });
		if (mod.fallback) {
			kindFields.push({ ...sel(`k${k.id}.fallback`, "editor.modes.fallback", (r) => fallbackOptions(m, t, r.fallback, r)), get: (r) => r.fallback, set: (r, v) => on(r) && (r.fallback = v), show: (r) => on(r) && !r.noFallback });
		}
		for (const spec of mod.fields()) kindFields.push(specField(spec, t, holder, `k${k.id}`));
	}
	return [
		{ group: t("editor.form.basic") },
		txt("name", "editor.col.name", name(LIMITS.name)(t)),
		sel("mode", "editor.col.mode", mode.options, { get: mode.get, set: mode.set }),
		{ type: "note", key: "kindNote", text: (value, w) => (w?.kind && !ownFreqs(w) ? t("editor.modes.freqs_system") : "") },
		{ ...sel("power", "editor.col.power", () => powerOptions(t)), disabled: noTx },
		// in a receive-only band TX follows RX, locked (rxFreq); RX Only locks TX where it is
		{ type: "pair", key: "freqs", show: ownFreqs, disabled: noTx,
			fields: [txt("rxHz", "editor.col.rx", rxFreq(t), mhz), { ...txt("txHz", "editor.col.tx", txFreq(t), mhz), disabled: noTx, follows: "rxHz", followsIf: listening }], buttons: [
			["→", t("editor.freq.rx_to_tx"), ([rx]) => [rx, rx]],
			["⇄", t("editor.freq.swap"), ([rx, tx]) => [tx, rx]],
			["←", t("editor.freq.tx_to_rx"), ([, tx]) => [tx, tx]],
		] },
		{ type: "note", key: "band", text: (value) => {
			const band = listenBand(parseFreq(value("rxHz")));
			return band ? t(`editor.band.${band}`) : "";
		} },
		sel("tot", "editor.col.tot", () => totOptions(t), { disabled: noTx }),
		// a receive-only band makes the channel RX only on save (rxFreq): the box shows it as the RX frequency is typed
		{ ...chk("rxOnly", "editor.col.rx_only"), implied: listening, get: rxOnlyGet, set: rxOnlySet },
		{ group: t("editor.form.mode_params"), show: (r) => !!r.kind },
		...kindFields,
		fm({ group: t("editor.form.analog") }),
		fm(sel("bw", "editor.col.bw", () => [[12.5, "12.5 kHz"], [25, "25 kHz"]])),
		fm({ ...txt("rxTone", "editor.col.rx_tone", tone(t), (v) => v), combo: toneCombo(t) }),
		fm({ ...txt("txTone", "editor.col.tx_tone", tone(t), (v) => v), combo: toneCombo(t), disabled: noTx }),
		fm(sel("squelch", "editor.col.squelch", () => [[0, t("editor.squelch.default")], ...[...Array(21).keys()].map((n) => [n + 1, String(n + 1)])])),
		fm({ ...chk("vox", "editor.col.vox"), disabled: noTx }),
		fm({ ...chk("toneBurst", "editor.col.tone_burst"), disabled: noTx }),
		dmr({ group: t("editor.form.digital") }),
		dmr(sel("colorCode", "editor.col.cc", () => [...Array(16).keys()].map((n) => [n, String(n)]))),
		dmr(sel("timeslot", "editor.col.ts", () => [[1, "1"], [2, "2"]])),
		dmr(sel("contact", "editor.col.contact", () => refOptions(m.contacts, t))),
		dmr(sel("rxList", "editor.col.rx_list", () => refOptions(m.rxLists, t))),
		dmr(txt("dmrId", "editor.col.channel_id", dmrId(t), (v) => (v ? String(v) : ""))),
		dmr(chk("dmo", "editor.col.dmo")),
		dmr(chk("acceptBadCrc", "editor.col.accept_bad_crc")),
		dmr(sel("aliasTs1", "editor.col.alias_ts1", () => aliasOptions(t))),
		dmr(sel("aliasTs2", "editor.col.alias_ts2", () => aliasOptions(t))),
		dmr(chk("noBeep", "editor.col.no_beep")),
		{ group: t("editor.form.scan") },
		chk("zoneSkip", "editor.col.zone_skip"),
		chk("allSkip", "editor.col.all_skip"),
		chk("noEco", "editor.col.no_eco"),
		// roaming hunts DMR repeaters by their own frequencies: a channel of a kind follows its system instead
		{ ...chk("roam", "editor.col.roam"), show: (r) => !r.kind },
		{ group: t("editor.form.location") },
		sel("aprs", "editor.col.aprs", () => refOptions(m.aprs, t)),
		chk("useLocation", "editor.col.use_location"),
		{ ...txt("lat", "editor.col.lat", coord(t, 90), (v) => v.toFixed(4)), show: (r) => r.useLocation },
		{ ...txt("lon", "editor.col.lon", coord(t, 180), (v) => v.toFixed(4)), show: (r) => r.useLocation },
		dmr({ group: t("editor.form.sfr") }),
		dmr(chk("sfr", "editor.col.sfr")),
		sfr(["role", 0, 7], "editor.sfr.role", () => [0, 1, 2, 3, 4].map((n) => [n, t(`editor.sfr.role${n}`)])),
		sfr(["framing", 3, 3], "editor.sfr.framing", () => [0, 1, 2, 3].map((n) => [n, t(`editor.sfr.framing${n}`)])),
		sfr(["inbound", 5, 1], "editor.sfr.inbound", () => [[0, "TS1"], [1, "TS2"]]),
		sfr(["hang", 6, 3], "editor.sfr.hang", () => [[0, "1 s"], [1, "2 s"], [2, "4 s"], [3, t("editor.none")]]),
		sfr(["ccSet", 7, 1, "sfrB"], "editor.sfr.cc_set", () => [[0, t("editor.sfr.cc_channel")], [1, t("editor.sfr.cc_own")]]),
		sfr(["cc", 0, 15, "sfrB"], "editor.sfr.cc", () => [...Array(16).keys()].map((n) => [n, String(n)])),
	];
}

const aliasOptions = (t) => [[0, t("editor.alias.off")], [1, "APRS"], [2, t("editor.alias.text")], [3, t("editor.alias.both")]];

const coord = (t, max) => (r, k, v) => {
	const n = Number(String(v).trim());
	if (!Number.isFinite(n) || Math.abs(n) > max) return t("editor.err.range", { min: -max, max });
	r[k] = n;
};

// A form in a dialog over a working copy of the record; Save hands the copy back
// opts.list: one field per line, for forms whose fields carry help text
export function formDialog(title, fields, record, t, onSave, opts = {}) {
	const work = { ...record };
	const errs = h("p", { class: "notice bad", hidden: true });
	const rows = [];
	const body = h("div", { class: opts.list ? "form-grid form-list" : "form-grid" });
	// One field: its label and input, kept in rows for sync() and save()
	const field = (f) => {
		const id = "f-" + f.key;
		const get = f.get || ((r) => r[f.key]);
		let input;
		if (f.type === "bool") {
			input = h("input", { type: "checkbox", id });
			input.checked = !!get(work);
		} else if (f.type === "select") {
			const opts = f.options(work);
			input = h("select", { id }, ...opts.map(([, label], i) => h("option", { value: String(i) }, label)));
			input.selectedIndex = Math.max(0, opts.findIndex(([v]) => v === get(work)));
			input._opts = opts;
		} else {
			const v = get(work);
			input = h("input", { type: "text", id, value: f.format ? f.format(v ?? "") : v ?? "", spellcheck: "false" });
		}
		const label = h("label", { for: id }, typeof f.label === "function" ? f.label(work) : f.label);
		const row = h("div", { class: "form-row" + (f.type === "bool" ? " form-check" : "") }, label, input, f.help ? h("span", { class: "small muted" }, f.help) : null);
		// a long list of usual values (the tones): a searchable list over the input, which keeps the value
		if (f.combo && input.tagName === "INPUT") combo(input, f.combo(work));
		rows.push({ f, input, row, label });
		return row;
	};
	// a group with collapsed set is a section the user opens: its rows stay hidden until then
	let section = null;
	for (const f of fields) {
		const n0 = rows.length;
		if (f.group) {
			let row;
			if (f.collapsed != null) {
				const sec = { open: !f.collapsed };
				const btn = h("button", { type: "button", class: "form-toggle", "aria-expanded": String(sec.open) }, f.group);
				btn.addEventListener("click", () => {
					sec.open = !sec.open;
					btn.setAttribute("aria-expanded", String(sec.open));
					sync();
				});
				row = h("h3", { class: "form-group" }, btn);
				section = sec;
			} else {
				row = h("h3", { class: "form-group" }, f.group);
				section = null;
			}
			rows.push({ f: { key: null, type: "group", show: f.show }, input: null, row });
			body.append(row);
			continue;
		} else if (f.type === "custom") {
			// a part the caller draws (a table of rows): f.render() gives its element, f.parse(work) its errors on save
			const row = f.render();
			rows.push({ f, input: null, row });
			body.append(row);
		} else if (f.type === "note") {
			// a line of text from the inputs as they are typed; f.notice (a .notice kind: "bad") draws it as a callout
			const row = h("p", { class: f.notice ? `form-note notice ${f.notice}` : "form-note small muted", hidden: true });
			rows.push({ f, input: null, row });
			body.append(row);
		} else if (f.type === "pair") {
			// two fields side by side with small buttons between them that act on both inputs
			const a = field(f.fields[0]), b = field(f.fields[1]);
			const [ia, ib] = rows.slice(-2).map((r) => r.input);
			const tools = f.buttons.map(([icon, name, fn]) => {
				const btn = h("button", { type: "button", class: "btn pair-btn", title: name, "aria-label": name, onclick: () => {
					const v = fn([ia.value, ib.value]);
					[ia.value, ib.value] = v;
					sync();
				} }, icon);
				return btn;
			});
			const mid = h("div", { class: "pair-tools" }, ...tools);
			const pair = h("div", { class: "form-pair" }, a, mid, b);
			rows.push({ f: { key: f.key, type: "pair", disabled: f.disabled, show: f.show }, input: null, row: pair, tools });
			body.append(pair);
		} else body.append(field(f));
		for (const r of rows.slice(n0)) r.section = section;
	}
	const byKey = (k) => rows.find((r) => r.f.key === k);
	// a row inside a closed section is hidden whatever its own show says
	const shut = (r) => r.section && !r.section.open;
	const value = (k) => byKey(k)?.input?.value;
	const checked = (k) => !!byKey(k)?.input?.checked;
	const sync = () => {
		for (const r of rows) if (r.section) r.row.hidden = false;
		for (const { f, input, row, label, tools } of rows) {
			if (f.type === "pair") {
				for (const b of tools) b.disabled = !!f.disabled?.(work, value, checked);
				if (f.show && row) row.hidden = !f.show(work, value);
				continue;
			}
			if (f.type === "group" || f.type === "custom") {
				// a custom part may redraw itself from the inputs as they are typed
				if (f.type === "custom") f.sync?.(work, value);
				if (f.show) row.hidden = !f.show(work, value);
				continue;
			}
			if (f.type === "note") {
				row.textContent = f.text(value, work);
				row.hidden = !row.textContent;
				continue;
			}
			if (f.type === "select") {
				if (f.dynamicOptions) {
					const current = input._opts[input.selectedIndex]?.[0];
					const options = f.options(work, value);
					if (options.length !== input._opts.length || options.some(([v, l], i) => v !== input._opts[i][0] || l !== input._opts[i][1])) {
						input.replaceChildren(...options.map(([, l], i) => h("option", { value: String(i) }, l)));
						input.selectedIndex = Math.max(0, options.findIndex(([v]) => v === current));
						input._opts = options;
					}
				}
				const v = input._opts[input.selectedIndex]?.[0];
				f.set ? f.set(work, v) : (work[f.key] = v);
			}
			if (f.implied) {
				const on = f.implied(value);
				if (on) input.checked = true;
				input.disabled = on;
			}
			if (f.type === "bool") (f.set ? f.set(work, input.checked) : (work[f.key] = input.checked));
			if (f.show) row.hidden = !f.show(work, value);
			if (typeof f.label === "function") label.textContent = f.label(work);
			if (f.disabled) {
				input.disabled = f.disabled(work, value, checked);
				const ts = input.tomselect;
				if (ts && ts.isDisabled !== input.disabled) input.disabled ? ts.disable() : ts.enable();
				if (input.disabled && f.follows && (!f.followsIf || f.followsIf(value))) input.value = byKey(f.follows).input.value;
			}
		}
		for (const r of rows) if (shut(r)) r.row.hidden = true;
	};
	body.addEventListener("change", sync);
	body.addEventListener("input", sync);
	sync();
	const dlg = h("dialog", { class: "confirm editor-dialog" });
	const save = () => {
		const bad = [];
		for (const { f, input } of rows) {
			if (f.type === "custom") {
				if (!f.show || f.show(work, value)) bad.push(...(f.parse(work) ?? []));
				continue;
			}
			if (f.type === "bool" || f.type === "pair" || f.type === "group" || f.type === "note") continue;
			if (f.type === "select") {
				const v = input._opts[input.selectedIndex]?.[0];
				f.set ? f.set(work, v) : (work[f.key] = v);
				continue;
			}
			const text = f.combo ? comboText(input) : input.value;
			const err = f.parse ? f.parse(work, f.key, text) : ((work[f.key] = text), null);
			if (err) bad.push(`${typeof f.label === "function" ? f.label(work) : f.label}: ${err}`);
		}
		if (bad.length) {
			errs.hidden = false;
			errs.textContent = bad.join(" · ");
			return;
		}
		onSave(work);
		dlg.close();
	};
	dlg.append(h("h2", {}, title), body, errs, h("div", { class: "confirm-actions" },
		button(t("editor.cancel"), () => dlg.close()), button(t("editor.save_record"), save, { primary: true })));
	dlg.addEventListener("close", () => dlg.remove());
	document.body.append(dlg);
	dlg.showModal();
}

// --- Members of a zone or an RX group list ---

export function membersDialog(title, members, pool, max, t, label, onSave) {
	let list = members.slice();
	const filter = h("input", { type: "search", placeholder: t("editor.filter"), "aria-label": t("editor.filter") });
	const avail = h("select", { multiple: true, size: 14, class: "members-list", "aria-label": t("editor.members.available") });
	const chosen = h("select", { multiple: true, size: 14, class: "members-list", "aria-label": t("editor.members.chosen") });
	const count = h("p", { class: "small muted" });
	const fill = () => {
		const f = filter.value.toLowerCase();
		const inList = new Set(list);
		avail.replaceChildren(...pool.filter((x) => !inList.has(x) && label(x).toLowerCase().includes(f))
			.map((x) => Object.assign(h("option", {}, label(x)), { _v: x })));
		chosen.replaceChildren(...list.map((x) => Object.assign(h("option", {}, typeof x === "number" ? t("editor.missing", { n: x }) : label(x)), { _v: x })));
		count.textContent = t("editor.members.count", { n: list.length, max });
	};
	const picked = (sel) => [...sel.selectedOptions].map((o) => o._v);
	const add = () => {
		list = [...list, ...picked(avail)].slice(0, max);
		fill();
	};
	const del = () => {
		const p = new Set(picked(chosen));
		list = list.filter((x) => !p.has(x));
		fill();
	};
	const move = (d) => {
		const p = new Set(picked(chosen));
		const idx = list.map((x, i) => (p.has(x) ? i : -1)).filter((i) => i >= 0);
		if (!idx.length || (d < 0 && idx[0] === 0) || (d > 0 && idx[idx.length - 1] === list.length - 1)) return;
		for (const i of d < 0 ? idx : idx.reverse()) [list[i], list[i + d]] = [list[i + d], list[i]];
		fill();
		for (const o of chosen.options) o.selected = p.has(o._v);
	};
	filter.addEventListener("input", fill);
	avail.addEventListener("dblclick", add);
	chosen.addEventListener("dblclick", del);
	fill();
	const dlg = h("dialog", { class: "confirm editor-dialog" });
	dlg.append(h("h2", {}, title),
		h("div", { class: "members" },
			h("div", {}, h("h3", {}, t("editor.members.available")), filter, avail),
			h("div", { class: "members-actions" }, button(t("editor.members.add"), add), button(t("editor.members.remove"), del),
				button(t("editor.move_up"), () => move(-1)), button(t("editor.move_down"), () => move(1))),
			h("div", {}, h("h3", {}, t("editor.members.chosen")), chosen, count)),
		h("div", { class: "confirm-actions" }, button(t("editor.cancel"), () => dlg.close()),
			button(t("editor.save_record"), () => {
				onSave(list);
				dlg.close();
			}, { primary: true })));
	dlg.addEventListener("close", () => dlg.remove());
	document.body.append(dlg);
	dlg.showModal();
}

export { refNumber, refLabel };
