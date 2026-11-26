// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Contacts of both modes in the codeplug editor, in one list (docs/spec/air-contacts.md): a DMR contact is stored in the
// contact list, a P25 contact in the air mode contact table. The table's bytes and what is kept byte for byte (other
// modes' entries, invalid ones, reserved bits), free slots, a mode change that moves a contact, the timeslot override a
// P25 contact doesn't have, the links of channels and RX group lists to contacts, converting an older codeplug, the
// grid's column filters and what they give a new row, the firmware warning, validation and the CSV table.

import { test } from "node:test";
import assert from "node:assert/strict";

import { CodeplugImage, EDITOR_REGIONS, REGIONS, AIR_CONTACTS_AREA } from "../static/js/codeplug/image.js";
import { decode, encode, empty, validate, newContact, newChannel, numberTable, assignNumbers, contactFromFilters } from "../static/js/codeplug/model.js";
import { AIR_CONTACTS, AIR_CONTACT_SIZE, CONTACTS, CONTACT_SIZE, LIMITS, decodeAirContact, encodeAirContact, decodeContact } from "../static/js/codeplug/records.js";
import { isP25, isDmr, setContactMode, clearP25Timeslots, wantsP25Contacts, flaggedChannels } from "../static/js/codeplug/contactmode.js";
import { exportTable, exportAll, importTable, importAll, detect } from "../static/js/codeplug/csv.js";
import { p25Offered } from "../static/js/codeplug/modes.js";
import { setKind, newSystem } from "../static/js/codeplug/channelmodes.js";
import { decodeConv, encodeConv, p25TrunkKind } from "../static/js/codeplug/mode_p25.js";
import { p25ChannelsToConvert, convertP25Channels, unreadFlagged } from "../static/js/codeplug/p25convert.js";
import { exportQdmr } from "../static/js/codeplug/qdmr.js";
import { writeRanges } from "../static/js/codeplug/transport.js";
import { History } from "../static/js/editor/undo.js";
import { Grid } from "../static/js/editor/grid.js";
import { columns, channelForm } from "../static/js/editor/views.js";

const t = (k) => k;
const slot = (n) => AIR_CONTACTS + (n - 1) * AIR_CONTACT_SIZE;
const at = (img, n) => Array.from(img.bytes.subarray(slot(n), slot(n) + AIR_CONTACT_SIZE));
const hex = (a) => a.map((x) => x.toString(16).padStart(2, "0")).join(" ");
const p25 = (m) => m.contacts.filter(isP25);
const dmr = (m) => m.contacts.filter(isDmr);
const codes = (m, table = "contacts") => validate(m).filter((p) => p.table === table).map((p) => `${p.level}:${p.item?.name?.slice(0, 9) ?? ""}:${p.code}`);

// a slot's bytes: mode, flags, a 24-bit ID, three reserved bytes, a name padded with 0xFF
function entry(mode, flags, id, name, { reserved = [0xFF, 0xFF, 0xFF] } = {}) {
	const r = new Array(24).fill(0xFF);
	r[0] = mode;
	r[1] = flags;
	r[2] = id & 0xFF;
	r[3] = (id >> 8) & 0xFF;
	r[4] = (id >> 16) & 0xFF;
	r.splice(5, 3, ...reserved);
	for (let i = 0; i < name.length; i++) r[8 + i] = name.charCodeAt(i);
	return r;
}

function imageWith(entries) {
	const img = CodeplugImage.blank();
	for (const [n, r] of Object.entries(entries)) img.bytes.set(r, slot(Number(n)));
	return img;
}

const dmrContact = (name, id, fields = {}) => newContact({ name, id, mode: "dmr", ...fields });
const p25Contact = (name, id, fields = {}) => newContact({ name, id, mode: "p25", ...fields });

// a channel of a kind (2 conventional, 3 trunked) or a plain DMR one, in model m
function channel(m, name, kind = 0, fields = {}) {
	const c = newChannel({ name, mode: "dmr", ...fields });
	m.channels.push(c);
	if (kind) setKind(m, c, kind);
	// a trunked channel needs a system
	if (kind === 3) {
		let sys = m.systems.find((x) => x.type === 3);
		if (!sys) m.systems.push((sys = newSystem(m, 3, "Net")));
		c.system = sys;
	}
	return c;
}

test("the table is a region the editor reads and writes, outside qdmr's", () => {
	assert.deepEqual(AIR_CONTACTS_AREA, [0xC000, 0xE000]);
	assert.ok(EDITOR_REGIONS.some(([a, b]) => a === 0xC000 && b === 0xE000));
	assert.ok(!REGIONS.some(([a, b]) => a < 0xE000 && b > 0xC000));
	assert.ok(EDITOR_REGIONS.every(([a], i) => i === 0 || EDITOR_REGIONS[i - 1][1] <= a), "in address order");
	assert.ok(writeRanges().some(([a, b]) => a <= 0xC000 && b >= 0xE000));
	assert.equal(LIMITS.p25Contacts, 341);
	assert.ok(slot(341) + AIR_CONTACT_SIZE <= 0xE000 - 8);
});

test("a blank codeplug has no contacts and encodes unchanged", () => {
	const img = CodeplugImage.blank();
	const m = decode(img);
	assert.deepEqual(m.contacts, []);
	assert.equal(m.p25Contacts, undefined, "one list of contacts");
	assert.deepEqual(encode(m).bytes, img.bytes);
});

test("decode: group, unit and all call; the name stops at 0xFF or 0x00", () => {
	const img = imageWith({
		1: entry(2, 1, 1310, "Fire Dispatch"),
		2: entry(2, 0, 0xFFFFFE, "Chief"),
		3: entry(2, 2, 0x123456, "Everyone"),
		4: entry(2, 1, 65535, "Max"),
	});
	img.bytes[slot(4) + 8 + 3] = 0x00;
	const m = decode(img);
	assert.deepEqual(p25(m).map(({ number, name, type, id }) => [number, name, type, id]), [
		[1, "Fire Dispatch", "group", 1310],
		[2, "Chief", "unit", 0xFFFFFE],
		[3, "Everyone", "all", 0xFFFF],
		[4, "Max", "group", 65535],
	]);
	assert.ok(p25(m).every((c) => c.timeslot === 0));
});

test("decode: only valid P25 entries are listed", () => {
	const img = imageWith({
		1: entry(4, 1, 5, "NXDN"),				// another mode
		2: entry(2, 3, 5, "Reserved type"),
		3: entry(2, 1, 0, "Zero group"),
		4: entry(2, 1, 65536, "Group too big"),
		5: entry(2, 0, 0xFFFFFF, "Unit too big"),
		6: entry(2, 0, 0, "Zero unit"),
		7: entry(0x7F, 1, 5, "Unknown mode"),
		8: entry(2, 1, 5, "Good"),
	});
	assert.deepEqual(p25(decode(img)).map((c) => [c.number, c.name]), [[8, "Good"]]);
});

test("a DMR contact and a P25 contact are both number 1: each list has its own slots", () => {
	const m = empty();
	m.contacts.push(dmrContact("DMR One", 91), p25Contact("P25 One", 1310));
	const out = encode(m);
	assert.deepEqual([m.contacts[0].number, m.contacts[1].number], [1, 1]);
	assert.equal(decodeContact(out.bytes.subarray(CONTACTS, CONTACTS + CONTACT_SIZE)).name, "DMR One");
	assert.equal(decodeAirContact(out.bytes.subarray(slot(1), slot(1) + AIR_CONTACT_SIZE)).name, "P25 One");
	const d = decode(out);
	assert.deepEqual(d.contacts.map((c) => [c.mode, c.number, c.name]), [["dmr", 1, "DMR One"], ["p25", 1, "P25 One"]]);
	assert.deepEqual(encode(d).bytes, out.bytes);
});

test("a contact with no mode is stored nowhere, and has no number", () => {
	const m = empty();
	const c = newContact({ name: "Pending", id: 9, mode: null });
	m.contacts.push(c, dmrContact("Real", 5));
	const out = encode(m);
	assert.equal(c.number, null);
	assert.deepEqual(decode(out).contacts.map((x) => x.name), ["Real"]);
	assert.deepEqual(codes(m), ["error:Pending:mode_required"]);
});

test("encode: an untouched table round-trips byte for byte, kept slots and reserved bits included", () => {
	const img = imageWith({
		1: entry(2, 0xA5, 100, "A", { reserved: [1, 2, 3] }),		// flags bits 2-7 and reserved bytes set
		2: entry(4, 1, 5, "NXDN"),
		3: entry(2, 3, 5, "Bad type"),
		4: entry(2, 1, 0, "Bad ID"),
		5: entry(9, 0x42, 0x010203, "Future", { reserved: [7, 8, 9] }),
	});
	// a tail the spec leaves unused
	img.bytes.fill(0x5A, 0xDFF8, 0xE000);
	const m = decode(img);
	assert.equal(p25(m).length, 1);
	assert.deepEqual(encode(m).bytes, img.bytes);
});

test("encode: an edit changes only its record, and keeps reserved bits and bytes", () => {
	const img = imageWith({
		1: entry(2, 0xA5, 100, "Old name", { reserved: [1, 2, 3] }),
		2: entry(4, 1, 5, "NXDN"),
		3: entry(2, 1, 200, "Other"),
	});
	const m = decode(img);
	const a = p25(m)[0];
	a.name = "New";
	const out = encode(m);
	const want = entry(2, 0xA5, 100, "New", { reserved: [1, 2, 3] });
	assert.equal(hex(at(out, 1)), hex(want));
	assert.equal(hex(at(out, 2)), hex(at(img, 2)));
	assert.equal(hex(at(out, 3)), hex(at(img, 3)));
	// the call type: bits 0-1 only; the ID bytes follow when it changes
	a.type = "unit";
	a.id = 0xABCDEF;
	assert.equal(hex(at(encode(m), 1)), hex(entry(2, 0xA4, 0xABCDEF, "New", { reserved: [1, 2, 3] })));
	a.type = "all";
	assert.equal(hex(at(encode(m), 1)), hex(entry(2, 0xA6, 0xFFFF, "New", { reserved: [1, 2, 3] })));
});

test("encode: a changed ID alone leaves the name's stored padding", () => {
	const img = imageWith({ 1: entry(2, 1, 100, "Pad") });
	img.bytes.fill(0x00, slot(1) + 8 + 3, slot(1) + 24);	// zero padding, as another tool may write
	const m = decode(img);
	p25(m)[0].id = 101;
	const out = at(encode(m), 1);
	assert.deepEqual(out.slice(2, 5), [101, 0, 0]);
	assert.deepEqual(out.slice(11), at(img, 1).slice(11));
});

test("encode: a new entry goes in the first free slot, whole, with the reserved bytes 0xFF", () => {
	const img = imageWith({ 1: entry(2, 1, 1, "One"), 2: entry(4, 1, 5, "NXDN"), 4: entry(2, 0, 7, "Seven") });
	const m = decode(img);
	m.contacts.push(p25Contact("Fresh", 4242));
	const out = encode(m);
	// slot 2 is another mode's, slot 3 is free
	assert.equal(hex(at(out, 2)), hex(at(img, 2)));
	assert.equal(hex(at(out, 3)), hex(entry(2, 1, 4242, "Fresh")));
	assert.equal(m.contacts.at(-1).number, 3);
	assert.deepEqual(p25(decode(out)).map((c) => c.name), ["One", "Fresh", "Seven"]);
});

test("encode: a deleted entry is erased, the kept ones beside it are not", () => {
	const img = imageWith({ 1: entry(2, 1, 1, "One"), 2: entry(4, 1, 5, "NXDN"), 3: entry(2, 1, 3, "Three") });
	const m = decode(img);
	m.contacts.splice(0, 1);
	const out = encode(m);
	assert.deepEqual(at(out, 1), new Array(24).fill(0xFF));
	assert.equal(hex(at(out, 2)), hex(at(img, 2)));
	assert.equal(hex(at(out, 3)), hex(at(img, 3)));
});

test("encode: a number a kept slot holds is never taken", () => {
	const img = imageWith({ 1: entry(2, 1, 1, "One"), 2: entry(0x7F, 1, 5, "Future") });
	const m = decode(img);
	const x = p25Contact("Wants slot 2", 9, { number: 2 });
	m.contacts.push(x);
	const out = encode(m);
	assert.equal(x.number, 3);
	assert.equal(hex(at(out, 2)), hex(at(img, 2)));
	assert.equal(p25(decode(out)).find((c) => c.name === "Wants slot 2").number, 3);
});

test("assignNumbers: reserved numbers are skipped", () => {
	const list = [{ number: null }, { number: null }, { number: 2 }];
	assignNumbers(list, 5, new Set([1, 3]));
	assert.deepEqual(list.map((x) => x.number), [4, 5, 2]);
});

test("decodeAirContact and encodeAirContact on a loose record", () => {
	const r = new Uint8Array(24).fill(0xFF);
	assert.equal(decodeAirContact(r), null);
	encodeAirContact({ name: "TG", type: "group", id: 300 }, r);
	assert.deepEqual(decodeAirContact(r), { name: "TG", type: "group", id: 300 });
	assert.equal(r[1], 1);
	assert.deepEqual(Array.from(r.subarray(5, 8)), [0xFF, 0xFF, 0xFF]);
	// a 16-character name fills the field
	encodeAirContact({ name: "0123456789ABCDEF", type: "unit", id: 1 }, r);
	assert.equal(decodeAirContact(r).name, "0123456789ABCDEF");
});

// --- A contact changing mode ---

test("a mode change moves a contact: it frees its slot, takes the first free one of the other list, and maps Private and Unit", () => {
	const m = empty();
	m.contacts.push(dmrContact("A", 4000, { type: "private" }), dmrContact("B", 91));
	const d = decode(encode(m));
	const [a, b] = d.contacts;
	assert.deepEqual([a.number, b.number], [1, 2]);
	setContactMode(a, "p25");
	numberTable(d, "contacts");
	assert.deepEqual([a.mode, a.type, a.id, a.number], ["p25", "unit", 4000, 1]);
	const out = encode(d);
	assert.deepEqual(Array.from(out.bytes.subarray(CONTACTS, CONTACTS + CONTACT_SIZE)), new Array(CONTACT_SIZE).fill(0xFF), "the DMR slot is freed");
	assert.equal(decodeAirContact(out.bytes.subarray(slot(1), slot(1) + AIR_CONTACT_SIZE)).type, "unit");
	assert.deepEqual(decode(out).contacts.map((c) => [c.mode, c.number, c.name]), [["dmr", 2, "B"], ["p25", 1, "A"]]);
	// and back: Unit is Private again, in the first free DMR slot
	setContactMode(a, "dmr");
	numberTable(d, "contacts");
	assert.deepEqual([a.mode, a.type, a.number], ["dmr", "private", 1]);
	assert.deepEqual(at(encode(d), 1), new Array(24).fill(0xFF), "the P25 slot is freed");
});

test("a mode change skips the slots kept entries hold, and an all call takes the mode's ID", () => {
	const m = decode(imageWith({ 1: entry(4, 1, 5, "NXDN"), 2: entry(2, 1, 1, "One") }));
	const c = dmrContact("Everyone", 0xFFFFFF, { type: "all" });
	m.contacts.push(c);
	setContactMode(c, "p25");
	numberTable(m, "contacts");
	assert.deepEqual([c.type, c.id, c.number], ["all", 0xFFFF, 3]);
	setContactMode(c, "dmr");
	assert.equal(c.id, 0xFFFFFF);
	assert.equal(c.number, null, "numbered when the table is");
});

test("a contact keeps the channels and lists that name it when its mode changes; Problems says where that breaks", () => {
	const m = empty();
	const tg = dmrContact("TG", 91);
	m.contacts.push(tg);
	const ch = channel(m, "DMR Ch", 0, { contact: tg });
	m.rxLists.push({ number: null, name: "L", contacts: [tg] });
	ch.rxList = m.rxLists[0];
	numberTable(m, "contacts");
	setContactMode(tg, "p25");
	numberTable(m, "contacts");
	assert.equal(ch.contact, tg);
	assert.deepEqual(codes(m, "channels").filter((c) => !c.endsWith("no_contact")).sort(), ["error:DMR Ch:p25_contact_on_channel", "error:DMR Ch:rx_list_p25_on_channel"]);
});

// --- A P25 contact has no timeslot override ---

test("timeslot: a P25 contact's is not encoded, and clearing it from the rows is up to the save", () => {
	const m = empty();
	const c = dmrContact("T", 7, { timeslot: 2 });
	m.contacts.push(c);
	setContactMode(c, "p25");
	numberTable(m, "contacts");
	assert.equal(c.timeslot, 2, "kept while it is a P25 contact in the editor");
	const out = encode(m);
	assert.equal(c.timeslot, 2, "encoding leaves the rows alone (the draft encodes after every edit)");
	assert.equal(decode(out).contacts[0].timeslot, 0, "and the air table never holds it");
	assert.equal(clearP25Timeslots(m), 1);
	assert.equal(c.timeslot, 0);
	assert.equal(clearP25Timeslots(m), 0);
	// back to DMR before a save: the value is still there
	const k = dmrContact("K", 8, { timeslot: 1 });
	m.contacts.push(k);
	setContactMode(k, "p25");
	setContactMode(k, "dmr");
	assert.equal(k.timeslot, 1);
});

test("timeslot: the cell is greyed out and read-only on a P25 row, with its value kept; the ID of an all call too", () => {
	const m = empty();
	const d = dmrContact("D", 5, { timeslot: 2 }), p = p25Contact("P", 6, { timeslot: 2 }), all = p25Contact("All", 0xFFFF, { type: "all" });
	m.contacts.push(d, p, all);
	const cols = columns("contacts", m, t);
	const ts = cols.find((c) => c.key === "timeslot"), id = cols.find((c) => c.key === "id");
	assert.deepEqual([ts.readOnly(d), ts.readOnly(p)], [false, true]);
	assert.deepEqual([ts.format(2, d), ts.format(2, p)], ["2", "2"]);	// shown greyed on P25, not hidden
	assert.equal(p.timeslot, 2);
	assert.deepEqual([id.readOnly(d), id.readOnly(p), id.readOnly(all)], [false, false, true]);
	assert.equal(id.format(0, all), 0xFFFF);
});

// --- Columns ---

test("the contacts table: Mode, ID, call types and ID ranges follow the mode", () => {
	const m = empty();
	const d = dmrContact("D", 5), p = p25Contact("P", 6), fresh = newContact({ name: "N", id: 9, mode: null });
	m.contacts.push(d, p, fresh);
	numberTable(m, "contacts");
	const cols = columns("contacts", m, t);
	assert.deepEqual(cols.map((c) => c.key), ["number", "name", "mode", "system", "id", "type", "timeslot"]);
	assert.equal(cols.find((c) => c.key === "id").label, "editor.col.id");
	const type = cols.find((c) => c.key === "type"), mode = cols.find((c) => c.key === "mode"), id = cols.find((c) => c.key === "id");
	assert.deepEqual(type.options(d).map(([k]) => k), ["group", "private", "all"]);
	assert.deepEqual(type.options(p).map(([k]) => k), ["group", "unit", "all"]);
	assert.deepEqual(mode.options(d).map(([k]) => k), ["dmr", "p25"]);
	assert.deepEqual(mode.options(fresh).map(([k]) => k), [null, "dmr", "p25"], "a new row has no mode until one is chosen");
	// P25 only where a P25 channel kind is offered, or the row already is P25
	const cols2 = columns("contacts", m, t, { features: "FM,DMR" });
	assert.deepEqual(cols2.find((c) => c.key === "mode").options(d).map(([k]) => k), ["dmr"]);
	assert.deepEqual(cols2.find((c) => c.key === "mode").options(p).map(([k]) => k), ["dmr", "p25"]);
	assert.equal(p25Offered(m, "FM,DMR"), false);
	assert.equal(p25Offered(m, "P25"), true);
	assert.equal(p25Offered(m, null), true);
	// ID ranges: DMR 1-16777215; P25 group 1-65535, unit 1-16777214
	assert.equal(id.set(d, "16777215"), undefined);
	assert.equal(id.set(d, "16777216"), "editor.err.range");
	assert.equal(id.set(p, "65535"), undefined);
	assert.equal(id.set(p, "65536"), "editor.err.range");
	type.set(p, "unit");
	assert.equal(id.set(p, "16777214"), undefined);
	assert.equal(id.set(p, "16777215"), "editor.err.range");
	// the all call: the mode's ID
	type.set(p, "all");
	assert.equal(p.id, 0xFFFF);
	// the Mode cell moves the row to the other list
	mode.set(d, "p25");
	assert.deepEqual([d.mode, d.number], ["p25", 2]);
	assert.equal(mode.set(fresh, null), undefined);
	assert.equal(fresh.mode, null);
	mode.set(fresh, "dmr");
	assert.deepEqual([fresh.mode, fresh.number], ["dmr", 1]);
});

test("channels pick a talk target of the mode they read: P25 contacts on a P25 channel, DMR ones otherwise", () => {
	const m = empty();
	const d = dmrContact("DMR TG", 91), p = p25Contact("P25 TG", 91);
	m.contacts.push(d, p);
	const pc = channel(m, "P25 Ch", 2), dc = channel(m, "DMR Ch", 0), tc = channel(m, "Trunk", 3);
	const col = columns("channels", m, t).find((c) => c.key === "contact");
	const names = (r) => col.options(r).map(([, l]) => l);
	assert.deepEqual([names(pc), names(tc), names(dc)], [["editor.none", "P25 TG"], ["editor.none", "P25 TG"], ["editor.none", "DMR TG"]]);
	// a DMR contact an older codeplug's P25 channel names stays listed, to be replaced
	pc.contact = d;
	assert.ok(col.options(pc).some(([x]) => x === d));
	const form = channelForm(m, t).find((f) => f.key === "contact");
	assert.equal(form.dynamicOptions, true, "the list follows the mode picked in the form");
	assert.deepEqual(form.options(pc).map(([, l]) => l), ["editor.none", "P25 TG", "DMR TG"]);
});

// --- Channels, RX group lists and the contact flag ---

test("the contact flag: bit 2 of the conventional block, bit 1 of the trunked block, other bits kept", () => {
	const prev = new Uint8Array([0xF0, 0, 0, 0, 0]);
	const on = encodeConv({ squelch: 0, rxNac: 0x293, txNac: 0x293, p25Contacts: true }, prev);
	assert.equal(on[0], 0xF4);
	assert.deepEqual(decodeConv(on), { squelch: 0, rxNac: 0x293, txNac: 0x293, p25Contacts: true });
	assert.equal(encodeConv({ squelch: 1, rxNac: 0x293, txNac: 0x293, p25Contacts: false }, on)[0], 0xF1);
	assert.equal(decodeConv(new Uint8Array([0x03, 0, 0, 0, 0])).p25Contacts, false);
	const t1 = p25TrunkKind.encode({ followAll: false, p25Contacts: true }, new Uint8Array([0xF1]));
	assert.equal(t1[0], 0xF2);
	assert.deepEqual(p25TrunkKind.decode(t1), { followAll: false, p25Contacts: true });
	assert.deepEqual(p25TrunkKind.decode(new Uint8Array([0x01])), { followAll: true, p25Contacts: false });
});

test("a P25 channel with a P25 contact is written flagged; its contact and RX list read back as P25 contacts", () => {
	const m = empty();
	const tg = p25Contact("P25 TG", 1310), other = p25Contact("P25 Two", 77), dtg = dmrContact("DMR TG", 91);
	m.contacts.push(dtg, tg, other);
	const ch = channel(m, "P25 Ops", 2, { contact: tg });
	const tr = channel(m, "P25 Trunk", 3);
	m.rxLists.push({ number: null, name: "P25 Groups", contacts: [tg, other] });
	tr.rxList = m.rxLists[0];
	const plain = channel(m, "DMR Ch", 0, { contact: dtg });
	assert.deepEqual(flaggedChannels(m).map((c) => c.name), ["P25 Ops", "P25 Trunk"]);
	const img = encode(m);
	assert.equal(ch.params.p25Contacts, true);
	const d = decode(img);
	const by = (n) => d.channels.find((c) => c.name === n);
	assert.equal(by("P25 Ops").params.p25Contacts, true);
	assert.equal(by("P25 Ops").contact.mode, "p25");
	assert.equal(by("P25 Ops").contact.name, "P25 TG");
	assert.equal(by("P25 Trunk").params.p25Contacts, true);
	assert.deepEqual(by("P25 Trunk").rxList.contacts.map((c) => [c.mode, c.name]), [["p25", "P25 TG"], ["p25", "P25 Two"]]);
	assert.equal(by("DMR Ch").contact.mode, "dmr");
	assert.equal(plain.params, undefined);
	assert.deepEqual(validate(d).filter((p) => p.level === "error"), []);
	assert.deepEqual(encode(d).bytes, img.bytes, "an unedited codeplug round-trips");
});

test("an older codeplug's P25 channel names a DMR contact: shown with a warning, written unflagged as it was", () => {
	const m = empty();
	const tg = dmrContact("TG 91", 91);
	m.contacts.push(tg);
	const ch = channel(m, "Old P25", 2, { contact: tg });
	const img = encode(m);
	assert.equal(ch.params.p25Contacts, false);
	assert.equal(wantsP25Contacts(ch), false);
	const d = decode(img);
	assert.equal(d.channels[0].contact.mode, "dmr");
	assert.deepEqual(codes(d, "channels"), ["warning:Old P25:dmr_contact_on_p25"]);
	assert.deepEqual(encode(d).bytes, img.bytes);
});

test("a P25 channel with no contact follows its RX group list, else the flag it was read with", () => {
	const m = empty();
	const tg = p25Contact("TG", 5);
	m.contacts.push(tg);
	const ch = channel(m, "T", 3);
	assert.equal(wantsP25Contacts(ch), false);
	ch.params = { ...ch.params, p25Contacts: true };
	assert.equal(wantsP25Contacts(ch), true, "as read");
	ch.params = { ...ch.params, p25Contacts: false };
	m.rxLists.push({ number: null, name: "L", contacts: [tg] });
	ch.rxList = m.rxLists[0];
	assert.equal(wantsP25Contacts(ch), true);
});

test("a shared RX group list is read as DMR contacts; a list only flagged channels use, as P25 contacts", () => {
	const m = empty();
	const dtg = dmrContact("D", 1), ptg = p25Contact("P", 1);
	m.contacts.push(dtg, ptg);
	const only = channel(m, "Only", 2, { contact: ptg });
	const shared = channel(m, "Shared", 2, { contact: ptg });
	const dc = channel(m, "Dmr", 0);
	m.rxLists.push({ number: null, name: "Mine", contacts: [ptg] }, { number: null, name: "Both", contacts: [dtg] });
	only.rxList = m.rxLists[0];
	shared.rxList = m.rxLists[1];
	dc.rxList = m.rxLists[1];
	const d = decode(encode(m));
	const list = (n) => d.rxLists.find((l) => l.name === n);
	assert.equal(list("Mine").contacts[0].mode, "p25");
	assert.equal(list("Both").contacts[0].mode, "dmr");
	// the flagged channel reading the shared list as P25 slots is an error
	assert.ok(codes(d, "channels").includes("error:Shared:rx_list_dmr_on_p25"));
});

test("link validation: a list that mixes modes; a flagged P25 channel with a DMR list; a DMR channel with a P25 list or contact", () => {
	const m = empty();
	const d1 = dmrContact("D1", 1), p1 = p25Contact("P1", 1), p2 = p25Contact("P2", 2);
	m.contacts.push(d1, p1, p2);
	m.rxLists.push(
		{ number: null, name: "Mixed", contacts: [d1, p1] },
		{ number: null, name: "OnlyDmr", contacts: [d1] },
		{ number: null, name: "OnlyP25", contacts: [p1, p2] },
	);
	const mixed = channel(m, "Mixed use", 0, { rxList: m.rxLists[0] });
	const flaggedDmrList = channel(m, "P25 DMR list", 2, { contact: p1, rxList: m.rxLists[1] });
	const dmrP25List = channel(m, "DMR P25 list", 0, { rxList: m.rxLists[2] });
	const dmrP25Contact = channel(m, "DMR P25 TG", 0, { contact: p1 });
	const fine = channel(m, "P25 fine", 2, { contact: p1, rxList: m.rxLists[2] });
	assert.deepEqual(codes(m, "rxLists"), ["error:Mixed:rx_list_mixed"]);
	const link = validate(m).filter((p) => p.table === "channels" && p.code !== "no_contact").map((p) => `${p.level}:${p.item.name}:${p.code}`).sort();
	assert.deepEqual(link, [
		"error:DMR P25 TG:p25_contact_on_channel", "error:DMR P25 list:rx_list_p25_on_channel", "error:P25 DMR list:rx_list_dmr_on_p25",
	]);
	assert.ok(mixed && flaggedDmrList && dmrP25List && dmrP25Contact && fine);
});

// --- Validation ---

test("validate: names, ID range by call type, the lists' room", () => {
	const m = empty();
	m.contacts.push(
		p25Contact("", 1),
		p25Contact("x".repeat(17), 2),
		p25Contact("Big group", 65536),
		p25Contact("Zero unit", 0, { type: "unit" }),
		p25Contact("Big unit", 0xFFFFFF, { type: "unit" }),
		p25Contact("Fine unit", 0xFFFFFE, { type: "unit" }),
		p25Contact("All call", 0, { type: "all" }),
	);
	assert.deepEqual(codes(m), [
		"error::no_name", "error:xxxxxxxxx:too_long", "error:Big group:p25_group_range", "error:Zero unit:p25_unit_range",
		"error:Big unit:p25_unit_range",
	]);
	m.contacts = Array.from({ length: 342 }, (_, i) => p25Contact(`T${i}`, i + 1));
	assert.ok(validate(m).some((p) => p.table === "contacts" && p.code === "too_many_mode" && p.params.max === 341 && p.params.mode === "P25"));
	// the DMR list has its own room
	m.contacts = Array.from({ length: 1025 }, (_, i) => dmrContact(`D${i}`, i + 1));
	assert.ok(validate(m).some((p) => p.table === "contacts" && p.code === "too_many_mode" && p.params.max === 1024 && p.params.mode === "DMR"));
	// 1024 DMR and 341 P25 fit: each list has its slots
	m.contacts = [...Array.from({ length: 1024 }, (_, i) => dmrContact(`D${i}`, i + 1)), ...Array.from({ length: 341 }, (_, i) => p25Contact(`T${i}`, i + 1))];
	assert.ok(!validate(m).some((p) => p.code === "too_many_mode"));
	// the slots kept entries hold are not free
	const kept = decode(imageWith({ 1: entry(4, 1, 5, "NXDN"), 2: entry(2, 3, 1, "Bad") }));
	kept.contacts = Array.from({ length: 340 }, (_, i) => p25Contact(`T${i}`, i + 1));
	assert.ok(validate(kept).some((p) => p.table === "contacts" && p.code === "too_many_mode" && p.params.max === 339));
	kept.contacts.length = 339;
	assert.ok(!validate(kept).some((p) => p.table === "contacts"));
});

test("validate: a later entry for an ID is a warning; call types and all call count as the radio looks them up", () => {
	const m = decode(imageWith({
		1: entry(2, 1, 1310, "First"),
		2: entry(2, 1, 1310, "Second"),
		3: entry(2, 0, 1310, "Unit 1310"),		// a unit, a different ID space
		4: entry(2, 2, 0, "All call"),			// names group 0xFFFF
		5: entry(2, 1, 65535, "Group 65535"),
		6: entry(2, 1, 77, "Seventy-seven"),
	}));
	m.contacts.push(dmrContact("Same ID, DMR", 1310), p25Contact("", 77, { number: 7 }));	// DMR: its own list; nameless: names nothing
	assert.deepEqual(codes(m), ["error::no_name", "warning:Second:p25_duplicate", "warning:Group 655:p25_duplicate"]);
});

// --- Converting an older codeplug ---

// a codeplug as an older editor wrote it: P25 channels naming DMR contacts and lists
function legacy() {
	const m = empty();
	const tg91 = dmrContact("TG 91", 91), unit = dmrContact("Chief", 4000, { type: "private" });
	const all = dmrContact("Everyone", 0xFFFFFF, { type: "all" });
	m.contacts.push(tg91, unit, all);
	const a = channel(m, "P25 A", 2, { contact: tg91 });
	const b = channel(m, "P25 Trunk", 3);
	const dc = channel(m, "DMR", 0, { contact: tg91 });
	m.rxLists.push({ number: null, name: "Shared", contacts: [tg91, all] }, { number: null, name: "Mine", contacts: [tg91, unit] });
	a.rxList = dc.rxList = m.rxLists[0];
	b.rxList = m.rxLists[1];
	return { m: decode(encode(m)), tg91, unit, all, a, b, dc };
}

test("convert: the P25 channels' talk targets become P25 contacts, a list only they use is converted, a shared one copied", () => {
	const { m } = legacy();
	const by = (n) => m.channels.find((c) => c.name === n);
	assert.deepEqual(p25ChannelsToConvert(m).map((c) => c.name), ["P25 A", "P25 Trunk"]);
	const r = convertP25Channels(m);
	assert.deepEqual(r, { channels: 2, contacts: 3, lists: 1, copies: 1, left: 0, full: false });
	assert.deepEqual(p25ChannelsToConvert(m), []);
	const a = by("P25 A"), b = by("P25 Trunk"), dc = by("DMR");
	// the talk target: a P25 contact of the same ID, call type and name; the DMR contact stays for the DMR channel
	assert.deepEqual([a.contact.mode, a.contact.name, a.contact.type, a.contact.id], ["p25", "TG 91", "group", 91]);
	assert.equal(dc.contact.mode, "dmr");
	assert.equal(dmr(m).length, 3);
	// the shared list was copied for the P25 channel; the DMR channel keeps its list
	assert.equal(dc.rxList.name, "Shared");
	assert.deepEqual(dc.rxList.contacts.map((c) => c.mode), ["dmr", "dmr"]);
	assert.equal(a.rxList.name, "Shared P25");
	assert.deepEqual(a.rxList.contacts.map((c) => [c.mode, c.type, c.id]), [["p25", "group", 91], ["p25", "all", 0xFFFF]]);
	assert.ok(a.rxList.number && a.rxList.number !== dc.rxList.number);
	// a list only P25 channels use was converted in place; a private call is a unit call
	assert.equal(b.rxList.name, "Mine");
	assert.deepEqual(b.rxList.contacts.map((c) => [c.mode, c.type, c.id]), [["p25", "group", 91], ["p25", "unit", 4000]]);
	assert.equal(b.rxList.contacts[0], a.contact, "one P25 contact for one talk target");
	// every new contact has a P25 slot, and nothing is an error
	assert.ok(p25(m).every((c) => c.number >= 1));
	assert.deepEqual(validate(m).filter((p) => p.level === "error"), []);
	// written: flagged, and it reads back
	const d = decode(encode(m));
	assert.deepEqual(flaggedChannels(d).map((c) => c.name), ["P25 A", "P25 Trunk"]);
	assert.equal(d.channels.find((c) => c.name === "P25 A").rxList.contacts[1].type, "all");
	assert.deepEqual(validate(d).filter((p) => p.level === "error"), []);
});

test("convert: a P25 contact already there is reused", () => {
	const { m } = legacy();
	const mine = p25Contact("My TG 91", 91);
	m.contacts.push(mine);
	numberTable(m, "contacts");
	const r = convertP25Channels(m);
	assert.equal(r.contacts, 2, "the unit and the all call are new");
	assert.equal(m.channels.find((c) => c.name === "P25 A").contact, mine);
	assert.equal(p25(m).filter((c) => c.id === 91).length, 1);
});

test("convert: what P25 has no entry for stays a DMR contact, and Problems says so", () => {
	const m = empty();
	const big = dmrContact("Big unit", 0xFFFFFF, { type: "private" }), huge = dmrContact("Huge TG", 70000), ok = dmrContact("TG 5", 5);
	m.contacts.push(big, huge, ok);
	const c1 = channel(m, "Big", 2, { contact: big });
	const c2 = channel(m, "Huge", 2, { contact: huge });
	const c3 = channel(m, "Ok", 2, { contact: ok });
	m.rxLists.push({ number: null, name: "L", contacts: [ok, big] });
	c3.rxList = m.rxLists[0];
	const r = convertP25Channels(decode(encode(m)));
	assert.equal(r.left, 3);
	const d = decode(encode(m));
	const r2 = convertP25Channels(d);
	assert.deepEqual([r2.channels, r2.left], [3, 3]);
	const by = (n) => d.channels.find((c) => c.name === n);
	assert.equal(by("Big").contact.mode, "dmr");
	assert.equal(by("Huge").contact.mode, "dmr");
	assert.equal(by("Ok").contact.mode, "p25");
	assert.deepEqual(by("Ok").rxList.contacts.map((c) => c.mode), ["p25", "dmr"]);
	assert.deepEqual(codes(d, "rxLists"), ["error:L:rx_list_mixed"]);
	assert.ok(codes(d, "channels").includes("warning:Big:dmr_contact_on_p25"));
	assert.ok(c1 && c2);
});

test("convert: no room for the copy of a shared list is reported, and the channel keeps its list", () => {
	const { m } = legacy();
	while (m.rxLists.length < LIMITS.rxLists) m.rxLists.push({ number: null, name: `Filler ${m.rxLists.length}`, contacts: [] });
	numberTable(m, "rxLists");
	const a = m.channels.find((c) => c.name === "P25 A");
	const was = a.rxList;
	const r = convertP25Channels(m);
	assert.equal(r.full, true);
	assert.equal(r.copies, 0);
	assert.equal(a.rxList, was);
	assert.equal(m.rxLists.length, LIMITS.rxLists);
	assert.equal(a.rxList.contacts[0].mode, "dmr");
});

test("convert: one undo takes it all back", () => {
	const { m, a, b } = legacy();
	const h = new History();
	const nContacts = m.contacts.length, nLists = m.rxLists.length;
	const aContact = m.channels.find((c) => c.name === "P25 A").contact;
	h.record(m, "Convert");
	convertP25Channels(m);
	assert.ok(m.contacts.length > nContacts && m.rxLists.length > nLists);
	h.undo(m);
	assert.deepEqual([m.contacts.length, m.rxLists.length], [nContacts, nLists]);
	const aa = m.channels.find((c) => c.name === "P25 A");
	assert.equal(aa.contact, aContact);
	assert.equal(aa.contact.mode, "dmr");
	assert.deepEqual(p25ChannelsToConvert(m).length, 2);
	assert.ok(a && b);
});

// --- The firmware warning ---

test("firmware: channels written flagged are counted for a radio whose firmware lacks p25contacts", () => {
	const m = empty();
	const tg = p25Contact("TG", 1), dtg = dmrContact("D", 1);
	m.contacts.push(tg, dtg);
	channel(m, "Flagged", 2, { contact: tg });
	channel(m, "Flagged 2", 3, { contact: tg });
	channel(m, "Legacy", 2, { contact: dtg });
	channel(m, "DMR", 0, { contact: dtg });
	assert.equal(unreadFlagged(m, "FM,DMR,P25"), 2, "old firmware: two would talk to the wrong talk group");
	assert.equal(unreadFlagged(m, "FM,DMR,P25,p25contacts"), 0);
	assert.equal(unreadFlagged(m, "FM,P25,P25CONTACTS"), 0, "case does not matter");
	assert.equal(unreadFlagged(m, ""), 2);
	assert.equal(unreadFlagged(m, null), 0, "a file: no radio to ask");
	const none = empty();
	none.contacts.push(dtg);
	channel(none, "Legacy", 2, { contact: dtg });
	assert.equal(unreadFlagged(none, "FM,DMR,P25"), 0, "nothing flagged: nothing to warn about");
});

// --- Column filters ---

// a Grid without the DOM: what it shows, offers and gives a new row
function gridOf(m, filters = {}) {
	return Object.assign(Object.create(Grid.prototype), { columns: columns("contacts", m, t), rows: m.contacts, filter: "", sort: null, filters, t, render() {} });
}

function filterSample() {
	const m = empty();
	m.contacts.push(
		dmrContact("Alpha", 91, { timeslot: 0 }), dmrContact("Bravo", 4000, { type: "private", timeslot: 1 }),
		p25Contact("Charlie", 1310, { timeslot: 2 }), p25Contact("Delta", 4001, { type: "unit" }),
	);
	numberTable(m, "contacts");
	return m;
}

const names = (g) => g.shown().map((r) => r.name);

test("filters: Mode, System, Call Type and Timeslot Override have a filter; the others don't", () => {
	const cols = columns("contacts", empty(), t);
	assert.deepEqual(cols.filter((c) => c.filter).map((c) => c.key), ["mode", "system", "type", "timeslot"]);
});

test("filters: a column shows the rows whose value is checked; the columns' filters and the text filter combine", () => {
	const g = gridOf(filterSample());
	assert.deepEqual(names(g), ["Alpha", "Bravo", "Charlie", "Delta"]);
	g.filters.mode = new Set(["p25"]);
	assert.deepEqual(names(g), ["Charlie", "Delta"]);
	g.filters.type = new Set(["unit"]);
	assert.deepEqual(names(g), ["Delta"]);
	g.filters.type = new Set(["unit", "group"]);
	assert.deepEqual(names(g), ["Charlie", "Delta"]);
	g.filter = "delt";
	assert.deepEqual(names(g), ["Delta"]);
	g.filter = "";
	delete g.filters.mode;
	g.filters.type = new Set(["private", "unit"]);
	assert.deepEqual(names(g), ["Bravo", "Delta"]);
});

test("filters: the timeslot of a P25 row is blank, whatever value it keeps", () => {
	const g = gridOf(filterSample());
	g.filters.timeslot = new Set([1, 2]);
	assert.deepEqual(names(g), ["Bravo"], "Charlie keeps 2 but has none");
	g.filters.timeslot = new Set([null]);
	assert.deepEqual(names(g), ["Charlie", "Delta"]);
	g.filters.timeslot = new Set([0]);
	assert.deepEqual(names(g), ["Alpha"]);
});

test("filters: the checklist offers the column's values, and (Blanks) when a row has none", () => {
	const m = filterSample();
	const g = gridOf(m);
	const col = (k) => g.columns.find((c) => c.key === k);
	assert.deepEqual(g.filterChoices(col("mode")), [["dmr", "DMR"], ["p25", "P25"]]);
	assert.deepEqual(g.filterChoices(col("type")).map(([k]) => k), ["group", "private", "unit", "all"]);
	assert.deepEqual(g.filterChoices(col("timeslot")).map(([k]) => k), [0, 1, 2, null]);
	assert.equal(g.filterChoices(col("timeslot")).at(-1)[1], "editor.grid.filter_blanks");
	// a value only the rows hold is offered too
	m.contacts.push(newContact({ name: "Pending", id: 1, mode: null }));
	assert.deepEqual(g.filterChoices(col("mode")).map(([k]) => k), ["dmr", "p25", null]);
});

test("filters: checking every value is no filter, and the funnel shows when one is on", () => {
	const g = gridOf(filterSample());
	const mode = g.columns.find((c) => c.key === "mode");
	g.setColumnFilter(mode, new Set(["p25"]));
	assert.deepEqual([...g.filters.mode], ["p25"]);
	g.setColumnFilter(mode, new Set(["p25", "dmr"]));
	assert.equal(g.filters.mode, undefined);
});

test("new row defaults: a column filtered to one value gives it to the new row", () => {
	const g = gridOf(filterSample());
	assert.deepEqual(g.newRowDefaults(), {});
	g.filters.mode = new Set(["p25"]);
	g.filters.type = new Set(["group"]);
	g.filters.timeslot = new Set([1]);
	assert.deepEqual(g.newRowDefaults(), { mode: "p25", type: "group", timeslot: 1 });
	g.filters.type = new Set(["group", "unit"]);
	g.filters.timeslot = new Set([null]);
	assert.deepEqual(g.newRowDefaults(), { mode: "p25" });
});

test("new row defaults: a new contact takes them; with no mode filtered the mode stays blank", () => {
	const base = { name: "New", id: 9 };
	const none = contactFromFilters(base, {});
	assert.deepEqual([none.mode, none.type, none.timeslot, none.name], [null, "group", 0, "New"]);
	assert.equal(none.number, null);
	const p = contactFromFilters(base, { mode: "p25", type: "group" });
	assert.deepEqual([p.mode, p.type], ["p25", "group"]);
	const d = contactFromFilters(base, { mode: "dmr", type: "private", timeslot: 2 });
	assert.deepEqual([d.mode, d.type, d.timeslot], ["dmr", "private", 2]);
	// the call type as the mode names it; no timeslot on a P25 contact; an all call has the mode's ID
	assert.deepEqual(((c) => [c.type, c.timeslot])(contactFromFilters(base, { mode: "p25", type: "private", timeslot: 1 })), ["unit", 0]);
	assert.equal(contactFromFilters(base, { mode: "p25", type: "all" }).id, 0xFFFF);
	assert.equal(contactFromFilters(base, { mode: "dmr", type: "all" }).id, 0xFFFFFF);
	// a call type with no mode filtered stays as it is
	assert.equal(contactFromFilters(base, { type: "unit" }).type, "unit");
});

// --- CSV ---

test("CSV: one contacts file with a Mode column; export and import round trip", () => {
	const m = decode(imageWith({
		1: entry(2, 1, 1310, "Fire, Dispatch"),
		2: entry(4, 1, 5, "NXDN"),
		3: entry(2, 0, 4000, "Chief"),
		4: entry(2, 2, 0, "Everyone"),
	}));
	m.contacts.unshift(dmrContact("Local", 9, { number: 1, timeslot: 2 }));
	const before = encode(m).bytes;
	const text = exportTable(m, "contacts");
	assert.match(text, /^Number,Name,Mode,System,ID,Call Type,Timeslot\r\n/);
	assert.match(text, /\r\n1,Local,DMR,,9,Group,2\r\n/);
	assert.match(text, /\r\n1,"Fire, Dispatch",P25,,1310,Group,\r\n3,Chief,P25,,4000,Unit,\r\n4,Everyone,P25,,65535,All,\r\n$/);
	assert.equal(detect(text), "contacts");
	assert.equal(detect("Number,Name,Mode,Call Type,ID\r\n1,A,P25,Group,5\r\n"), "contacts");
	const r = importTable(m, text);
	assert.deepEqual([r.problems, r.added, r.updated, r.removed], [[], 0, 4, 0]);
	assert.deepEqual(encode(m).bytes, before);
	// into a blank codeplug
	const n = empty();
	importTable(n, text);
	const view = (x) => x.contacts.map(({ name, mode, type, id, number, timeslot }) => [name, mode, type, id, number, timeslot]);
	assert.deepEqual(view(n), view(m).map((r) => (r[1] === "p25" ? [...r.slice(0, 5), 0] : r)));
	const all = importAll(empty(), exportAll(m).map((f) => ({ name: f.name, text: f.data })));
	assert.deepEqual(all.results.map((x) => [x.table, x.added]), [["contacts", 4], ["rxLists", 0], ["dtmfContacts", 0], ["aprs", 0], ["channels", 0], ["zones", 0]].slice(0, all.results.length));
	assert.equal(all.results[0].table, "contacts");
	assert.ok(!exportAll(m).some((f) => f.name === "p25-contacts.csv"), "no separate file");
});

test("CSV: a file without a Mode column holds DMR contacts, and leaves the P25 ones alone", () => {
	const m = decode(imageWith({ 1: entry(2, 1, 1310, "Fire") }));
	m.contacts.unshift(dmrContact("Old", 5, { number: 1 }));
	const r = importTable(m, "Number,Name,ID,Call Type,Timeslot\n1,Old,5,Group,0\n2,New,6,Private,1\n");
	assert.deepEqual([r.problems, r.added, r.updated, r.removed], [[], 1, 1, 0]);
	assert.deepEqual(m.contacts.map((c) => [c.mode, c.number, c.name]), [["dmr", 1, "Old"], ["dmr", 2, "New"], ["p25", 1, "Fire"]]);
	assert.equal(m.contacts[1].type, "private");
	// a DMR contact not in the file goes, a P25 one stays
	const r2 = importTable(m, "Name,ID\nOld,5\n");
	assert.equal(r2.removed, 1);
	assert.deepEqual(m.contacts.map((c) => c.name), ["Old", "Fire"]);
});

test("CSV: a row stands for the contact of its own mode; names and slots can repeat across modes", () => {
	const m = empty();
	m.contacts.push(dmrContact("Fire", 91, { number: 1 }), p25Contact("Fire", 1310, { number: 1 }));
	const r = importTable(m, "Number,Name,Mode,ID,Call Type\n1,Fire,P25,2000,Group\n1,Fire,DMR,92,Group\n");
	assert.deepEqual([r.added, r.updated], [0, 2]);
	assert.deepEqual(m.contacts.map((c) => [c.mode, c.id]), [["p25", 2000], ["dmr", 92]], "in the file's order");
});

test("CSV: new rows skip the slots of kept entries; bad values are problems; a Mode that is neither DMR nor P25 skips a new row", () => {
	const m = decode(imageWith({ 1: entry(4, 1, 5, "NXDN") }));
	const r = importTable(m, "Name,Mode,Call Type,ID\nAlpha,P25,Group,10\nBeta,NXDN,Group,11\nGamma,P25,Group,x\nDelta,P25,Unit,0xFFFFFF\nEcho,DMR,Private,12\n");
	assert.deepEqual(m.contacts.map((c) => [c.mode, c.number, c.name, c.type]), [["p25", 2, "Alpha", "group"], ["dmr", 1, "Echo", "private"]]);
	assert.deepEqual(r.problems.map((p) => [p.row, p.column, p.code, !!p.skipped]), [
		[3, "Mode", "bad_value", true], [4, "ID", "range", true], [5, "ID", "range", true],
	]);
	// a call type as the mode names it: a P25 file's Private is a Unit, a DMR file's Unit is Private
	const n = empty();
	importTable(n, "Name,Mode,Call Type,ID\nU,P25,Private,5\nV,DMR,Unit,6\nA,P25,All,1\n");
	assert.deepEqual(n.contacts.map((c) => [c.name, c.type, c.id]), [["U", "unit", 5], ["V", "private", 6], ["A", "all", 0xFFFF]]);
});

test("CSV: RX group lists name contacts of either mode; a P25 contact's slot is told from a DMR one's", () => {
	const m = empty();
	const d = dmrContact("Same", 1, { number: 1 }), p = p25Contact("Same", 1, { number: 1 }), q = p25Contact("Other", 2, { number: 2 });
	m.contacts.push(d, p, q);
	m.rxLists.push({ number: 1, name: "L", contacts: [d, p, q] });
	const text = exportTable(m, "rxLists");
	assert.match(text, /\r\n1,L,#1\|#p25:1\|Other\r\n$/);
	const n = empty();
	n.contacts.push(...m.contacts);
	n.rxLists.push({ number: 1, name: "L", contacts: [] });
	const r = importTable(n, text);
	assert.deepEqual(r.problems, []);
	assert.deepEqual(n.rxLists[0].contacts, [d, p, q]);
});

test("CSV: a channel's talk target is looked up among the contacts of the mode its kind reads", () => {
	const m = empty();
	const d = dmrContact("TG", 91, { number: 1 }), p = p25Contact("TG", 91, { number: 1 });
	m.contacts.push(d, p);
	const ch = channel(m, "P25 Ch", 2, { contact: p });
	const dc = channel(m, "DMR Ch", 0, { contact: d });
	numberTable(m, "channels");
	const text = exportTable(m, "channels");
	assert.match(text, /P25 Ch,.*,TG,/);
	const r = importTable(m, text);
	assert.deepEqual(r.problems, []);
	assert.equal(ch.contact, p);
	assert.equal(dc.contact, d);
});

test("qdmr export: a note counts channels whose portable P25 links qdmr itself ignores", () => {
	const m = empty();
	const tg = p25Contact("P25 TG", 1310), dtg = dmrContact("DMR TG", 91);
	m.contacts.push(dtg, tg);
	channel(m, "P25 Ops", 2, { contact: tg });
	const tr = channel(m, "P25 Trunk", 3);
	m.rxLists.push({ number: null, name: "P25 Groups", contacts: [tg] });
	tr.rxList = m.rxLists[0];
	channel(m, "DMR Ch", 0, { contact: dtg });
	const { notes } = exportQdmr(m);
	assert.deepEqual(notes.filter((n) => n.code === "p25_contacts"), [{ code: "p25_contacts", n: 2 }]);
});
