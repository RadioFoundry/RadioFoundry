// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The Codeplug Editor's contact lists (static/js/codeplug/contactlists.js, docs/spec/contact-lists.md): the keys the
// editor writes are the bytes the firmware reads, the contact table check, the void rule, moving a list with its
// contacts, the CSV List and Network columns and the Default network migration.
// Run with node --test site/tests/ (site/tests/run.sh).

import { test } from "node:test";
import assert from "node:assert/strict";

import { decode, encode, empty, newChannel, newContact, remove, validate } from "../static/js/codeplug/model.js";
import { setKind, newSystem, MODE_KEY } from "../static/js/codeplug/channelmodes.js";
import { decodeChain, decodeArea, getValue, putContinuation, KV_SIZE, KV_CONT_ADDR } from "../static/js/codeplug/kv.js";
import { CONTACTS, CONTACT_SIZE, AIR_CONTACTS, AIR_CONTACT_SIZE, encodeContact } from "../static/js/codeplug/records.js";
import { crc32 } from "../static/js/flasher/bytes.js";
import { exportTable, importTable, parse } from "../static/js/codeplug/csv.js";
import {
	LIST_KEY, LIST_IDDB, contactTablesCheck, decodeListName, listNameValid, listNameProblem, isNetwork, newNetwork, freeNetworkNumber, makeDefaultNetwork,
	needsContactListMigration, removeLists, listUsers, resolveVoid, takesNetwork,
} from "../static/js/codeplug/contactlists.js";

const CONT = KV_CONT_ADDR;
const text = (s) => [...s].map((c) => c.charCodeAt(0));
const keysOf = (img) => decodeChain(img.bytes).entries;
const val = (img, key) => {
	const v = getValue(keysOf(img), key);
	return v && [...v];
};
const contSector = (img) => img.bytes.subarray(CONT, CONT + KV_SIZE);
const crc32Of = (bytes) => (bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24)) >>> 0;

// The check as the spec words it, computed another way than contactlists.js: one running CRC over the slots' identity bytes
function specCheck(b) {
	let crc = 0;
	for (let n = 0; n < 1024; n++) crc = crc32(b.subarray(CONTACTS + n * CONTACT_SIZE + 0x10, CONTACTS + n * CONTACT_SIZE + 0x15), crc);
	for (let n = 0; n < 341; n++) crc = crc32(b.subarray(AIR_CONTACTS + n * AIR_CONTACT_SIZE, AIR_CONTACTS + n * AIR_CONTACT_SIZE + 5), crc);
	return [crc & 0xFF, (crc >>> 8) & 0xFF, (crc >>> 16) & 0xFF, crc >>> 24];
}

// Two lists, two networks, three contacts and two channels
function sample() {
	const m = empty();
	const tg = newContact({ name: "TG 91", id: 91 }), local = newContact({ name: "Local", id: 9 });
	const p25 = newContact({ name: "P25 TG", id: 1000, mode: "p25" });
	m.contacts.push(tg, local, p25);
	tg.list = 0;
	local.list = 1;
	p25.list = 1;
	m.contactLists = [{ number: 0, name: "BM" }, { number: 1, name: "Club" }];
	const net = newNetwork(m, "Net A", [0, LIST_IDDB]);
	m.systems.push(net);
	const solo = newNetwork(m, "Net B", [1, null]);
	m.systems.push(solo);
	const ch = newChannel({ name: "Rpt", mode: "dmr", network: net, contact: tg });
	const ch2 = newChannel({ name: "Other", mode: "dmr", network: solo, contact: local });
	m.channels.push(ch, ch2);
	return { m, tg, local, p25, net, solo, ch, ch2 };
}

test("a new network takes a number above the Tier III runtime's, and is not shared", () => {
	const m = empty();
	const a = newNetwork(m, "A");
	assert.equal(a.number, 9);
	assert.equal(a.type, 6);
	assert.equal(a.sharing, 0);
	assert.deepEqual(a.lists, [null, null]);
	m.systems.push(a);
	assert.equal(freeNetworkNumber(m), 10);
});

test("round trip: the keys are the bytes the firmware reads", () => {
	const { m } = sample();
	const img = encode(m);
	assert.deepEqual(val(img, LIST_KEY.name), text("BM"));
	assert.deepEqual(val(img, LIST_KEY.name + 1), text("Club"));
	assert.equal(val(img, LIST_KEY.name + 2), null);
	// system 9 and 10 (keys by number - 1): two bytes, and one for a primary only
	assert.deepEqual(val(img, LIST_KEY.system + 8), [0, 0xFE]);
	assert.deepEqual(val(img, LIST_KEY.system + 9), [1]);
	assert.deepEqual(val(img, LIST_KEY.dmr), [0, 1]);
	assert.deepEqual(val(img, LIST_KEY.air), [1]);
	assert.deepEqual(val(img, LIST_KEY.network), [8, 9]);
	assert.deepEqual(val(img, LIST_KEY.check), specCheck(img.bytes));
	assert.deepEqual([...img.bytes.subarray(0xE000 + 12, 0xE000 + 16)], [0, 0xF0, 0, 0], "the first sector names the continuation");
	// the lists' keys are in the continuation only
	assert.ok(decodeArea(img.bytes).entries.every((e) => e.key < 0x1E00 || e.key > 0x20FF));
	assert.ok(decodeArea(img.bytes, CONT).entries.every((e) => e.key >= 0x1E00 && e.key <= 0x20FF));
	// a network is a system record of the header only
	const rec = getValue(decodeArea(img.bytes).entries, MODE_KEY.system + 8);
	assert.equal(rec.length, 28);
	assert.deepEqual([rec[0], rec[1], rec[2], rec[3]], [6, 0, 0, 0]);

	const d = decode(img);
	assert.deepEqual(d.contactLists, [{ number: 0, name: "BM" }, { number: 1, name: "Club" }]);
	assert.equal(d.contactListsVoid, false);
	const by = (name) => d.contacts.find((c) => c.name === name);
	assert.deepEqual([by("TG 91").list, by("Local").list, by("P25 TG").list], [0, 1, 1]);
	const a = d.systems.find((s) => s.name === "Net A"), b = d.systems.find((s) => s.name === "Net B");
	assert.deepEqual([a.type, a.number, a.lists], [6, 9, [0, LIST_IDDB]]);
	assert.deepEqual(b.lists, [1, null]);
	assert.equal(d.channels.find((c) => c.name === "Rpt").network, a);
	assert.equal(d.channels.find((c) => c.name === "Other").network, b);
	// an unedited codeplug writes the same bytes
	assert.deepEqual(encode(d).bytes, img.bytes);
});

test("a codeplug without lists reads as none and writes nothing", () => {
	const m = empty();
	m.contacts.push(newContact({ name: "A", id: 1 }));
	const img = encode(m);
	assert.ok(contSector(img).every((x) => x === 0xFF), "no continuation is made");
	const d = decode(img);
	assert.deepEqual(d.contactLists, []);
	assert.equal(d.contactListsVoid, false);
	assert.equal(d.contacts[0].list, null);
	assert.deepEqual(encode(d).bytes, img.bytes);
	// a blank codeplug
	assert.deepEqual(encode(empty()).bytes, empty().base.bytes);
});

test("an erased continuation reads as no lists, and is left as it is", () => {
	const { m } = sample();
	const img = encode(m);
	const erased = img.clone();
	erased.bytes.fill(0xFF, CONT, CONT + KV_SIZE);
	const d = decode(erased);
	assert.deepEqual(d.contactLists, []);
	assert.equal(d.contactListsVoid, false);
	assert.ok(d.contacts.every((c) => c.list === null));
	assert.ok(d.systems.every((s) => s.lists[0] === null && s.lists[1] === null));
	assert.ok(d.channels.every((c) => c.network === null));
	const out = encode(d);
	assert.ok(contSector(out).every((x) => x === 0xFF), "nothing is written for a model with no lists");
	assert.deepEqual([...out.bytes.subarray(0xE000 + 12, 0xE000 + 16)], [0, 0xF0, 0, 0]);
});

test("the table check: a contact table changed behind the lists voids them; a rename doesn't", () => {
	const { m } = sample();
	const img = encode(m);
	assert.equal(contactTablesCheck(img.bytes), crc32Of(specCheck(img.bytes)));
	const slot = (image, c) => encodeContact(c, image.bytes.subarray(CONTACTS, CONTACTS + CONTACT_SIZE));

	// another tool changed contact 1's ID
	const foreign = img.clone();
	slot(foreign, { name: "TG 91", id: 92, type: "group", timeslot: 0 });
	const d = decode(foreign);
	assert.equal(d.contactListsVoid, true);
	assert.ok(d.contacts.every((c) => c.list === null), "every contact reads as in no list");
	assert.equal(d.contactLists.length, 2, "the lists themselves stay");
	assert.deepEqual(d.systems.find((s) => s.name === "Net A").lists, [0, LIST_IDDB], "so do the selections");
	assert.ok(d.channels.find((c) => c.name === "Rpt").network, "and the channel networks");
	assert.ok(validate(d).some((p) => p.code === "lists_void" && p.level === "warning"));
	// saved as it is, the keys are kept as found: still void, never a confirmed wrong assignment
	const kept = encode(d);
	assert.deepEqual(contSector(kept), contSector(foreign));
	assert.equal(decode(kept).contactListsVoid, true);
	// the user assigns the contacts again: the next write stores a new check
	resolveVoid(d);
	for (const c of d.contacts) c.list = 0;
	const fixed = encode(d);
	assert.deepEqual(val(fixed, LIST_KEY.check), specCheck(fixed.bytes));
	assert.deepEqual(val(fixed, LIST_KEY.dmr), [0, 0]);
	const back = decode(fixed);
	assert.equal(back.contactListsVoid, false);
	assert.ok(back.contacts.every((c) => c.list === 0));

	// a rename changes no byte the check covers
	const renamed = img.clone();
	slot(renamed, { name: "Renamed", id: 91, type: "group", timeslot: 0 });
	const r = decode(renamed);
	assert.equal(r.contactListsVoid, false);
	assert.equal(r.contacts.find((c) => c.name === "Renamed").list, 0);

	// a membership key with no check key is void, and no membership key has nothing to void
	const noCheck = img.clone();
	putContinuation(noCheck.bytes, keysOf(noCheck).filter((e) => e.key >= 0x1E00 && e.key !== LIST_KEY.check));
	assert.equal(decode(noCheck).contactListsVoid, true);
	const noMember = img.clone();
	putContinuation(noMember.bytes, keysOf(noMember).filter((e) => e.key >= 0x1E00 && e.key !== LIST_KEY.check && e.key !== LIST_KEY.dmr && e.key !== LIST_KEY.air));
	const nm = decode(noMember);
	assert.equal(nm.contactListsVoid, false);
	assert.equal(nm.contactLists.length, 2);
	// a check that is not 4 bytes is void
	const short = img.clone();
	putContinuation(short.bytes, keysOf(short).filter((e) => e.key >= 0x1E00).map((e) => (e.key === LIST_KEY.check ? { key: e.key, value: e.value.slice(0, 3) } : e)));
	assert.equal(decode(short).contactListsVoid, true);
});

test("a contact's list moves with it when contacts are renumbered or removed", () => {
	const { m, tg, local } = sample();
	remove(m, "contacts", [tg]);
	local.number = null;	// takes the lowest free number when saved
	const d = decode(encode(m));
	const l = d.contacts.find((c) => c.name === "Local"), p = d.contacts.find((c) => c.name === "P25 TG");
	assert.deepEqual([l.number, l.list], [1, 1]);
	assert.deepEqual([p.number, p.list], [1, 1]);
	assert.equal(d.contacts.some((c) => c.name === "TG 91"), false);
	assert.deepEqual(val(encode(m), LIST_KEY.dmr), [1]);
});

test("a list can't be deleted while a system selects it; deleting one clears its contacts' membership", () => {
	const { m, tg, net, solo } = sample();
	const bm = m.contactLists[0];
	assert.deepEqual(listUsers(m, bm).map((u) => [u.system, u.slot]), [[net, 0]]);
	assert.equal(removeLists(m, [bm]), false);
	assert.equal(m.contactLists.length, 2);
	assert.equal(tg.list, 0);
	net.lists = [null, LIST_IDDB];
	assert.equal(removeLists(m, [bm]), true);
	assert.deepEqual(m.contactLists.map((l) => l.name), ["Club"]);
	assert.equal(tg.list, null);
	const d = decode(encode(m));
	assert.deepEqual(d.contactLists, [{ number: 1, name: "Club" }]);
	assert.equal(d.contacts.find((c) => c.name === "TG 91").list, null);
	// a selection that names a list that isn't there is an error
	solo.lists = [5, null];
	assert.ok(validate(m).some((p) => p.table === "systems" && p.code === "list_missing" && p.level === "error"));
});

test("deleting a system clears the channels' networks that point at it, and its selection key goes", () => {
	const { m, net, solo, ch, ch2 } = sample();
	remove(m, "systems", [net]);
	assert.equal(ch.network, null);
	assert.equal(ch2.network, solo);
	const img = encode(m);
	assert.equal(val(img, LIST_KEY.system + 8), null);
	assert.deepEqual(val(img, LIST_KEY.network), [0xFF, 9]);
	const d = decode(img);
	assert.equal(d.channels.find((c) => c.name === "Rpt").network, null);
	assert.equal(d.channels.find((c) => c.name === "Other").network.name, "Net B");
});

test("validation: names, list numbers, networks", () => {
	const { m, tg, ch } = sample();
	const codes = () => validate(m).filter((p) => ["contactLists", "contacts", "channels"].includes(p.table) && ["no_name", "too_long", "list_name_chars", "list_name_duplicate", "range", "list_missing", "network_type"].includes(p.code)).map((p) => [p.table, p.code, p.level]);
	assert.deepEqual(codes(), []);
	m.contactLists[0].name = "";
	assert.deepEqual(codes(), [["contactLists", "no_name", "error"]]);
	m.contactLists[0].name = "x".repeat(17);
	assert.deepEqual(codes(), [["contactLists", "too_long", "error"]]);
	m.contactLists[0].name = "a\tb";
	assert.deepEqual(codes(), [["contactLists", "list_name_chars", "error"]]);
	m.contactLists[0].name = "Club";
	assert.deepEqual(codes(), [["contactLists", "list_name_duplicate", "warning"], ["contactLists", "list_name_duplicate", "warning"]]);
	m.contactLists[0].name = "BM";
	tg.list = 7;
	assert.deepEqual(codes(), [["contacts", "list_missing", "warning"]]);
	tg.list = 40;
	assert.deepEqual(codes(), [["contacts", "range", "error"]]);
	tg.list = 0;
	ch.network = newSystem(m, 1, "T3");
	assert.deepEqual(codes(), [["channels", "network_type", "error"]]);
});

test("list names: a key holds 1 to 16 bytes with none below 0x20", () => {
	assert.equal(decodeListName(Uint8Array.from(text("Club"))), "Club");
	assert.equal(decodeListName(new Uint8Array(0)), null);
	assert.equal(decodeListName(Uint8Array.from(text("a".repeat(17)))), null);
	assert.equal(decodeListName(Uint8Array.of(0x41, 0x1F)), null);
	assert.equal(decodeListName(new TextEncoder().encode("Équipe")), "Équipe");
	assert.equal(listNameValid("Équipe"), true);
	assert.equal(listNameValid("é".repeat(9)), false, "16 bytes, not 16 letters");
	assert.deepEqual([null, "", "a".repeat(17), "a\tb", "Club", "a".repeat(16)].map(listNameProblem),
		["no_name", "no_name", "too_long", "list_name_chars", null, null]);
});

test("shortened values read 0xFF for what is missing, and unknown bytes read as none", () => {
	const { m } = sample();
	const img = encode(m);
	const keys = keysOf(img).filter((e) => e.key >= 0x1E00).map((e) => {
		// a DMR table longer than the slots in use, with a 0xFE (never stored in a table) for slot 2; an air table that
		// holds nothing; a selection whose primary is past the lists, with a longer value than two bytes
		if (e.key === LIST_KEY.dmr) return { key: e.key, value: Uint8Array.of(0, 0xFE, 1) };
		if (e.key === LIST_KEY.air) return { key: e.key, value: new Uint8Array(0) };
		if (e.key === LIST_KEY.system + 8) return { key: e.key, value: Uint8Array.of(0x20, 1, 9, 9) };
		return e;
	});
	const odd = img.clone();
	putContinuation(odd.bytes, keys);
	// the contact tables are as they were, so the check still holds
	const d = decode(odd);
	assert.equal(d.contactListsVoid, false);
	assert.deepEqual(d.contacts.map((c) => [c.name, c.list]), [["TG 91", 0], ["Local", null], ["P25 TG", null]]);
	assert.deepEqual(d.systems.find((s) => s.name === "Net A").lists, [null, 1], "bytes after the second are ignored");
});

test("which channels take a network: plain DMR and P25, not FM or one with a system of its own", () => {
	const m = empty();
	const dmr = newChannel({ name: "D", mode: "dmr" }), fm = newChannel({ name: "F", mode: "fm" }), p25 = newChannel({ name: "P", mode: "dmr" });
	m.channels.push(dmr, fm, p25);
	setKind(m, p25, 2);
	assert.deepEqual([takesNetwork(dmr), takesNetwork(fm), takesNetwork(p25)], [true, false, true]);
});

// --- CSV ---

test("CSV: the List column names the contact's list, and import assigns it", () => {
	const { m, tg, local } = sample();
	const rows = parse(exportTable(m, "contacts"));
	const col = rows[0].indexOf("List");
	assert.ok(col > 0);
	assert.deepEqual(rows.slice(1).map((r) => [r[1], r[col]]), [["TG 91", "BM"], ["Local", "Club"], ["P25 TG", "Club"]]);

	let r = importTable(m, "Name,ID,List\nTG 91,91,Club\nLocal,9,\n");
	assert.deepEqual(r.problems, []);
	assert.deepEqual([tg.list, local.list], [1, null]);
	// a list that isn't there is a problem and the contact keeps its list
	r = importTable(m, "Name,ID,List\nTG 91,91,Nope\nLocal,9,\n");
	assert.deepEqual(r.problems.map((p) => [p.column, p.code]), [["List", "unknown_ref"]]);
	assert.equal(tg.list, 1);
	// by number, and case does not matter for a name
	r = importTable(m, "Name,ID,List\nTG 91,91,#0\nLocal,9,bm\n");
	assert.deepEqual(r.problems, []);
	assert.deepEqual([tg.list, local.list], [0, 0]);
	// a file without the column leaves the lists alone
	r = importTable(m, "Name,ID\nTG 91,91\nLocal,9\n");
	assert.deepEqual([tg.list, local.list], [0, 0]);
	// two lists of one name are told apart by number
	m.contactLists[1].name = "BM";
	assert.deepEqual(parse(exportTable(m, "contacts")).slice(1).map((x) => x[col]), ["#0", "#0", "#1"]);
});

test("CSV: a List column assigns the contacts again after the lists went void", () => {
	const { m } = sample();
	const img = encode(m);
	encodeContact({ name: "TG 91", id: 92, type: "group", timeslot: 0 }, img.bytes.subarray(CONTACTS, CONTACTS + CONTACT_SIZE));
	const d = decode(img);
	assert.equal(d.contactListsVoid, true);
	// a blank column assigns nothing
	importTable(d, "Name,ID,List\nTG 91,92,\nLocal,9,\n");
	assert.equal(d.contactListsVoid, true);
	importTable(d, "Name,ID,List\nTG 91,92,BM\nLocal,9,Club\n");
	assert.equal(d.contactListsVoid, false);
	const out = encode(d);
	assert.deepEqual(val(out, LIST_KEY.check), specCheck(out.bytes));
	assert.deepEqual(val(out, LIST_KEY.dmr), [0, 1]);
});

test("CSV: the Network column names a channel's network", () => {
	const { m, ch, ch2, net, solo } = sample();
	const rows = parse(exportTable(m, "channels"));
	const col = rows[0].indexOf("Network");
	assert.ok(col > 0);
	assert.deepEqual(rows.slice(1).map((r) => [r[1], r[col]]), [["Rpt", "Net A"], ["Other", "Net B"]]);
	const r = importTable(m, "Name,RX Frequency,Network\nRpt,446.00625,Net B\nOther,446.00625,\n");
	assert.deepEqual(r.problems, []);
	assert.equal(ch.network, solo);
	assert.equal(ch2.network, null);
	const bad = importTable(m, "Name,RX Frequency,Network\nRpt,446.00625,Nowhere\nOther,446.00625,Net A\n");
	assert.deepEqual(bad.problems.map((p) => [p.column, p.code]), [["Network", "unknown_ref"]]);
	assert.equal(ch.network, solo);
	assert.equal(ch2.network, net);
	// only networks are named: a system of another type with the same name is not a candidate
	m.systems.push(Object.assign(newSystem(m, 1, "Net A"), { number: 1 }));
	const same = importTable(m, "Name,RX Frequency,Network\nRpt,446.00625,Net A\nOther,446.00625,\n");
	assert.deepEqual(same.problems, []);
	assert.equal(ch.network, net);
});

// --- Migration ---

test("migration: a codeplug with contacts and plain channels and no lists is offered the Default network", () => {
	assert.equal(needsContactListMigration(empty()), false);
	const m = empty();
	m.contacts.push(newContact({ name: "A", id: 1 }), newContact({ name: "P", id: 5, mode: "p25" }));
	const dmr = newChannel({ name: "D", mode: "dmr" }), fm = newChannel({ name: "F", mode: "fm" }), p25 = newChannel({ name: "P", mode: "dmr" });
	m.channels.push(dmr, fm, p25);
	setKind(m, p25, 2);
	const old = decode(encode(m));
	assert.equal(needsContactListMigration(old), true);

	const r = makeDefaultNetwork(old);
	assert.deepEqual([r.list.number, r.list.name, r.contacts, r.channels], [0, "Default", 2, 2]);
	assert.deepEqual([r.system.type, r.system.name, r.system.number, r.system.sharing, r.system.lists], [6, "Default", 9, 0, [0, LIST_IDDB]]);
	assert.ok(old.contacts.every((c) => c.list === 0));
	const net = (name) => old.channels.find((c) => c.name === name).network;
	assert.deepEqual([net("D") === r.system, net("F"), net("P") === r.system], [true, null, true]);
	assert.equal(needsContactListMigration(old), false);

	const img = encode(old);
	assert.deepEqual(val(img, LIST_KEY.network), [8, 0xFF, 8]);
	assert.deepEqual(val(img, LIST_KEY.system + 8), [0, 0xFE]);
	const d = decode(img);
	assert.deepEqual(d.contactLists, [{ number: 0, name: "Default" }]);
	assert.ok(d.contacts.every((c) => c.list === 0));
	const dn = d.systems.find((s) => s.type === 6);
	assert.deepEqual([dn.name, dn.lists], ["Default", [0, LIST_IDDB]]);
	assert.equal(d.channels.find((c) => c.name === "D").network, dn);
	assert.equal(d.channels.find((c) => c.name === "F").network, null);
	assert.equal(d.channels.find((c) => c.name === "P").network, dn);
	assert.equal(needsContactListMigration(d), false);
});

test("migration: a codeplug that writes no contact list key is on the old system, as the radio sees it", () => {
	const keys = (m) => keysOf(encode(m)).filter((e) => e.key >= 0x1E00 && e.key <= 0x20FF);
	const m = empty();
	m.contacts.push(newContact({ name: "A", id: 1 }));
	assert.equal(needsContactListMigration(m), true);
	assert.equal(keys(m).length, 0, "no key: the radio uses the old lookup");
	// a network that selects nothing and no channel uses writes no key: still the old system
	const net = newNetwork(m, "Idle");
	m.systems.push(net);
	assert.equal(isNetwork(net), true);
	assert.equal(needsContactListMigration(m), true);
	assert.equal(keys(m).length, 0);
	// any key ends it: a selection, a channel's network, a contact in a list, a list
	net.lists = [LIST_IDDB, null];
	assert.equal(needsContactListMigration(m), false);
	assert.ok(keys(m).length > 0);
	net.lists = [null, null];
	const ch = newChannel({ name: "C", mode: "dmr", network: net });
	m.channels.push(ch);
	assert.equal(needsContactListMigration(m), false);
	assert.ok(keys(m).length > 0);
	ch.network = null;
	m.contactLists = [{ number: 3, name: "Club" }];
	assert.equal(needsContactListMigration(m), false);
	assert.ok(keys(m).length > 0);
	m.contactLists = [];
	m.contacts[0].list = 3;
	assert.equal(needsContactListMigration(m), false);
	assert.ok(keys(m).length > 0);
	// a void membership keeps its keys
	m.contacts[0].list = null;
	m.contactListsVoid = true;
	assert.equal(needsContactListMigration(m), false);
	// nothing to name, nothing to migrate
	assert.equal(needsContactListMigration(empty()), false);
});

test("migration: a network takes a free number, and nothing changes when no list number is left", () => {
	const m = empty();
	const used = newSystem(m, 6, "X");
	used.number = 9;
	m.systems.push(used);
	m.contacts.push(newContact({ name: "A", id: 1 }));
	const r = makeDefaultNetwork(m);
	assert.equal(r.system.number, 10);
	const full = empty();
	full.contacts.push(newContact({ name: "A", id: 1 }));
	full.contactLists = Array.from({ length: 32 }, (_, i) => ({ number: i, name: `L${i}` }));
	assert.equal(makeDefaultNetwork(full), null);
	assert.equal(full.systems.length, 0);
	assert.equal(full.contacts[0].list, null);
	assert.equal(needsContactListMigration(full), false);
});
