// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The channel form (static/js/editor/views.js formDialog) driven as a user does, against a small stand-in for the DOM:
// a stored FM or DMR channel made D-STAR or Fusion, the mode picked before or after the frequency, then Save, and the
// image written and read back. Run with site/tests/run.sh.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

// ---- just enough DOM for dom.js h() and formDialog: inputs keep value, checked and selectedIndex as properties
class El {
	constructor(tag) {
		Object.assign(this, { tagName: tag.toUpperCase(), children: [], attrs: {}, listeners: {}, className: "", dataset: {}, style: {} });
		Object.assign(this, { id: "", value: "", checked: false, selectedIndex: 0, disabled: false, hidden: false });
	}
	append(...cs) { for (const c of cs) this.children.push(c instanceof El ? c : { text: String(c) }); }
	showModal() {}
	close() { for (const f of this.listeners.close || []) f({}); }
	remove() { document.body.children = document.body.children.filter((c) => c !== this); }
	replaceChildren(...cs) { this.children = []; this.append(...cs); }
	setAttribute(k, v) {
		this.attrs[k] = v;
		if (k === "id" || k === "value" || k === "hidden") this[k] = k === "hidden" ? true : v;
	}
	getAttribute(k) { return this.attrs[k] ?? null; }
	addEventListener(ev, f) { (this.listeners[ev] ||= []).push(f); }
	fire(ev) { for (const f of this.listeners[ev] || []) f({ target: this }); }
	click() { this.fire("click"); }
	get textContent() { return this.children.map((c) => (c instanceof El ? c.textContent : c.text)).join(""); }
	set textContent(v) { this.children = [{ text: String(v) }]; }
	*walk() {
		yield this;
		for (const c of this.children) if (c instanceof El) yield* c.walk();
	}
}
globalThis.Node = El;
const strings = readFileSync(fileURLToPath(new URL("../i18n/en.json", import.meta.url)), "utf8");
globalThis.document = { createElement: (tag) => new El(tag), createElementNS: (ns, tag) => new El(tag), body: new El("body"),
	getElementById: (id) => (id === "i18n" ? { textContent: strings } : null) };

const { channelForm, columns, formDialog } = await import("../static/js/editor/views.js");
const { decode, encode, empty, newChannel, numberAll, validate } = await import("../static/js/codeplug/model.js");
const { MODE_KEY, decodeTag, syncModes } = await import("../static/js/codeplug/channelmodes.js");
const { decodeArea, getValue } = await import("../static/js/codeplug/kv.js");

const t = (k) => k;
// what a DM-32 build with both modes reports (core/hostlink/build_info.c), the radio connected
const FEATURES = "FM,DMR,DSTAR,YSF";
const ascii = (b) => String.fromCharCode(...b);

// The channel's form opened as the editor does (app.js openRecord): Save copies the form's record back and brings
// the channel's stored record to its stand-in
function open(m, c) {
	formDialog("Channel", channelForm(m, t, { features: FEATURES }), c, t, (work) => {
		Object.assign(c, work);
		syncModes(m);
	});
	const dlg = document.body.children.at(-1);
	const body = dlg.children[1];
	const el = (k) => [...dlg.walk()].find((e) => e.id === "f-" + k);
	return {
		options: (k) => el(k)._opts.map(([v]) => v),
		pick(k, v) {
			const e = el(k);
			e.selectedIndex = e._opts.findIndex(([x]) => x === v);
			assert.ok(e.selectedIndex >= 0, `${k} offers ${v}`);
			body.fire("change");
		},
		type(k, v) {
			el(k).value = v;
			body.fire("input");
		},
		shows: (k) => el(k).value,
		save() {
			[...dlg.walk()].find((e) => e.tagName === "BUTTON" && e.className.includes("btn-primary")).click();
			assert.ok(!document.body.children.includes(dlg), "Save took the form");
		},
	};
}

// One stored channel of the mode, read back from an image as from the radio
function stored(mode) {
	const m = empty();
	m.channels.push(newChannel({ name: "Rpt", mode, rxHz: 439412500, txHz: 431812500, colorCode: 1, rxOnly: false }));
	numberAll(m);
	const d = decode(encode(m));
	return { m: d, c: d.channels[0] };
}

const KINDS = {
	dstar: { kind: 5, box: "k5.ur", defaults: { "k5.ur": "CQCQCQ", "k5.rpt1": "" }, params: { ur: "CQCQCQ", rpt1: "", rpt2: "", viaRepeater: false }, stored: [0, 8, "CQCQCQ  "] },
	fusion: { kind: 6, box: "k6.dest", defaults: { "k6.dest": "ALL", "k6.txDgid": "0", "k6.rxDgid": "0" }, params: { dest: "ALL", txDgid: 0, rxDgid: 0, narrow: false, voice: 0 }, stored: [0, 10, "ALL       "] },
};
const ORDERS = {
	"mode, then frequency": (f, k) => {
		f.pick("mode", `k${k}`);
		f.type("rxHz", "145.5");
		f.type("txHz", "145.5");
	},
	"frequency, then mode": (f, k) => {
		f.type("rxHz", "145.5");
		f.type("txHz", "145.5");
		f.pick("mode", `k${k}`);
	},
};

for (const [name, K] of Object.entries(KINDS)) {
	test(`channel form: a stored FM or DMR channel made ${name}, either order, saves the kind with its defaults and reads back`, () => {
		for (const from of ["fm", "dmr"]) for (const [order, steps] of Object.entries(ORDERS)) {
			const what = `${from}, ${order}`;
			const { m, c } = stored(from);
			const f = open(m, c);
			assert.ok(f.options("mode").includes(`k${K.kind}`), `${what}: offered for a radio that names it`);
			steps(f, K.kind);
			// the mode's own boxes show the new channel's values, not the blanks drawn when the form opened on FM/DMR
			for (const [box, v] of Object.entries(K.defaults)) assert.equal(f.shows(box), v, `${what}: ${box}`);
			f.save();
			assert.equal(c.kind, K.kind, what);
			assert.deepEqual(c.params, K.params, `${what}: the kind's defaults`);
			assert.deepEqual([c.mode, c.rxOnly, c.tagTx, c.rxHz, c.txHz], ["dmr", true, true, 145500000, 145500000], `${what}: the stand-in; TX allowed in the tag`);
			assert.deepEqual(validate(m, { features: FEATURES }).filter((p) => p.item === c).map((p) => p.code), [], `${what}: nothing to fix`);
			const img = encode(m);
			// the saved tag: the kind, transmit allowed, the name's check, and the parameter block as the radio reads it
			const tag = decodeTag(getValue(decodeArea(img.bytes).entries, MODE_KEY.tag + c.number - 1));
			assert.equal(tag?.kind, K.kind, `${what}: tag kind`);
			assert.equal(tag.flags & 1, 1, `${what}: transmit allowed`);
			const [at, n, text] = K.stored;
			assert.equal(ascii(tag.params.subarray(at, at + n)), text, `${what}: stored field`);
			const back = decode(img).channels[0];
			assert.deepEqual([back.kind, back.tagTx, back.rxHz, back.params], [K.kind, true, 145500000, K.params], `${what}: read back`);
			assert.deepEqual(encode(decode(img)).bytes, img.bytes, `${what}: unedited, the same bytes`);
		}
	});

	test(`channel form: typed ${name} fields are kept, and a stored ${name} channel shows its own`, () => {
		const { m, c } = stored("fm");
		const f = open(m, c);
		f.pick("mode", `k${K.kind}`);
		f.type(K.box, "N0CALL");
		f.save();
		const back = decode(encode(m)).channels[0];
		assert.equal(back.params[K.box.slice(3)], "N0CALL");
		// opened again: the box shows what is stored, and a change of frequency keeps the kind
		const g = open(m, c);
		assert.equal(g.shows(K.box), "N0CALL");
		g.type("rxHz", "145.6");
		g.save();
		const again = decode(encode(m)).channels[0];
		assert.deepEqual([again.kind, again.rxHz, again.params[K.box.slice(3)]], [K.kind, 145600000, "N0CALL"]);
	});
}

test("channel table: a stored channel made D-STAR or Fusion in its Mode cell, before or after its frequency", () => {
	for (const K of Object.values(KINDS)) for (const from of ["fm", "dmr"]) for (const modeFirst of [true, false]) {
		const { m, c } = stored(from);
		const cols = columns("channels", m, t, { features: FEATURES });
		const col = (k) => cols.find((x) => x.key === k);
		const steps = [() => col("mode").set(c, `k${K.kind}`), () => col("rxHz").set(c, "145.5")];
		for (const s of modeFirst ? steps : steps.reverse()) assert.equal(s(), undefined);
		const back = decode(encode(m)).channels[0];
		assert.deepEqual([back.kind, back.tagTx, back.rxHz, back.params], [K.kind, true, 145500000, K.params], `${K.kind} from ${from}`);
	}
});

test("the kinds offered follow the radio's feature list: none it doesn't name, unless the codeplug holds one", () => {
	const { m, c } = stored("fm");
	const mode = (features) => columns("channels", m, t, { features }).find((x) => x.key === "mode").options(c).map(([v]) => v);
	assert.deepEqual(mode("FM,DMR"), ["fm", "dmr"]);
	assert.deepEqual(mode(FEATURES), ["fm", "dmr", "k5", "k6"]);
	assert.deepEqual(mode(null), ["fm", "dmr", "k1", "k2", "k3", "k4", "k5", "k6"], "no radio: every kind");
});
