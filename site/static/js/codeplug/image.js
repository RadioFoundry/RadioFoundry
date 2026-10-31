// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The codeplug as one flat image: file offset = the STM32 wire address (docs/spec/open-firmware-cps.md 7.1), the
// EEPROM image at 0 and the flash image at +0x20000, as tools/cps_client.py and backups store it. A radio whose
// codeplug sits elsewhere in its data flash (the DM-32UV at 0x700000) adds its base on the wire only. The image keeps
// every byte the editor doesn't decode, and grows past CODEPLUG_SPAN when a DMR ID database needs more room.

export const CODEPLUG_SPAN = 0x120000;
export const EE = 0x00000;	// EEPROM image
export const FL = 0x20000;	// flash image

// The ranges qdmr transfers (open-firmware-cps.md 7.1): what a read from a radio needs for every record the editor
// shows. Calibration and the radio's own settings (0x10000-0x1FFFF) are never part of it
export const REGIONS = [
	[EE + 0x00080, EE + 0x06060],
	[EE + 0x07500, EE + 0x0B000],
	[FL + 0x00000, FL + 0x011A0],
	[FL + 0x7B000, FL + 0x8EE60],
];
// The DMR ID database header (7.5; format C's is 32 bytes, iddb-compact.md 3.1): enough to tell whether one is there,
// its format and how big it is
export const IDDB_HEADER = [FL + 0x30000, FL + 0x30020];

// The file the radio's TFTP server takes as codeplug.bin (docs/spec/tftp.md): the image up to the end of the flash
// codeplug
export const TFTP_SIZE = 0xAEE60;

// 4 KB sectors a range touches
export function sectorsOf(ranges, sector = 0x1000) {
	const out = new Set();
	for (const [a, b] of ranges) for (let s = Math.floor(a / sector) * sector; s < b; s += sector) out.add(s);
	return [...out].sort((x, y) => x - y);
}

export class CodeplugImage {
	constructor(bytes) {
		this.bytes = bytes;
	}

	// An empty codeplug: erased flash (0xFF) with the in-use bitmaps, zone table and list counts cleared, so nothing
	// reads as in use, and the zone area zeroed so the firmware sees the 80-channel layout
	static blank(size = CODEPLUG_SPAN) {
		const b = new Uint8Array(size).fill(0xFF);
		b.fill(0, EE + 0x3780, EE + 0x3790);
		for (let bank = 1; bank < 8; bank++) {
			const a = FL + 0x7B1B0 + (bank - 1) * 0x1C10;
			b.fill(0, a, a + 16);
		}
		b.fill(0, EE + 0x8010, EE + 0x8030 + 68 * 0xB0);
		b.fill(0, FL + 0x8D620, FL + 0x8D620 + 76);
		return new CodeplugImage(b);
	}

	// From a file or a read: anything shorter than the span is padded with 0xFF (an older 0xAEE60 TFTP file, say)
	static from(bytes) {
		const b = new Uint8Array(Math.max(bytes.length, CODEPLUG_SPAN)).fill(0xFF);
		b.set(bytes);
		return new CodeplugImage(b);
	}

	get size() {
		return this.bytes.length;
	}

	clone() {
		return new CodeplugImage(this.bytes.slice());
	}

	// Room for size bytes; new bytes are erased flash
	ensure(size) {
		if (size <= this.bytes.length) return;
		const b = new Uint8Array(size).fill(0xFF);
		b.set(this.bytes);
		this.bytes = b;
	}

	view(addr, len) {
		return this.bytes.subarray(addr, addr + len);
	}
}
