// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The Codeplug Editor's record form (views.js formDialog) against a small stand-in for the DOM: what the user picks in
// one field changes the choices another field offers before the form is saved. Run with site/tests/run.sh.

import { test } from "node:test";
import assert from "node:assert/strict";

// ---- just enough DOM for dom.js h() and formDialog
class El {
	constructor(tag) {
		this.tagName = tag.toUpperCase();
		this.children = [];
		this.attrs = {};
		this.listeners = {};
		this.className = "";
		this.hidden = false;
		this.disabled = false;
		this.checked = false;
		this.value = "";
		this.selectedIndex = -1;
		this.dataset = {};
	}
	append(...cs) { for (const c of cs) this.children.push(c instanceof El ? c : { text: String(c) }); }
	replaceChildren(...cs) { this.children = []; this.append(...cs); }
	setAttribute(k, v) {
		this.attrs[k] = v;
		if (k === "value") this.value = v;
		if (k === "id") this.id = v;
	}
	getAttribute(k) { return this.attrs[k] ?? null; }
	addEventListener(ev, f) { (this.listeners[ev] ||= []).push(f); }
	dispatch(ev) { for (const f of this.listeners[ev] || []) f({ target: this, preventDefault() {} }); }
	click() { if (!this.disabled) this.dispatch("click"); }
	showModal() { this.open = true; }
	close() { this.open = false; this.dispatch("close"); }
	remove() { document.body.children = document.body.children.filter((c) => c !== this); }
	get textContent() { return this.children.map((c) => (c instanceof El ? c.textContent : c.text)).join(""); }
	set textContent(v) { this.children = [{ text: String(v) }]; }
	// the first element below this one for which pick() holds
	find(pick) {
		for (const c of this.children) {
			if (!(c instanceof El)) continue;
			if (pick(c)) return c;
			const deeper = c.find(pick);
			if (deeper) return deeper;
		}
		return null;
	}
}
globalThis.Node = El;
globalThis.document = { createElement: (t) => new El(t), createElementNS: (ns, t) => new El(t), body: new El("body"),
	hasFocus: () => false, getElementById: () => null };

const { channelForm, formDialog } = await import("../static/js/editor/views.js");
const { decode, encode, empty, newChannel, numberAll } = await import("../static/js/codeplug/model.js");
const { newSystem, syncModes } = await import("../static/js/codeplug/channelmodes.js");
const { kindName } = await import("../static/js/codeplug/modes.js");

const t = (k, p) => (p ? `${k} ${JSON.stringify(p)}` : k);

// The open dialog: its inputs by field key, the choices of a select, picking one as the user does (a change event on
// the form's body), and Save
function openForm(fields, record, onSave) {
	formDialog("Channel", fields, record, t, onSave);
	const dlg = document.body.children.at(-1);
	const body = dlg.find((e) => e.tagName === "DIV" && String(e.className).startsWith("form-grid"));
	const input = (key) => {
		const el = dlg.find((e) => e.id === `f-${key}`);
		assert.ok(el, `field ${key} in the form`);
		return el;
	};
	const choices = (key) => input(key).children.map((o) => o.textContent);
	const pick = (key, label) => {
		const el = input(key);
		const i = choices(key).indexOf(label);
		assert.ok(i >= 0, `${key} offers ${label} (offers ${choices(key).join(", ")})`);
		el.selectedIndex = i;
		body.dispatch("change");
	};
	const save = () => dlg.find((e) => e.tagName === "BUTTON" && e.textContent === t("editor.save_record")).click();
	return { dlg, input, choices, pick, save };
}

test("channel form: a new DMR channel made DMR AES and given a system in the same form lists the system's keys (#1042)", () => {
	const m = empty();
	const sys = newSystem(m, 5, "Ops");
	sys.trafficKeys = [{ id: 7, key: "AB".repeat(32) }, { id: 3, key: "CD".repeat(32) }];
	m.systems.push(sys);
	const c = newChannel({ name: "Secure", mode: "dmr", rxHz: 461462500, txHz: 461462500 });
	m.channels.push(c);
	numberAll(m);

	let saved = null;
	const form = openForm(channelForm(m, t, { features: "FM,DMR,DMR_AES" }), c, (work) => (saved = work));
	form.pick("mode", kindName(5));
	assert.deepEqual(form.choices("k5.id"), ["editor.none"], "no system yet: no key");
	form.pick("k5.system", "Ops");
	assert.deepEqual(form.choices("k5.id"), ["editor.none", "3", "7"], "the system's keys, without saving first");
	form.pick("k5.id", "7");
	form.save();
	assert.ok(saved, "the form saved");

	// as app.js stores a saved channel form
	Object.assign(c, saved);
	syncModes(m);
	const back = decode(encode(m)).channels.find((x) => x.name === "Secure");
	assert.equal(back.kind, 5);
	assert.equal(back.system?.name, "Ops");
	assert.equal(back.params.id, 7);
});

test("channel form: the AES key list follows a change of system and keeps a key both systems have", () => {
	const m = empty();
	const a = newSystem(m, 5, "North"), b = newSystem(m, 5, "South");
	a.trafficKeys = [{ id: 1, key: "11".repeat(32) }, { id: 2, key: "22".repeat(32) }];
	b.trafficKeys = [{ id: 2, key: "33".repeat(32) }];
	m.systems.push(a, b);
	const c = newChannel({ name: "Ch", mode: "dmr" });
	m.channels.push(c);
	numberAll(m);

	let saved = null;
	const form = openForm(channelForm(m, t, { features: "FM,DMR,DMR_AES" }), c, (work) => (saved = work));
	form.pick("mode", kindName(5));
	form.pick("k5.system", "North");
	form.pick("k5.id", "2");
	form.pick("k5.system", "South");
	assert.deepEqual(form.choices("k5.id"), ["editor.none", "2"]);
	assert.equal(form.input("k5.id").selectedIndex, 1, "key 2 stays picked");
	form.save();
	assert.equal(saved.system, b);
	assert.equal(saved.params.id, 2);
});

// ---- The system form (modesview.js), opened as the Systems tab's Edit button opens it

const { systemsTab } = await import("../static/js/editor/modesview.js");
const { h, button } = await import("../static/js/flasher/dom.js");

// every element below el for which pick() holds
function all(el, pick, out = []) {
	for (const c of el.children) {
		if (!(c instanceof El)) continue;
		if (pick(c)) out.push(c);
		all(c, pick, out);
	}
	return out;
}

function openSystem(m, s) {
	let edit = null;
	const api = { model: m, t, h, change: (name, fn) => fn(), confirmDialog() {},
		button: (text, fn, opts) => {
			if (text === t("editor.modes.edit")) edit = fn;
			return button(text, fn, opts);
		} };
	systemsTab(api, null);
	assert.ok(edit, "the system's Edit button");
	edit();
	const dlg = document.body.children.at(-1);
	const body = dlg.find((e) => e.tagName === "DIV" && String(e.className).startsWith("form-grid"));
	const input = (key) => dlg.find((e) => e.id === `f-${key}`);
	const type = (key, text) => {
		input(key).value = text;
		body.dispatch("input");
	};
	const row = (key) => dlg.find((e) => String(e.className).startsWith("form-row") && e.find((x) => x.id === `f-${key}`));
	// the column headings of every entry table, and the cell of a row by its column's label
	const heads = () => all(dlg, (e) => e.tagName === "TH").map((e) => e.textContent);
	const cellOf = (name) => dlg.find((e) => e.tagName === "INPUT" && e.getAttribute("aria-label") === name);
	const typeCell = (name, text) => {
		const el = cellOf(name);
		el.value = text;
		el.dispatch("input");
		body.dispatch("input");
	};
	const note = () => all(dlg, (e) => String(e.className).startsWith("form-note") && !e.hidden).map((e) => e.textContent);
	const save = () => dlg.find((e) => e.tagName === "BUTTON" && e.textContent === t("editor.save_record")).click();
	return { dlg, input, type, row, heads, typeCell, note, save, choices: (key) => input(key).children.map((o) => o.textContent) };
}

function trunked() {
	const m = empty();
	const s = newSystem(m, 3, "County");
	Object.assign(s, { tx: true, ownId: 1234567 });
	s.fields.wacn = 0x12345;
	s.entries.push({ cls: 0, group: 0, control: true, flags: 0, iden: 0, chanNo: 0, chan: 0, rxHz: 851012500, txHz: 806012500 });
	s.entries.push({ cls: 2, group: 1, control: false, flags: 0, baseHz: 851006250, spacingHz: 6250, txOffsetHz: -45000000, bwHz: 12500, tdma: false, slots: 2 });
	m.systems.push(s);
	numberAll(m);
	return { m, s };
}

test("system form: a blocked network ID, as it is typed, locks Transmit and hides what only a transmitter reads (#1062)", () => {
	const { m, s } = trunked();
	const form = openSystem(m, s);
	const open = ["editor.modes.tx0", "editor.modes.tx1", "editor.modes.tx2"];
	assert.deepEqual(form.choices("transmit"), open);
	assert.equal(form.row("ownId").hidden, false);
	assert.ok(form.heads().includes("editor.col.tx") && form.heads().includes("editor.p25.tx_offset"));
	assert.ok(!form.note().some((x) => x.includes("editor.p25.no_tx_")));

	form.type("sys.wacn", "bee5a");
	assert.deepEqual(form.choices("transmit"), ["editor.modes.tx0"], "one choice, listen only");
	assert.equal(form.input("transmit").disabled, true);
	assert.equal(form.row("ownId").hidden, true);
	assert.ok(form.note().includes('editor.p25.no_tx_wacn {"id":"BEE5A"}'), "one notice, naming the WACN as typed");
	assert.ok(!form.note().some((x) => x.includes("editor.p25.no_tx_sysid")));
	assert.ok(form.dlg.find((e) => e.className === "form-note notice bad" && !e.hidden), "drawn as a callout");
	// right under the identity fields that trigger it: after the NAC, before the control channel table and Transmit
	const order = all(form.dlg, (e) => e.id === "f-sys.ccNac" || e.className === "form-note notice bad" || e.tagName === "TABLE" || e.id === "f-transmit")
		.map((e) => (e.tagName === "TABLE" ? "table" : e.id || "notice"));
	assert.deepEqual(order.slice(0, 3), ["f-sys.ccNac", "notice", "table"], "the notice sits between the identity and the control channels");
	assert.ok(order.indexOf("f-transmit") > order.indexOf("notice"));
	assert.ok(!form.heads().includes("editor.col.tx") && !form.heads().includes("editor.p25.tx_offset"), "the TX columns are left out");
	assert.ok(form.heads().includes("editor.col.rx"), "receiving is all still there");

	// the identity changed back: what was chosen and entered returns
	form.type("sys.wacn", "12345");
	assert.deepEqual(form.choices("transmit"), open);
	assert.equal(form.input("transmit").selectedIndex, 1, "transmit, after registering, as before");
	assert.equal(form.input("transmit").disabled, false);
	assert.equal(form.row("ownId").hidden, false);
	assert.equal(form.input("ownId").value, "1234567");
	assert.ok(form.heads().includes("editor.col.tx") && form.heads().includes("editor.p25.tx_offset"));

	// the system ID alone blocks too
	form.type("sys.sysid", "00a");
	assert.deepEqual(form.choices("transmit"), ["editor.modes.tx0"]);
	assert.ok(form.note().includes('editor.p25.no_tx_sysid {"id":"00A"}'), "the notice names the system ID");

	// both match: the WACN is named
	form.type("sys.wacn", "beeff");
	assert.ok(form.note().includes('editor.p25.no_tx_wacn {"id":"BEEFF"}'));
});

test("system form: a blocked system saves as listen only, its transmit values kept; a TX cell left out can't refuse the save", () => {
	const { m, s } = trunked();
	const form = openSystem(m, s);
	form.typeCell("editor.col.tx", "not a frequency");
	form.save();
	assert.equal(form.dlg.open, true, "shown, the bad TX frequency refuses the save");

	form.type("sys.wacn", "bee5a");
	form.save();
	assert.equal(form.dlg.open, false, "saved");
	assert.equal(s.fields.wacn, 0xBEE5A);
	const back = decode(encode(m)).systems[0];
	assert.equal(back.tx, false, "written listen only");
	assert.equal(back.ownId, 1234567);
	assert.equal(back.entries.find((e) => e.cls === 0).txHz, 806012500, "the stored TX frequency, not the text that didn't parse");
	assert.equal(back.entries.find((e) => e.cls === 2).txOffsetHz, -45000000);
});
