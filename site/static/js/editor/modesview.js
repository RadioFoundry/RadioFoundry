// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The Codeplug Editor's Systems tab and the modes' part of the Radio tab (docs/spec/channel-modes.md): systems of
// every type in one list, each edited in a dialog of its generic header and its type's fields, its entries (sites,
// channels, plan rows) in a table per class, and its secret behind a button. What a type holds comes from its mode
// module (codeplug/mode_*.js); no mode is named here.

import { formDialog, fallbackOptions } from "./views.js";
import { specField, parseSpec, formatSpec, label } from "./modefields.js";
import { SYSTEM_TYPES, typeRow, typeName, offeredTypes, offeredKinds } from "../codeplug/modes.js";
import { newSystem, syncModes, SHARE, MODE_LIMITS } from "../codeplug/channelmodes.js";
import { remove, assignNumbers } from "../codeplug/model.js";

const SHARE_LEVELS = [SHARE.off, SHARE.identity, SHARE.whole];

// --- The tab ---

export function systemsTab(api, features) {
	const { model: m, t, h, button } = api;
	const systems = [...(m.systems ?? [])].sort((a, b) => a.number - b.number);
	const types = offeredTypes(m, features);
	const pick = h("select", { "aria-label": t("editor.modes.type") }, ...types.map((x) => h("option", { value: String(x.id) }, x.short)));
	const full = systems.length >= MODE_LIMITS.systems;
	const parts = [
		h("p", { class: "small muted" }, t("editor.modes.intro")),
		types.length ? h("div", { class: "ed-bar" }, h("div", { class: "ed-actions" }, pick,
			button(t("editor.modes.add"), () => {
				const s = newSystem(m, Number(pick.value), newName(m, t));
				systemDialog(api, s, true);
			}, { disabled: full }))) : null,
	];
	if (!systems.length) parts.push(h("p", { class: "grid-empty muted" }, t("editor.empty.systems")));
	else {
		const head = ["#", t("editor.col.name"), t("editor.modes.type"), t("editor.modes.identity"), t("editor.modes.listen_only"),
			t("editor.modes.sharing"), t("editor.modes.entries"), t("editor.modes.secret"), t("editor.tab.channels"), ""];
		const rows = systems.map((s) => {
			const row = typeRow(s.type), mod = row?.module;
			const used = m.channels.filter((c) => c.system === s).length;
			const actions = mod ? [
				button(t("editor.modes.edit"), () => systemDialog(api, s, false), { quiet: true }),
				button(t("editor.modes.entries"), () => entriesDialog(api, s), { quiet: true }),
				mod.secret ? button(t("editor.modes.secret"), () => secretDialog(api, s), { quiet: true }) : null,
			] : [];
			return h("tr", {},
				h("td", {}, String(s.number)), h("td", {}, s.name), h("td", {}, typeName(s.type)),
				h("td", {}, mod ? mod.identity(s.fields) : t("editor.modes.type_unknown")),
				h("td", {}, s.tx ? t("editor.general.off") : t("editor.general.on")),
				h("td", {}, t(`editor.modes.share${s.sharing <= 2 ? s.sharing : 0}`)),
				h("td", {}, String(s.entries.length)),
				h("td", {}, s.secret ? t("editor.modes.secret_set") : t("editor.modes.secret_none")),
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

// --- A system's header and its type's fields ---

function systemDialog(api, s, isNew) {
	const { model: m, t } = api;
	const mod = typeRow(s.type).module;
	const holder = { get: (r) => r.fields, put: (r, key, v) => (r.fields = { ...r.fields, [key]: v }) };
	const specs = mod.fields();
	const fields = [
		{ group: t("editor.form.basic") },
		{ key: "name", label: t("editor.col.name"), type: "text", parse: (r, k, v) => {
			v = String(v ?? "").trim();
			if (!v) return t("editor.err.no_name");
			if (v.length > MODE_LIMITS.name) return t("editor.err.too_long", { max: MODE_LIMITS.name });
			r.name = v;
		} },
		...specs.filter((x) => !x.advanced).map((x) => specField(x, t, holder, "sys")),
		// listen only is the user's per-system choice (7.1): never forced by sharing
		{ key: "listenOnly", label: t("editor.modes.listen_only"), type: "bool", get: (r) => !r.tx, set: (r, v) => (r.tx = !v), help: t("editor.modes.listen_only_help") },
		mod.secret ? { key: "needsKey", label: t("editor.modes.needs_key"), type: "bool", help: t("editor.modes.needs_key_help") } : null,
		{ key: "sharing", label: t("editor.modes.sharing"), type: "select", options: () => SHARE_LEVELS.map((n) => [n, t(`editor.modes.share${n}`)]), help: t("editor.modes.sharing_help") },
		{ key: "ownId", label: t("editor.modes.own_id"), type: "text", format: (v) => (v ? String(v) : ""), parse: (r, k, v) => {
			const res = parseSpec({ type: "int", min: 0, max: 0xFFFFFFFF }, v, t);
			if (res.error) return res.error;
			r.ownId = res.value;
		}, help: t("editor.modes.own_id_help") },
		{ key: "fallback", label: t("editor.modes.fallback"), type: "select", options: (r) => fallbackOptions(m, t, r.fallback, null, t("editor.none")) },
		{ group: t("editor.form.advanced") },
		...specs.filter((x) => x.advanced).map((x) => specField(x, t, holder, "sys")),
	].filter(Boolean);
	formDialog(t(isNew ? "editor.modes.add_title" : "editor.modes.edit_title", { name: s.name }), fields, s, t, (work) => api.change(t("editor.undo.edit"), () => {
		if (isNew) {
			m.systems.push(s);
			assignNumbers(m.systems, MODE_LIMITS.systems);
		}
		Object.assign(s, work, { number: s.number });
		syncModes(m);
	}));
}

// --- A system's entries: a table per class ---

function entriesDialog(api, s) {
	const { model: m, t, h, button } = api;
	const mod = typeRow(s.type).module;
	// working rows per class, copies; entries of classes the module doesn't edit are kept as they are
	const work = mod.classes.map((c) => ({ c, rows: s.entries.filter((e) => e.cls === c.cls).map((e) => ({ ...e })) }));
	const others = s.entries.filter((e) => !mod.classes.some((c) => c.cls === e.cls));
	const errs = h("p", { class: "notice bad", hidden: true });
	const body = h("div", {});
	const columns = (c) => [
		...(c.group ? [{ key: "group", label: c.groupLabel, type: "int", min: 0, max: c.groupMax }] : []),
		...c.fields,
	];
	const draw = () => {
		body.replaceChildren(...work.map(({ c, rows }) => {
			const cols = columns(c);
			const trs = rows.map((r, i) => h("tr", {}, ...cols.map((col) => h("td", {}, cell(h, t, col, r))),
				h("td", {}, h("div", { class: "ed-actions" },
					button("↑", () => {
						if (i) [rows[i - 1], rows[i]] = [rows[i], rows[i - 1]];
						draw();
					}, { quiet: true, disabled: !i }),
					button(t("editor.delete"), () => {
						rows.splice(i, 1);
						draw();
					}, { quiet: true, danger: true })))));
			return h("section", {}, h("h3", {}, t(`editor.modes.class.${c.name}`)),
				h("div", { class: "table-wrap" }, h("table", { class: "data" },
					h("thead", {}, h("tr", {}, ...cols.map((x) => h("th", {}, label(t, x.label))), h("th", {}, ""))), h("tbody", {}, ...trs))),
				button(t("editor.add"), () => {
					rows.push({ cls: c.cls, group: 0xFF, flags: 0, control: false, ...c.newEntry(rows) });
					draw();
				}, { quiet: true }));
		}));
	};
	draw();
	const dlg = h("dialog", { class: "confirm editor-dialog" });
	const save = () => {
		const bad = [];
		const out = [];
		for (const { c, rows } of work) {
			rows.forEach((r, i) => {
				const e = { ...r };
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
		if (bad.length) {
			errs.hidden = false;
			errs.textContent = bad.join(" · ");
			return;
		}
		api.change(t("editor.undo.edit"), () => {
			s.entries = [...out, ...others];
			syncModes(m);
		});
		dlg.close();
	};
	dlg.append(h("h2", {}, t("editor.modes.entries_title", { name: s.name })), body, errs, h("div", { class: "confirm-actions" },
		button(t("editor.cancel"), () => dlg.close()), button(t("editor.save_record"), save, { primary: true })));
	dlg.addEventListener("close", () => dlg.remove());
	document.body.append(dlg);
	dlg.showModal();
}

// One cell's input: a checkbox, a select, or text kept as typed until Save parses it
function cell(h, t, col, r) {
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
	const input = h("input", { type: "text", size: 10, spellcheck: "false", "aria-label": label(t, col.label), value: r._text?.[col.key] ?? formatSpec(col, r[col.key], t) });
	input.addEventListener("input", () => (r._text = { ...r._text, [col.key]: input.value }));
	return input;
}

// --- A system's secret (channel-modes.md 8): shown only when asked for ---

function secretDialog(api, s) {
	const { t } = api;
	const sec = typeRow(s.type).module.secret;
	const holder = { get: (r) => r.secret ?? sec.empty(), put: (r, key, v) => (r.secret = { ...(r.secret ?? sec.empty()), [key]: v }) };
	const fields = [
		{ type: "note", key: "secretNote", text: () => t("editor.modes.secret_note") },
		...sec.fields.map((x) => specField(x, t, holder, "sec")),
		{ key: "clear", label: t("editor.modes.secret_clear"), type: "bool" },
	];
	formDialog(t("editor.modes.secret_title", { name: s.name }), fields, { secret: s.secret, clear: false }, t, (work) => api.change(t("editor.undo.edit"), () => {
		const v = work.secret;
		s.secret = work.clear || !v || Object.values(v).every((x) => x === "" || x === 0) ? null : sec.valid(v) ? v : s.secret;
	}));
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
