// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The Codeplug Editor's Trunked Systems tab: the DMR Tier III systems (docs/spec/dmr-trunking.md 12) in a list, and
// each in a dialog of its basic and advanced settings with its channel table. parseSystem() turns what was typed
// into the system's fields, with no DOM, so the tests run it.

import { mhz, parseFreq } from "../codeplug/fields.js";
import { putSystem, remove, systemUsable } from "../codeplug/model.js";
import { T3_LIMITS, SEP_HZ, netMax, siteMax, newSystem, validKey, ID_MAX, POLL_FORMATS, POLL_MAX, encodePollReply } from "../codeplug/trunk.js";
import { fallbackOptions } from "./views.js";

const MODELS = [0, 1, 2, 3];
const OPTIONS = ["listenOnly", "allCalls", "emergencyAny", "privateCalls", "registerTalkgroups", "acceptAls", "acceptKill", "noRoaming", "acceptStun", "ipGateway"];

// A whole number typed, blank as 0; null when it isn't one in lo..hi. Accepts 0x for hex
function intIn(v, lo, hi) {
	const s = String(v ?? "").trim();
	if (s === "") return 0;
	const n = Number(s);
	return Number.isInteger(n) && n >= lo && n <= hi ? n : null;
}

// MHz typed, blank as 0, to Hz; null when it isn't a frequency
function hzIn(v) {
	const s = String(v ?? "").trim();
	if (s === "") return 0;
	return parseFreq(s);
}

// raw: the dialog's strings and choices {name, model, net, category, colorCode, key, psn, fallbackHold, ownId,
// planBaseHz, planFirstLcn, planSep, planSplitHz, leaveDbm, sites, the option booleans, authenticates, sharing,
// fallback, planTxLow, planNoLearn, chans: [{control, lcn, rxHz, txHz, cc}]}.
// Returns {fields, chans, errors: [text]}
export function parseSystem(raw, t) {
	const errors = [];
	const bad = (label, text) => errors.push(`${t(label)}: ${text}`);
	const range = (label, v, lo, hi) => {
		const n = intIn(v, lo, hi);
		if (n === null) bad(label, t("editor.err.range", { min: lo, max: hi }));
		return n ?? 0;
	};
	const f = {};
	f.name = String(raw.name ?? "").trim();
	if (!f.name) bad("editor.col.name", t("editor.err.no_name"));
	else if (f.name.length > T3_LIMITS.name) bad("editor.col.name", t("editor.err.too_long", { max: T3_LIMITS.name }));
	f.model = raw.model;
	f.net = range("editor.t3.net", raw.net, 0, netMax(f.model));
	f.category = raw.category;
	f.colorCode = raw.colorCode;
	f.authenticates = !!raw.authenticates;
	for (const k of OPTIONS) f[k] = !!raw[k];
	f.key = String(raw.key ?? "").replace(/[\s:-]/g, "").toUpperCase();
	if (f.key && !validKey(f.key)) bad("editor.t3.key", t("editor.err.t3_key"));
	f.psn = range("editor.t3.psn", raw.psn, 0, 0xFFFFFF);
	f.sharing = raw.sharing;
	f.fallback = raw.fallback ?? null;
	f.fallbackHold = range("editor.t3.fallback_hold", raw.fallbackHold, 0, 255);
	f.ownId = range("editor.t3.own_id", raw.ownId, 0, ID_MAX);
	f.planBaseHz = hzIn(raw.planBaseHz);
	if (f.planBaseHz === null) {
		bad("editor.t3.plan_base", t("editor.err.bad_freq"));
		f.planBaseHz = 0;
	}
	f.planFirstLcn = range("editor.t3.plan_first", raw.planFirstLcn, 0, T3_LIMITS.lcn);
	f.planSep = raw.planSep;
	const split = String(raw.planSplitHz ?? "").trim();
	f.planSplitHz = split === "" ? 0 : Math.round(Number(split) * 1e6);
	if (!(Number.isInteger(f.planSplitHz) && f.planSplitHz >= 0 && f.planSplitHz < 1e9)) {
		bad("editor.t3.plan_split", t("editor.err.bad_freq"));
		f.planSplitHz = 0;
	}
	f.planTxLow = !!raw.planTxLow;
	f.planNoLearn = !!raw.planNoLearn;
	f.leaveDbm = range("editor.t3.leave_dbm", raw.leaveDbm, -128, 127);
	f.powerSave = raw.powerSave;
	const sites = String(raw.sites ?? "").split(/[\s,;]+/).filter(Boolean).map((x) => intIn(x, 0, siteMax(f.model)));
	if (sites.includes(null) || sites.length > T3_LIMITS.sites) bad("editor.t3.sites", t("editor.err.t3_sites", { max: siteMax(f.model), n: T3_LIMITS.sites }));
	f.sites = sites.filter((x) => x !== null).slice(0, T3_LIMITS.sites);
	f.pollFormat = raw.pollFormat ?? POLL_FORMATS.iso8;
	f.pollBlocks = raw.pollBlocks ?? 4;
	if (raw.pollReply === null) f.pollReply = null;	// a reply in a format not written here: kept as stored
	else {
		f.pollReply = String(raw.pollReply ?? "");
		f.pollRaw = null;
		if (!encodePollReply(f.pollFormat, f.pollReply)) bad("editor.t3.poll_reply", t("editor.err.t3_poll_reply", { max: POLL_MAX }));
	}

	const chans = [];
	(raw.chans || []).forEach((c, i) => {
		const row = t("editor.t3.ch.row", { n: i + 1 });
		const lcn = intIn(c.lcn, 1, T3_LIMITS.lcn);
		if (!lcn) errors.push(`${row}: ${t("editor.t3.ch.lcn")} ${t("editor.err.range", { min: 1, max: T3_LIMITS.lcn })}`);
		const rxHz = parseFreq(c.rxHz), txHz = parseFreq(c.txHz);
		if (rxHz === null || txHz === null) errors.push(`${row}: ${t("editor.err.bad_freq")}`);
		chans.push({ control: !!c.control, lcn: lcn || 0, rxHz: rxHz || 0, txHz: txHz || 0, cc: c.cc ?? null });
	});
	return { fields: f, chans, errors };
}

// --- The tab ---

export function trunkTab(api) {
	const { model: m, t, h, button } = api;
	const systems = [...(m.trunkSystems ?? [])].sort((a, b) => a.number - b.number);
	const full = systems.length >= T3_LIMITS.systems;
	const parts = [
		h("p", { class: "small muted" }, t("editor.t3.intro")),
		h("div", { class: "ed-bar" }, h("div", { class: "ed-actions" },
			button(t("editor.t3.add"), () => systemDialog(api, newSystem({ name: newName(m, t) }), true), { disabled: full }))),
	];
	if (!systems.length) parts.push(h("p", { class: "grid-empty muted" }, t("editor.empty.trunkSystems")));
	else {
		const head = ["#", t("editor.col.name"), t("editor.t3.col.identity"), t("editor.col.cc"), t("editor.t3.col.control"), t("editor.t3.col.key"),
			t("editor.t3.col.sharing"), t("editor.t3.col.channels"), t("editor.t3.col.status"), ""];
		const rows = systems.map((s) => {
			const ctrl = m.trunkChannels.filter((c) => c.system === s && c.control).length;
			const used = m.channels.filter((c) => c.system === s).length;
			const usable = systemUsable(m, s);
			const status = usable ? h("span", { class: "badge badge-supported" }, t("editor.t3.status.ready"))
				: h("span", { class: "badge badge-testing" }, t(ctrl ? "editor.t3.status.no_key" : "editor.t3.status.no_control"));
			return h("tr", {},
				h("td", {}, String(s.number)), h("td", {}, s.name), h("td", {}, identity(s, t)), h("td", {}, String(s.colorCode)),
				h("td", {}, String(ctrl)), h("td", {}, s.key ? t("editor.t3.key_set") : t("editor.none")),
				h("td", {}, t(`editor.t3.sharing.${s.sharing}`)), h("td", {}, String(used)), h("td", {}, status),
				h("td", {}, h("div", { class: "ed-actions" },
					button(t("editor.t3.edit"), () => systemDialog(api, s, false), { quiet: true }),
					button(t("editor.delete"), () => deleteSystem(api, s), { quiet: true, danger: true }))));
		});
		parts.push(h("div", { class: "table-wrap" }, h("table", { class: "data" },
			h("thead", {}, h("tr", {}, ...head.map((x) => h("th", {}, x)))), h("tbody", {}, ...rows))));
	}
	// channel records of a system the codeplug doesn't hold (another tool's): the radio ignores them
	const orphans = (m.trunkChannels ?? []).filter((c) => typeof c.system === "number");
	if (orphans.length) {
		parts.push(h("div", { class: "notice" }, h("p", {}, t("editor.t3.orphans", { n: orphans.length })),
			button(t("editor.t3.orphans_remove"), () => api.change(t("editor.undo.delete"), () => remove(m, "trunkChannels", orphans)))));
	}
	return h("div", { class: "t3" }, ...parts);
}

const identity = (s, t) => t("editor.t3.identity", { model: t(`editor.t3.model.${s.model}`), net: s.net, category: s.category.toUpperCase() });

function newName(m, t) {
	const names = new Set(m.trunkSystems.map((s) => s.name));
	let i = m.trunkSystems.length + 1;
	while (names.has(`${t("editor.new.system")} ${i}`)) i++;
	return `${t("editor.new.system")} ${i}`;
}

function deleteSystem(api, s) {
	const { t, model: m } = api;
	const used = m.channels.filter((c) => c.system === s).length;
	api.confirmDialog(t("editor.t3.delete_confirm", { name: s.name, n: used }), t("editor.delete"),
		() => api.change(t("editor.undo.delete"), () => remove(m, "trunkSystems", [s])));
}

// --- A system's dialog ---

function systemDialog(api, s, isNew) {
	const { model: m, t, h, button } = api;
	let uid = 0;
	const id = () => `t3-${++uid}`;
	const row = (label, input, help = null, cls = "") => {
		input.id ||= id();
		return h("div", { class: `form-row${cls}` }, h("label", { for: input.id }, label), input, help ? h("span", { class: "small muted" }, help) : null);
	};
	const text = (v, extra = {}) => h("input", { type: "text", value: v ?? "", spellcheck: "false", ...extra });
	const select = (opts, v) => {
		const el = h("select", {}, ...opts.map(([, label], i) => h("option", { value: String(i) }, label)));
		el.selectedIndex = Math.max(0, opts.findIndex(([x]) => x === v));
		el._opts = opts;
		return el;
	};
	const val = (el) => el._opts[el.selectedIndex]?.[0];
	const check = (label, v, help = null) => {
		const box = h("input", { type: "checkbox", id: id() });
		box.checked = !!v;
		return { box, row: h("div", { class: "form-row form-check" }, h("label", { for: box.id }, label), box, help ? h("span", { class: "small muted t3-check-help" }, help) : null) };
	};
	const blankZero = (v) => (v ? String(v) : "");

	// Basic
	const name = text(s.name, { maxlength: T3_LIMITS.name });
	const model = select(MODELS.map((n) => [n, t(`editor.t3.model.${n}`)]), s.model);
	const net = text(String(s.net));
	const netHelp = h("span", { class: "small muted" });
	const category = select([["a", "A"], ["b", "B"]], s.category);
	const cc = select([...Array(16).keys()].map((n) => [n, String(n)]), s.colorCode);
	const auth = check(t("editor.t3.authenticates"), s.authenticates, t("editor.t3.authenticates_help"));
	const key = text(s.key, { maxlength: 47, class: "t3-key", autocomplete: "off" });
	const psn = text(blankZero(s.psn));
	const sharing = select([0, 1, 2].map((n) => [n, t(`editor.t3.sharing.${n}`)]), s.sharing);
	const shareNote = h("div", {});
	const fallback = select(fallbackOptions(m, t, s.fallback, null, t("editor.none")), s.fallback);
	const hold = text(blankZero(s.fallbackHold));
	const warnings = h("div", {});

	// The channel table: one row of inputs per record, in hunt order
	const tbody = h("tbody", {});
	const ccOpts = [[null, t("editor.t3.ch.cc_system")], ...[...Array(16).keys()].map((n) => [n, String(n)])];
	const chanRow = (c) => {
		const ctl = h("input", { type: "checkbox", "aria-label": t("editor.t3.ch.control") });
		ctl.checked = !!c.control;
		const lcn = text(c.lcn ? String(c.lcn) : "", { "aria-label": t("editor.t3.ch.lcn"), size: 5 });
		const rx = text(c.rxHz ? mhz(c.rxHz) : "", { "aria-label": t("editor.col.rx"), size: 10 });
		const tx = text(c.txHz ? mhz(c.txHz) : "", { "aria-label": t("editor.col.tx"), size: 10 });
		const ccSel = select(ccOpts, c.cc ?? null);
		ccSel.setAttribute("aria-label", t("editor.col.cc"));
		const tr = h("tr", {});
		tr._read = () => ({ control: ctl.checked, lcn: lcn.value, rxHz: rx.value, txHz: tx.value, cc: val(ccSel) });
		const moveBy = (d) => {
			const to = d < 0 ? tr.previousElementSibling : tr.nextElementSibling?.nextElementSibling;
			if (d < 0 && !to) return;
			tbody.insertBefore(tr, to || null);
		};
		tr.append(h("td", { class: "c-bool" }, ctl), h("td", {}, lcn), h("td", {}, rx), h("td", {}, tx), h("td", {}, ccSel),
			h("td", {}, h("div", { class: "ed-actions" },
				button("↑", () => moveBy(-1), { quiet: true }), button("↓", () => moveBy(1), { quiet: true }),
				button(t("editor.t3.ch.remove"), () => {
					tr.remove();
					sync();
				}, { quiet: true }))));
		return tr;
	};
	for (const c of m.trunkChannels.filter((x) => x.system === s)) tbody.append(chanRow(c));
	const addChan = button(t("editor.t3.ch.add"), () => {
		tbody.append(chanRow({ control: tbody.children.length === 0, lcn: 0, rxHz: 0, txHz: 0, cc: null }));
		sync();
	});
	const chanTable = h("div", { class: "table-wrap t3-chans" }, h("table", { class: "data" },
		h("thead", {}, h("tr", {}, ...[t("editor.t3.ch.control"), t("editor.t3.ch.lcn"), t("editor.col.rx"), t("editor.col.tx"), t("editor.col.cc"), ""].map((x) => h("th", {}, x)))),
		tbody));

	// Advanced
	const opts = Object.fromEntries(OPTIONS.map((k) => [k, check(t(`editor.t3.opt.${k}`), s[k], k === "acceptStun" || k === "ipGateway" ? t(`editor.t3.opt.${k}_help`) : null)]));
	const ownId = text(blankZero(s.ownId));
	const planBase = text(s.planBaseHz ? mhz(s.planBaseHz) : "");
	const planFirst = text(blankZero(s.planFirstLcn));
	const planSep = select(SEP_HZ.map((hz, i) => [i, `${hz / 1000} kHz`]), s.planSep);
	const planSplit = text(s.planSplitHz ? String(s.planSplitHz / 1e6) : "");
	const txLow = check(t("editor.t3.plan_tx_low"), s.planTxLow);
	const noLearn = check(t("editor.t3.plan_no_learn"), s.planNoLearn);
	const leave = text(blankZero(s.leaveDbm));
	const ps = select([[0, t("editor.t3.power_save_off")], ...[1, 2, 3, 4, 5, 6, 7].map((n) => [n, `1:${2 ** n}`])], s.powerSave);
	const sites = text(s.sites.join(", "));
	const pollOpts = Object.entries(POLL_FORMATS).map(([k, n]) => [n, t(`editor.t3.poll_format.${k}`)]);
	const pollOther = !Object.values(POLL_FORMATS).includes(s.pollFormat);
	if (pollOther) pollOpts.push([s.pollFormat, t("editor.t3.poll_format.other", { n: s.pollFormat })]);
	const pollFormat = select(pollOpts, s.pollFormat);
	const pollBlocks = select([1, 2, 3, 4].map((n) => [n, String(n)]), s.pollBlocks);
	const pollReply = text(s.pollReply ?? "", { maxlength: 52 });
	const pollKept = () => (pollReply.disabled = s.pollReply === null && val(pollFormat) === s.pollFormat);
	pollFormat.addEventListener("change", pollKept);
	pollKept();

	const sync = () => {
		netHelp.textContent = t("editor.t3.net_help", { max: netMax(val(model)) });
		const hasKey = !!key.value.trim();
		const hasCtrl = [...tbody.children].some((tr) => tr._read().control);
		const w = [];
		if (!hasCtrl) w.push(t("editor.t3.no_control_warning"));
		else if (auth.box.checked && !hasKey) w.push(t("editor.t3.no_key_warning"));
		warnings.replaceChildren(...w.map((x) => h("p", { class: "notice" }, x)));
		const share = val(sharing);
		shareNote.replaceChildren(h("p", { class: share === 2 ? "notice bad" : share === 1 ? "notice" : "small muted" }, t(`editor.t3.sharing_help.${share}`)),
			h("p", { class: "small muted" }, t("editor.t3.own_id_never")));
	};

	const basic = h("div", { class: "form-grid" },
		row(t("editor.col.name"), name),
		row(t("editor.t3.model"), model),
		h("div", { class: "form-row" }, h("label", { for: (net.id = id()) }, t("editor.t3.net")), net, netHelp),
		row(t("editor.t3.category"), category),
		row(t("editor.col.cc"), cc),
		h("h3", { class: "form-group" }, t("editor.t3.channels")),
		h("p", { class: "form-note small muted" }, t("editor.t3.channels_help", { ctrl: T3_LIMITS.ctrl, max: T3_LIMITS.chans })),
		h("div", { class: "form-note" }, chanTable, addChan),
		h("h3", { class: "form-group" }, t("editor.t3.access")),
		auth.row,
		row(t("editor.t3.key"), key, t("editor.t3.key_help")),
		row(t("editor.t3.psn"), psn, t("editor.t3.psn_help")),
		h("div", { class: "form-note" }, warnings),
		h("h3", { class: "form-group" }, t("editor.t3.sharing")),
		row(t("editor.t3.sharing"), sharing),
		h("div", { class: "form-note" }, shareNote),
		h("h3", { class: "form-group" }, t("editor.t3.fallback")),
		row(t("editor.t3.fallback"), fallback, t("editor.t3.fallback_help")),
		row(t("editor.t3.fallback_hold"), hold, t("editor.t3.fallback_hold_help")));
	const advanced = h("details", { class: "t3-advanced" }, h("summary", {}, t("editor.t3.advanced")), h("div", { class: "form-grid" },
		h("h3", { class: "form-group" }, t("editor.t3.options")),
		...OPTIONS.map((k) => opts[k].row),
		h("h3", { class: "form-group" }, t("editor.t3.identity_group")),
		row(t("editor.t3.own_id"), ownId, t("editor.t3.own_id_help")),
		row(t("editor.t3.sites"), sites, t("editor.t3.sites_help", { n: T3_LIMITS.sites })),
		h("h3", { class: "form-group" }, t("editor.t3.plan")),
		h("p", { class: "form-note small muted" }, t("editor.t3.plan_help")),
		row(t("editor.t3.plan_base"), planBase, t("editor.t3.plan_base_help")),
		row(t("editor.t3.plan_first"), planFirst),
		row(t("editor.t3.plan_sep"), planSep),
		row(t("editor.t3.plan_split"), planSplit),
		txLow.row, noLearn.row,
		h("h3", { class: "form-group" }, t("editor.t3.roaming")),
		row(t("editor.t3.leave_dbm"), leave, t("editor.t3.leave_dbm_help")),
		row(t("editor.t3.power_save"), ps),
		h("h3", { class: "form-group" }, t("editor.t3.poll")),
		h("p", { class: "form-note small muted" }, t("editor.t3.poll_help")),
		row(t("editor.t3.poll_format"), pollFormat),
		row(t("editor.t3.poll_blocks"), pollBlocks, t("editor.t3.poll_blocks_help")),
		row(t("editor.t3.poll_reply"), pollReply, t(s.pollReply === null ? "editor.t3.poll_reply_kept" : "editor.t3.poll_reply_help"))));
	const errs = h("p", { class: "notice bad", hidden: true });
	const body = [h("h3", { class: "form-group" }, t("editor.t3.basic")), basic, advanced, errs];
	for (const el of [model, auth.box, key, sharing]) {
		el.addEventListener("change", sync);
		el.addEventListener("input", sync);
	}
	tbody.addEventListener("change", sync);
	sync();

	api.dialog(t("editor.t3.title", { name: s.name }), body, t("editor.save_record"), (close) => {
		const raw = {
			name: name.value, model: val(model), net: net.value, category: val(category), colorCode: val(cc),
			authenticates: auth.box.checked, key: key.value, psn: psn.value, sharing: val(sharing), fallback: val(fallback),
			fallbackHold: hold.value, ownId: ownId.value, planBaseHz: planBase.value, planFirstLcn: planFirst.value,
			planSep: val(planSep), planSplitHz: planSplit.value, planTxLow: txLow.box.checked, planNoLearn: noLearn.box.checked,
			leaveDbm: leave.value, powerSave: val(ps), sites: sites.value, pollFormat: val(pollFormat), pollBlocks: val(pollBlocks),
			// a reply kept as stored stays so while its format does; another format starts a text reply
			pollReply: s.pollReply === null && val(pollFormat) === s.pollFormat ? null : pollReply.value,
			chans: [...tbody.children].map((tr) => tr._read()),
			...Object.fromEntries(OPTIONS.map((k) => [k, opts[k].box.checked])),
		};
		const { fields, chans, errors } = parseSystem(raw, t);
		if (errors.length) {
			errs.hidden = false;
			errs.textContent = errors.join(" · ");
			return;
		}
		api.change(t(isNew ? "editor.undo.add" : "editor.undo.edit"), () => putSystem(m, s, fields, chans));
		close();
	});
}
