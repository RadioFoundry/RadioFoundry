// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The temporary version 1 slot rebuild (c7000v1.js): a slot from before the codec moved becomes hrc7000_codec.py's
// version 2 slot for the same build, and the install writes it. Goes when c7000v1.js goes.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

import { crc32, equal } from "../static/js/flasher/bytes.js";
import * as c7 from "../static/js/flasher/c7000.js";
import { upgradeSlot } from "../static/js/flasher/c7000v1.js";

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
const json = (n) => JSON.parse(readFileSync(join(DIR, n), "utf8"));
const bin = (n) => new Uint8Array(readFileSync(join(DIR, n)));
const C = json("flash-boards.json").c7000;

// The version 1 slot for the same code and tables: the header as version 2 has it, version 1, CRC of code and
// tables, no list
function v1Of(v2) {
	const view = new DataView(v2.buffer, v2.byteOffset);
	const w = (i) => view.getUint32(4 * i, true);
	const [codeOff, codeLen, tabOff, tabLen] = [w(2), w(3), w(5), w(6)];
	const out = v2.slice(0, tabOff + tabLen);
	const o = new DataView(out.buffer);
	o.setUint32(4, 1, true);
	o.setUint32(64, crc32(out.subarray(tabOff, tabOff + tabLen), crc32(out.subarray(codeOff, codeOff + codeLen))), true);
	o.setUint32(68, 0, true);
	o.setUint32(72, 0, true);
	return out;
}

test("a version 1 slot is rebuilt as the version 2 slot hrc7000_codec.py writes, and installed", async () => {
	const fx = json("c7000.json").v2;
	const want = bin("c7000_slot_v2.bin");
	const c = { ...C, codec_maps: { [fx.map.profile]: fx.map }, boards: { ...C.boards, db25d: { ...C.boards.db25d, run: fx.run } } };
	const old = v1Of(want);
	assert.equal(await c7.parseSlot(c, "db25d", old), null, "the firmware's format refuses it");
	assert.ok(equal(await upgradeSlot(c, "db25d", old), want), "rebuilt as the version 2 slot");
	const d = await c7.decide(c, "db25d", { movedSlot: await upgradeSlot(c, "db25d", old) });
	assert.equal(d.mode, "write");
	assert.equal(d.source, "moved");
	assert.equal(await upgradeSlot({ ...c, codec_maps: {} }, "db25d", old), null, "no map: not rebuilt");
	assert.equal(await upgradeSlot(c, "db25d", want), null, "a version 2 slot is left to parseSlot");
	const bad = old.slice();
	bad[200] ^= 1;
	assert.equal(await upgradeSlot(c, "db25d", bad), null, "a damaged slot is not rebuilt");
});
