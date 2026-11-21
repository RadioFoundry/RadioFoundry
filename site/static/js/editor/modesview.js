// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The Codeplug Editor's Systems tab and the modes' part of the Radio tab (docs/spec/channel-modes.md): systems of
// every type in one list, each edited in a dialog of its generic header and its type's fields, its entries (sites,
// channels, plan rows) in a table per class, and its secret behind a button. What a type holds comes from its mode
// module (codeplug/mode_*.js); no mode is named here.

import { formDialog, fallbackOptions } from "./views.js";
import { specField, parseSpec, formatSpec, label } from "./modefields.js";
import { SYSTEM_TYPES, typeRow, typeName, offeredTypes, offeredKinds } from "../codeplug/modes.js";
import { newSystem, newHamSystem, syncModes, SHARE, MODE_LIMITS } from "../codeplug/channelmodes.js";
import { remove, assignNumbers } from "../codeplug/model.js";

const SHARE_LEVELS = [SHARE.off, SHARE.identity, SHARE.whole];
const shareLabel = (mod, level) => mod?.conventional && level === SHARE.whole
	? "editor.nxdn.share_whole" : `editor.modes.share${level}`;

// --- The tab ---

export function systemsTab(api, features) {
	const { model: m, t, h, button } = api;
	const systems = [...(m.systems ?? [])].sort((a, b) => a.number - b.number);
	const types = offeredTypes(m, features);
	const pick = h("select", { "aria-label": t("editor.modes.type") }, ...types.map((x) => h("option", { value: String(x.id) }, t(x.module.label ?? `editor.mode_name.${x.token}`))));
	const full = systems.length >= MODE_LIMITS.systems;
	// the type's recommended ham setup, when it has one (docs/spec/ham-trunking.md)
	const picked = () => Number(pick.value || types[0]?.id);
	const hamOf = () => typeRow(picked())?.module.ham ?? null;
	const ham = types.length ? button(t("editor.modes.add_ham"), () => hamDialog(api, picked()), { disabled: full || !hamOf() }) : null;
	pick.addEventListener?.("change", () => (ham.disabled = full || !hamOf()));
	const parts = [
		h("p", { class: "small muted" }, t("editor.modes.intro")),
		types.length ? h("div", { class: "ed-bar" }, h("div", { class: "ed-actions" }, pick,
			button(t("editor.modes.add"), () => {
				const s = newSystem(m, Number(pick.value), newName(m, t));
				systemDialog(api, s, true);
			}, { disabled: full }), ham)) : null,
	];
	if (!systems.length) parts.push(h("p", { class: "grid-empty muted" }, t("editor.empty.systems")));
	else {
		const head = ["#", t("editor.col.name"), t("editor.modes.type"), t("editor.modes.identity"), t("editor.modes.transmit"),
			t("editor.modes.sharing"), t("editor.modes.class.control"), t("editor.modes.secret"), t("editor.tab.channels"), ""];
		const rows = systems.map((s) => {
			const row = typeRow(s.type), mod = row?.module;
			const used = m.channels.filter((c) => c.system === s).length;
			const actions = mod ? [
				button(t("editor.modes.edit"), () => systemDialog(api, s, false), { quiet: true }),
			] : [];
			return h("tr", {},
				h("td", {}, String(s.number)), h("td", {}, s.name), h("td", {}, row ? t(mod.label ?? `editor.mode_name.${row.token}`) : typeName(s.type)),
				h("td", {}, mod ? mod.identity(s.fields) : t("editor.modes.type_unknown")),
				h("td", {}, t(`editor.modes.tx${!s.tx ? 0 : mod?.noRegField && s.fields?.[mod.noRegField] ? 2 : 1}_short`)),
				h("td", {}, t(shareLabel(mod, s.sharing <= 2 ? s.sharing : 0))),
				h("td", {}, mod?.conventional ? "" : String(s.entries.filter((e) => e.control).length)),
				h("td", {}, mod?.conventional ? "" : t(s.secret ? "editor.modes.secret_set" : "editor.modes.secret_none")),
				h("td", {}, String(used)),
				h("td", {}, h("div", { class: "ed-actions" }, ...actions,
					button(t("editor.delete"), () => deleteSystem(api, s), { quiet: true, danger: true }))));
		});
		parts.push(h("div", { class: "table-wrap" }, h("table", { class: "data" },
			h("thead", {}, h("tr", {}, ...head.map((x) => h("th", {}, x)))), h("tbody", {}, ...rows))));
	}
	return h("div", {}, ...parts);
}

// Whether the tab is shown: a system type offered, or systems in the codeplug
export const systemsShown = (m, features) => offeredTypes(m, features).length > 0 || (m.systems ?? []).length > 0;

function newName(m, t) {
	const names = new Set((m.systems ?? []).map((s) => s.name));
	let i = (m.systems ?? []).length + 1;
	while (names.has(`${t("editor.modes.new_name")} ${i}`)) i++;
	return `${t("editor.modes.new_name")} ${i}`;
}

function deleteSystem(api, s) {
	const { t, model: m } = api;
	const used = m.channels.filter((c) => c.system === s).length;
	api.confirmDialog(t("editor.modes.delete_confirm", { name: s.name, n: used }), t("editor.delete"),
		() => api.change(t("editor.undo.delete"), () => remove(m, "systems", [s])));
}

// The recommended ham system of a type: its name, its control channel and what else the setup asks, then the system
// form, filled in, to check and save
function hamDialog(api, type) {
	const { model: m, t } = api;
	const row = typeRow(type), ham = row.module.ham;
	const freq = (key, zero) => ({ key, type: "freq", zero });
	const fields = [
		{ type: "note", key: "ham.intro", text: () => t("editor.modes.ham_intro") },
		{ key: "name", label: t("editor.col.name"), type: "text", parse: (r, k, v) => {
			v = String(v ?? "").trim();
			if (!v) return t("editor.err.no_name");
			if (v.length > MODE_LIMITS.name) return t("editor.err.too_long", { max: MODE_LIMITS.name });
			r.name = v;
		} },
		specField({ ...freq("rxHz", false), label: "editor.modes.ham_rx", help: "editor.modes.ham_rx_help" }, t, HOLD, "ham"),
		specField({ ...freq("txHz", true), label: "editor.modes.ham_tx", help: "editor.modes.ham_tx_help" }, t, HOLD, "ham"),
		...ham.asks.map((x) => specField(x, t, HOLD, "ham")),
	];
	const start = { name: ham.name, rxHz: 0, txHz: 0, ...Object.fromEntries(ham.asks.map((x) => [x.key, x.min ?? 0])) };
	formDialog(t("editor.modes.ham_title", { type: t(row.module.label ?? `editor.mode_name.${row.token}`) }), fields, start, t,
		// the system form opens once this one has closed
		(work) => setTimeout(() => systemDialog(api, newHamSystem(m, type, work), true)));
}
// The recommended setup's answers sit on the form's record itself
const HOLD = { get: (r) => r, put: (r, k, v) => (r[k] = v) };

// --- A system's header and its type's fields ---

export function systemOwnIdField(mod, t) {
	const spec = { type: "int", min: mod.ownIdMax == null ? 0 : 1, max: mod.ownIdMax ?? 0xFFFFFFFF };
	return {
		key: "ownId", label: t("editor.modes.own_id"), type: "text",
		format: (v) => (v ? String(v) : ""),
		parse: (r, k, v) => {
			const res = parseSpec(spec, v, t);
			if (res.error) return res.error;
			r.ownId = res.value;
		},
		help: t("editor.modes.own_id_help"), disabled: (r) => !r.tx,
	};
}

function systemDialog(api, s, isNew) {
	const { model: m, t } = api;
	const row = typeRow(s.type), mod = row.module;
	const holder = { get: (r) => r.fields, put: (r, key, v) => (r.fields = { ...r.fields, [key]: v }) };
	const specs = mod.fields();
	// The editor's own fields, by section; the type's fields follow them in each
	const own = {
		identity: [
			{ key: "name", label: t("editor.col.name"), type: "text", help: t("editor.modes.name_help"), parse: (r, k, v) => {
				v = String(v ?? "").trim();
				if (!v) return t("editor.err.no_name");
				if (v.length > MODE_LIMITS.name) return t("editor.err.too_long", { max: MODE_LIMITS.name });
				r.name = v;
			} },
		],
		radio: [
			// listen only is the user's per-system choice (7.1): never forced by sharing
			{ key: "transmit", label: t("editor.modes.transmit"), type: "select",
				options: () => [[0, t("editor.modes.tx0")], [1, t(mod.conventional ? "editor.modes.tx1_short" : "editor.modes.tx1")], ...(mod.noRegField ? [[2, t("editor.modes.tx2")]] : [])],
				get: (r) => (!r.tx ? 0 : mod.noRegField && r.fields[mod.noRegField] ? 2 : 1),
				set: (r, v) => {
					r.tx = v > 0;
					if (mod.noRegField) r.fields = { ...r.fields, [mod.noRegField]: v === 2 };
				}, help: mod.conventional ? null : t(mod.noRegField ? "editor.modes.transmit_help_noreg" : "editor.modes.transmit_help") },
			systemOwnIdField(mod, t),
			mod.secret ? { key: "needsKey", label: t("editor.modes.needs_key"), type: "bool", help: t("editor.modes.needs_key_help") } : null,
		],
		fallback: mod.conventional ? [] : [
			{ key: "fallback", label: t("editor.modes.fallback"), type: "select", options: (r) => fallbackOptions(m, t, r.fallback, null, t("editor.none")),
				help: t(mod.fallbackHelp ?? "editor.modes.fallback_help") },
		],
		sharing: [
			{ key: "sharing", label: t("editor.modes.share_what"), type: "select", options: () => SHARE_LEVELS.map((n) => [n, t(shareLabel(mod, n))]), help: t(mod.sharingHelp ?? (mod.conventional ? "editor.nxdn.sharing_help" : "editor.modes.sharing_help")) },
		],
	};
	const sections = mod.sections ?? [{ id: "identity" }, { id: "radio" }, { id: "fallback" }, { id: "sharing" }];
	const fields = sections.flatMap((sec) => {
		const items = [...(own[sec.id] ?? []), ...specs.filter((x) => (x.section ?? "identity") === sec.id).map((x) => specField(x, t, holder, "sys"))].filter(Boolean);
		if (!items.length) return [];
		const intro = sec.intro ? [{ type: "note", key: `sys.intro.${sec.id}`, text: () => t(sec.intro) }] : [];
		return [{ group: t(`editor.modes.sec.${sec.id}`), ...(sec.advanced ? { collapsed: true } : {}) }, ...intro, ...items];
	});
	// the entry tables: control channels right after the identity, the optional ones (sites, plan) before sharing
	const tables = entryTables(api, s);
	const at = (id) => fields.findIndex((f) => f.group === t(`editor.modes.sec.${id}`));
	const ctl = tables.filter((x) => x.control), opt = tables.filter((x) => !x.control);
	const radio = at("radio");
	fields.splice(radio < 0 ? fields.length : radio, 0, ...ctl.flatMap((x) => x.fields));
	const share = at("sharing");
	fields.splice(share < 0 ? fields.length : share, 0, ...opt.flatMap((x) => x.fields));
	// the key, when the type has one: blank = none
	const sec = mod.secret;
	if (sec) {
		const keyHolder = { get: (r) => r.secret ?? sec.empty(), put: (r, key, v) => (r.secret = { ...(r.secret ?? sec.empty()), [key]: v }) };
		const share2 = at("sharing");
		fields.splice(share2 < 0 ? fields.length : share2, 0, { group: t("editor.modes.secret") },
			{ type: "note", key: "sys.keynote", text: () => t("editor.modes.secret_note") },
			...sec.fields.map((x) => ({ ...specField(x, t, keyHolder, "sec"), disabled: (r) => !r.needsKey })));
	}
	if (mod.traffic) fields.push(...trafficFields(api, s, mod.traffic));
	formDialog(t(isNew ? "editor.modes.add_title" : "editor.modes.edit_title", { name: s.name, type: t(mod.label ?? `editor.mode_name.${row.token}`) }), fields, s, t, (work) => api.change(t("editor.undo.edit"), () => {
		if (isNew) {
			m.systems.push(s);
			assignNumbers(m.systems, MODE_LIMITS.systems);
		}
		const v = work.secret;
		Object.assign(s, work, { number: s.number });
		if (sec) s.secret = !v || Object.values(v).every((x) => x === "" || x === 0) ? null : v;
		syncModes(m);
	}), { list: true });
}

function trafficFields({ t, h, button }, system, traffic) {
	const rows = (system.trafficKeys ?? []).map((key) => ({ ...key }));
	const labels = { title: "editor.nxdn.traffic_keys", add: "editor.nxdn.add_key", show: "editor.nxdn.show_seeds", ...traffic.labels };
	let reveal = false;
	const tbody = h("tbody", {});
	const add = button(`+ ${t(labels.add)}`, () => {
		rows.push({});
		draw();
	}, { quiet: true });
	// A secret column's cell: "set" while hidden, else its input; a Generate button where the column makes values
	const secretCell = (col, row) => {
		const hidden = !reveal;
		const has = row[col.key] != null || row._text?.[col.key];
		const body = hidden && has ? t("editor.modes.secret_set") : cell(h, t, col, row, hidden);
		if (!col.generate) return body;
		return h("span", {}, body, button(t("editor.aes.generate"), () => {
			row._text = { ...row._text, [col.key]: col.generate() };
			draw();
		}, { quiet: true }));
	};
	const draw = () => {
		add.disabled = rows.length >= traffic.max;
		tbody.replaceChildren(...rows.map((row, i) => h("tr", {},
			...traffic.columns.map((col) => h("td", {}, col.secret ? secretCell(col, row) : cell(h, t, col, row, false))),
			h("td", {}, button(t("editor.modes.remove_row"), () => {
				rows.splice(i, 1);
				draw();
			}, { quiet: true, danger: true })))));
	};
	draw();
	const show = h("input", { type: "checkbox", id: "show-traffic-seeds" });
	show.addEventListener("change", () => {
		reveal = show.checked;
		draw();
	});
	const table = h("div", { class: "form-table" },
		h("div", { class: "form-row form-check" }, h("label", { for: show.id }, t(labels.show)), show),
		h("div", { class: "table-wrap" }, h("table", { class: "data" },
			h("thead", {}, h("tr", {}, ...traffic.columns.map((col) => h("th", {}, label(t, col.label))), h("th", {}))), tbody)),
		add);
	const blank = (col) => (col.type === "hexbytes" ? t("editor.err.hex_bytes", { n: col.bytes }) : t("editor.err.range", { min: col.min, max: col.max }));
	return [
		{ group: t(labels.title) },
		{ type: "custom", key: "trafficKeys", render: () => table, parse: (form) => {
			const errors = [], keys = [], ids = new Set();
			for (const row of rows) {
				const key = { slot: row.slot, flags: row.flags };
				for (const col of traffic.columns) {
					const raw = row._text?.[col.key] ?? formatSpec(col, row[col.key], t);
					const result = raw.trim() ? parseSpec(col, raw, t) : { error: blank(col) };
					if (result.error) errors.push(`${label(t, col.label)}: ${result.error}`);
					else key[col.key] = result.value;
				}
				if (key.id != null && ids.has(key.id)) errors.push(t("editor.nxdn.duplicate_key", { id: key.id }));
				ids.add(key.id);
				keys.push(key);
			}
			if (!errors.length) form.trafficKeys = keys;
			return errors;
		} },
	];
}

// --- A system's entries: a table per class, inside the system form ---

// Each class's part of the system form: { control, fields: [heading, help, the table] }. The rows are copies until the
// form saves; entries of classes the module doesn't edit are kept as they are.
function entryTables(api, s) {
	const { t, h, button } = api;
	const mod = typeRow(s.type).module;
	// a small button with a symbol: its name for the tooltip and screen readers
	const icon = (sym, name, fn, opts) => {
		const b = button(sym, fn, opts);
		b.title = name;
		b.setAttribute("aria-label", name);
		return b;
	};
	const others = s.entries.filter((e) => !mod.classes.some((c) => c.cls === e.cls));
	const work = [...mod.classes].sort((a, b) => !!b.control - !!a.control).map((c) => ({ c, rows: s.entries.filter((e) => e.cls === c.cls).map((e) => ({ ...e })) }));
	// a control channel table starts with one row to type into; left untouched, it isn't saved
	const fresh = (c, rows) => ({ cls: c.cls, group: 0xFF, flags: 0, control: false, ...c.newEntry(rows) });
	for (const w of work) if (w.c.control && !w.rows.length) w.rows.push({ ...fresh(w.c, w.rows), _blank: true });
	// a control channel's site goes last: the frequencies come first
	const columns = (c) => {
		const g = c.group ? [{ key: "group", label: c.groupLabel, type: "int", min: 0, max: c.groupMax }] : [];
		return c.control ? [...c.fields, ...g] : [...g, ...c.fields];
	};
	const parseAll = (form) => {
		const bad = [], out = [];
		for (const { c, rows } of work) {
			rows.forEach((r, i) => {
				if (r._blank && !r._text) return;
				const e = { ...r };
				delete e._blank;
				for (const col of columns(c)) {
					if (col.type === "bool" || col.type === "select") continue;
					const res = parseSpec(col, r._text?.[col.key] ?? formatSpec(col, r[col.key], t), t, e);
					if (res.error) bad.push(`${t(`editor.modes.class.${c.name}`)} ${i + 1}, ${label(t, col.label)}: ${res.error}`);
					else e[col.key] = res.value;
				}
				delete e._text;
				out.push(e);
			});
		}
		if (!bad.length) form.entries = [...out, ...others];
		return bad;
	};
	return work.map(({ c, rows }, n) => {
		const cols = columns(c);
		const tbody = h("tbody", {});
		const draw = () => {
			tbody.replaceChildren(...rows.map((r, i) => h("tr", {}, ...cols.map((col) => h("td", {}, cell(h, t, col, r))),
				h("td", {}, h("div", { class: "ed-actions" },
					c.control ? icon("↑", t("editor.modes.up"), () => {
						if (i) [rows[i - 1], rows[i]] = [rows[i], rows[i - 1]];
						draw();
					}, { quiet: true, disabled: !i }) : null,
					icon("−", t("editor.modes.remove_row"), () => {
						rows.splice(i, 1);
						draw();
					}, { quiet: true, danger: true }))))));
		};
		draw();
		const table = h("div", { class: "form-table" },
			h("div", { class: "table-wrap" }, h("table", { class: "data" },
				h("thead", {}, h("tr", {}, ...cols.map((x) => h("th", {}, label(t, x.label))), h("th", {}, ""))), tbody)),
			button(`+ ${t(`editor.modes.add_${c.name}`)}`, () => {
				rows.push(fresh(c, rows));
				draw();
			}, { quiet: true }));
		return {
			control: !!c.control,
			fields: [
				{ group: t(`editor.modes.class.${c.name}`), ...(c.control ? {} : { collapsed: !rows.length }) },
				{ type: "note", key: `sys.cls${c.cls}`, text: () => t(`editor.modes.class.${c.name}_help`) },
				// one parse covers every table: the first table's field does it
				{ type: "custom", key: `sys.tbl${c.cls}`, render: () => table, parse: (form) => (n === 0 ? parseAll(form) : []) },
			],
		};
	});
}

// One cell's input: a checkbox, a select, or text kept as typed until Save parses it
function cell(h, t, col, r, masked = false) {
	if (col.type === "bool") {
		const box = h("input", { type: "checkbox", "aria-label": label(t, col.label) });
		box.checked = !!r[col.key];
		box.addEventListener("change", () => (r[col.key] = box.checked));
		return box;
	}
	if (col.type === "select") {
		const opts = col.options();
		const sel = h("select", { "aria-label": label(t, col.label) }, ...opts.map(([, l], i) => h("option", { value: String(i) }, label(t, l))));
		sel.selectedIndex = Math.max(0, opts.findIndex(([v]) => v === r[col.key]));
		sel.addEventListener("change", () => (r[col.key] = opts[sel.selectedIndex][0]));
		return sel;
	}
	const input = h("input", { type: masked ? "password" : "text", size: col.type === "freq" ? 9 : col.type === "hexbytes" ? 2 * col.bytes : 5, spellcheck: "false", "aria-label": label(t, col.label), value: r._text?.[col.key] ?? formatSpec(col, r[col.key], t) });
	input.addEventListener("input", () => (r._text = { ...r._text, [col.key]: input.value }));
	return input;
}

// --- Radio tab: each offered kind's radio-wide record ---

export function modeSettingsPart(api, features) {
	const { model: m, t, h } = api;
	const out = [];
	for (const k of offeredKinds(m, features)) {
		const st = k.module.settings;
		if (!st) continue;
		const now = m.modeSettings?.[k.id] ?? st.decode(null);
		const rows = st.fields.map((spec) => {
			const id = `ms-${k.id}-${spec.key}`;
			const input = h("input", { type: "text", id, value: formatSpec(spec, now[spec.key] || null, t), spellcheck: "false" });
			const err = h("span", { class: "small ed-err" });
			input.addEventListener("change", () => {
				const res = parseSpec(spec, input.value, t, now);
				err.textContent = res.error ?? "";
				if (res.error) return;
				api.change(t("editor.undo.edit"), () => (m.modeSettings = { ...m.modeSettings, [k.id]: { ...(m.modeSettings?.[k.id] ?? now), [spec.key]: res.value } }));
			});
			return h("div", { class: "form-row" }, h("label", { for: id }, `${k.short}: ${label(t, spec.label)}`), input, spec.help ? h("span", { class: "small muted" }, t(spec.help)) : null, err);
		});
		out.push(...rows);
	}
	return out;
}

export { SYSTEM_TYPES };
