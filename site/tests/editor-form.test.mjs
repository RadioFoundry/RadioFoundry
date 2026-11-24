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
