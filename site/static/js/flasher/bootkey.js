// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The bootloader key from a radio that runs RadioFoundry, so an update needs no stock file. The stock bootloader holds the key
// as one plain block: the page reads the bootloader sectors over the debug link (READ_MEM, docs/spec/debuglink.md
// 3.10) and takes the block whose SHA-256 is the board's key_sha256. No offset or layout is assumed, and a key that
// doesn't match the hash can't come out of this. Same rules as rfoundry/bootkey.py.

import { sha256 } from "./bytes.js";

// The key-sized block of `region` whose SHA-256 is `wantSha`, or null. 4-byte aligned offsets first (the likely
// place), then the rest.
export async function findKey(region, keyLen, wantSha) {
	const last = region.length - keyLen;
	for (const pass of [(o) => o % 4 === 0, (o) => o % 4 !== 0]) {
		for (let start = 0; start <= last; start += 1024) {
			const offs = [];
			for (let o = start; o < Math.min(start + 1024, last + 1); o++) if (pass(o)) offs.push(o);
			const hashes = await Promise.all(offs.map((o) => sha256(region.subarray(o, o + keyLen))));
			const i = hashes.indexOf(wantSha);
			if (i >= 0) return region.slice(offs[i], offs[i] + keyLen);
		}
	}
	return null;
}

// The key, or null when this board has none, the firmware can't read the bootloader, or no block matches
export async function readBootKey(link, board, keyLen) {
	const boot = board.protected?.[0];
	if (!board.key_sha256 || !boot) return null;
	let region;
	try {
		region = await link.readMem(boot[0], boot[1] - boot[0]);
	} catch (e) {
		return null;
	}
	const key = await findKey(region, keyLen, board.key_sha256);
	region.fill(0);
	return key && (await sha256(key)) === board.key_sha256 ? key : null;
}
