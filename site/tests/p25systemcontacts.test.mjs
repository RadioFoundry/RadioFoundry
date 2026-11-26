// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// System-qualified P25 contacts use the existing contact/System owners (docs/spec/air-contacts.md), including
// catalogs made before personalities, independent wire vectors, references and portable file round trips.

import { test } from "node:test";
import assert from "node:assert/strict";
import { empty, encode, decode, newContact, newChannel, numberAll, remove, validate, contactFromFilters } from "../static/js/codeplug/model.js";
import { newSystem, setKind, systemIdentityCheck, MODE_KEY } from "../static/js/codeplug/channelmodes.js";
import { decodeAirContact, encodeAirContact, AIR_CONTACTS, AIR_CONTACT_SIZE, LIMITS } from "../static/js/codeplug/records.js";
import { contactScope } from "../static/js/codeplug/contactmode.js";
import { decodeArea, getValue, setValue, putArea } from "../static/js/codeplug/kv.js";
import { exportTable, importTable } from "../static/js/codeplug/csv.js";
import { exportQdmr, importQdmr } from "../static/js/codeplug/qdmr.js";
import { columns, channelForm } from "../static/js/editor/views.js";
import { Grid } from "../static/js/editor/grid.js";
import { convertP25Channels } from "../static/js/codeplug/p25convert.js";
import { parse, stringify } from "../static/js/codeplug/yaml.js";

const t = (key) => key;
const bytes = (hex) => Uint8Array.from(hex.match(/../g) ?? [], (s) => parseInt(s, 16));
const hex = (b) => [...b].map((v) => v.toString(16).padStart(2, "0")).join("").toUpperCase();
const slot = (image, n) => image.bytes.subarray(AIR_CONTACTS + (n - 1) * AIR_CONTACT_SIZE, AIR_CONTACTS + n * AIR_CONTACT_SIZE);
const error = (m, code) => validate(m).some((p) => p.level === "error" && p.code === code);

function catalog() {
	const m = empty(), a = newSystem(m, 3, "North System"), b = newSystem(m, 3, "South System");
	Object.assign(a.fields, { wacn: 0x12345, sysid: 0x321 });
	Object.assign(b.fields, { wacn: 0x23456, sysid: 0x432 });
	m.systems.push(a, b);
	const x = newContact({ mode: "p25", name: "North", id: 100, system: a });
	const y = newContact({ mode: "p25", name: "South", id: 100, system: b });
	m.contacts.push(x, y);
	numberAll(m);
	return { m, a, b, x, y };
}

function personalities(f) {
	const { m, a, b, x, y } = f;
	const ca = newChannel({ name: "North Ops" }), cb = newChannel({ name: "South Ops" });
	m.channels.push(ca, cb);
	for (const [c, system, contact] of [[ca, a, x], [cb, b, y]]) {
		setKind(m, c, 3);
		Object.assign(c, { system, contact });
		const list = { number: null, name: c.name + " RX", contacts: [contact] };
		m.rxLists.push(list);
		c.rxList = list;
	}
	m.zones.push({ name: "Patrol", channels: [cb, ca, cb] });
	numberAll(m);
	return { ...f, ca, cb };
}

test("System scope: independent literal guard and 24-byte record vectors", () => {
	const { m, a, b } = catalog();
	// Independent FNV-1a preimages/answers, also literal C fixtures: 03 05, WACN LE3, System ID LE2.
	for (const [wacn, sysid, check] of [[0x12345, 0x321, 0x34CC], [0x23456, 0x432, 0x927E], [0, 0, 0xD8D2], [0xFFFFF, 0xFFF, 0xA639]]) {
		Object.assign(a.fields, { wacn, sysid });
		assert.equal(systemIdentityCheck(a), check);
	}
	Object.assign(a.fields, { wacn: 0x12345, sysid: 0x321 });
	assert.equal(systemIdentityCheck(b), 0x927E);
	const vector = bytes("020564000000CC344E6F727468FFFFFFFFFFFFFFFFFFFFFF");
	assert.equal(vector.length, 24);
	assert.deepEqual(decodeAirContact(vector), { name: "North", type: "group", id: 100, system: 1, systemCheck: 0x34CC });
	const out = new Uint8Array(24).fill(0xFF);
	encodeAirContact({ name: "North", type: "group", id: 100, system: 1, systemCheck: 0x34CC }, out);
	assert.deepEqual(out, vector);
	assert.deepEqual(slot(encode(m), 1), vector, "catalog exists and serializes before any personalities");
	assert.equal(m.channels.length, 0);
	assert.equal(error(m, "p25_duplicate"), false);
});

test("System scope: incomplete, out-of-range, short or noncanonical identities do not qualify", () => {
	const { m, a } = catalog();
	for (const fields of [{ wacn: null }, { sysid: null }, { wacn: 0x100000 }, { sysid: 0x1000 }, { wacn: -1 }, { sysid: 1.5 }]) {
		const saved = { ...a.fields };
		Object.assign(a.fields, fields);
		assert.equal(systemIdentityCheck(a), null);
		assert.ok(error(m, "p25_system_unresolved"));
		a.fields = saved;
	}
	for (const mutate of [(r) => (r[30] |= 0x10), (r) => (r[32] |= 0x10), (r) => r.slice(0, 31)]) {
		const img = encode(m), entries = decodeArea(img.bytes).entries;
		const raw = getValue(entries, MODE_KEY.system).slice();
		const replacement = mutate(raw);
		putArea(img.bytes, setValue(entries, MODE_KEY.system, replacement instanceof Uint8Array ? replacement : raw));
		const back = decode(img);
		assert.equal(systemIdentityCheck(back.systems[0]), null);
		assert.equal(typeof back.contacts[0].system, "number");
		assert.deepEqual(encode(back).bytes, img.bytes, "invalid scope remains explicit and raw bytes survive");
	}
});

test("System scope: CPS checks the existing serializer's proposed System, preserving its header canonicalization", () => {
	const { m } = catalog(), img = encode(m);
	// Literal complete System with malformed identity-length header4. The firmware reader rejects that raw header;
	// the established System writer canonicalizes it to5 when producing the proposed saved profile.
	const raw = bytes("0300010400000000000000004E6F7274682053797374656DFFFFFFFF45230121030000000000");
	assert.equal(raw.length, 38);
	putArea(img.bytes, setValue(decodeArea(img.bytes).entries, MODE_KEY.system, raw));
	const proposed = decode(img);
	assert.equal(proposed.systems[0].raw[3], 4);
	assert.equal(systemIdentityCheck(proposed.systems[0]), 0x34CC);
	assert.equal(proposed.contacts[0].system, proposed.systems[0], "the reference is to the proposed saved owner");
	const saved = encode(proposed), header = getValue(decodeArea(saved.bytes).entries, MODE_KEY.system);
	const expected = raw.slice(); expected[3] = 5;
	assert.deepEqual(header, expected, "only the existing owner's known identity-length header is normalized");
	assert.deepEqual(slot(saved, 1), slot(img, 1));
	assert.equal(systemIdentityCheck(decode(saved).systems[0]), 0x34CC);
});

test("System scope: binary round trip, rename, identity edit and reordering keep actual references", () => {
	const { m, a, b } = personalities(catalog());
	a.name = "Renamed";
	assert.equal(systemIdentityCheck(a), 0x34CC, "rename is independent of identity");
	[a.number, b.number] = [b.number, a.number];
	m.systems.reverse();
	Object.assign(a.fields, { wacn: 0x34567 });
	const back = decode(encode(m)), north = back.contacts.find((c) => c.name === "North");
	assert.equal(north.system.name, "Renamed");
	assert.equal(north.system.fields.wacn, 0x34567);
	assert.equal(north.system.number, 2);
	assert.equal(back.channels[0].contact, north);
	assert.equal(back.channels[0].system, north.system);
	assert.equal(back.channels[0].rxList.contacts[0], north);
	assert.deepEqual(back.zones[0].channels.map((c) => c.name), ["South Ops", "North Ops", "South Ops"]);
	assert.ok(!error(back, "p25_contact_wrong_system"));
	assert.deepEqual(encode(back).bytes, encode(m).bytes);
});

test("System scope: deletion tombstones contacts even when a new System reuses the same slot and identity", () => {
	const { m, a, x } = catalog();
	remove(m, "systems", [a]);
	assert.equal(x.system, 256);
	const replacement = newSystem(m, 3, "Replacement");
	Object.assign(replacement.fields, { wacn: 0x12345, sysid: 0x321 });
	m.systems.push(replacement);
	numberAll(m);
	assert.equal(replacement.number, 1);
	const img = encode(m), back = decode(img);
	assert.equal(slot(img, 1)[5], 0xFF);
	assert.equal(back.contacts[0].system, 256);
	assert.ok(error(back, "p25_system_unresolved"));
	assert.deepEqual(contactScope(back.contacts[0]), { system: 256, systemCheck: 0x34CC });
});

test("System scope: a mismatched check stays scoped; unknown record bits survive editing", () => {
	const { m } = catalog(), img = encode(m);
	slot(img, 1)[1] |= 0xA8;
	slot(img, 1)[6] ^= 1;
	const back = decode(img);
	assert.equal(back.contacts[0].system, 1);
	assert.ok(error(back, "p25_system_unresolved"));
	assert.deepEqual(encode(back).bytes, img.bytes);
	back.contacts[0].name = "Renamed group";
	const changed = encode(back);
	assert.deepEqual(slot(changed, 1).subarray(0, 8), slot(img, 1).subarray(0, 8));
	assert.deepEqual(slot(changed, 2), slot(img, 2));
});

test("System scope: wrong-System targets and RX members fail validation; firmware capability is explicit", () => {
	const { m, ca, cb, x, y } = personalities(catalog());
	assert.ok(!validate(m).some((p) => p.code === "p25_duplicate"), "the same group in distinct Systems is not a duplicate");
	ca.contact = y;
	assert.ok(error(m, "p25_contact_wrong_system"));
	ca.contact = x;
	ca.rxList.contacts.push(y);
	assert.ok(error(m, "p25_contact_wrong_system"));
	ca.rxList.contacts.pop();
	cb.contact = x;
	assert.ok(error(m, "p25_contact_wrong_system"));
	assert.ok(validate(m, { features: "P25_TRUNK,p25contacts" }).some((p) => p.code === "p25_system_contacts_not_in_radio"));
	assert.ok(!validate(m, { features: "P25_TRUNK,p25contacts,P25_SYSTEM_CONTACTS" }).some((p) => p.code === "p25_system_contacts_not_in_radio"));
	assert.ok(!validate(m).some((p) => p.code === "p25_system_contacts_not_in_radio"));
});

test("System scope: existing Contacts filter creates catalog members before channels and excludes other Systems", () => {
	const { m, a, x, y } = catalog();
	const filters = { mode: new Set(["p25"]), type: new Set(["group"]), system: new Set([a.number]) };
	const g = Object.assign(Object.create(Grid.prototype), { columns: columns("contacts", m, t), rows: m.contacts, filter: "", sort: null, filters, t });
	assert.deepEqual(g.shown(), [x]);
	const d = g.newRowDefaults();
	const fresh = contactFromFilters({ name: "New TG", id: 101, system: m.systems.find((s) => s.number === d.system) }, d);
	assert.equal(fresh.system, a);
	assert.equal(fresh.mode, "p25");
	const c = newChannel({ kind: 3, system: a, contact: x });
	const choices = channelForm(m, t).find((f) => f.key === "contact").options(c).map(([ct]) => ct);
	assert.ok(choices.includes(x));
	assert.ok(!choices.includes(y));
});

test("System scope: CSV preserves qualified contacts and dangling checks through the existing System owner", () => {
	const { m, a } = catalog();
	const out = exportTable(m, "contacts");
	const target = empty();
	for (const s of m.systems) { const copy = newSystem(target, 3, s.name); copy.fields = { ...s.fields }; target.systems.push(copy); }
	numberAll(target);
	assert.deepEqual(importTable(target, out).problems, []);
	assert.deepEqual(target.contacts.map((c) => [c.name, c.id, c.system.name]), [["North", 100, "North System"], ["South", 100, "South System"]]);
	remove(m, "systems", [a]);
	assert.deepEqual(importTable(target, exportTable(m, "contacts")).problems, []);
	assert.equal(target.contacts.find((c) => c.name === "North").system, 256);
});

test("System scope: YAML carries catalogs, Systems, personality/RX references and ordered zones portably", () => {
	const { m } = personalities(catalog());
	const file = exportQdmr(m);
	assert.match(file.text, /airContacts:/);
	assert.match(file.text, /system: sys1/);
	assert.match(file.text, /p25Contact: p25c1/);
	const back = empty();
	importQdmr(back, file.text);
	assert.deepEqual(back.contacts.map((c) => [c.name, c.id, c.system.name]), [["North", 100, "North System"], ["South", 100, "South System"]]);
	const image = decode(encode(back));
	for (const c of image.channels) {
		assert.equal(c.system, c.contact.system);
		assert.equal(c.rxList.contacts[0], c.contact);
	}
	assert.deepEqual(image.zones[0].channels.map((c) => c.name), ["South Ops", "North Ops", "South Ops"]);
	const { m: onlyCatalog } = catalog(), blank = empty();
	importQdmr(blank, exportQdmr(onlyCatalog).text);
	assert.equal(blank.channels.length, 0);
	assert.equal(blank.contacts.length, 2);
});

test("System scope: legacy YAML preserves contacts, replacement Systems rebind by identity, tombstones stay unresolved", () => {
	const { m, a } = catalog();
	const ordinary = empty();
	importQdmr(m, exportQdmr(ordinary).text);
	assert.equal(m.contacts.length, 2, "a plain qdmr file keeps P25 contacts and their existing System owners");
	assert.equal(m.contacts[0].system, a);
	const old = parse(exportQdmr(m).text);
	delete old.radioFoundry.airContacts;
	old.radioFoundry.systems.reverse();
	importQdmr(m, stringify(old));
	assert.equal(m.contacts[0].system.name, "North System");
	assert.equal(m.contacts[0].system.number, 2, "an older block can reorder Systems without moving a catalog to another one");
	remove(m, "systems", [m.contacts[0].system]);
	const back = empty(); importQdmr(back, exportQdmr(m).text);
	assert.equal(back.contacts[0].system, 256);
	assert.ok(error(back, "p25_system_unresolved"));
});

test("System scope: malformed text scopes fail closed instead of becoming unscoped contacts", () => {
	const { m } = catalog();
	const badCsv = importTable(m, 'Number,Name,Mode,System,ID,Call Type\r\n1,Bad,P25,Missing,100,Group\r\n');
	assert.equal(badCsv.problems[0].code, "unknown_ref");
	assert.equal(m.contacts[0].system, 256);
	const { m: valid } = catalog(), doc = parse(exportQdmr(valid).text);
	const first = doc.radioFoundry.airContacts[0];
	delete first.system;
	first.systemIndex = 0;
	first.systemCheck = "not hex";
	const back = empty(); importQdmr(back, stringify(doc));
	assert.equal(back.contacts[0].system, 256);
	assert.ok(error(back, "p25_system_unresolved"));
});

test("System scope: an explicit missing YAML P25 target retains its mode and fails validation, never legacy TG1", () => {
	const { m, a, x } = catalog();
	const c = newChannel({ name: "Explicit target" });
	m.channels.push(c); setKind(m, c, 3); Object.assign(c, { system: a, contact: x });
	// Export before binary encoding: the derived contact flag is still false in the editing model.
	assert.equal(c.params.p25Contacts, false);
	const doc = parse(exportQdmr(m).text);
	doc.radioFoundry.airContacts = [];
	const back = empty(); importQdmr(back, stringify(doc));
	assert.equal(back.channels[0].contact, 0xFFFF);
	assert.equal(back.channels[0].params.p25Contacts, true);
	assert.ok(error(back, "dangling"));
	assert.equal(decode(encode(back)).channels[0].params.p25Contacts, true);
	const again = empty(); importQdmr(again, exportQdmr(back).text);
	assert.equal(again.channels[0].contact, 0xFFFF);
	assert.equal(again.channels[0].params.p25Contacts, true);
	assert.ok(error(again, "dangling"));
});

test("System scope: missing portable P25 RX members retain explicit intent and errors through repeated exports", () => {
	const { m, a, x } = catalog();
	const c = newChannel({ name: "RX fallback" });
	m.channels.push(c); setKind(m, c, 3); c.system = a;
	const list = { number: 1, name: "Explicit P25 RX", contacts: [x] };
	m.rxLists.push(list); c.rxList = list;
	assert.equal(c.contact, null);
	assert.equal(c.params.p25Contacts, false);
	const doc = parse(exportQdmr(m).text);
	doc.radioFoundry.airContacts = [];
	let back = empty(); importQdmr(back, stringify(doc));
	for (let n = 0; n < 2; n++) {
		assert.equal(back.channels[0].params.p25Contacts, true);
		assert.deepEqual(back.channels[0].rxList.contacts, [LIMITS.contacts]);
		assert.ok(validate(back).some((p) => p.table === "rxLists" && p.level === "error" && p.code === "dangling"));
		const image = decode(encode(back));
		assert.equal(image.channels[0].params.p25Contacts, true);
		assert.deepEqual(image.channels[0].rxList.contacts, [LIMITS.contacts]);
		const file = exportQdmr(back);
		assert.match(file.text, /#p25:1024/);
		back = empty(); importQdmr(back, file.text);
	}
});

test("System scope: legacy conversion never adopts another System's catalog entry", () => {
	const { m, x } = catalog();
	const d = newContact({ name: "Legacy", id: 100 });
	m.contacts.push(d);
	const c = newChannel({ name: "Conventional" }); m.channels.push(c); setKind(m, c, 2); c.contact = d;
	convertP25Channels(m);
	assert.notEqual(c.contact, x);
	assert.equal(c.contact.system, undefined);
});
