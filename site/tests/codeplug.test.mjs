// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The codeplug editor's decoder and encoder (static/js/codeplug/), checked against the radio's own codeplug code on
// the host: tests/host/cpsSim stores a codeplug and lists what the radio reads (--detail), both ways. Fixtures from
// site/tests/fixtures.py; run with site/tests/run.sh.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, writeFileSync, mkdtempSync, rmSync } from "node:fs";
import { spawnSync } from "node:child_process";
import { join } from "node:path";
import { tmpdir } from "node:os";

import { CodeplugImage, CODEPLUG_SPAN } from "../static/js/codeplug/image.js";
import { decode, encode, empty, remove, validate, newChannel, newContact, refNumber } from "../static/js/codeplug/model.js";
import { getTone, putTone, parseTone, parseFreq, mhz, getCoord, coordBytes } from "../static/js/codeplug/fields.js";
import * as iddb from "../static/js/codeplug/iddb.js";

const DIR = process.env.ODR_SITE_FIXTURES || "build/site-tests";
const fx = JSON.parse(readFileSync(join(DIR, "codeplug.json"), "utf8"));
const simImage = () => CodeplugImage.from(new Uint8Array(readFileSync(join(DIR, "codeplug_sim.bin"))).slice(0, CODEPLUG_SPAN));

// What cpsSim --detail prints for a model, as the radio would read it (core/codeplug/codeplug.c)
const POWER_LEVELS = 9;	// cpsSim's board
function detail(m, ids = [], db = []) {
	const out = [];
	const g = m.general;
	out.push(`general name "${g.radioName}" id ${g.dmrId} boot "${g.bootLine1}" "${g.bootLine2}"`);
	const tone = (t) => (t ? (t.startsWith("D") ? t : t) : "-");
	const used = new Set(m.channels.map((c) => c.number));
	for (const c of [...m.channels].sort((a, b) => a.number - b.number)) {
		const power = c.power === 0 ? 255 : Math.min(c.power - 1, POWER_LEVELS - 1);
		const alias = (c.aliasTs1 & 2 ? 1 : 0) | (c.aliasTs2 & 2 ? 2 : 0);
		const b = (x) => (x ? 1 : 0);
		out.push(`channel ${c.number} "${c.name}" rx ${c.rxHz} tx ${c.txHz} mode ${b(c.mode === "dmr")} bw ${b(c.bw === 25)}` +
			` power ${power} tot ${c.tot} rxtone ${tone(c.rxTone)} txtone ${tone(c.txTone)} cc ${c.colorCode} ts ${c.timeslot}` +
			` contact ${refNumber(c.contact)} list ${refNumber(c.rxList)} id ${c.dmrId} dmo ${b(c.dmo)} rxonly ${b(c.rxOnly)}` +
			` skip ${b(c.zoneSkip)} vox ${b(c.vox)} nobeep ${b(c.noBeep)} noeco ${b(c.noEco)} roam ${b(c.roam)} alias ${alias}`);
	}
	m.zones.forEach((z, i) => {
		const chs = z.channels.map(refNumber).filter((n) => used.has(n));
		out.push(`zone ${i} "${z.name}":` + chs.map((n) => " " + n).join(""));
	});
	const types = { group: 0, private: 1, all: 2 };
	for (const c of [...m.contacts].sort((a, b) => a.number - b.number)) {
		out.push(`contact ${c.number} "${c.name}" id ${c.type === "all" ? 0xFFFFFF : c.id} type ${types[c.type]} ts ${c.timeslot}`);
	}
	for (const l of [...m.rxLists].sort((a, b) => a.number - b.number)) {
		out.push(`list ${l.number} "${l.name}":` + l.contacts.map((c) => " " + refNumber(c)).join(""));
	}
	out.push(`iddb count ${db.length}`);
	const byId = new Map(db.map((r) => [r.id, r.text]));
	for (const id of ids) out.push(byId.has(id) ? `iddb ${id} "${byId.get(id)}"` : `iddb ${id} -`);
	return out.join("\n") + "\n";
}

function simDetail(image, ids = []) {
	const d = mkdtempSync(join(tmpdir(), "odr-cp-"));
	try {
		const f = join(d, "image.bin");
		writeFileSync(f, image.bytes);
		const args = ["--image", f, "--detail", ...ids.flatMap((id) => ["--iddb", String(id)])];
		const r = spawnSync(fx.cpssim, args, { encoding: "utf8" });
		assert.equal(r.status, 0, r.stderr);
		return r.stdout;
	} finally {
		rmSync(d, { recursive: true, force: true });
	}
}

test("tone, frequency and coordinate fields", () => {
	const b = new Uint8Array(2);
	for (const t of ["", "67.0", "88.5", "254.1", "D023N", "D023I", "D754N"]) {
		putTone(b, 0, t);
		assert.equal(getTone(b, 0), t);
	}
	putTone(b, 0, "88.5");
	assert.deepEqual([...b], [0x85, 0x08]);	// BCD tenths, LE (open-firmware-cps.md 7.3.3)
	putTone(b, 0, "D023I");
	assert.deepEqual([...b], [0x23, 0xC0]);
	assert.equal(parseTone("d23"), "D023N");
	assert.equal(parseTone("100"), "100.0");
	assert.equal(parseTone("D089N"), null);
	assert.equal(parseTone("20"), null);
	assert.equal(parseFreq("446.00625"), 446006250);
	assert.equal(parseFreq("438500000"), 438500000);
	assert.equal(parseFreq("x"), null);
	assert.equal(mhz(446006250), "446.00625");
	assert.equal(mhz(438500000), "438.500");
	for (const x of [0, 33.7489, -84.3880, -0.0001]) {
		const [h, mi, l] = coordBytes(x);
		assert.ok(Math.abs(getCoord(h, mi, l) - x) < 0.00005);
	}
});

test("the radio's stored codeplug decodes as the radio reads it", () => {
	const m = decode(simImage());
	assert.equal(m.channels.length, 40);
	assert.equal(detail(m), fx.detail);
});

test("an unedited codeplug encodes to the same bytes", () => {
	const img = simImage();
	const out = encode(decode(img));
	assert.deepEqual(out.bytes, img.bytes);
	const blank = CodeplugImage.blank();
	assert.deepEqual(encode(decode(blank)).bytes, blank.bytes);
});

// A codeplug using every field the radio reads, built in the editor's model
function rich() {
	const m = empty();
	m.general = { ...m.general, radioName: "N0ODR", dmrId: 3101276, bootLine1: "Open", bootLine2: "Digital Radio" };
	const tg91 = newContact({ name: "Worldwide", id: 91, type: "group" });
	const tg3100 = newContact({ name: "USA", id: 3100, type: "group", timeslot: 1 });
	const priv = newContact({ name: "N4NQV", id: 3101276, type: "private", timeslot: 2 });
	const all = newContact({ name: "All Call", type: "all" });
	m.contacts.push(tg91, tg3100, priv, all);
	const list = { number: null, name: "Local", contacts: [tg91, tg3100] };
	m.rxLists.push(list);
	const chs = [
		newChannel({ name: "Simplex", rxHz: 146520000, txHz: 146520000, bw: 25, rxTone: "100.0", txTone: "D023I", power: 9 }),
		newChannel({ name: "Repeater", rxHz: 442100000, txHz: 447100000, txTone: "88.5", tot: 180, vox: true, zoneSkip: true }),
		newChannel({ name: "DMR TG91", rxHz: 438500000, txHz: 438500000, mode: "dmr", colorCode: 7, timeslot: 2,
			contact: tg91, rxList: list, dmrId: 3101277, dmo: true, noBeep: true, noEco: true, aliasTs1: 2, aliasTs2: 3 }),
		newChannel({ name: "RX only", rxHz: 162550000, txHz: 162550000, rxOnly: true, power: 1 }),
	];
	m.channels.push(...chs);
	const far = newChannel({ name: "Bank 3", rxHz: 430000000, txHz: 430000000, number: 300 });
	m.channels.push(far);
	m.zones.push({ name: "Home", channels: [chs[0], chs[2], far] }, { name: "Weather", channels: [chs[3]] });
	return m;
}

test("an edited codeplug is read by the radio as written", () => {
	const m = rich();
	const img = encode(m);
	assert.equal(simDetail(img), detail(m));
	// and decodes back to the same model
	assert.equal(detail(decode(img)), detail(m));
	assert.equal(m.channels[4].number, 300);
	assert.equal(m.contacts.map((c) => c.number).join(), "1,2,3,4");
});

test("an edit rewrites only what changed", () => {
	const img = encode(rich());
	const m = decode(img);
	m.channels.find((c) => c.name === "Repeater").txTone = "123.0";
	const out = encode(m).bytes;
	const diff = [];
	for (let i = 0; i < out.length; i++) if (out[i] !== img.bytes[i]) diff.push(i);
	assert.ok(diff.length > 0 && diff.length <= 2, `changed bytes: ${diff.length}`);
});

test("deleting records clears every reference to them", () => {
	const m = decode(encode(rich()));
	const tg91 = m.contacts.find((c) => c.id === 91);
	remove(m, "contacts", [tg91]);
	const dmr = m.channels.find((c) => c.mode === "dmr");
	assert.equal(dmr.contact, null);
	assert.equal(m.rxLists[0].contacts.length, 1);
	remove(m, "channels", [dmr]);
	assert.deepEqual(m.zones[0].channels.map(refNumber), [1, 300]);
	const img = encode(m);
	assert.equal(simDetail(img), detail(m));
});

test("validation finds the problems the radio would refuse", () => {
	const m = rich();
	assert.equal(validate(m).filter((p) => p.level === "error").length, 0);
	m.channels[0].rxHz = 0;
	m.channels[1].txTone = "12";
	m.contacts[0].id = 0;
	m.zones[0].channels = Array(81).fill(m.channels[0]);
	const codes = validate(m).filter((p) => p.level === "error").map((p) => p.code).sort();
	assert.deepEqual(codes, ["bad_freq", "bad_tone", "id_range", "too_many"]);
});

test("the DMR ID database is found by the radio across both areas", () => {
	const img = CodeplugImage.blank();
	const records = [];
	for (let i = 0; i < 17500; i++) records.push({ id: 1000000 + 7 * i, text: `K${String(i).padStart(5, "0")} Op${i % 10}` });
	records.push({ id: 3101276, text: "N4NQV Spencer Fowler Atlanta GA" }, { id: 3101276, text: "duplicate" });
	records.push({ id: 2, text: "Ünïcode/Ä-test" });
	const r = iddb.write(img, records, { textLen: 16 });
	assert.equal(r.count, 17502);
	assert.equal(r.rec, 15);
	assert.ok(img.size > CODEPLUG_SPAN || r.top <= CODEPLUG_SPAN);
	const ids = [2, 1000000, 1000000 + 7 * 17474, 1000000 + 7 * 17475, 1000000 + 7 * 17499, 3101276, 1000001];
	const got = simDetail(img, ids).split("\n").filter((l) => l.startsWith("iddb"));
	assert.deepEqual(got, [
		"iddb count 17502",
		'iddb 2 "Unicode A test"',
		'iddb 1000000 "K00000 Op0"',
		`iddb ${1000000 + 7 * 17474} "K17474 Op4"`,
		`iddb ${1000000 + 7 * 17475} "K17475 Op5"`,
		`iddb ${1000000 + 7 * 17499} "K17499 Op9"`,
		'iddb 3101276 "N4NQV Spencer Fo"',
		"iddb 1000001 -",
	]);
	const back = iddb.read(img.bytes);
	assert.equal(back.length, 17502);
	assert.deepEqual(back[0], { id: 2, text: "Unicode A test" });
	assert.deepEqual(iddb.readHeader(img.bytes), { format: "N", rec: 15, count: 17502, textLen: 16 });

	// A record size no editor text length gives (qdmr takes any): 41 bytes = 50 whole characters and 4 spare bits.
	// The radio rounds down, and a read must not run into the next record
	const odd = CodeplugImage.from(new Uint8Array(0));
	iddb.write(odd, [{ id: 1, text: "A".repeat(48) }, { id: 2, text: "B".repeat(48) }], { textLen: 48 });
	const oh = odd.bytes.subarray(iddb.AREA1, iddb.AREA1 + iddb.HEADER);
	const rec39 = odd.bytes.slice(iddb.AREA1 + iddb.HEADER, iddb.AREA1 + iddb.HEADER + 2 * 39);
	oh[3] = 41 + 0x4A;
	odd.bytes.fill(0, iddb.AREA1 + iddb.HEADER, iddb.AREA1 + iddb.HEADER + 2 * 41);
	odd.bytes.set(rec39.subarray(0, 39), iddb.AREA1 + iddb.HEADER);
	odd.bytes.set(rec39.subarray(39), iddb.AREA1 + iddb.HEADER + 41);
	assert.equal(iddb.readHeader(odd.bytes).textLen, 50);
	assert.deepEqual(iddb.read(odd.bytes), [{ id: 1, text: "A".repeat(48) }, { id: 2, text: "B".repeat(48) }]);
	// the room a radio has bounds it
	const small = CodeplugImage.blank();
	const cut = iddb.write(small, records, { textLen: 48, end: iddb.AREA2 + 39 * 10 });
	assert.equal(cut.count, iddb.capacity(39, iddb.AREA2 + 39 * 10));
	assert.ok(cut.dropped > 0);
});

test("longer database text is kept up to 48 characters", () => {
	const img = CodeplugImage.blank();
	iddb.write(img, [{ id: 3101276, text: "N4NQV Spencer Fowler Atlanta Georgia United States" }], { textLen: 48 });
	const got = simDetail(img, [3101276]).split("\n").filter((l) => l.startsWith("iddb 3"));
	assert.deepEqual(got, ['iddb 3101276 "N4NQV Spencer Fowler Atlanta Georgia United Stat"']);
});

// --- Reaching the radio (transport.js) against a simulated radio ---

import { CpsSerial, changes, writeRanges, TransportError } from "../static/js/codeplug/transport.js";
import { FakeCpsRadio } from "../static/js/codeplug/fakecps.js";

const RADIOS = [
	{ id: "dm1701", codeplug: { base: 0, end: 0xE00000, names: ["DM-1701", "dm1701"] } },
	{ id: "dm32", codeplug: { base: 0x700000, end: 0xF00000, names: ["DM-32UV", "dm32"] } },
];

test("a radio with its codeplug at a base reads and writes at that base", async () => {
	const start = encode(rich());
	const radio = new FakeCpsRadio({ board: "DM-32UV", base: 0x700000, end: 0xF00000, image: start.bytes, cable: true });
	radio.spi.fill(0x5A, 0x700000 + 0x604B, 0x700000 + 0x6100);	// the radio's own settings block
	const tr = new CpsSerial(radio.stream(), { cable: true });
	const { radio: r } = await tr.identify(RADIOS);
	assert.equal(r.id, "dm32");
	const read = await tr.read();
	const m = decode(read);
	assert.equal(detail(m), detail(decode(start)));

	m.channels.find((c) => c.name === "Repeater").name = "Repeater 2";
	m.contacts.push(newContact({ name: "Local 9", id: 9 }));
	const want = encode(m);
	want.bytes.fill(0x00, 0x604B, 0x6060);	// a file's stale settings bytes must never reach the radio
	const res = await tr.write(want, read);
	assert.ok(res.bytes < 64, `bytes sent: ${res.bytes}`);
	assert.ok(radio.writes.every(([a]) => a >= 0x700000), "every write at the base");
	await tr.verify(want, res.plan);
	await tr.finish();
	assert.equal(radio.reloads, 1);
	assert.ok(radio.spi.subarray(0x700000 + 0x604B, 0x700000 + 0x6100).every((x) => x === 0x5A), "settings block kept");
	assert.equal(detail(decode(await tr.read())), detail(m));
});

test("an ID database goes to the radio and nothing past its room", async () => {
	const radio = new FakeCpsRadio({ board: "dm1701" });
	const tr = new CpsSerial(radio.stream());
	await tr.identify(RADIOS);
	const img = encode(rich());
	const recs = Array.from({ length: 20000 }, (_, i) => ({ id: 3100000 + i, text: `W${i} Name` }));
	iddb.write(img, recs, { textLen: 32 });
	await tr.write(img, null, { iddb: true });
	const back = await tr.read();
	assert.equal(iddb.read(back.bytes).length, 20000);
	assert.deepEqual(iddb.read(back.bytes)[19999], { id: 3119999, text: "W19999 Name" });
	const tiny = new CpsSerial(new FakeCpsRadio({ board: "dm1701" }).stream());
	await tiny.identify([{ id: "x", codeplug: { base: 0, end: 0xD9000, names: ["dm1701"] } }]);
	await assert.rejects(tiny.write(img, null, { iddb: true }), (e) => e instanceof TransportError && e.code === "iddb_too_big");
});

test("a refused write and an unknown radio are reported", async () => {
	const radio = new FakeCpsRadio({ board: "DM-1701", end: 0x30000 });	// no room for the flash codeplug
	const tr = new CpsSerial(radio.stream());
	await tr.identify(RADIOS);
	await assert.rejects(tr.write(encode(rich()), null), (e) => e.code === "write_refused");
	const other = new CpsSerial(new FakeCpsRadio({ board: "GD-77" }).stream());
	await assert.rejects(other.identify(RADIOS), (e) => e.code === "unknown_radio" && e.params.board === "GD-77");
});

test("changed runs stay inside their ranges and sectors", () => {
	const a = new Uint8Array(0x3000), b = a.slice();
	b[0x0FFE] = 1;
	b[0x1001] = 2;
	b[0x1010] = 3;
	b[0x2500] = 4;
	const plan = changes(b, a, [[0, 0x2000]]);
	assert.deepEqual(plan.map(([s, runs]) => [s, runs.map(([x, d]) => [x, d.length])]), [[0, [[0x0FFE, 1]]], [0x1000, [[0x1001, 16]]]]);
	assert.ok(writeRanges().every(([x, y]) => y <= 0x604B || x >= 0x6060));
});

import { tftpBytes } from "../static/js/editor/files.js";
import { TFTP_SIZE } from "../static/js/codeplug/image.js";

test("codeplug.bin for USB network mode carries the ID database when there is one", () => {
	const plain = CodeplugImage.from(new Uint8Array(0));
	assert.equal(tftpBytes(plain).length, TFTP_SIZE);
	const withDb = CodeplugImage.from(new Uint8Array(0));
	const recs = Array.from({ length: 40000 }, (_, i) => ({ id: 3100000 + i, text: `W${i} Name` }));
	const r = iddb.write(withDb, recs, { textLen: 16 });
	assert.ok(r.top > CODEPLUG_SPAN);	// area 2 in use: past the cps_client image size
	const out = tftpBytes(withDb);
	assert.equal(out.length, r.top);
	assert.deepEqual(iddb.readHeader(out), { format: "N", rec: 15, count: 40000, textLen: 16 });
	assert.deepEqual(iddb.read(out)[39999], { id: 3139999, text: "W39999 Name" });
	assert.deepEqual(out.subarray(0, TFTP_SIZE), withDb.bytes.subarray(0, TFTP_SIZE));
	// a small database (area 1 only, inside the plain size) keeps the plain size
	const small = CodeplugImage.from(new Uint8Array(0));
	iddb.write(small, recs.slice(0, 10), { textLen: 16 });
	assert.equal(tftpBytes(small).length, TFTP_SIZE);
});
