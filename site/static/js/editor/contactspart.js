// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DMR ID Database tab: build the database, or private-call contacts, from RadioID.net users. Users come from a
// user.csv the user downloads, or from RadioID's API for a few regions. The user picks regions, filters and the text
// each record shows. The choices and the user's own RadioID API key are kept in this browser (localStorage), never
// sent anywhere but RadioID itself.

import { register } from "./extensions.js";
import * as diag from "../diag.js";
import { h, button, progressBar } from "../flasher/dom.js";
import * as radioid from "../codeplug/sources/radioid.js";
import { writeIddb, eraseIddb, convertIddb, addContacts, iddbEnd, iddbSize, iddbRecords } from "../codeplug/sources/output.js";
import { TEXT_LENGTHS } from "../codeplug/iddb.js";

const PREFS = "rfoundry-editor-radioid";
const KEY = "rfoundry-radioid-api-key";

const DEFAULTS = {
	mode: "file",	// "file" (user.csv) or "api"
	regions: [],	// [{country, state?}]
	prefixes: "",
	idMin: "",
	idMax: "",
	template: "{call} {first} {city} {st}",
	contactTemplate: "{call} {first}",
	upper: false,
	textLen: 32,
	radio: "",	// radio id for the room check when none is connected
};

function load(k, fallback) {
	try {
		const v = localStorage.getItem(k);
		return v === null ? fallback : JSON.parse(v);
	} catch {
		return fallback;
	}
}

function store(k, v) {
	try {
		localStorage.setItem(k, JSON.stringify(v));
	} catch {
		// storage blocked: the choices last for this visit only
	}
}

// One builder for the page's life, so loaded users and half-made choices survive switching tabs
const B = {
	prefs: { ...DEFAULTS, ...load(PREFS, {}) },
	key: load(KEY, ""),
	users: null,		// [user] loaded from a file or fetched
	source: "",		// what they came from, for the status line
	api: null,
	el: null,
	parts: null,
};

const save = () => store(PREFS, B.prefs);

register({
	iddbTab(api) {
		B.api = api;
		if (!B.el) build(api);
		update();
		return B.el;
	},
	tableActions(tab, api) {
		if (tab !== "contacts") return [];
		return [button(api.t("editor.rid.add_from"), () => {
			api.show("iddb");
			setTimeout(() => B.parts?.output.scrollIntoView({ behavior: "smooth", block: "start" }), 0);
		}, { quiet: true })];
	},
});

// --- Building the tab ---

function build(api) {
	const { t } = api;
	const P = (B.parts = {});
	const field = (label, input, help) => h("label", { class: "rid-field" }, h("span", {}, label), input, help ? h("span", { class: "small muted" }, help) : null);
	const changed = () => {
		save();
		update();
	};

	// Current database
	P.current = h("div", { class: "rid-current" });

	// 1. Users
	const modeBtn = (mode, title, text) => h("button", { type: "button", class: "choice", "aria-pressed": "false", "data-mode": mode,
		onclick: () => {
			B.prefs.mode = mode;
			changed();
		} }, h("strong", {}, title), h("span", { class: "small muted" }, text));
	P.modes = h("div", { class: "choice-grid" },
		modeBtn("file", t("editor.rid.mode_file"), t("editor.rid.mode_file_help")),
		modeBtn("api", t("editor.rid.mode_api"), t("editor.rid.mode_api_help")));
	const picker = h("input", { type: "file", accept: ".csv,.txt", hidden: true, onchange: async () => {
		const f = picker.files?.[0];
		picker.value = "";
		if (!f) return;
		P.loadStatus.textContent = t("editor.rid.reading");
		const users = radioid.parseUsers(await f.text());
		if (!users.length) {
			P.loadStatus.textContent = "";
			return api.say(t("editor.rid.no_users", { name: f.name }), "bad");
		}
		B.users = users;
		B.source = f.name;
		update();
	} });
	P.loadStatus = h("span", { class: "small muted" });
	P.fileBox = h("div", { class: "rid-row" },
		button(t("editor.rid.choose_file"), () => picker.click()), picker,
		h("a", { href: radioid.DOWNLOAD, target: "_blank", rel: "noopener" }, t("editor.rid.download")),
		P.loadStatus);
	const keyInput = h("input", { type: "password", autocomplete: "off", spellcheck: "false", value: B.key, placeholder: t("editor.rid.key_placeholder"),
		oninput: (e) => {
			B.key = e.target.value.trim();
			store(KEY, B.key);
			if (B.apiClient) B.apiClient.key = B.key;
		} });
	P.apiBox = h("div", {},
		h("p", { class: "small" }, t("editor.rid.api_note", { max: (radioid.MAX_PAGES * radioid.PER_PAGE).toLocaleString() })),
		field(t("editor.rid.key"), keyInput, t("editor.rid.key_help")),
		h("p", { class: "small muted" }, t("editor.rid.proxy_note")));

	// 2. Regions
	P.country = h("select", { onchange: () => renderStates() });
	P.states = h("div", { class: "rid-states" });
	P.chosen = h("div", { class: "rid-chosen" });
	P.addRegion = button(t("editor.rid.add_region"), addRegion);
	P.fetch = button(t("editor.rid.fetch"), fetchUsers, { primary: true });
	P.fetchStatus = h("span", { class: "small muted", "aria-live": "polite" });

	// 3. Filters
	const text = (k, attrs = {}) => h("input", { type: "text", value: B.prefs[k], ...attrs, oninput: (e) => {
		B.prefs[k] = e.target.value;
		changed();
	} });
	P.prefixes = text("prefixes", { placeholder: "N4, K4, W4" });
	P.idMin = text("idMin", { inputmode: "numeric", placeholder: "1" });
	P.idMax = text("idMax", { inputmode: "numeric", placeholder: "16777215" });

	// 4. Text
	P.template = text("template", { class: "rid-template" });
	P.contactTemplate = text("contactTemplate", { class: "rid-template" });
	const tokens = (input, k) => h("div", { class: "rid-tokens" }, ...radioid.TOKENS.map((tok) => h("button", { type: "button", class: "btn-quiet rid-token",
		title: t(`editor.rid.token.${tok}`), onclick: () => {
			const at = input.selectionStart ?? input.value.length;
			const ins = (at && input.value[at - 1] !== " " ? " " : "") + `{${tok}}`;
			input.value = input.value.slice(0, at) + ins + input.value.slice(input.selectionEnd ?? at);
			B.prefs[k] = input.value;
			changed();
			input.focus();
		} }, `{${tok}}`)));
	P.upper = h("input", { type: "checkbox", checked: B.prefs.upper, onchange: (e) => {
		B.prefs.upper = e.target.checked;
		changed();
	} });
	P.textLen = h("select", { onchange: (e) => {
		B.prefs.textLen = Number(e.target.value);
		changed();
	} }, ...TEXT_LENGTHS.map((n) => h("option", { value: n, selected: n === B.prefs.textLen }, t("editor.rid.chars", { n }))));
	P.radio = h("select", { onchange: (e) => {
		B.prefs.radio = e.target.value;
		changed();
	} });
	P.preview = h("div", { class: "table-wrap" });

	// Output
	P.summary = h("p", { class: "rid-summary" });
	P.writeDb = button(t("editor.rid.write_db"), () => output("iddb"), { primary: true });
	P.addContacts = button(t("editor.rid.add_contacts"), () => output("contacts"));
	P.output = h("div", { class: "rid-output" }, P.summary, h("div", { class: "rid-row" }, P.writeDb, P.addContacts));

	const step = (n, title, ...body) => h("section", { class: "rid-step" }, h("h3", {}, h("span", { class: "rid-n" }, String(n)), title), ...body);
	B.el = h("div", { class: "rid" },
		P.current,
		h("h2", { class: "rid-title" }, t("editor.rid.title")),
		h("p", { class: "small muted" }, t("editor.rid.lead"), " ", h("a", { href: radioid.SITE, target: "_blank", rel: "noopener" }, "RadioID.net"), "."),
		step(1, t("editor.rid.step_users"), P.modes, P.fileBox, P.apiBox),
		step(2, t("editor.rid.step_regions"),
			h("div", { class: "rid-row" }, field(t("editor.rid.country"), P.country), P.addRegion),
			P.states, P.chosen, h("div", { class: "rid-row" }, P.fetch, P.fetchStatus)),
		step(3, t("editor.rid.step_filters"),
			h("div", { class: "rid-grid" },
				field(t("editor.rid.prefixes"), P.prefixes, t("editor.rid.prefixes_help")),
				field(t("editor.rid.id_min"), P.idMin), field(t("editor.rid.id_max"), P.idMax))),
		step(4, t("editor.rid.step_text"),
			field(t("editor.rid.template"), P.template), tokens(P.template, "template"),
			h("div", { class: "rid-grid" },
				field(t("editor.rid.text_len"), P.textLen),
				h("label", { class: "rid-check" }, P.upper, t("editor.rid.upper")),
				field(t("editor.rid.radio"), P.radio)),
			field(t("editor.rid.contact_template"), P.contactTemplate), tokens(P.contactTemplate, "contactTemplate"),
			h("p", { class: "small muted rid-help" }, t("editor.rid.contact_help")),
			h("h4", {}, t("editor.rid.preview")), P.preview),
		P.output);
	renderCountries();
}

// --- State to screen ---

function update() {
	const { t } = B.api;
	const P = B.parts;
	const api = B.api;
	const pr = B.prefs;
	// current database
	const db = api.model.iddb;
	const present = db?.format === "C" ? "editor.iddb.present_compact" : "editor.iddb.present";
	// a database in format N for a radio that reads format C: the offer to convert (iddb-compact.md 6)
	const upgrade = db?.format === "N" && api.compact === true;
	// a read that skipped the radio's database: it stays on the radio as it is, until read here or replaced
	const skipped = api.iddbSkipped;
	P.current.replaceChildren(...[skipped ? h("p", { class: "notice" }, t("editor.iddb_read.skipped", { n: skipped.count.toLocaleString() }), " ",
		button(t("editor.iddb_read.read"), () => api.readSkippedIddb(), { primary: true }))
		: h("p", {}, db ? t(present, { n: db.count.toLocaleString(), len: db.textLen }) : t("editor.iddb.none")),
		upgrade ? h("p", { class: "notice" }, t("editor.iddb.upgrade", { radio: api.radio.name }), " ",
			button(t("editor.iddb.convert"), () => api.upgradeIddb(), { primary: true })) : null,
		db ? button(t("editor.rid.remove_db"), () => api.confirmDialog(t("editor.rid.remove_confirm"), t("editor.rid.remove_db"), () => {
			api.change(t("editor.rid.undo_remove"), () => eraseIddb(api.model));
			api.markIddb();
		}), { danger: true, quiet: true }) : null].filter(Boolean));
	// source
	for (const b of P.modes.querySelectorAll("[data-mode]")) b.setAttribute("aria-pressed", String(b.dataset.mode === pr.mode));
	P.fileBox.hidden = pr.mode !== "file";
	P.apiBox.hidden = pr.mode !== "api";
	P.fetch.hidden = pr.mode !== "api";
	P.fetch.disabled = !pr.regions.length;
	P.loadStatus.textContent = B.users ? t("editor.rid.loaded", { n: B.users.length.toLocaleString(), name: B.source }) : "";
	if (B.users && B.parts.counted !== B.users) renderCountries();
	renderChosen();
	// radio for the room check
	const conn = api.radio;
	P.radio.disabled = !!conn;
	P.radio.replaceChildren(...(conn ? [h("option", {}, conn.name)] : [h("option", { value: "" }, t("editor.rid.any_radio")),
		...api.radios.filter((r) => r.codeplug).map((r) => h("option", { value: r.id, selected: r.id === pr.radio }, r.name))]));
	renderOutput();
}

function selection() {
	if (!B.users) return [];
	const pr = B.prefs;
	const num = (s, d) => (/^\d+$/.test(String(s).trim()) ? Number(s) : d);
	return radioid.select(B.users, {
		regions: pr.mode === "file" ? pr.regions : [],	// fetched users are the regions already
		prefixes: String(pr.prefixes).split(/[,\s]+/),
		idMin: num(pr.idMin, 0), idMax: num(pr.idMax, 0xFFFFFF),
	});
}

// The database format to write: C (iddb-compact.md) unless the connected radio can't read it. Without a radio, C: a
// write to a radio without it converts (iddb-compact.md 6)
function idFormat() {
	return B.api.compact === false ? "N" : "C";
}

// The database's size for the selection, kept until the selection or the text settings change: a format C estimate
// lays every record out (about 0.3 s for 130,000)
let sizeMemo = { key: "", size: null };
function selectionSize(sel, end) {
	const pr = B.prefs, fmt = idFormat();
	if (fmt === "N") return iddbSize(sel, pr.textLen, end, fmt);
	// every ID in the key: two selections of one length with the same first and last ID can differ in between
	const ids = sel.reduce((s, u) => (Math.imul(s, 31) + u.id) | 0, 0);
	const key = [sel.length, ids, pr.template, pr.upper, pr.textLen, end].join("|");
	if (sizeMemo.key !== key) sizeMemo = { key, size: iddbSize(iddbRecords(sel, pr), pr.textLen, end, fmt) };
	return sizeMemo.size;
}

function targetRadio() {
	const api = B.api;
	return api.radio || api.radios.find((r) => r.id === B.prefs.radio) || null;
}

function renderOutput() {
	const { t } = B.api;
	const P = B.parts;
	const pr = B.prefs;
	const sel = selection();
	const radio = targetRadio();
	const size = selectionSize(sel, iddbEnd(radio));
	const parts = [];
	if (!B.users) parts.push(t(pr.mode === "file" ? "editor.rid.need_file" : "editor.rid.need_fetch"));
	else {
		parts.push(t("editor.rid.selected", { n: sel.length.toLocaleString(), of: B.users.length.toLocaleString() }));
		if (sel.length) parts.push(t("editor.rid.size", { kb: Math.ceil(size.bytes / 1024).toLocaleString() }));
		if (radio && sel.length > size.fit) parts.push(t("editor.rid.too_many", { fit: size.fit.toLocaleString(), radio: radio.name }));
		else if (radio && size.fit !== Infinity && sel.length) parts.push(t("editor.rid.room", { fit: size.fit.toLocaleString(), radio: radio.name }));
	}
	P.summary.textContent = parts.join(" ");
	P.summary.classList.toggle("bad", !!(radio && sel.length > size.fit));
	P.writeDb.disabled = !sel.length;
	P.addContacts.disabled = !sel.length;
	// preview: the first few, as the radio will show them
	const shown = sel.slice(0, 6);
	P.preview.replaceChildren(shown.length ? h("table", { class: "rid-preview" },
		h("thead", {}, h("tr", {}, h("th", {}, t("editor.rid.id")), h("th", {}, t("editor.rid.preview_db", { n: pr.textLen })), h("th", {}, t("editor.rid.preview_contact")))),
		h("tbody", {}, ...shown.map((u) => h("tr", {}, h("td", { class: "c-num" }, String(u.id)),
			h("td", {}, h("code", {}, radioid.format(u, pr.template, { upper: pr.upper, max: pr.textLen }))),
			h("td", {}, h("code", {}, radioid.format(u, pr.contactTemplate, { upper: pr.upper, max: 16 }))))))) :
		h("p", { class: "small muted" }, t("editor.rid.preview_none")));
}

// --- Regions ---

function renderCountries() {
	const { t } = B.api;
	const P = B.parts;
	const counts = B.users ? new Map(radioid.regionsOf(B.users).map((r) => [r.country.toLowerCase(), r])) : null;
	P.counted = B.users;
	const pick = P.country.value;
	P.country.replaceChildren(h("option", { value: "" }, t("editor.rid.pick_country")),
		...radioid.COUNTRIES.map((c) => {
			const n = counts?.get(c.name.toLowerCase())?.count;
			if (counts && !n) return null;
			return h("option", { value: c.name, selected: c.name === pick }, n ? `${c.name} (${n.toLocaleString()})` : c.name);
		}));
	renderStates();
}

function renderStates() {
	const { t } = B.api;
	const P = B.parts;
	const name = P.country.value;
	const c = radioid.COUNTRIES.find((x) => x.name === name);
	P.addRegion.disabled = !c;
	if (!c?.states.length) return P.states.replaceChildren();
	const counts = B.users ? radioid.regionsOf(B.users.filter((u) => u.country.toLowerCase() === name.toLowerCase()))[0] : null;
	const n = (s) => counts?.states.find((x) => x.name.toLowerCase() === s.toLowerCase())?.count;
	P.states.replaceChildren(h("p", { class: "small muted" }, t("editor.rid.states_help")),
		h("div", { class: "rid-state-list" }, ...c.states.map((s) => h("label", {}, h("input", { type: "checkbox", value: s }),
			n(s) ? `${s} (${n(s).toLocaleString()})` : s))));
}

function addRegion() {
	const P = B.parts;
	const country = P.country.value;
	if (!country) return;
	const states = [...P.states.querySelectorAll("input:checked")].map((x) => x.value);
	const add = states.length ? states.map((state) => ({ country, state })) : [{ country }];
	const k = (r) => `${r.country}\n${r.state || ""}`.toLowerCase();
	const have = new Set(B.prefs.regions.map(k));
	// a whole country replaces its states; a state under a chosen whole country adds nothing
	if (!states.length) B.prefs.regions = B.prefs.regions.filter((r) => r.country !== country);
	else if (have.has(k({ country }))) return;
	for (const r of add) if (!have.has(k(r))) B.prefs.regions.push(r);
	for (const x of P.states.querySelectorAll("input:checked")) x.checked = false;
	save();
	update();
}

function renderChosen() {
	const { t } = B.api;
	const P = B.parts;
	const rs = B.prefs.regions;
	P.chosen.replaceChildren(rs.length ? h("ul", { class: "rid-chips" }, ...rs.map((r, i) => h("li", {},
		r.state ? `${r.state}, ${r.country}` : r.country,
		h("button", { type: "button", class: "btn-quiet", "aria-label": t("editor.rid.remove_region"), title: t("editor.rid.remove_region"), onclick: () => {
			B.prefs.regions.splice(i, 1);
			save();
			update();
		} }, "×")))) :
		h("p", { class: "small muted" }, t(B.prefs.mode === "file" ? "editor.rid.no_regions_file" : "editor.rid.no_regions_api")));
}

// --- Fetching ---

async function fetchUsers() {
	const api = B.api;
	const { t } = api;
	const P = B.parts;
	const regions = B.prefs.regions.slice();
	if (!regions.length) return;
	B.apiClient = B.apiClient || new radioid.RadioIdApi({ key: B.key });
	const client = B.apiClient;
	const ctrl = new AbortController();
	const bar = progressBar();
	const d = api.dialog(t("editor.rid.fetching"), [bar.el], null, null, () => ctrl.abort(), { cancel: t("editor.stop") });
	try {
		// sizes first, so a selection too big for the API is caught before the long part
		let pages = 0, count = 0;
		for (const [i, r] of regions.entries()) {
			bar.set(i, regions.length, t("editor.rid.counting"));
			const c = await client.count(r, ctrl.signal);
			pages += c.pages;
			count += c.count;
		}
		if (pages > radioid.MAX_PAGES) {
			d.close();
			return api.say(t("editor.rid.too_big", { n: count.toLocaleString(), max: (radioid.MAX_PAGES * radioid.PER_PAGE).toLocaleString() }), "bad");
		}
		const users = [];
		let done = 0;
		for (const r of regions) {
			const got = await client.fetch(r, { signal: ctrl.signal, progress: (n) => bar.set(done + n, pages, t("editor.rid.fetch_progress", { n: done + n, of: pages })) });
			done += Math.max(1, Math.ceil(got.length / radioid.PER_PAGE));
			users.push(...got);
		}
		const seen = new Set();
		B.users = users.filter((u) => !seen.has(u.id) && seen.add(u.id)).sort((a, b) => a.id - b.id);
		B.source = t("editor.rid.from_api");
		d.close();
		P.fetchStatus.textContent = t(client.via === "proxy" ? "editor.rid.via_proxy" : "editor.rid.via_direct");
		update();
	} catch (e) {
		d.close();
		if (e?.name === "AbortError" || ctrl.signal.aborted) return;
		const rec = diag.error("radioid fetch", e);
		api.say(t("editor.rid.fetch_failed", { error: e?.params?.error || e?.message || String(e) }), "bad", { report: rec });
	}
}

// --- Output ---

function output(kind) {
	const api = B.api;
	const { t } = api;
	const pr = B.prefs;
	const sel = selection();
	if (!sel.length) return;
	if (kind === "iddb") {
		const go = () => {
			let r = null;
			api.change(t("editor.rid.undo_db"), () => {
				r = writeIddb(api.model, sel, { template: pr.template, upper: pr.upper, textLen: pr.textLen, end: iddbEnd(targetRadio()), fmt: idFormat() });
			});
			api.markIddb();
			api.say(t("editor.rid.db_done", { n: r.count.toLocaleString() }) + (r.dropped ? " " + t("editor.rid.db_dropped", { n: r.dropped.toLocaleString() }) : ""), r.dropped ? "warn" : "ok");
		};
		if (api.model.iddb) api.confirmDialog(t("editor.rid.replace_confirm", { n: api.model.iddb.count.toLocaleString() }), t("editor.rid.write_db"), go);
		else go();
		return;
	}
	let r = null;
	api.change(t("editor.rid.undo_contacts"), () => {
		r = addContacts(api.model, sel, { template: pr.contactTemplate, upper: pr.upper });
		return r.added ? undefined : false;
	});
	const msg = [r.added ? t("editor.rid.contacts_done", { n: r.added }) : t("editor.rid.contacts_none")];
	if (r.existing) msg.push(t("editor.rid.contacts_existing", { n: r.existing }));
	if (r.full) msg.push(t("editor.rid.contacts_full", { n: r.full }));
	api.say(msg.join(" "), r.full ? "warn" : "ok");
}
