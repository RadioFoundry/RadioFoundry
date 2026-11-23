// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
//
// A searchable list over a text input, for fields with many usual values (the tones): Tom Select
// (site/static/vendor/tom-select, loaded by the editor page). Typing filters the list, the arrow keys move, Enter or a
// click picks, and a value missing from the list is still taken when spec.accept() allows it. Without the library (the
// host tests, a blocked script) the input stays a text box with the values as a datalist.
import { h } from "../flasher/dom.js";

let listN = 0;

// spec: { values: [text], accept(text) -> normalized text or null, none: label of the empty value, use: (v) => label
// of the "use this value" row, noMatch: text shown when nothing matches and the text isn't a valid value }
// opts.floating: the list is placed in <body> (a grid cell clips anything inside it)
export function combo(input, spec, opts = {}) {
	const TS = globalThis.TomSelect;
	if (!TS || !input.parentNode) {
		const id = `combo-${++listN}`;
		input.setAttribute("list", id);
		input.autocomplete = "off";
		const list = h("datalist", { id }, ...spec.values.map((v) => h("option", { value: v })));
		if (input.parentNode) input.after(list);
		return { list, ts: null };
	}
	const current = spec.accept(input.value) ?? "";
	const values = place(spec.values, current);
	// the "Use ..." row only for a value the list doesn't already show for this text ("none" shows None itself)
	const shown = (v, text) => (v === "" || values.includes(v)) && (v || spec.none).toLowerCase().includes(text.trim().toLowerCase());
	const esc = (s) => String(s).replace(/[&<>"']/g, (c) => `&#${c.charCodeAt(0)};`);
	const ts = new TS(input, {
		options: [{ value: "", text: spec.none }, ...values.map((v) => ({ value: v, text: v }))],
		items: opts.typed != null ? [] : [current],
		maxItems: 1,
		maxOptions: null,
		allowEmptyOption: true,
		persist: false,
		selectOnTab: true,
		// filter on every key: the default waits 300 ms, so "88" and a quick Enter took the row highlighted before
		refreshThrottle: 0,
		// (sortField left at Tom Select's own: the list's order, best match first once something is typed. Naming
		// $score here without a direction would sort it worst first)
		create: (text) => {
			const v = spec.accept(text);
			return v == null ? false : { value: v, text: v === "" ? spec.none : v };
		},
		createFilter: (text) => {
			const v = spec.accept(text);
			return v != null && !shown(v, text);
		},
		dropdownParent: opts.floating ? "body" : null,
		render: {
			option_create: (d) => `<div class="create">${esc(spec.use(spec.accept(d.input) ?? d.input))}</div>`,
			no_results: () => `<div class="no-results">${esc(spec.noMatch)}</div>`,
		},
	});
	// text typed replaces the value shown, as in a plain text box, instead of sitting beside it
	const typing = () => ts.wrapper.classList.toggle("typing", ts.inputValue() !== "");
	ts.on("type", typing);
	ts.hook("after", "setTextboxValue", typing);
	// leaving the field takes a valid value typed there, as a plain text box would; Tom Select alone keeps only a
	// picked row and drops the text. Text that isn't a valid value leaves the field as it was. Tom Select clears the
	// text before "blur", so it is kept in ts.textOnBlur: the grid and the form's Save read it there. (Tom Select can
	// call onBlur a second time with the text already gone, so only focus clears it)
	ts.textOnBlur = "";
	ts.on("focus", () => { ts.textOnBlur = ""; });
	ts.hook("before", "onBlur", () => {
		const text = ts.inputValue().trim();
		if (text) ts.textOnBlur = text;
		if (!document.hasFocus() || !ts.isFocused || !text) return;
		const v = spec.accept(text);
		if (v == null || v === ts.getValue()) return;
		if (!ts.options[v]) ts.addOption({ value: v, text: v === "" ? spec.none : v });
		ts.addItem(v);
	});
	if (opts.typed != null) {
		ts.setTextboxValue(opts.typed);
		ts.refreshOptions(true);
	}
	return { list: null, ts };
}

// The list with the current value in it: a value off the list (a tone typed earlier) goes where it sorts, so the
// arrow keys still walk the list in order
function place(values, current) {
	if (current === "" || values.includes(current)) return values;
	const n = Number(current);
	const at = Number.isFinite(n) ? values.findIndex((v) => !Number.isFinite(Number(v)) || Number(v) > n) : -1;
	return at < 0 ? [...values, current] : [...values.slice(0, at), current, ...values.slice(at)];
}

// The value a combo would take now: an active row of the open list, else the text typed (when valid), else the
// selected item. null: typed text that isn't a valid value
export function comboValue(ts, spec) {
	if (ts.isOpen && ts.activeOption) {
		if (ts.activeOption.classList.contains("create")) return spec.accept(ts.inputValue());
		return ts.activeOption.dataset.value ?? "";
	}
	const typed = ts.inputValue().trim();
	if (typed) return spec.accept(typed);
	return ts.getValue();
}

// The text a field gives its parser on Save: the text typed or left there when the field lost focus (Tom Select
// drops text that isn't a valid value), else the selected value. Invalid text then gets its error instead of
// leaving the old value without a word
export function comboText(input) {
	const ts = input.tomselect;
	if (!ts) return input.value;
	return ts.inputValue().trim() || ts.textOnBlur || input.value;
}
