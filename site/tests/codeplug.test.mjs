// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The codeplug editor's decoder and encoder (static/js/codeplug/), checked against the radio's own codeplug code on
// the host: tests/host/cpsSim stores a codeplug and lists what the radio reads (--detail), both ways. Fixtures from
// site/tests/fixtures.py; run with site/tests/run.sh.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, writeFileSync, mkdtempSync, rmSync } from "node:fs";
import { spawnSync, spawn } from "node:child_process";
import { join } from "node:path";
import { tmpdir } from "node:os";

import { CodeplugImage, CODEPLUG_SPAN } from "../static/js/codeplug/image.js";
import { decode, encode, empty, remove, validate, newChannel, newContact, refNumber, listenBand } from "../static/js/codeplug/model.js";
import { getTone, putTone, parseTone, parseFreq, mhz, getCoord, coordBytes } from "../static/js/codeplug/fields.js";
import * as iddb from "../static/js/codeplug/iddb.js";
import { targets } from "../static/js/flasher/restore.js";

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
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

// cpsSim on a flash image a simulated radio left (simRadio's close)
function simDetailFlash(flash, ids = []) {
	return simDetail({ bytes: flash }, ids);
}

function simRadioFrom(flash) {
	const d = mkdtempSync(join(tmpdir(), "rfoundry-img-"));
	const f = join(d, "flash.bin");
	writeFileSync(f, flash);
	const s = simRadio(["--image", f]);
	const close = s.close;
	s.close = async () => {
		const out = await close();
		rmSync(d, { recursive: true, force: true });
		return out;
	};
	return s;
}

function simDetail(image, ids = []) {
	const d = mkdtempSync(join(tmpdir(), "rfoundry-cp-"));
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
	m.general = { ...m.general, radioName: "RD0FDY", dmrId: 3101276, bootLine1: "Radio", bootLine2: "Foundry" };
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

test("the air band and broadcast FM are receive only: a TX frequency there without RX Only, or a DMR channel, is warned", () => {
	assert.deepEqual([64990000, 65000000, 107990000, 108000000, 136990000, 137000000].map(listenBand), [null, "bcast", "bcast", "air", "air", null]);
	const m = empty();
	const ch = (name, rxHz, mode, rxOnly) => newChannel({ name, rxHz, txHz: rxHz, mode, rxOnly });
	m.channels.push(ch("c0", 118100000, "fm", true), ch("c1", 98500000, "fm", false), ch("c2", 121500000, "dmr", true), ch("c3", 146520000, "fm", false));
	const warned = validate(m).filter((p) => p.code.startsWith("listen_")).map((p) => [p.item.name, p.code]);
	assert.deepEqual(warned, [["c1", "listen_tx_bcast"], ["c2", "listen_dmr_air"]]);
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

import { CpsSerial, changes, writeRanges, TransportError, keepIdentity } from "../static/js/codeplug/transport.js";
import { concat } from "../static/js/flasher/bytes.js";
import { FakeCpsRadio } from "../static/js/codeplug/fakecps.js";
import { updateFor } from "../static/js/flasher/version.js";

const RADIOS = [
	{ id: "dm1701", codeplug: { base: 0, end: 0xE00000, names: ["DM-1701", "dm1701"] } },
	{ id: "dm32", codeplug: { base: 0x700000, end: 0xF00000, names: ["DM-32UV", "dm32"] } },
	{ id: "sim", codeplug: { base: 0, end: 0xE00000, names: ["sim"] } },
];

// cpsSim as a radio on a serial port: its stdin and stdout as a stream (buf, clear, take, write, close). The flash is
// dumped to a file when the stream closes. drop(frame bytes) -> true loses that write on the way, as an overrun would
function simRadio(args = [], { drop = null } = {}) {
	const d = mkdtempSync(join(tmpdir(), "rfoundry-fast-"));
	const dump = join(d, "flash.bin");
	const p = spawn(fx.cpssim, [...args, "--dump", dump], { stdio: ["pipe", "pipe", "inherit"] });
	const s = {
		buf: new Uint8Array(0),
		clear() { s.buf = new Uint8Array(0); },
		take(n) {
			const o = s.buf.slice(0, n);
			s.buf = s.buf.slice(n);
			return o;
		},
		async write(data) {
			if (drop?.(data)) return;
			p.stdin.write(data);
		},
		// the flash as the radio left it
		async close() {
			const done = new Promise((r) => p.on("close", r));
			p.stdin.end();
			await done;
			const flash = new Uint8Array(readFileSync(dump));
			rmSync(d, { recursive: true, force: true });
			return flash;
		},
	};
	p.stdout.on("data", (b) => { s.buf = concat(s.buf, new Uint8Array(b)); });
	return s;
}

const flashImage = (flash) => CodeplugImage.from(flash.slice(0, CODEPLUG_SPAN));

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

test("the radio's release reaches the editor, which offers a newer one (editor update notice)", async () => {
	const tr = new CpsSerial(new FakeCpsRadio({ board: "DM-1701", release: "v26.8" }).stream());
	await tr.identify(RADIOS);
	assert.equal(tr.probe.version.release, "v26.8");
	const releases = [{ board: tr.radio.id, version: "v26.10", variant: "full" }, { board: tr.radio.id, version: "v26.11", variant: "fm" }];
	assert.equal(updateFor(releases, tr.radio.id, tr.probe.version.release)?.version, "v26.10");
	const dev = new CpsSerial(new FakeCpsRadio({ board: "DM-1701" }).stream());
	await dev.identify(RADIOS);
	assert.equal(dev.probe.version.release, undefined, "a developer's build names no release");
	const built = new CpsSerial(new FakeCpsRadio({ board: "DM-1701", gitTime: 1000 }).stream());
	await built.identify(RADIOS);
	assert.equal(built.probe.version.git_time, "1000");
	assert.equal(updateFor([{ board: built.radio.id, version: "v26.10", git_time: 2000 }], built.radio.id, built.probe.version.release,
		Number(built.probe.version.git_time))?.version, "v26.10");
});

test("an ID database goes to the radio and nothing past its room", async () => {
	const radio = new FakeCpsRadio({ board: "dm1701" });
	const tr = new CpsSerial(radio.stream());
	await tr.identify(RADIOS);
	const img = encode(rich());
	const recs = Array.from({ length: 20000 }, (_, i) => ({ id: 3100000 + i, text: `W${i} Name` }));
	iddb.write(img, recs, { textLen: 32 });
	await tr.write(img, null, { iddb: true });
	// its header erased before anything else of it and its sector written last (iddb-compact.md 4.3): a write cut
	// short leaves no database, never a new header over old records
	const db = radio.writes.filter(([a]) => a >= iddb.AREA1);
	assert.deepEqual(db[0], [iddb.AREA1, 32], "the header erased first");
	assert.ok(db[db.length - 1][0] < iddb.AREA1 + 4096, "the header's sector last");
	const steps = [];
	const back = await tr.read({ progress: (n, total) => steps.push([n, total]) });
	assert.ok(steps.every(([n, t], i) => t === steps[0][1] && (!i || n >= steps[i - 1][0])), "CPS read progress");
	assert.equal(steps.at(-1)[0], steps[0][1]);
	assert.equal(iddb.read(back.bytes).length, 20000);
	assert.deepEqual(iddb.read(back.bytes)[19999], { id: 3119999, text: "W19999 Name" });
	// the codeplug first, the database after it (a read that offers the database): the same bytes
	const part = await tr.read({ iddb: false });
	assert.ok(tr.iddbBytes(part) > 20000 * 24, `database bytes ${tr.iddbBytes(part)}`);
	await tr.readIddb(part);
	assert.deepEqual(part.bytes, back.bytes);
	const tiny = new CpsSerial(new FakeCpsRadio({ board: "dm1701" }).stream());
	await tiny.identify([{ id: "x", codeplug: { base: 0, end: 0xD9000, names: ["dm1701"] } }]);
	await assert.rejects(tiny.write(img, null, { iddb: true }), (e) => e instanceof TransportError && e.code === "iddb_too_big");
});

test("a radio swapped on the cable is told apart by its chip UID", async () => {
	// one cable, two radios: the stream answers for whichever is plugged in
	const a = new FakeCpsRadio({ board: "DM-1701", image: encode(rich()).bytes, uid: 0x11 });
	const other = rich();
	other.channels[0].name = "Other radio";
	const b = new FakeCpsRadio({ board: "DM-1701", image: encode(other).bytes, uid: 0x22 });
	let on = a;
	const sa = a.stream(), sb = b.stream();
	const cable = {
		get buf() { return (on === a ? sa : sb).buf; },
		clear() { (on === a ? sa : sb).clear(); },
		take(n) { return (on === a ? sa : sb).take(n); },
		write(d) { return (on === a ? sa : sb).write(d); },
		async close() {},
	};
	const tr = new CpsSerial(cable);
	const uidA = await tr.who(RADIOS);
	const seen = await tr.read({ iddb: false });
	on = b;
	const uidB = await tr.who(RADIOS);
	assert.ok(uidA && uidB && uidA !== uidB);
	// what a write against the first radio's bytes would leave on the second: the channel it never sent
	const want = encode(rich());
	const stale = await tr.plan(want, seen);
	assert.equal(stale.bytes, 0, "nothing differs from the first radio's bytes");
	const res = await tr.write(want, await tr.read({ iddb: false }));
	assert.ok(res.bytes > 0);
	assert.equal(detail(decode(await tr.read({ iddb: false }))), detail(decode(want)));
});

test("one codeplug into several radios keeps each radio's name, DMR ID and boot text", async () => {
	const own = rich();
	Object.assign(own.general, { radioName: "N0CALL", dmrId: 3101234, bootLine1: "Unit 7", bootLine2: "Fleet" });
	const radio = new FakeCpsRadio({ board: "DM-1701", image: encode(own).bytes });
	const tr = new CpsSerial(radio.stream());
	await tr.identify(RADIOS);
	const have = await tr.read({ iddb: false });
	const fleet = rich();
	Object.assign(fleet.general, { radioName: "BASE", dmrId: 3100001, bootLine1: "Base", bootLine2: "" });
	fleet.channels[0].name = "Fleet ch 1";
	const file = encode(fleet), want = keepIdentity(file, have);
	assert.ok((await tr.plan(want, have)).bytes < (await tr.plan(file, have)).bytes);
	const res = await tr.write(want, have);
	await tr.verify(want, res.plan);
	const back = decode(await tr.read({ iddb: false }));
	assert.deepEqual([back.general.radioName, back.general.dmrId, back.general.bootLine1, back.general.bootLine2],
		["N0CALL", 3101234, "Unit 7", "Fleet"]);
	assert.equal(back.channels[0].name, "Fleet ch 1");
	assert.equal(back.general.groupHang, fleet.general.groupHang);
});

test("the fast protocol reads what the CPS reads, and writes only the blocks that changed", async () => {
	const s = simRadio(["--populate"]);
	const tr = new CpsSerial(s);
	await tr.identify(RADIOS);
	assert.ok(tr.fast && tr.fast.stageMax > 0, "INFO");
	assert.ok(tr.compact, "IDDB_COMPACT listed");
	const fast = await tr.read({ iddb: false });
	tr.fast = null;
	const cps = await tr.read({ iddb: false });
	await tr.identify(RADIOS);
	assert.equal(detail(decode(fast)), detail(decode(cps)));
	const before = tr.fl.wire;
	const again = await tr.read({ iddb: false, cache: fast });		// nothing changed: SUMS only
	assert.ok(tr.fl.wire - before < 1000, `cached read moved ${tr.fl.wire - before} bytes`);
	assert.deepEqual(again.bytes, fast.bytes);

	const m = decode(fast);
	m.channels[0].name = "Fast 1";
	const want = encode(m);
	const plan = await tr.plan(want, fast);
	assert.equal(plan.blocks, 1, "one channel: one block");
	const res = await tr.write(want, fast);
	await tr.verify(want, res.plan);
	await tr.finish();
	assert.equal(simDetailFlash(await s.close()), detail(m));
});

test("a DATA frame lost in a window sends the block again", async () => {
	let lost = 0;
	const s = simRadio(["--populate"], { drop: (f) => f.length > 64 * 3 && lost++ === 0 });	// a window of DATA frames
	const tr = new CpsSerial(s);
	await tr.identify(RADIOS);
	const have = await tr.read({ iddb: false });
	const m = decode(have);
	for (const c of m.channels) c.name = `Again ${c.number}`;
	const want = encode(m);
	const res = await tr.write(want, have);
	await tr.verify(want, res.plan);
	await tr.finish();
	assert.equal(lost, 1);
	assert.equal(simDetailFlash(await s.close()), detail(m));
});

test("a format C database written fast is read by the radio; an older radio gets the CPS", async () => {
	const recs = Array.from({ length: 30000 }, (_, i) => ({ id: 3100000 + 3 * i, text: `K${i} ${["Bob", "Ann", "Joe"][i % 3]} Atlanta GA` }));
	const s = simRadio(["--populate"]);
	const tr = new CpsSerial(s);
	await tr.identify(RADIOS);
	const have = await tr.read({ iddb: false });
	const want = have.clone();
	iddb.write(want, recs, { format: "C" });
	const res = await tr.write(want, have, { iddb: true });
	await tr.verify(want, res.plan);
	await tr.finish();
	const flash = await s.close();
	const ids = [3100000, 3100000 + 3 * 29999, 3100001];
	const got = simDetailFlash(flash, ids).split("\n").filter((l) => l.startsWith("iddb"));
	assert.deepEqual(got, ["iddb count 30000", 'iddb 3100000 "K0 Bob Atlanta GA"', `iddb ${3100000 + 3 * 29999} "K29999 Joe Atlanta GA"`, "iddb 3100001 -"]);

	// the codeplug first, the database after it, fast: the same bytes as one read
	const s4 = simRadioFrom(flash);
	const tr4 = new CpsSerial(s4);
	await tr4.identify(RADIOS);
	// one pass: the total known from the start, never going back
	const steps = [];
	const whole = await tr4.read({ progress: (n, total) => steps.push([n, total]) });
	assert.ok(steps.length > 10 && steps.every(([n, t], i) => t === steps[0][1] && n <= t && (!i || n >= steps[i - 1][0])), "fast read progress");
	assert.equal(steps.at(-1)[0], steps[0][1]);
	const part = await tr4.read({ iddb: false });
	assert.ok(tr4.iddbBytes(part) > 0);
	await tr4.readIddb(part);
	assert.deepEqual(part.bytes, whole.bytes);
	await s4.close();

	// refreshed: one record more near the start changes few pages
	const again = flashImage(flash);
	const more = again.clone();
	iddb.write(more, [...recs, { id: 3100001, text: "N0NEW New" }], { format: "C" });
	const s3 = simRadioFrom(flash);
	const tr3 = new CpsSerial(s3);
	await tr3.identify(RADIOS);
	const p = await tr3.plan(more, again, { iddb: true });
	assert.ok(p.blocks <= 4, `blocks for one new record: ${p.blocks}`);
	await s3.close();

	const old = simRadio(["--populate", "--features", "FM,DMR"]);
	const tr2 = new CpsSerial(old);
	await tr2.identify(RADIOS);
	assert.equal(tr2.fast, null);
	assert.equal(tr2.compact, false);
	const h2 = await tr2.read({ iddb: false });
	const m2 = decode(h2);
	m2.channels[0].name = "Old radio";
	const w2 = encode(m2);
	const r2 = await tr2.write(w2, h2);
	await tr2.verify(w2, r2.plan);
	await tr2.finish();
	assert.equal(simDetailFlash(await old.close()), detail(m2));
});

test("a refused write and an unknown radio are reported", async () => {
	const radio = new FakeCpsRadio({ board: "DM-1701", end: 0x30000 });	// no room for the flash codeplug
	const tr = new CpsSerial(radio.stream());
	await tr.identify(RADIOS);
	await assert.rejects(tr.write(encode(rich()), null), (e) => e.code === "write_refused");
	const other = new CpsSerial(new FakeCpsRadio({ board: "GD-77" }).stream());
	await assert.rejects(other.identify(RADIOS), (e) => e.code === "unknown_radio" && e.params.board === "GD-77");
});

test("a radio still holding another firmware's settings data is prepared, then written", async () => {
	const radio = new FakeCpsRadio({ board: "DM-1701", foreign: true });
	const tr = new CpsSerial(radio.stream());
	await tr.identify(RADIOS);
	assert.equal((await tr.storage()).region, "foreign");
	await assert.rejects(tr.write(encode(rich()), null), (e) => e.code === "storage_foreign");
	assert.equal(await tr.claimStorage(), true);
	assert.equal(radio.claims, 1);
	assert.equal((await tr.storage()).region, "ours");
	const want = encode(rich());
	const res = await tr.write(want, null);
	await tr.verify(want, res.plan);
});

test("a sector left pending by an interrupted write is retried; verify skips the live VFOs", async () => {
	const radio = new FakeCpsRadio({ board: "DM-1701" });
	const tr = new CpsSerial(radio.stream());
	await tr.identify(RADIOS);
	radio.sector = new Uint8Array(4096);	// a session that stopped between X 1 and X 3
	radio.pending = 0x5000;
	const want = encode(rich());
	const res = await tr.write(want, null);
	radio.spi[0x7590] ^= 0xFF;		// the radio shows its live VFO A there, not the bytes written
	radio.spi[0x75FF] ^= 0xFF;
	await tr.verify(want, res.plan);
	radio.spi[0x7600] ^= 0xFF;
	await assert.rejects(tr.verify(want, res.plan), (e) => e.code === "verify_failed" && e.params.addr === 0x7600);
});

test("a backup's codeplug is offered for restore only in RadioFoundry's layout", () => {
	const bundle = (data) => ({ meta: { firmware_now: { kind: "rfoundry" } }, parts: new Map([["spi-flash", { data }]]) });
	assert.deepEqual(targets(bundle(encode(rich()).bytes)), ["codeplug"]);
	const noise = new Uint8Array(CODEPLUG_SPAN);
	for (let i = 0; i < noise.length; i++) noise[i] = (i * 7919 + (i >> 5)) & 0xFF;
	assert.deepEqual(targets(bundle(noise)), []);
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

test("a format C database is read by the radio's lookup: codes, literals, empty tokens, both areas", () => {
	const first = ["Bob", "Ann", "Joe", "Sue"], cities = Array.from({ length: 300 }, (_, k) => `City${k} GA`);
	const recs = [];
	for (let i = 0; i < 90000; i++) recs.push({ id: 1000000 + 11 * i, text: `K${i} ${first[i % 4]} ${cities[i % 300]}` });
	recs.push({ id: 7, text: "W1AW" }, { id: 8, text: "N0CALL  Spaced" }, { id: 9, text: "X".repeat(60) }, { id: 10, text: "" });
	const img = CodeplugImage.blank();
	const r = iddb.write(img, recs, { format: "C" });
	assert.equal(r.format, "C");
	assert.equal(r.count, 90004);
	assert.ok(iddb.extent(img.bytes) > iddb.AREA2, "area 2 in use");
	const h = iddb.readHeader(img.bytes);
	assert.equal(h.format, "C");
	assert.ok(h.dict > 128, "2-byte codes in use");
	const last = 1000000 + 11 * 89999;
	const ids = [7, 8, 9, 10, 1000000, 1000000 + 11 * 45000, last, 1000001, last + 1];
	const got = simDetail(img, ids).split("\n").filter((l) => l.startsWith("iddb"));
	assert.deepEqual(got, [
		"iddb count 90004", 'iddb 7 "W1AW"', 'iddb 8 "N0CALL Spaced"', `iddb 9 "${"X".repeat(48)}"`, 'iddb 10 ""',
		'iddb 1000000 "K0 Bob City0 GA"', `iddb ${1000000 + 11 * 45000} "K45000 Bob City0 GA"`,
		`iddb ${last} "K89999 Sue City299 GA"`, "iddb 1000001 -", `iddb ${last + 1} -`,
	]);
	const back = iddb.read(img.bytes);
	assert.equal(back.length, 90004);
	assert.deepEqual(back.find((x) => x.id === 8), { id: 8, text: "N0CALL Spaced" });

	// written again against itself with one record more: the dictionary and most pages stay as they were
	const again = img.clone();
	iddb.write(again, [...recs, { id: 1000001, text: "N0NEW Bob City5 GA" }], { format: "C" });
	let pages = 0;
	for (let a = iddb.AREA1; a < iddb.extent(again.bytes); a += 4096) {
		if (a >= iddb.AREA1 + iddb.AREA1_SIZE && a < iddb.AREA2) continue;
		if (!again.bytes.subarray(a, a + 4096).every((x, k) => x === img.bytes[a + k])) pages++;
	}
	assert.ok(pages <= 4, `pages changed by one record: ${pages}`);
	assert.deepEqual(simDetail(again, [1000001]).split("\n").filter((l) => l.startsWith("iddb 1")), ['iddb 1000001 "N0NEW Bob City5 GA"']);

	// the size estimate is close to the size written
	const est = iddb.estimateCompact(recs);
	const size = iddb.extent(img.bytes) - iddb.AREA1 - (iddb.AREA2 - iddb.AREA1 - iddb.AREA1_SIZE);
	assert.ok(Math.abs(est.bytes - size) / size < 0.1, `estimate ${est.bytes}, written ${size}`);
});

test("a format C database goes to an older radio as format N, and to TFTP as format N", () => {
	const recs = Array.from({ length: 300000 }, (_, i) => ({ id: 1000000 + i, text: `K${i} Name ${i % 7 ? "Short" : "A much longer city name here"}` }));
	const img = CodeplugImage.blank();
	iddb.write(img, recs, { format: "C" });
	const end = 0xE00000;	// a radio's room
	const n = img.clone();
	const r = iddb.convert(n, "N", { end });
	assert.equal(iddb.readHeader(n.bytes).format, "N");
	assert.equal(iddb.readHeader(n.bytes).textLen, 48);
	assert.equal(r.count + r.dropped, 300000);
	// a tighter room: records left out, counted
	const tight = img.clone();
	const t2 = iddb.convert(tight, "N", { end: iddb.AREA2 + 0x100000 });
	assert.ok(t2.dropped > 0 && t2.count + t2.dropped === 300000);
	const out = tftpBytes(img);
	assert.equal(iddb.readHeader(out).format, "N");
	assert.equal(iddb.readHeader(img.bytes).format, "C", "the editor's image is left as it was");
	assert.equal(iddb.convert(n, "N"), null, "already format N");
	// a format the editor can't read back (n: area 2 in the voice prompts) is left alone, never rewritten empty
	const vp = CodeplugImage.blank();
	iddb.write(vp, recs.slice(0, 10), { textLen: 16 });
	vp.bytes[iddb.AREA1 + 2] = 0x6E;
	const before = vp.bytes.slice();
	assert.equal(iddb.convert(vp, "C"), null);
	assert.equal(iddb.convert(vp, "N"), null);
	assert.deepEqual(tftpBytes(vp).subarray(iddb.AREA1, iddb.AREA1 + 64), before.subarray(iddb.AREA1, iddb.AREA1 + 64));
	// TFTP: cut to the radio's room like a write
	const capped = tftpBytes(img, iddb.AREA2 + 0x100000);
	assert.ok(capped.length <= iddb.AREA2 + 0x100000 && iddb.readHeader(capped).count === t2.count);
});
