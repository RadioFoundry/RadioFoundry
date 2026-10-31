// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Stock AnyTone codeplugs (codeplug/stock_anytone.js, docs/spec/codeplug-stock-anytone.md): the pair's current block,
// tones, a channel record, the read of what the zones reach through the stock PC mode against the simulated radio
// (fakeanytone.js), and the conversion into a RadioFoundry codeplug image. Run with site/tests/run.sh.

import { test } from "node:test";
import assert from "node:assert/strict";

import * as sa from "../static/js/codeplug/stock_anytone.js";
import { decode, empty } from "../static/js/codeplug/model.js";
import { CodeplugImage } from "../static/js/codeplug/image.js";
import { AnytoneProgram } from "../static/js/flasher/anytone.js";
import { FakeAnytone, stockAnytone, SAMPLE_ANYTONE } from "../static/js/flasher/fakeanytone.js";
import { convertAnytone, imageCounts } from "../static/js/flasher/migrate.js";

const tail = (valid, obsolete) => {
	const t = new Uint8Array(16).fill(0xFF);
	if (obsolete) t.set([0xAA, 0xAA, 0x55, 0x55], 8);
	if (valid) t.set([0x55, 0x55, 0xAA, 0xAA], 12);
	return t;
};

test("stock anytone: the first block is current only with the valid marker and without the obsolete one", () => {
	const X = 0x02540000;
	assert.equal(sa.currentBlock(X, tail(true, false)), X);
	assert.equal(sa.currentBlock(X, tail(true, true)), X + sa.BLOCK);
	assert.equal(sa.currentBlock(X, tail(false, false)), X + sa.BLOCK);
});

test("stock anytone: tones by index, the custom tone and DCS codes", () => {
	assert.equal(sa.tone(0, 0, 0, 0), "");
	assert.equal(sa.tone(1, 0, 0, 0), "62.5");
	assert.equal(sa.tone(1, 50, 0, 0), "254.1");
	assert.equal(sa.tone(1, 0x33, 0, 1318), "131.8");
	assert.equal(sa.tone(2, 0, 0x013, 0), "D023N");
	assert.equal(sa.tone(2, 0, 0x1FF, 0), "D777N");
	assert.equal(sa.tone(2, 0, 0x200 | 0x013, 0), "D023I");
	assert.equal(sa.tone(1, 60, 0, 0), null);
	assert.equal(sa.CTCSS.length, 51);
});

const read = async (opts = {}) => {
	const fake = new FakeAnytone({ memory: stockAnytone({ ...SAMPLE_ANYTONE, ...opts }), pairs: true });
	const p = new AnytoneProgram(fake.stream, { timeout: 50 });
	await p.hello();
	const steps = [];
	const tables = await sa.readAnytone((a, n) => p.read(a, n), (d, n) => steps.push([d, n]));
	return { fake, tables, steps };
};

test("stock anytone: what the zones reach is read, from whichever block of each pair is current", async () => {
	for (const first of [false, true]) {
		const { tables, steps, fake } = await read({ first });
		assert.deepEqual(tables.zones.map((z) => [z.name, z.channels]), [["Local", [0, 1, 2]], ["DMR", [2, 130]]]);
		const ch = Object.fromEntries(tables.channels.map((c) => [c.index, c]));
		assert.deepEqual(Object.keys(ch).map(Number), [0, 1, 2, 130]);
		assert.deepEqual([ch[1].rxHz, ch[1].txHz, ch[1].txTone, ch[1].rxTone, ch[1].wide], [147000000, 147600000, "100.0", "", true]);
		assert.deepEqual([ch[2].txHz, ch[2].type, ch[2].timeslot, ch[2].contact, ch[2].rxList], [430900000, 1, 2, 1001, 0]);
		assert.deepEqual(tables.lists.map((l) => [l.name, l.contacts]), [["Local", [3, 1001]]]);
		assert.deepEqual(tables.contacts.map((c) => [c.index, c.name, c.id, c.type]), [[3, "Local 9", 9, "group"], [1001, "World", 91, "group"]]);
		assert.deepEqual(tables.radio, { dmrId: 1234567, name: "N0CALL" });
		// progress only grows, and ends complete; about 13 KB read, nowhere near the whole NAND
		const [d, n] = steps[steps.length - 1];
		assert.equal(d, n);
		assert.ok(n < 16000, n);
		assert.ok(fake.reads * 16 >= n);
	}
});

test("stock anytone: what the bitmaps say is gone isn't read: a zone's deleted channel, a free contact, a dropped list", async () => {
	const memory = stockAnytone(SAMPLE_ANYTONE);
	const clear = (addr, i, on) => {
		const e = memory.find(([a, b]) => a <= addr + sa.BLOCK && addr + sa.BLOCK < a + b.length);
		const b = e[1], o = addr + sa.BLOCK - e[0];
		if (on) b[o + (i >> 3)] |= 1 << (i & 7); else b[o + (i >> 3)] &= ~(1 << (i & 7));
	};
	clear(sa.MAP.channels, 130, false);
	clear(sa.MAP.contacts, 3, true);
	const fake = new FakeAnytone({ memory, pairs: true });
	const p = new AnytoneProgram(fake.stream, { timeout: 50 });
	await p.hello();
	let tables = await sa.readAnytone((a, n) => p.read(a, n));
	assert.deepEqual(tables.zones.map((z) => z.channels), [[0, 1, 2], [2]]);
	assert.deepEqual(tables.contacts.map((c) => c.index), [1001]);
	clear(sa.MAP.lists, 0, false);
	tables = await sa.readAnytone((a, n) => p.read(a, n));
	assert.deepEqual(tables.lists, []);
});

test("stock anytone: the conversion fills a RadioFoundry codeplug, mixed channels in their transmit mode", async () => {
	const { tables } = await read({ channels: { ...SAMPLE_ANYTONE.channels, 0: { name: "Mixed", rx: 446000000, type: 2 } } });
	const conv = convertAnytone(tables);
	assert.deepEqual(conv.counts, { channels: 4, contacts: 2, rxLists: 1, zones: 2 });
	assert.deepEqual(conv.notes.map((x) => x.code).sort(), ["mixed_mode", "zone_only"]);
	assert.deepEqual(imageCounts(conv.image), conv.counts);
	const m = decode(CodeplugImage.from(conv.image));
	assert.deepEqual(m.channels.map((c) => [c.name, c.mode, c.rxHz, c.txHz]),
		[["Mixed", "fm", 446000000, 446000000], ["Repeater", "fm", 147000000, 147600000], ["DMR Local", "dmr", 438500000, 430900000],
			["DMR TG9", "dmr", 439500000, 439500000]]);
	assert.equal(m.channels[1].txTone, "100.0");
	assert.equal(m.channels[1].bw, 25);
	assert.equal(m.channels[2].timeslot, 2);
	assert.equal(m.channels[2].contact?.name, "World");
	assert.equal(m.channels[3].rxList?.name, "Local");
	assert.deepEqual(m.zones.map((z) => [z.name, z.channels.map((c) => c.name)]), [["Local", ["Mixed", "Repeater", "DMR Local"]], ["DMR", ["DMR Local", "DMR TG9"]]]);
	assert.equal(m.general.dmrId, 1234567);
});

test("stock anytone: an empty radio reads only the zone names and converts to nothing", async () => {
	const fake = new FakeAnytone({ memory: [], pairs: true });
	const p = new AnytoneProgram(fake.stream, { timeout: 50 });
	await p.hello();
	const tables = await sa.readAnytone((a, n) => p.read(a, n));
	assert.deepEqual([tables.zones, tables.channels], [[], []]);
	assert.equal(convertAnytone(tables).counts.channels, 0);
	assert.ok(empty());
});
