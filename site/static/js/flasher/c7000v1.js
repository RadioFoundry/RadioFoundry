// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Version 1 codec slots, from before the codec moved (docs/spec/codec-placement.md, "HR_C7000 radios"): the code ran at
// its stock address, with no relocation list. The firmware accepts only version 2 (platform/hrc7000/codec_slot.h), so
// a radio with a version 1 slot reports no codec. Only a few developer radios have one. This module rebuilds such a
// slot as version 2 from its own code and tables, so their update needs no stock file. Temporary: everything else in
// the flasher is version 2 only. To remove it, delete this file, its test and its one use in c7000ui.js.

import { crc32, sha256, u32le } from "./bytes.js";
import { codecMap, slotBytes } from "./c7000.js";

const V1 = 1, ENTRIES = 7;

// A sound version 1 slot (magic, parts inside it, CRC of code and tables, signature): its fields, or null
function readV1(c, slot) {
	if (!slot || slot.length < c.slot_header) return null;
	const w = (i) => u32le(slot, 4 * i);
	if (w(0) !== c.slot_magic || w(1) !== V1) return null;
	const [codeOff, codeLen, run, tabOff, tabLen, tabRam] = [w(2), w(3), w(4), w(5), w(6), w(7)];
	if (codeOff < c.slot_header || !codeLen || codeOff + codeLen > slot.length || tabOff + tabLen > slot.length) return null;
	const code = slot.subarray(codeOff, codeOff + codeLen), tables = slot.subarray(tabOff, tabOff + tabLen);
	if (crc32(tables, crc32(code)) !== w(16) || u32le(code, 0) !== c.codec_sig[0] || u32le(code, 4) !== c.codec_sig[1]) return null;
	return { run, tabRam, code, tables, tag: w(15), entries: Array.from({ length: ENTRIES }, (_, i) => w(8 + i)) };
}

// The version 2 slot for a version 1 slot whose build has a codec map, or null (not version 1, damaged, no map, or
// it doesn't fit this board's windows)
export async function upgradeSlot(c, boardId, slot) {
	const r = readV1(c, slot);
	if (!r) return null;
	const got = { profile: { name: `tag ${r.tag}`, run: r.run, tables_ram: r.tabRam, entries: r.entries, tag: r.tag },
		code: r.code.slice(), tables: r.tables.slice(), codeSha: await sha256(r.code) };
	if (!codecMap(c, got)) return null;
	try {
		return slotBytes(c, boardId, got);
	} catch {
		return null;
	}
}
