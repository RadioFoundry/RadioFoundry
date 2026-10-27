// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The STM32F4 stub route (f4stub.js) against the simulated radio (fake.js): the hand-over from RadioFoundry, the board record
// (CONFIG, protocol 4) and its echo, an install in the interlock's order, the MD-380's sectors, a board with other SPI wiring (f4stub.py's test profile), a shared
// sector, a cable pulled halfway, SPI flash writes, and backups through the loader (readers.js). The records and the
// HELLO layout are checked against stub/f4stub.py's (f4-vectors.json from fixtures.py). Run with
// site/tests/run.sh, or: node --test site/tests/

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

import { crc32, equal, fromHex, toHex } from "../static/js/flasher/bytes.js";
import { FakeRadio, simulation, randomBytes, rng } from "../static/js/flasher/fake.js";
import { F4Stub, f4Image, f4StubHeader, HDR, APP_BASE, MEM_SPI, OP } from "../static/js/flasher/f4stub.js";
import { Stub, StubError } from "../static/js/flasher/stub.js";
import { Link } from "../static/js/flasher/link.js";
import { readRadio } from "../static/js/flasher/backup.js";
import { f4LoaderReader } from "../static/js/flasher/readers.js";

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
const json = (n) => JSON.parse(readFileSync(join(DIR, n), "utf8"));
const fb = json("flash-boards.json");
const stubJson = json("f4-stub.json");
const vectors = json("f4-vectors.json");
// the registry's boards and f4stub.py's test profile, a board with other SPI wiring than any of them
const boards = { ...fb.f4stub.boards, [vectors.test_board.id]: vectors.test_board };
const OTHER = vectors.test_board.id;
const image = f4Image(stubJson);
const quiet = () => {};
const at = (radio, addr, n) => radio.flash.subarray(addr - 0x08000000, addr - 0x08000000 + n);

// An application image: a RAM stack pointer first (the bootloader checks it), then firmware-like bytes
function app(seed, size) {
	const d = randomBytes(rng(seed), size).map((b) => (b < 90 ? b : 0));
	d.set([0xFC, 0xFF, 0x01, 0x20], 0);	// 0x2001FFFC, as RadioFoundry's
	return d;
}

async function running(boardId = "dm1701", opts = {}) {
	const sim = await simulation(fb, boardId, 7);
	const radio = new FakeRadio(sim, { state: "rfoundry", spiSize: 0x200000, ...opts });
	radio.flash.set(app(1, 0x30000), APP_BASE - 0x08000000);	// a RadioFoundry already installed
	return { sim, radio };
}

// The hand-over, then the radio's board record as the install step sends it (configure: false leaves it out)
async function enter(radio, { board = radio.board.id, configure = true } = {}) {
	const s = new F4Stub(radio.serialStream(), { log: quiet, reopen: async () => radio.serialStream(), board: boards[board] });
	await s.enterFromRFoundry(image, () => {}, { entry: stubJson.run });
	if (configure) await s.configure();
	return s;
}

test("the embedded stub matches its checksum", async () => {
	const { createHash } = await import("node:crypto");
	assert.equal(createHash("sha256").update(image).digest("hex"), stubJson.sha256);
	assert.ok(image.length < 8192, "fits RadioFoundry's staging buffer");
	// The page serves stub/f4stub.py's image (site/rfoundrysite/repo.py), never a copy of its own: one whose header says it
	// feeds the IWDG, which a watchdog build of RadioFoundry insists on (docs/spec/watchdog.md 3.1)
	const h = f4StubHeader(image);
	assert.ok(h, "the stub has its image header");
	assert.ok(h.version >= HDR.VERSION_FEEDS_IWDG && (h.flags & HDR.FLAG_FEEDS_IWDG) && h.feedsIwdg, "the header says it feeds the IWDG");
	const old = image.slice();
	old.fill(0, HDR.OFFSET, HDR.OFFSET + 4);
	assert.equal(f4StubHeader(old), null, "no magic: no header");
	// The page's numbers are f4stub_header.h's (the one definition the stub and RadioFoundry share)
	const src = readFileSync("stub/stm32f4/f4stub_header.h", "utf8");
	const def = (n) => Number(new RegExp(`#define\\s+F4STUB_${n}\\s+(0x[0-9A-Fa-f]+|\\d+)u`).exec(src)[1]);
	for (const [k, n] of [["OFFSET", "HDR_OFFSET"], ["MAGIC", "HDR_MAGIC"], ["LAYOUT", "HDR_LAYOUT"], ["SIZE", "HDR_SIZE"],
		["VERSION_FEEDS_IWDG", "VERSION_FEEDS_IWDG"], ["FLAG_FEEDS_IWDG", "FLAG_FEEDS_IWDG"]])
		assert.equal(HDR[k], def(n), `HDR.${k} matches F4STUB_${n}`);
});

test("hand-over: the stub adopts RadioFoundry's port, or comes back on a new one", async () => {
	for (const adopt of [true, false]) {
		const { radio } = await running("dm1701", { adopt });
		const s = await enter(radio);
		assert.equal(s.info.flashKb, 1024);
		assert.equal(s.info.spiSize, 0x200000);
		assert.equal(s.info.adopted, adopt);
		assert.deepEqual(radio.events, ["stub (from RadioFoundry)"]);
	}
});

test("a build without stub entry is refused before anything is sent", async () => {
	const { radio } = await running("dm1701", { stubEntry: false });
	await assert.rejects(enter(radio), /CONFIG_STUB_ENTRY/);
	assert.equal(radio.mode, "normal");
});

test("DM-1701 install: sector 3 erased first and committed last, codec sectors untouched, then RadioFoundry starts", async () => {
	const { sim, radio } = await running();
	const owned = fb.f4stub.boards.dm1701.owned;
	const codecBefore = sim.codecSegments.map(([a, d]) => crc32(at(radio, a, d.length)));
	const s = await enter(radio);
	const segs = [[APP_BASE, app(2, 0x48000)]];
	const r = await s.writeImage(owned, segs);
	assert.equal(radio.events[1], "erase 3", "sector 3 first");
	assert.ok(!radio.events.some((e) => /^erase (0|1|2|10|11)$/.test(e)), "bootloader and codec sectors never erased");
	assert.ok(r.written >= 3);
	assert.ok(equal(at(radio, APP_BASE, 0x48000), segs[0][1]));
	assert.ok(at(radio, APP_BASE + 0x48000, 0x1000).every((b) => b === 0xFF), "the old image's tail is gone");
	assert.deepEqual(sim.codecSegments.map(([a, d]) => crc32(at(radio, a, d.length))), codecBefore);
	await s.resetChip();
	assert.equal(radio.mode, "normal");
	assert.equal(radio.running, "rfoundry");
	// the same image again: nothing written
	const s2 = await enter(radio);
	assert.deepEqual(await s2.writeImage(owned, segs), { written: 0, same: r.written + r.same });
});

test("MD-380: sectors 3-7 are the application's, the codec sectors stay as they are", async () => {
	const { sim, radio } = await running("md380");
	const owned = fb.f4stub.boards.md380.owned;
	assert.deepEqual(owned, [[APP_BASE, 0x08080000]]);
	const s = await enter(radio);
	const codec = at(radio, 0x08080000, 0x80000).slice();
	const kept = [];
	await s.writeImage(owned, [[APP_BASE, app(4, 0x74000)]], { onKept: async (sec) => kept.push(sec) });
	assert.equal(kept.length, 0, "no sector shared with the codec");
	assert.ok(equal(at(radio, APP_BASE, 0x74000), app(4, 0x74000)));
	assert.ok(equal(at(radio, 0x08080000, 0x80000), codec), "sectors 8-11 unchanged");
	for (const [a, d] of sim.codecSegments) assert.ok(equal(at(radio, a, d.length), d));
});

test("a sector shared with other data: a copy is handed out first and the other bytes stay", async () => {
	// No radio has one since the codec moved; the rule stays for one that does
	const { radio } = await running("md380");
	const owned = [[APP_BASE, 0x08050C44]];
	const s = await enter(radio);
	const sec6 = at(radio, 0x08040000, 0x20000).slice();
	const kept = [];
	await s.writeImage(owned, [[APP_BASE, app(4, 0x44000)]], { onKept: async (sec) => kept.push(sec) });
	assert.equal(kept.length, 1);
	assert.equal(kept[0].index, 6);
	assert.ok(equal(kept[0].kept, sec6), "the copy is sector 6 as it was");
	assert.ok(equal(at(radio, 0x08050C44, 0x08060000 - 0x08050C44), sec6.subarray(0x10C44)), "the rest of sector 6 is unchanged");
});

test("images outside the owned flash, or without a stack pointer, are refused before anything is erased", async () => {
	const { radio } = await running();
	const s = await enter(radio);
	const owned = fb.f4stub.boards.dm1701.owned;
	await assert.rejects(s.writeImage(owned, [[APP_BASE, app(5, 0xB8000)]]), /outside the flash the firmware owns/);
	await assert.rejects(s.writeImage(owned, [[APP_BASE, app(5, 0x1000)], [0x080C0000, app(5, 0x1000)]]), /outside the flash the firmware owns/);
	await assert.rejects(s.writeImage(owned, [[APP_BASE, new Uint8Array(0x1000)]]), /stack pointer/);
	assert.ok(!radio.events.some((e) => e.startsWith("erase")));
});

test("a cable pulled halfway leaves the radio in its bootloader, and a second run finishes the job", async () => {
	const { radio } = await running("dm1701", { cutAfter: 30 });
	const owned = fb.f4stub.boards.dm1701.owned;
	const segs = [[APP_BASE, app(6, 0x48000)]];
	const s = await enter(radio);
	await assert.rejects(s.writeImage(owned, segs));
	radio.cutAfter = null;
	radio.powerOn();
	assert.equal(radio.mode, "bootloader", "nothing committed: the bootloader stays in DFU");
	// the bootloader route would finish it; here RadioFoundry's image is put back by hand and the stub runs again
	radio.flash.set(app(1, 0x30000), APP_BASE - 0x08000000);
	radio.powerOn();
	const s2 = await enter(radio);
	await s2.writeImage(owned, segs);
	assert.ok(equal(at(radio, APP_BASE, 0x48000), segs[0][1]));
});

test("SPI flash: only changed sectors are written, then verified; outside the chip is refused", async () => {
	const { radio } = await running();
	const s = await enter(radio);
	const data = radio.spi.slice(0, 0x20000);
	data.set(randomBytes(rng(9), 0x1800), 0x5000);
	assert.deepEqual(await s.spiWrite(data, 0), { written: 2, same: 30 });
	assert.ok(equal(radio.spi.subarray(0, 0x20000), data));
	assert.deepEqual(await s.diff(data, 0, MEM_SPI), []);
	const back = await s.readFlash(0x4000, 0x4000, { mem: MEM_SPI });
	assert.ok(equal(back, data.subarray(0x4000, 0x8000)));
	await assert.rejects(s.spiWrite(new Uint8Array(0x2000), 0x1FF000), StubError);
});

// A backup through the loader the way the backup step runs it
async function backupVia(radio, sim, what) {
	const link = new Link(radio.serialStream());
	let handedOver = false;
	const loader = f4LoaderReader(link, { stubJson: async () => stubJson, reopen: async () => radio.serialStream(), board: fb.f4stub.boards[radio.board.id], onHandOver: () => { handedOver = true; } });
	const parts = await readRadio(link, sim.board, { backup: { spi_size: radio.spi.length } }, { what, readers: [loader] });
	return { parts, loader, handedOver, get: (w) => parts.find((x) => x.what === w) };
}

test("backup through the loader, every loader board: the security registers over the debug link, both memories through the loader, then RadioFoundry again", async () => {
	for (const [id, adopt] of Object.keys(fb.f4stub.boards).flatMap((id) => [[id, true], [id, false]])) {
		const { sim, radio } = await running(id, { adopt });
		const b = await backupVia(radio, sim, {});
		assert.ok(b.loader.used && b.handedOver);
		assert.deepEqual(radio.events, ["stub (from RadioFoundry)", "reset"]);
		assert.equal(radio.mode, "normal", "the radio runs RadioFoundry again");
		assert.ok(b.get("security-registers").complete, "read over the debug link before the hand-over");
		const internal = b.get("internal-flash");
		assert.equal(internal.addr, APP_BASE);
		assert.ok(internal.complete);
		assert.deepEqual(internal.data, radio.flash.subarray(APP_BASE - 0x08000000));
		const spi = b.get("spi-flash");
		assert.ok(spi.complete);
		assert.deepEqual(spi.data, radio.spi);
	}
});

test("backup without stub entry: the loader doesn't start and the debug link reads everything, RadioFoundry untouched", async () => {
	const { sim, radio } = await running("dm1701", { stubEntry: false });
	const b = await backupVia(radio, sim, { spi: "none" });
	assert.ok(!b.loader.used && !b.handedOver);
	assert.deepEqual(radio.events, []);
	assert.deepEqual(b.get("internal-flash").data, radio.flash.subarray(APP_BASE - 0x08000000));
	assert.equal(b.get("spi-flash"), undefined);
});

test("a chip whose flash size register reads short: the internal flash over the debug link, in full, the SPI flash through the loader", async () => {
	const { sim, radio } = await running("dm1701", { flashKb: 512 });
	radio.flash.set(randomBytes(rng(4), 0x4000), 0x0F0000);	// the top sectors, past the register's 512 KB
	const b = await backupVia(radio, sim, {});
	assert.ok(b.loader.used, "the loader still reads the SPI flash");
	assert.equal(radio.mode, "normal");
	const internal = b.get("internal-flash");
	assert.ok(internal.complete);
	assert.deepEqual(internal.data, radio.flash.subarray(APP_BASE - 0x08000000), "up to the board's flash end");
	assert.deepEqual(b.get("spi-flash").data, radio.spi);
	// the same chip, internal flash only: no hand-over at all
	const only = await backupVia(radio, sim, { spi: "none" });
	assert.ok(!only.loader.used);
	assert.deepEqual(only.get("internal-flash").data, radio.flash.subarray(APP_BASE - 0x08000000));
});

test("the loader refuses a read past the chip's register size (what the debug link read avoids)", async () => {
	const { radio } = await running("dm1701", { flashKb: 512 });
	const s = await enter(radio);
	assert.equal(s.info.flashKb, 512);
	await assert.rejects(s.readFlash(0x0807C000, 0x10000), /refused: status 0xd/);
});

test("hand-over on a port that went away: says so, not \"built without CONFIG_STUB_ENTRY\"", async () => {
	const dead = { buf: new Uint8Array(0), clear() {}, take: () => new Uint8Array(0), write: async () => {} };
	const s = new F4Stub(dead, { log: quiet });
	await assert.rejects(s.enterFromRFoundry(image, () => {}, { entry: stubJson.run }), (e) => {
		assert.ok(e instanceof StubError);
		assert.match(e.message, /does not answer/);
		assert.doesNotMatch(e.message, /CONFIG_STUB_ENTRY/);
		return true;
	});
	assert.equal(await Stub.rfoundryOffer(new Link(dead, { timeout: 50 })), null, "not strict: null as before");
});

test("a codeplug-only backup with no reader reads over the debug link", async () => {
	const { sim, radio } = await running("dm1701");
	const parts = await readRadio(new Link(radio.serialStream()), sim.board, { backup: { spi_size: radio.spi.length } }, { what: { internal: false, spi: "codeplug" } });
	assert.deepEqual(radio.events, []);
	assert.equal(parts.find((x) => x.what === "spi-flash").data.length, 0x120000);
});

test("a read that fails inside the loader restarts the radio before the error goes up", async () => {
	const { sim, radio } = await running("dm1701");
	const link = new Link(radio.serialStream());
	const loader = f4LoaderReader(link, { stubJson: async () => stubJson, reopen: async () => radio.serialStream(), board: fb.f4stub.boards.dm1701 });
	const read = loader.read;
	loader.read = async (mem, ...rest) => { if (mem === "spi") throw new StubError("cable trouble"); return read(mem, ...rest); };
	await assert.rejects(readRadio(link, sim.board, { backup: { spi_size: radio.spi.length } }, { readers: [loader] }), /cable trouble/);
	assert.deepEqual(radio.events, ["stub (from RadioFoundry)", "reset"]);
	assert.equal(radio.mode, "normal");
});


// ---- protocol 4: the board record

// A stream that answers each request op with a fixed reply frame (stub/f4stub.py's FakeF4 bytes)
function replay(answers, sent = []) {
	const s = {
		buf: new Uint8Array(0),
		clear() { s.buf = new Uint8Array(0); },
		take(n) { const o = s.buf.slice(0, n); s.buf = s.buf.slice(n); return o; },
		async write(frame) {
			sent.push(frame);
			const a = answers[frame[1]].shift();
			const body = fromHex(a.body);
			const sum = body.reduce((x, y) => (x + y) & 0xFF, 0);
			s.buf = new Uint8Array([0x02, a.status, body.length >> 8, body.length & 0xFF, sum, 0x03, ...body]);
		},
	};
	return s;
}

test("board records: the page sends stub/f4stub.py's bytes for every radio", () => {
	assert.equal(fb.f4stub.protocol, vectors.protocol);
	assert.equal(stubJson.protocol, vectors.protocol, "the embedded stub speaks the protocol the records are for");
	assert.deepEqual(Object.keys(fb.f4stub.boards).sort(), Object.keys(vectors.records).sort());
	for (const [bid, rec] of Object.entries(vectors.records)) {
		assert.equal(fb.f4stub.boards[bid].record, rec, bid);
		assert.equal(fromHex(rec).length, fb.f4stub.record_len);
		assert.equal(fromHex(rec)[0], fb.f4stub.boards[bid].stub_id);
	}
	assert.ok(fb.f4stub.boards.dm1701.v2_wiring && fb.f4stub.boards.md380.v2_wiring && !vectors.test_board.v2_wiring);
});

test("CONFIG against stub/f4stub.py's simulated stub: the record goes out byte for byte and the echo is accepted", async () => {
	for (const [bid, h] of Object.entries(vectors.hello)) {
		const sent = [];
		const s = new F4Stub(replay({ [OP.HELLO]: [h.before, h.after], [OP.CONFIG]: [h.config] }, sent), { log: quiet, board: fb.f4stub.boards[bid] });
		const before = await s.hello();
		assert.equal(before.version, vectors.protocol);
		assert.equal(before.appBase, 0, "no application base before a record");
		assert.equal(before.spiSize, 0);
		assert.ok(!s.configured);
		await s.configure();
		assert.ok(s.configured, bid);
		assert.equal(sent[1][1], OP.CONFIG);
		assert.equal(toHex(sent[1].subarray(6)), vectors.records[bid], `${bid}: CONFIG payload`);
		assert.equal(s.info.appBase, APP_BASE);
		assert.equal(s.info.boardId, fb.f4stub.boards[bid].stub_id);
	}
});

test("the fake radio's stub answers HELLO as stub/f4stub.py's does, before and after CONFIG", async () => {
	for (const [bid, h] of Object.entries(vectors.hello)) {
		const { radio } = await running(bid);
		radio.spi = new Uint8Array(0x10000).fill(0xFF);	// FakeF4's SPI flash size
		radio.mode = "stub";
		const out = [];
		const ask = (op, p = new Uint8Array(0)) => { out.length = 0; radio.stubOp(op, p, (st, body = new Uint8Array(0)) => out.push({ status: st, body: toHex(body) })); return out[0]; };
		assert.deepEqual(ask(OP.HELLO), h.before, `${bid} before`);
		assert.deepEqual(ask(OP.CONFIG, fromHex(vectors.records[bid])), h.config);
		assert.deepEqual(ask(OP.HELLO), h.after, `${bid} after`);
	}
});

test("writes before CONFIG: the page refuses them, and the stub answers 0x11 to a page that sends one anyway", async () => {
	const { radio } = await running();
	const s = await enter(radio, { configure: false });
	assert.ok(!s.configured);
	assert.equal(s.info.appBase, 0);
	await assert.rejects(s.writeImage(fb.f4stub.boards.dm1701.owned, [[APP_BASE, app(2, 0x1000)]]), /not configured/);
	await assert.rejects(s.spiWrite(new Uint8Array(0x1000), 0), /not configured/);
	// bypass the page's own check: the stub's status 0x11
	await assert.rejects(s.request(OP.ERASE, new Uint8Array([3, 0x46, 0x34, 0x53, 0x42]), { retries: 0 }), (e) => e.status === 0x11 && /no board configuration/.test(e.message));
	assert.ok(!radio.events.some((e) => e.startsWith("erase")));
	assert.ok(at(radio, APP_BASE, 4).some((b) => b !== 0xFF), "RadioFoundry untouched");
	// reads of the internal flash work unconfigured
	assert.equal((await s.readFlash(APP_BASE, 0x1000)).length, 0x1000);
});

test("a refused record and an echo that differs both stop before anything is written", async () => {
	for (const [stubConfig, why] of [["refuse", /refused the Baofeng DM-1701's board record/], ["garble", /did not take the Baofeng DM-1701's board record/]]) {
		const { radio } = await running("dm1701", { stubConfig });
		const s = await enter(radio, { configure: false });
		await assert.rejects(s.configure(), why);
		assert.ok(!s.configured);
		await assert.rejects(s.writeImage(fb.f4stub.boards.dm1701.owned, [[APP_BASE, app(2, 0x1000)]]), /not configured/);
		assert.ok(!radio.events.some((e) => e.startsWith("erase")), stubConfig);
		assert.equal(radio.configs, 1);
	}
});

test("a record the stub's checks refuse (application base in sector 0) answers bad argument and leaves it unconfigured", async () => {
	const { radio } = await running();
	const s = await enter(radio);
	const bad = fromHex(vectors.records.dm1701);
	bad.set([0x08, 0x00, 0x00, 0x00], 8);
	await assert.rejects(s.request(OP.CONFIG, bad, { retries: 0 }), (e) => e.status === 0x0D);
	await s.hello();
	assert.ok(!s.configured, "the earlier record no longer holds");
	assert.equal(s.info.appBase, 0);
	await assert.rejects(s.request(OP.ERASE, new Uint8Array([3, 0x46, 0x34, 0x53, 0x42]), { retries: 0 }), (e) => e.status === 0x11);
});

// A second page session on a stub that still runs (the "again" button, or a page reloaded): HELLO only, no hand-over
async function reattach(radio, board) {
	const s = new F4Stub(radio.serialStream(), { log: quiet, board: boards[board] });
	await s.hello();
	return s;
}

test("another radio's record finds no SPI flash, and a stub configured once is not sent CONFIG again", async () => {
	const { radio } = await running("dm1701");
	const s = await enter(radio, { board: OTHER });	// other wiring on a DM-1701
	assert.ok(s.configured);
	assert.equal(s.info.spiSize, 0);
	assert.throws(() => s.needSpi(), /no SPI flash/);
	const again = await reattach(radio, "dm1701");
	assert.ok(!again.configured, "the echo is the other record");
	await again.configure();
	assert.equal(radio.configs, 2);
	assert.equal(again.info.spiSize, radio.spi.length);
	const third = await reattach(radio, "dm1701");
	assert.ok(third.configured, "the echo already matches");
	await third.configure();
	assert.equal(radio.configs, 2, "no second CONFIG");
});

test("a protocol 2 stub left running serves the DM-1701 but not a board with other wiring", async () => {
	const { radio } = await running("dm1701", { stubVersion: 2 });
	const s = await enter(radio);
	assert.ok(s.configured);
	assert.equal(radio.configs, 0, "no CONFIG to a protocol 2 stub");
	assert.equal(s.info.spiSize, radio.spi.length);
	const s2 = await reattach(radio, OTHER);
	await assert.rejects(s2.configure(), /can't serve the SPI2 test board/);
	assert.ok(!s2.configured);
});

test("backup through the loader on a radio whose stub refuses the record: the radio restarts into RadioFoundry and the error goes up", async () => {
	const { sim, radio } = await running("dm1701", { stubConfig: "refuse" });
	await assert.rejects(backupVia(radio, sim, {}), /refused/);
	assert.deepEqual(radio.events, ["stub (from RadioFoundry)", "reset"]);
	assert.equal(radio.mode, "normal");
});
