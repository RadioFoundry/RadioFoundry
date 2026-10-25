// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Undo and redo for the codeplug editor. Before each change the page takes a snapshot: the order of every table and
// a shallow copy of every record. Records hold plain values and references to other records (never shared arrays),
// so restoring the copies into the same objects brings back the codeplug exactly, references included. The image
// under the model (base, which holds the DMR ID database) is kept by reference: a change to it puts a new image in
// place, never edits the old one.

const TABLES = ["channels", "zones", "contacts", "rxLists", "dtmfContacts", "aprs"];
const LIMIT = 100;

function snapshot(m) {
	const objects = new Map();
	const tables = {};
	for (const t of TABLES) {
		tables[t] = m[t].slice();
		for (const r of m[t]) objects.set(r, copy(r));
	}
	for (const v of m.vfo) if (v) objects.set(v, copy(v));
	return { tables, objects, general: { ...m.general }, base: m.base, iddb: m.iddb };
}

function copy(r) {
	const c = { ...r };
	for (const k of Object.keys(c)) if (Array.isArray(c[k])) c[k] = c[k].slice();
	return c;
}

function restore(m, s) {
	for (const t of TABLES) m[t] = s.tables[t].slice();
	for (const [obj, c] of s.objects) {
		for (const k of Object.keys(obj)) if (!(k in c)) delete obj[k];
		Object.assign(obj, copy(c));
	}
	m.general = { ...s.general };
	m.base = s.base;
	m.iddb = s.iddb;
}

export class History {
	constructor() {
		this.past = [];
		this.future = [];
	}

	reset() {
		this.past = [];
		this.future = [];
	}

	// Call before changing m
	record(m, label = "") {
		this.past.push({ s: snapshot(m), label });
		if (this.past.length > LIMIT) this.past.shift();
		this.future = [];
	}

	// Drop the last record() when the change it guarded didn't happen
	cancel() {
		this.past.pop();
	}

	get canUndo() {
		return this.past.length > 0;
	}

	get canRedo() {
		return this.future.length > 0;
	}

	undo(m) {
		const e = this.past.pop();
		if (!e) return null;
		this.future.push({ s: snapshot(m), label: e.label });
		restore(m, e.s);
		return e.label;
	}

	redo(m) {
		const e = this.future.pop();
		if (!e) return null;
		this.past.push({ s: snapshot(m), label: e.label });
		restore(m, e.s);
		return e.label;
	}
}
