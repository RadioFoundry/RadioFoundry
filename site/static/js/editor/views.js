// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// What the codeplug editor shows for each kind of record: table columns, the full form of a record, and the
// membership editor for zones and RX group lists. Setters parse what was typed and return an error text, or
// nothing when the value was taken.

import { h, button } from "../flasher/dom.js";
import { parseFreq, parseTone, mhz } from "../codeplug/fields.js";
import { LIMITS, POWER_NAMES } from "../codeplug/records.js";
import { refNumber, listenBand } from "../codeplug/model.js";

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

// TX timeout in seconds: 0 (blank, or the default label) is the radio's own, the rest rounds up to the 15 s the
// record stores, so the form shows what will be saved
const tot = (t) => (r, k, v) => {
	const s = String(v ?? "").trim();
	if (!s || /^(0|default)$/i.test(s) || s.toLowerCase() === t("editor.tot.default").toLowerCase()) {
		r[k] = 0;
		return;
	}
	const n = Number(s);
	if (!Number.isInteger(n) || n < 1 || n > 3825) return t("editor.err.tot");
	r[k] = Math.ceil(n / 15) * 15;
};

const freq = (t) => (r, k, v) => {
	const hz = parseFreq(v);
	if (hz === null) return t("editor.err.bad_freq");
	r[k] = hz;
};

// An RX frequency in a receive-only band (model.js listenBand) makes the channel RX only
const rxFreq = (t) => (r, k, v) => {
	const err = freq(t)(r, k, v);
	if (!err && listenBand(r.rxHz)) r.rxOnly = true;
	return err;
};

// The mode as the radio shows it: an FM channel in the air band is received in AM
const modeText = (r, t) => (r.mode === "dmr" ? "DMR" : listenBand(r.rxHz) ? t(`editor.mode.${listenBand(r.rxHz)}`) : "FM");

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

const pick = (r, k, v) => {
	r[k] = v;
};

const bool = (r, k, v) => {
	r[k] = !!v;
};

export const toneText = (v, t) => v || t("editor.none");

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

export function columns(table, m, t) {
	const col = (key, label, type, parse, extra = {}) => ({
		key, label: t(label), type,
		get: (r) => r[key],
		set: parse ? (r, v) => parse(r, key, v) : undefined,
		readOnly: !parse,
		...extra,
	});
	// a column of one mode only: blank and locked on a channel of the other mode (its value is kept)
	const only = (mode, c) => {
		const format = c.format;
		return { ...c, readOnly: (r) => r.mode !== mode, format: (v, r) => (r.mode !== mode ? "" : format ? format(v, r) : c.type === "select" ? c.options(r).find(([k]) => k === v)?.[1] ?? "" : v) };
	};
	const num = { key: "number", label: "#", type: "number", get: (r) => r.number, readOnly: true, width: "3.5rem", format: (v) => v ?? "–" };
	switch (table) {
	case "channels":
		return [
			num,
			col("name", "editor.col.name", "text", name(LIMITS.name)(t)),
			col("mode", "editor.col.mode", "select", pick, { options: () => [["fm", "FM"], ["dmr", "DMR"]], format: (v, r) => modeText(r, t) }),
			col("rxHz", "editor.col.rx", "text", (r, k, v) => {
				// a simplex channel stays simplex: TX follows RX when they were the same
				const was = r.rxHz;
				const err = rxFreq(t)(r, k, v);
				if (!err && r.txHz === was) r.txHz = r.rxHz;
				return err;
			}, { format: mhz, numeric: true }),
			col("txHz", "editor.col.tx", "text", freq(t), { format: mhz, numeric: true }),
			col("power", "editor.col.power", "select", pick, { options: () => powerOptions(t) }),
			only("fm", col("bw", "editor.col.bw", "select", pick, { options: () => [[12.5, "12.5"], [25, "25"]] })),
			only("fm", col("rxTone", "editor.col.rx_tone", "text", tone(t), { format: (v) => toneText(v, t), editValue: (v) => v })),
			only("fm", col("txTone", "editor.col.tx_tone", "text", tone(t), { format: (v) => toneText(v, t), editValue: (v) => v })),
			only("dmr", col("colorCode", "editor.col.cc", "select", pick, { options: () => [...Array(16).keys()].map((n) => [n, String(n)]) })),
			only("dmr", col("timeslot", "editor.col.ts", "select", pick, { options: () => [[1, "1"], [2, "2"]] })),
			only("dmr", col("contact", "editor.col.contact", "select", pick, { options: () => refOptions(m.contacts, t), format: (v) => refLabel(v, t), sortValue: (r) => refLabel(r.contact, t) })),
			only("dmr", col("rxList", "editor.col.rx_list", "select", pick, { options: () => refOptions(m.rxLists, t), format: (v) => refLabel(v, t), sortValue: (r) => refLabel(r.rxList, t) })),
			col("rxOnly", "editor.col.rx_only", "bool", bool),
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
			}, { format: (v) => (v ? mhz(v) : t("editor.aprs.channel_freq")), numeric: true }),
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

export function channelForm(m, t) {
	const sel = (key, label, options, extra = {}) => ({ key, label: t(label), type: "select", options, ...extra });
	const chk = (key, label) => ({ key, label: t(label), type: "bool" });
	const txt = (key, label, parse, format) => ({ key, label: t(label), type: "text", parse, format });
	// the FM and DMR parts show only for a channel of that mode; the other mode's values are kept as they are
	const fm = (f) => ({ ...f, show: (r) => r.mode === "fm" });
	const dmr = (f) => ({ ...f, show: (r) => r.mode === "dmr" && (!f.show || f.show(r)) });
	const sfr = (f, label, options) => dmr({ ...sfrField(...f), label: t(label), type: "select", options, show: (r) => r.sfr });
	return [
		{ group: t("editor.form.basic") },
		txt("name", "editor.col.name", name(LIMITS.name)(t)),
		sel("mode", "editor.col.mode", () => [["fm", "FM"], ["dmr", "DMR"]]),
		sel("power", "editor.col.power", () => powerOptions(t)),
		{ type: "pair", key: "freqs", fields: [txt("rxHz", "editor.col.rx", rxFreq(t), mhz), txt("txHz", "editor.col.tx", freq(t), mhz)], buttons: [
			["→", t("editor.freq.rx_to_tx"), ([rx]) => [rx, rx]],
			["⇄", t("editor.freq.swap"), ([rx, tx]) => [tx, rx]],
			["←", t("editor.freq.tx_to_rx"), ([, tx]) => [tx, tx]],
		] },
		{ type: "note", key: "band", text: (value) => {
			const band = listenBand(parseFreq(value("rxHz")));
			return band ? t(`editor.band.${band}`) : "";
		} },
		txt("tot", "editor.col.tot", tot(t), (v) => (v ? String(v) : t("editor.tot.default"))),
		chk("rxOnly", "editor.col.rx_only"),
		fm({ group: t("editor.form.analog") }),
		fm(sel("bw", "editor.col.bw", () => [[12.5, "12.5 kHz"], [25, "25 kHz"]])),
		fm(txt("rxTone", "editor.col.rx_tone", tone(t), (v) => v)),
		fm(txt("txTone", "editor.col.tx_tone", tone(t), (v) => v)),
		fm(sel("squelch", "editor.col.squelch", () => [[0, t("editor.squelch.default")], ...[...Array(21).keys()].map((n) => [n + 1, String(n + 1)])])),
		fm(chk("vox", "editor.col.vox")),
		dmr({ group: t("editor.form.digital") }),
		dmr(sel("colorCode", "editor.col.cc", () => [...Array(16).keys()].map((n) => [n, String(n)]))),
		dmr(sel("timeslot", "editor.col.ts", () => [[1, "1"], [2, "2"]])),
		dmr(sel("contact", "editor.col.contact", () => refOptions(m.contacts, t))),
		dmr(sel("rxList", "editor.col.rx_list", () => refOptions(m.rxLists, t))),
		dmr(txt("dmrId", "editor.col.channel_id", dmrId(t), (v) => (v ? String(v) : ""))),
		dmr(chk("dmo", "editor.col.dmo")),
		dmr(sel("aliasTs1", "editor.col.alias_ts1", () => aliasOptions(t))),
		dmr(sel("aliasTs2", "editor.col.alias_ts2", () => aliasOptions(t))),
		dmr(chk("noBeep", "editor.col.no_beep")),
		{ group: t("editor.form.scan") },
		chk("zoneSkip", "editor.col.zone_skip"),
		chk("allSkip", "editor.col.all_skip"),
		chk("noEco", "editor.col.no_eco"),
		chk("roam", "editor.col.roam"),
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
export function formDialog(title, fields, record, t, onSave) {
	const work = { ...record };
	const errs = h("p", { class: "notice bad", hidden: true });
	const rows = [];
	const body = h("div", { class: "form-grid" });
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
		const row = h("div", { class: "form-row" + (f.type === "bool" ? " form-check" : "") }, label, input);
		rows.push({ f, input, row, label });
		return row;
	};
	for (const f of fields) {
		if (f.group) {
			const row = h("h3", { class: "form-group" }, f.group);
			rows.push({ f: { key: null, type: "group", show: f.show }, input: null, row });
			body.append(row);
		} else if (f.type === "note") {
			// a line of text from the inputs as they are typed
			const row = h("p", { class: "form-note small muted", hidden: true });
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
			rows.push({ f: { key: f.key, type: "pair", disabled: f.disabled }, input: null, row: null, tools });
			body.append(h("div", { class: "form-pair" }, a, mid, b));
		} else body.append(field(f));
	}
	const byKey = (k) => rows.find((r) => r.f.key === k);
	const sync = () => {
		for (const { f, input, row, label, tools } of rows) {
			if (f.type === "pair") {
				for (const b of tools) b.disabled = !!f.disabled?.(work);
				continue;
			}
			if (f.type === "group") {
				if (f.show) row.hidden = !f.show(work);
				continue;
			}
			if (f.type === "note") {
				row.textContent = f.text((k) => byKey(k).input.value);
				row.hidden = !row.textContent;
				continue;
			}
			if (f.type === "select") {
				const v = input._opts[input.selectedIndex]?.[0];
				f.set ? f.set(work, v) : (work[f.key] = v);
			}
			if (f.type === "bool") (f.set ? f.set(work, input.checked) : (work[f.key] = input.checked));
			if (f.show) row.hidden = !f.show(work);
			if (typeof f.label === "function") label.textContent = f.label(work);
			if (f.disabled) {
				input.disabled = f.disabled(work);
				if (input.disabled && f.follows) input.value = byKey(f.follows).input.value;
			}
		}
	};
	body.addEventListener("change", sync);
	body.addEventListener("input", sync);
	sync();
	const dlg = h("dialog", { class: "confirm editor-dialog" });
	const save = () => {
		const bad = [];
		for (const { f, input } of rows) {
			if (f.type === "bool" || f.type === "pair" || f.type === "group" || f.type === "note") continue;
			if (f.type === "select") {
				const v = input._opts[input.selectedIndex]?.[0];
				f.set ? f.set(work, v) : (work[f.key] = v);
				continue;
			}
			const err = f.parse ? f.parse(work, f.key, input.value) : ((work[f.key] = input.value), null);
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
