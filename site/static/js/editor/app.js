// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The Codeplug Editor page: a codeplug from a file or a radio, edited in tables and forms, saved to a file or
// written back to the radio. Everything runs in the page; the codeplug never leaves it unless the user puts it in a
// bug report (report.js). The working codeplug is kept in this browser (IndexedDB) so a reload doesn't lose edits.

import { h, t, ROOT, button, notice, download, progressBar } from "../flasher/dom.js";
import { SerialStream, serialSupported, RFOUNDRY_USB } from "../flasher/link.js";
import { CodeplugImage } from "../codeplug/image.js";
import { decode, encode, empty, remove, validate, newChannel, newContact, numberAll, assignNumbers } from "../codeplug/model.js";
import { LIMITS } from "../codeplug/records.js";
import { CpsSerial, TransportError, estimate } from "../codeplug/transport.js";
import { FakeCpsRadio } from "../codeplug/fakecps.js";
import { Grid } from "./grid.js";
import { History } from "./undo.js";
import { columns, channelForm, formDialog, membersDialog, refLabel } from "./views.js";
import { openFile, saveBytes, tftpBytes, FileError, imageFrom, saveDraft, loadDraft, clearDraft } from "./files.js";
import { extensions } from "./extensions.js";
import "./parts.js";
import * as diag from "../diag.js";
import { initReporting, reportLine } from "../report.js";

const CABLES = [0x0403, 0x1A86, 0x10C4, 0x067B].map((usbVendorId) => ({ usbVendorId }));
const TABS = ["radio", "channels", "zones", "contacts", "rxLists", "dtmfContacts", "aprs", "iddb", "problems"];
const TABLE_MAX = { channels: LIMITS.channels, zones: LIMITS.zones, contacts: LIMITS.contacts, rxLists: LIMITS.rxLists, dtmfContacts: LIMITS.dtmfContacts, aprs: LIMITS.aprs };

const S = {
	model: null,
	history: new History(),
	fileName: "codeplug",
	dirty: false,
	tab: "channels",
	grid: null,
	radios: [],
	conn: null,		// {transport, radio, image: the radio's bytes as last read or written, cable}
	lastRadioImage: null,	// the last connection's image, for a bug report after it closed
	iddbChanged: false,
	sim: location.hash.startsWith("#sim"),		// #sim-foreign: a radio still holding stock data in its settings area
	filter: {},
};

const root = document.getElementById("editor");
const toolbar = h("div", { class: "ed-toolbar" });
const status = h("div", { class: "ed-status small muted", "aria-live": "polite" });
const tabs = h("nav", { class: "ed-tabs", role: "tablist", "aria-label": t("editor.tabs") });
const view = h("div", { class: "ed-view" });
const messages = h("div", { class: "ed-messages", "aria-live": "polite" });

// --- Model changes ---

function load(model, name, { fromRadio = false } = {}) {
	S.model = model;
	S.fileName = name;
	S.history.reset();
	S.dirty = false;
	S.iddbChanged = false;
	if (!fromRadio && S.conn) S.conn.image = S.conn.image || null;
	refresh();
	draftSoon();
}

// Run fn as one undoable change
function change(label, fn) {
	S.history.record(S.model, label);
	const r = fn();
	if (r === false) {
		S.history.cancel();
		return false;
	}
	S.dirty = true;
	refresh();
	draftSoon();
	return true;
}

let draftTimer = null;
function draftSoon() {
	clearTimeout(draftTimer);
	draftTimer = setTimeout(() => {
		try {
			saveDraft({ bytes: saveBytes(encode(S.model)), name: S.fileName, dirty: S.dirty, at: new Date().toISOString() });
		} catch {
			// an invalid model can't be encoded yet; the next change saves it
		}
	}, 800);
}

// report: a failure worth a bug report gets the way to make one
function say(text, kind = "info", { report = false } = {}) {
	if (kind === "bad") diag.note(`shown: ${text}`);
	const n = notice(text, kind);
	if (report) n.append(reportLine());
	const close = button("×", () => n.remove(), { quiet: true });
	close.setAttribute("aria-label", t("editor.dismiss"));
	n.prepend(close);
	close.classList.add("ed-dismiss");
	messages.replaceChildren(n);
}

// --- Rendering ---

function problems() {
	return validate(S.model, { bands: S.conn?.radio?.bands_hz || null });
}

function refresh() {
	const p = problems();
	const errors = p.filter((x) => x.level === "error").length;
	renderToolbar(errors);
	const counts = {
		channels: S.model.channels.length, zones: S.model.zones.length, contacts: S.model.contacts.length,
		rxLists: S.model.rxLists.length, dtmfContacts: S.model.dtmfContacts.length, aprs: S.model.aprs.length,
		iddb: S.model.iddb?.count || 0, problems: p.length,
	};
	tabs.replaceChildren(...TABS.map((k) => {
		const b = h("button", { type: "button", role: "tab", "aria-selected": String(S.tab === k), class: k === "problems" && errors ? "has-errors" : "",
			onclick: () => show(k) }, t(`editor.tab.${k}`), k in counts && k !== "radio" ? h("span", { class: "ed-count" }, String(counts[k])) : null);
		return b;
	}));
	const conn = S.conn ? t("editor.status.connected", { radio: S.conn.radio.name }) : "";
	status.textContent = [S.fileName, S.dirty ? t("editor.status.unsaved") : t("editor.status.saved"), conn].filter(Boolean).join(" · ");
	renderView(p);
}

function show(tab) {
	S.tab = tab;
	S.grid = null;
	refresh();
}

function renderToolbar(errors) {
	const save = h("details", { class: "ed-menu" }, h("summary", { class: "btn" }, t("editor.save")),
		h("div", { class: "ed-menu-list" },
			button(t("editor.save.bin"), () => saveFile("bin")),
			button(t("editor.save.tftp"), () => saveFile("tftp")),
			...extensions.saveItems(api()).map(([label, fn]) => button(label, fn))));
	save.addEventListener("click", (e) => {
		if (e.target.closest(".ed-menu-list button")) save.open = false;
	});
	const input = h("input", { type: "file", accept: ".bin,.zip,.yaml,.yml,.csv,.txt", hidden: true, onchange: async () => {
		const f = input.files?.[0];
		input.value = "";
		if (f) await openPicked(f.name, new Uint8Array(await f.arrayBuffer()));
	} });
	toolbar.replaceChildren(
		h("div", { class: "ed-group" },
			button(t("editor.new"), newCodeplug),
			button(t("editor.open"), () => input.click()), input,
			save),
		h("div", { class: "ed-group" },
			button(t("editor.read"), readRadio),
			button(t("editor.write"), writeRadio, { primary: true })),
		h("div", { class: "ed-group" },
			button(t("editor.undo"), undo, { disabled: !S.history.canUndo, quiet: true }),
			button(t("editor.redo"), redo, { disabled: !S.history.canRedo, quiet: true })),
	);
}

function renderView(p) {
	const tab = S.tab;
	if (tab === "radio") return view.replaceChildren(radioTab());
	if (tab === "iddb") return view.replaceChildren(extensions.iddbTab(api()));
	if (tab === "problems") return view.replaceChildren(problemsTab(p));
	// a table: keep the grid (focus, sort, selection) when only its rows changed
	if (S.grid && S.grid.table === tab) {
		S.grid.g.setRows(S.model[tab]);
		S.grid.update();
		return;
	}
	const g = new Grid({
		columns: columns(tab, S.model, t), rows: S.model[tab], t, empty: t(`editor.empty.${tab}`),
		onChange: (row, col, value) => {
			let err = null;
			change(t("editor.undo.edit"), () => {
				err = col.set(row, value);
				return err ? false : undefined;
			});
			return err;
		},
		onSelect: () => S.grid?.update(),
		onOpen: tab === "channels" || tab === "zones" || tab === "rxLists" ? (r) => openRecord(tab, r) : null,
	});
	g.filter = S.filter[tab] || "";
	g.render();
	const filter = h("input", { type: "search", class: "ed-filter", placeholder: t("editor.filter"), "aria-label": t("editor.filter"), value: g.filter,
		oninput: (e) => {
			S.filter[tab] = e.target.value;
			g.setFilter(e.target.value);
		} });
	const actions = h("div", { class: "ed-actions" });
	const update = () => actions.replaceChildren(...tableActions(tab, g));
	S.grid = { table: tab, g, update };
	update();
	view.replaceChildren(h("div", { class: "ed-bar" }, actions, filter), g.el);
}

function tableActions(tab, g) {
	const sel = g.selection();
	const full = S.model[tab].length >= TABLE_MAX[tab];
	const numbered = tab !== "zones";
	const out = [
		// first, so it sits above the row checkboxes it acts on
		button(sel.length ? t("editor.bulk_edit_n", { n: sel.length }) : t("editor.bulk_edit"), () => bulkSet(tab, g), { disabled: sel.length < 2 }),
		button(t("editor.add"), () => addRecord(tab, g), { disabled: full }),
		button(t("editor.duplicate"), () => duplicate(tab, g), { disabled: full || !g.targets().length }),
		button(t("editor.delete"), () => del(tab, g), { disabled: !g.targets().length, danger: true }),
		button(t("editor.move_up"), () => move(tab, g, -1), { disabled: !g.targets().length, quiet: true }),
		button(t("editor.move_down"), () => move(tab, g, 1), { disabled: !g.targets().length, quiet: true }),
	];
	if (numbered) out.push(button(t("editor.renumber"), () => renumber(tab), { quiet: true }));
	out.push(...extensions.tableActions(tab, api()));
	return out;
}

// --- Table actions ---

function blank(tab) {
	const n = (base) => {
		const names = new Set(S.model[tab].map((x) => x.name));
		let i = S.model[tab].length + 1;
		while (names.has(`${base} ${i}`)) i++;
		return `${base} ${i}`;
	};
	switch (tab) {
	case "channels": return newChannel({ name: n(t("editor.new.channel")) });
	case "contacts": return newContact({ name: n(t("editor.new.contact")), id: 9 });
	case "zones": return { name: n(t("editor.new.zone")), channels: [] };
	case "rxLists": return { number: null, name: n(t("editor.new.rx_list")), contacts: [] };
	case "dtmfContacts": return { number: null, name: n(t("editor.new.dtmf")), code: "0" };
	case "aprs": return { number: null, name: "APRS", ssid: 7, lat: 0, lon: 0, path1: "WIDE1-1", path2: "", symTable: "/", symCode: ">", comment: "", txHz: 0, baud300: false, useFixed: false, qsy: false, silent: false, ambiguity: 0 };
	}
}

function addRecord(tab, g) {
	const r = blank(tab);
	change(t("editor.undo.add"), () => {
		S.model[tab].push(r);
		if (tab !== "zones") assignNumbers(S.model[tab], TABLE_MAX[tab]);
	});
	g.focusRow(r);
	if (tab === "zones" || tab === "rxLists") openRecord(tab, r);
}

function duplicate(tab, g) {
	const src = g.targets();
	const room = TABLE_MAX[tab] - S.model[tab].length;
	change(t("editor.undo.duplicate"), () => {
		for (const r of src.slice(0, room)) {
			const c = { ...r, number: null, name: (r.name + " +").slice(0, tab === "aprs" ? LIMITS.aprsName : LIMITS.name) };
			for (const k of Object.keys(c)) if (Array.isArray(c[k])) c[k] = c[k].slice();
			S.model[tab].push(c);
		}
		if (tab !== "zones") assignNumbers(S.model[tab], TABLE_MAX[tab]);
	});
}

function del(tab, g) {
	const items = g.targets();
	if (!items.length) return;
	const go = () => change(t("editor.undo.delete"), () => remove(S.model, tab, items));
	if (items.length === 1) return go();
	confirmDialog(t("editor.delete_confirm", { n: items.length }), t("editor.delete"), go);
}

function move(tab, g, d) {
	const items = g.targets();
	change(t("editor.undo.move"), () => {
		if (tab === "zones") {
			const list = S.model.zones;
			const idx = items.map((x) => list.indexOf(x)).sort((a, b) => (d < 0 ? a - b : b - a));
			for (const i of idx) {
				const j = i + d;
				if (j < 0 || j >= list.length || items.includes(list[j])) continue;
				[list[i], list[j]] = [list[j], list[i]];
			}
			return;
		}
		// numbered records swap numbers with their neighbor in number order
		const list = [...S.model[tab]].sort((a, b) => a.number - b.number);
		const idx = items.map((x) => list.indexOf(x)).sort((a, b) => (d < 0 ? a - b : b - a));
		for (const i of idx) {
			const j = i + d;
			if (j < 0 || j >= list.length || items.includes(list[j])) continue;
			[list[i].number, list[j].number] = [list[j].number, list[i].number];
			[list[i], list[j]] = [list[j], list[i]];
		}
		S.model[tab].sort((a, b) => a.number - b.number);
	});
	g.sort = null;
	g.render();
}

function renumber(tab) {
	change(t("editor.undo.renumber"), () => {
		const list = [...S.model[tab]].sort((a, b) => a.number - b.number);
		list.forEach((r, i) => (r.number = i + 1));
		S.model[tab] = list;
	});
}

function bulkSet(tab, g) {
	const rows = g.selection();
	const cols = columns(tab, S.model, t).filter((c) => c.readOnly !== true && c.key !== "name" && c.key !== "number");
	const pickCol = h("select", {}, ...cols.map((c, i) => h("option", { value: String(i) }, c.label)));
	const slot = h("div");
	const errs = h("p", { class: "notice bad", hidden: true });
	let input;
	const fill = () => {
		const c = cols[pickCol.selectedIndex];
		if (c.type === "select") {
			const opts = c.options(rows[0]);
			input = h("select", {}, ...opts.map(([, label], i) => h("option", { value: String(i) }, label)));
			input._opts = opts;
		} else if (c.type === "bool") {
			input = h("select", {}, h("option", { value: "1" }, t("editor.yes")), h("option", { value: "0" }, t("editor.no")));
		} else input = h("input", { type: "text" });
		slot.replaceChildren(h("label", {}, t("editor.value"), input));
	};
	pickCol.addEventListener("change", fill);
	fill();
	dialog(t("editor.bulk_edit_n", { n: rows.length }), [h("label", {}, t("editor.field"), pickCol), slot, errs], t("editor.apply"), (close) => {
		const c = cols[pickCol.selectedIndex];
		const v = c.type === "select" ? input._opts[input.selectedIndex][0] : c.type === "bool" ? input.value === "1" : input.value;
		let err = null;
		change(t("editor.undo.edit"), () => {
			for (const r of rows) {
				err = c.set(r, v);
				if (err) return false;
			}
		});
		if (err) {
			errs.hidden = false;
			errs.textContent = err;
			return;
		}
		close();
	});
}

function openRecord(tab, r) {
	if (tab === "channels") {
		formDialog(t("editor.channel_title", { name: r.name }), channelForm(S.model, t), r, t, (work) => change(t("editor.undo.edit"), () => Object.assign(r, work)));
	} else if (tab === "zones") {
		membersDialog(t("editor.zone_title", { name: r.name }), r.channels, [...S.model.channels].sort((a, b) => a.number - b.number), LIMITS.zoneChannels, t,
			(c) => `${c.number} · ${c.name}`, (list) => change(t("editor.undo.edit"), () => (r.channels = list)));
	} else if (tab === "rxLists") {
		membersDialog(t("editor.rx_list_title", { name: r.name }), r.contacts, S.model.contacts.filter((c) => c.type === "group"), LIMITS.rxListContacts, t,
			(c) => `${c.name} (${c.id})`, (list) => change(t("editor.undo.edit"), () => (r.contacts = list)));
	}
}

// --- Radio tab: the radio-wide settings ---

function radioTab() {
	const g = S.model.general;
	const field = (key, label, max, help, parse) => {
		const input = h("input", { type: "text", value: g[key] ?? "", maxlength: max, id: "g-" + key, spellcheck: "false" });
		const err = h("span", { class: "small ed-err" });
		input.addEventListener("change", () => {
			const res = parse(input.value);
			if (res.error) {
				err.textContent = res.error;
				return;
			}
			err.textContent = "";
			change(t("editor.undo.edit"), () => (S.model.general = { ...S.model.general, [key]: res.value }));
		});
		return h("div", { class: "form-row" }, h("label", { for: "g-" + key }, label), input, help ? h("span", { class: "small muted" }, help) : null, err);
	};
	const text = (max) => (v) => (v.length > max ? { error: t("editor.err.too_long", { max }) } : { value: v });
	const num = (lo, hi) => (v) => {
		const n = Number(v.trim() || 0);
		return Number.isInteger(n) && n >= lo && n <= hi ? { value: n } : { error: t("editor.err.range", { min: lo, max: hi }) };
	};
	const parts = [
		h("div", { class: "form-grid ed-radio-form" },
			field("dmrId", t("editor.general.dmr_id"), 8, t("editor.general.dmr_id_help"), num(0, 0xFFFFFF)),
			field("radioName", t("editor.general.radio_name"), LIMITS.radioName, t("editor.general.radio_name_help"), text(LIMITS.radioName)),
			field("bootLine1", t("editor.general.boot1"), LIMITS.bootText, null, text(LIMITS.bootText)),
			field("bootLine2", t("editor.general.boot2"), LIMITS.bootText, null, text(LIMITS.bootText)),
			field("groupHang", t("editor.general.group_hang"), 3, t("editor.general.hang_help"), num(0, 255)),
			field("privateHang", t("editor.general.private_hang"), 3, t("editor.general.hang_help"), num(0, 255))),
	];
	if (S.conn) {
		const p = S.conn.probe;
		parts.push(h("h3", {}, t("editor.connected_radio")), h("dl", { class: "specs" },
			h("dt", {}, t("editor.radio_model")), h("dd", {}, S.conn.radio.name),
			h("dt", {}, t("editor.firmware")), h("dd", {}, p?.git || "?")));
	}
	return h("div", {}, ...parts);
}

// --- Problems tab ---

function problemsTab(p) {
	if (!p.length) return notice(t("editor.problems.none"), "ok");
	const rows = p.map((x) => {
		const what = x.item ? (x.item.number ? `${t(`editor.tab.${x.table}`)} ${x.item.number} · ${x.item.name}` : `${t(`editor.tab.${x.table}`)} · ${x.item.name}`) : t(`editor.tab.${x.table === "general" ? "radio" : x.table}`);
		const go = x.item ? button(t("editor.problems.show"), () => {
			show(x.table);
			S.grid?.g.focusRow(x.item);
		}, { quiet: true }) : button(t("editor.problems.show"), () => show(x.table === "general" ? "radio" : x.table), { quiet: true });
		return h("tr", {}, h("td", {}, h("span", { class: `badge ${x.level === "error" ? "badge-bad" : "badge-testing"}` }, t(`editor.level.${x.level}`))),
			h("td", {}, what), h("td", {}, t(`editor.problem.${x.code}`, { ...x.params, field: x.field ? t(`editor.field.${x.field}`) : "" })), h("td", {}, go));
	});
	return h("div", { class: "table-wrap" }, h("table", { class: "data" }, h("tbody", {}, ...rows)));
}

// --- Files ---

async function openPicked(name, bytes) {
	try {
		const f = await openFile(name, bytes);
		if (f.kind === "image") {
			load(decode(f.image), name.replace(/\.[^.]+$/, ""));
			say(t("editor.opened", { name }), "ok");
		} else if (f.kind === "converted") {
			// another firmware's codeplug (a stock radio's backup, say): converted, never shown as raw bytes
			load(decode(f.image), name.replace(/\.[^.]+$/, ""));
			const c = f.counts;
			diag.note("codeplug converted", { from: f.from, ...c, notes: f.notes });
			say(t("editor.converted", { name, format: t(`editor.format.${f.from}`), channels: c.channels, zones: c.zones, contacts: c.contacts, rxLists: c.rxLists }),
				f.notes.length ? "warn" : "ok");
			if (f.notes.length) {
				dialog(t("editor.converted_left_out"), [h("ul", {}, ...f.notes.map((x) => h("li", {}, t(`editor.qdmr.note.${x.code}`, { n: x.n }))))],
					null, null, () => {}, { cancel: t("editor.close") });
			}
		} else if (f.kind === "foreign") {
			diag.note("codeplug not readable", { format: f.format, size: bytes.length });
			say(f.format === "tyt-uv380-rdt" ? t("editor.err.foreign_rdt") : t("editor.err.foreign", { format: t(`editor.format.${f.format}`) }), "bad");
		} else {
			const handled = await extensions.openText(f, name, api());
			if (!handled) say(t("editor.err.unknown_file"), "bad");
		}
	} catch (e) {
		diag.error("open file", e, { name, size: bytes?.length });
		say(e instanceof FileError ? t(`editor.err.${e.message}`) : t("editor.err.open_failed", { error: e.message }), "bad", { report: true });
	}
}

function saveFile(kind) {
	const errors = problems().filter((p) => p.level === "error");
	if (errors.length) return say(t("editor.err.fix_first", { n: errors.length }), "bad");
	const img = encode(S.model);
	if (kind === "tftp") {
		download("codeplug.bin", tftpBytes(img));
		say(t("editor.saved_tftp"), "info");
		return;
	}
	download(`${S.fileName || "codeplug"}.bin`, saveBytes(img));
	S.dirty = false;
	refresh();
	draftSoon();
}

function newCodeplug() {
	const go = () => {
		load(empty(), "codeplug");
		clearDraft();
	};
	if (S.dirty) confirmDialog(t("editor.new_confirm"), t("editor.new"), go);
	else go();
}

// --- Radio ---

async function connect() {
	if (S.conn) return S.conn;
	let stream, cable = false;
	if (S.sim) {
		S.fake = S.fake || new FakeCpsRadio({ board: "DM-1701", image: CodeplugImage.blank().bytes, foreign: location.hash === "#sim-foreign" });
		stream = S.fake.stream();
	} else {
		if (!serialSupported()) throw new TransportError("no_serial");
		const choice = await chooseLink();
		if (!choice) return null;
		cable = choice === "cable";
		const port = await navigator.serial.requestPort(cable ? { filters: CABLES } : { filters: [RFOUNDRY_USB] });
		diag.note("port chosen", { cable, ...port.getInfo?.() });
		stream = await SerialStream.open(port);
	}
	const tr = new CpsSerial(stream, { cable });
	try {
		const { radio, probe } = await tr.identify(S.radios);
		diag.note("radio found", { radio: radio.id, board: probe.board, git: probe.git, features: probe.features });
		S.conn = { transport: tr, radio, probe, image: null, cable };
		return S.conn;
	} catch (e) {
		await tr.close();
		throw e;
	}
}

function chooseLink() {
	return new Promise((resolve) => {
		let picked = null;
		const d = dialog(t("editor.connect.title"), [
			h("p", {}, t("editor.connect.text")),
			h("div", { class: "choice-grid" },
				h("button", { type: "button", class: "choice", onclick: () => { picked = "usb"; d.close(); } },
					h("b", {}, t("editor.connect.usb")), h("span", { class: "small muted" }, t("editor.connect.usb_help"))),
				h("button", { type: "button", class: "choice", onclick: () => { picked = "cable"; d.close(); } },
					h("b", {}, t("editor.connect.cable")), h("span", { class: "small muted" }, t("editor.connect.cable_help")))),
			h("details", { class: "small" }, h("summary", {}, t("editor.connect.network")), h("p", {}, t("editor.connect.network_help"))),
		], null, null, () => resolve(picked));
	});
}

function radioError(e) {
	if (e instanceof TransportError) {
		const p = { ...e.params };
		if (typeof p.addr === "number") p.addr = `0x${p.addr.toString(16).toUpperCase()}`;
		return t(`editor.radio_err.${e.code}`, p);
	}
	if (e?.name === "NotFoundError") return null;	// the user closed the port chooser
	return t("editor.radio_err.other", { error: e?.message || String(e) });
}

// A failure with a radio: into the log and the console with what was being done, then shown with the way to report it
async function fail(stage, e) {
	diag.error(stage, e);
	await dropConnection();
	const msg = radioError(e);
	if (msg) say(msg, "bad", { report: !diag.expected(e) });
}

// A radio fresh from its original firmware may still hold that firmware's data where RadioFoundry keeps its settings: it then
// refuses every write (settings-store.md 5). Offer to prepare it; true when the radio can take the write
async function ensureStorage(c) {
	if (c.storage?.region === "ours") return true;
	c.storage = await c.transport.storage();
	diag.note("storage region", c.storage);
	if (c.storage?.region !== "foreign") return true;
	const go = await new Promise((resolve) => {
		let ok = false;
		dialog(t("editor.storage.title"), [h("p", {}, t("editor.storage.text", { radio: c.radio.name })), h("p", {}, t("editor.storage.text2"))],
			t("editor.storage.prepare"), (close) => { ok = true; close(); }, () => resolve(ok));
	});
	if (!go) return false;
	const done = await withProgress(t("editor.storage.preparing", { radio: c.radio.name }), async (bar) => {
		bar.set(0, 1);
		return c.transport.claimStorage();
	});
	diag.note("storage claim", { done });
	if (!done) throw new TransportError("storage_claim_failed");
	c.storage = { region: "ours" };
	say(t("editor.storage.done", { radio: c.radio.name }), "ok");
	c.prepared = true;	// said again with the write's outcome, which replaces this message
	return true;
}

async function withProgress(title, fn) {
	const bar = progressBar();
	const ctrl = new AbortController();
	const d = dialog(title, [bar.el], null, null, () => ctrl.abort(), { cancel: t("editor.stop") });
	try {
		return await fn(bar, ctrl.signal);
	} finally {
		d.close();
	}
}

async function readRadio() {
	const go = async () => {
		try {
			const c = await connect();
			if (!c) return;
			const img = await withProgress(t("editor.reading", { radio: c.radio.name }), (bar, signal) =>
				c.transport.read({ signal, progress: (n, total) => bar.set(n, total) }));
			diag.note("read done", { bytes: img.bytes.length });
			c.image = img.clone();
			load(decode(img), `${c.radio.id}-codeplug`, { fromRadio: true });
			say(t("editor.read_done", { radio: c.radio.name, channels: S.model.channels.length, contacts: S.model.contacts.length }), "ok");
		} catch (e) {
			await fail("read codeplug", e);
		}
	};
	if (S.dirty) confirmDialog(t("editor.read_confirm"), t("editor.read"), go);
	else go();
}

async function writeRadio() {
	const errors = problems().filter((p) => p.level === "error");
	if (errors.length) {
		say(t("editor.err.fix_first", { n: errors.length }), "bad");
		return show("problems");
	}
	let stage = "connect";
	try {
		const c = await connect();
		if (!c) return;
		stage = "prepare radio";
		if (!await ensureStorage(c)) return;
		stage = "encode codeplug";
		const want = encode(S.model);
		if (!c.image) {
			stage = "read before write";
			c.image = await withProgress(t("editor.comparing", { radio: c.radio.name }), (bar, signal) =>
				c.transport.read({ iddb: false, signal, progress: (n, total) => bar.set(n, total) }));
		}
		const iddb = S.iddbChanged;
		stage = "plan write";
		const { plan, bytes } = c.transport.plan(want, c.image, { iddb });
		diag.note("write plan", { bytes, sectors: plan.length, iddb });
		const prepared = c.prepared ? `${t("editor.storage.done", { radio: c.radio.name })} ` : "";
		c.prepared = false;
		if (!bytes) return say(prepared + t("editor.write_nothing", { radio: c.radio.name }), "ok");
		const text = [t("editor.write_summary", { radio: c.radio.name, bytes: bytes.toLocaleString(), seconds: estimate(bytes, c.cable) })];
		if (iddb) text.push(t("editor.write_iddb", { n: S.model.iddb?.count || 0 }));
		confirmDialog(text.join(" "), t("editor.write"), async () => {
			let step = "write codeplug";
			try {
				const res = await withProgress(t("editor.writing", { radio: c.radio.name }), async (bar, signal) => {
					const r = await c.transport.write(want, iddb ? null : c.image, { iddb, signal, progress: (n, total) => bar.set(n, total, t("editor.progress.write")) });
					step = "verify codeplug";
					await c.transport.verify(want, r.plan, { progress: (n, total) => bar.set(n, total, t("editor.progress.verify")) });
					step = "finish write";
					await c.transport.finish();
					return r;
				});
				diag.note("write done", { bytes: res.bytes, sectors: res.sectors });
				c.image = want.clone();
				S.iddbChanged = false;
				S.model.base = want;
				S.dirty = false;
				refresh();
				say(prepared + t("editor.write_done", { radio: c.radio.name, bytes: res.bytes.toLocaleString() }), "ok");
			} catch (e) {
				await fail(step, e);
			}
		}, { primary: true });
	} catch (e) {
		await fail(stage, e);
	}
}

async function dropConnection() {
	if (!S.conn) return;
	S.lastRadioImage = S.conn.image || S.lastRadioImage;	// a bug report can still offer it
	await S.conn.transport.close();
	S.conn = null;
	refresh();
}

// --- Undo ---

function undo() {
	if (S.history.undo(S.model) !== null) {
		S.dirty = true;
		refresh();
		draftSoon();
	}
}

function redo() {
	if (S.history.redo(S.model) !== null) {
		S.dirty = true;
		refresh();
		draftSoon();
	}
}

document.addEventListener("keydown", (e) => {
	if (!(e.ctrlKey || e.metaKey) || e.target.closest("input, select, textarea, dialog")) return;
	const k = e.key.toLowerCase();
	if (k === "z" && !e.shiftKey) {
		e.preventDefault();
		undo();
	} else if ((k === "z" && e.shiftKey) || k === "y") {
		e.preventDefault();
		redo();
	}
});

// --- Dialogs ---

function dialog(title, body, okLabel, onOk, onClose = () => {}, { cancel = t("editor.cancel"), primary = true } = {}) {
	const d = h("dialog", { class: "confirm editor-dialog" });
	const close = () => d.close();
	const acts = h("div", { class: "confirm-actions" }, button(cancel, close));
	if (okLabel) acts.append(button(okLabel, () => onOk(close), { primary }));
	d.append(h("h2", {}, title), ...body, acts);
	d.addEventListener("close", () => {
		d.remove();
		onClose();
	});
	document.body.append(d);
	d.showModal();
	return d;
}

function confirmDialog(text, okLabel, onOk, opts = {}) {
	return dialog(okLabel, [h("p", {}, text)], okLabel, (close) => {
		close();
		onOk();
	}, () => {}, opts);
}

// --- What other modules (CSV, YAML, contacts) get from the page ---

function api() {
	return {
		get model() { return S.model; },
		t, h, button, notice, say, change, load, refresh, download, dialog, confirmDialog,
		get fileName() { return S.fileName; },
		get tab() { return S.tab; },
		show,
		get radio() { return S.conn?.radio || null; },
		get radios() { return S.radios; },
		root: ROOT,
		markIddb() {
			S.iddbChanged = true;
		},
		encode: () => encode(S.model),
		decode, numberAll,
	};
}

// --- Start ---

// What a bug report says about the editor: no names, callsigns or IDs (automatic error reports carry it too)
function reportContext() {
	const c = S.conn, m = S.model;
	return {
		sim: S.sim,
		radio: c ? { id: c.radio?.id, board: c.probe?.board, git: c.probe?.git, features: c.probe?.features, codec: c.probe?.codec, cable: c.cable, storage: c.storage?.region } : null,
		codeplug: m ? { channels: m.channels.length, zones: m.zones.length, contacts: m.contacts.length, iddb: m.iddb?.count || 0,
			dirty: S.dirty, iddbChanged: S.iddbChanged, errors: problems().filter((p) => p.level === "error").length } : null,
		tab: S.tab,
	};
}

// What the user may add to a report: the codeplug being edited and the one last read from the radio
function reportFiles() {
	const out = [];
	if (S.model) out.push({ name: "codeplug-edited.bin", label: t("report.attach_codeplug"), data: () => saveBytes(encode(S.model)) });
	const radio = S.conn?.image || S.lastRadioImage;
	if (radio) out.push({ name: "codeplug-radio.bin", label: t("report.attach_radio"), data: () => saveBytes(radio) });
	return out;
}

async function start() {
	initReporting({ tool: "editor", context: reportContext, attachments: reportFiles });
	root.replaceChildren(h("div", { class: "ed-head" }, toolbar, status), messages, tabs, view);
	try {
		const r = await fetch(`${ROOT}data/radios.json`);
		S.radios = (await r.json()).radios || [];
	} catch {
		S.radios = [];
	}
	const draft = await loadDraft();
	if (draft?.bytes) {
		load(decode(imageFrom(draft.bytes)), draft.name || "codeplug");
		S.dirty = !!draft.dirty;
		refresh();
		say(t("editor.draft_restored", { time: new Date(draft.at).toLocaleString() }), "info");
	} else load(empty(), "codeplug");
	if (!serialSupported() && !S.sim) say(t("editor.no_serial"), "info");
	if (S.sim) say(t("editor.sim"), "info");
}

start();

export { S, refLabel };
