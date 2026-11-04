// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Stock TYT-family codeplugs built from the spec (docs/spec/codeplug-stock-tyt.md), for the tests that read or convert
// one. No radio's data.

import { STOCK } from "../static/js/codeplug/stock_tyt.js";
import { putFreq, putTone } from "../static/js/codeplug/fields.js";

export function putText(b, off, chars, text) {
	b.fill(0, off, off + 2 * chars);
	for (let i = 0; i < Math.min(chars, text.length); i++) {
		b[off + 2 * i] = text.charCodeAt(i) & 0xFF;
		b[off + 2 * i + 1] = text.charCodeAt(i) >> 8;
	}
}
const put16 = (b, o, v) => {
	b[o] = v & 0xFF;
	b[o + 1] = v >> 8;
};

export class Stock {
	constructor(format, size = STOCK[format].size) {
		this.L = STOCK[format];
		this.b = new Uint8Array(size).fill(0xFF);
		const L = this.L;
		// unused records as the stock software leaves them (3.3-3.6)
		for (let i = 0; i < L.nChannels; i++) {
			const r = this.b.subarray(L.channels + i * 64, L.channels + (i + 1) * 64);
			r.fill(0, 0, 0x0F);
			r[0] = 0x43;
			r.fill(0, 0x20);
		}
		for (let i = 0; i < L.nContacts; i++) {
			const a = L.contacts + i * 36;
			this.b.set([0xFF, 0xFF, 0xFF, 0xC0], a);
			this.b.fill(0, a + 4, a + 36);
		}
		this.b.fill(0, L.lists, L.lists + L.nLists * 96);
		this.b.fill(0, L.zones, L.zones + L.nZones * 64);
		if (L.zoneExt) this.b.fill(0, L.zoneExt, L.zoneExt + L.nZones * 224);
	}

	general({ name = "", id = 0, boot1 = "", boot2 = "" }) {
		putText(this.b, 0x20B0, 16, name);
		this.b.set([id & 0xFF, (id >> 8) & 0xFF, id >> 16], 0x2084);
		putText(this.b, 0x2040, 10, boot1);
		putText(this.b, 0x2054, 10, boot2);
	}

	channel(n, c) {
		const r = this.b.subarray(this.L.channels + (n - 1) * 64, this.L.channels + n * 64);
		r.fill(0);
		r[0] = (c.mode === "dmr" ? 2 : 1) | (c.wide ? 0x08 : 0) | 0x60;
		r[1] = ((c.cc || 0) << 4) | ((c.ts === 2 ? 2 : 1) << 2) | (c.rxOnly ? 0x02 : 0);
		r[4] = (c.high ? 0x20 : 0) | (c.vox ? 0x10 : 0) | 0x04;
		put16(r, 6, c.contact || 0);
		r[8] = (c.tot || 0) / 15;
		r[0x0A] = c.scan || 0;
		r[0x0C] = c.list || 0;
		r[0x0F] = 0xFF;
		putFreq(r, 0x10, c.rx);
		putFreq(r, 0x14, c.tx ?? c.rx);
		putTone(r, 0x18, c.rxTone || "");
		putTone(r, 0x1A, c.txTone || "");
		putText(r, 0x20, 16, c.name);
	}

	contact(n, { name, id, type }) {
		const a = this.L.contacts + (n - 1) * 36;
		this.b.set([id & 0xFF, (id >> 8) & 0xFF, id >> 16, 0xC0 | { group: 1, private: 2, all: 3 }[type]], a);
		putText(this.b, a + 4, 16, name);
	}

	list(n, name, members) {
		const a = this.L.lists + (n - 1) * 96;
		putText(this.b, a, 16, name);
		members.forEach((x, i) => put16(this.b, a + 0x20 + 2 * i, x));
	}

	zone(n, name, A, B = []) {
		const a = this.L.zones + (n - 1) * 64, x = this.L.zoneExt + (n - 1) * 224;
		putText(this.b, a, 16, name);
		A.slice(0, 16).forEach((c, i) => put16(this.b, a + 0x20 + 2 * i, c));
		A.slice(16).forEach((c, i) => put16(this.b, x + 2 * i, c));
		B.forEach((c, i) => put16(this.b, x + 0x60 + 2 * i, c));
	}
}

export function uv380() {
	const s = new Stock("tyt-uv380");
	s.general({ name: "N0CALL Radio", id: 3100123, boot1: "Hello", boot2: "World" });
	s.contact(1, { name: "TG 91", id: 91, type: "group" });
	s.contact(2, { name: "Echo", id: 9990, type: "private" });
	s.contact(3, { name: "All", id: 0xFFFFFF, type: "all" });
	s.contact(5, { name: "TG 3100", id: 3100, type: "group" });
	s.list(1, "Local", [1, 5]);
	s.list(3, "Echo list", [2, 4]);	// contact 4 is not in use
	s.channel(1, { name: "Repeater", mode: "dmr", rx: 439562500, tx: 431962500, cc: 7, ts: 2, contact: 1, list: 1, high: true, tot: 60 });
	s.channel(2, { name: "Simplex", mode: "fm", rx: 446006250, wide: true, rxTone: "67.0", txTone: "D754I", vox: true, tot: 0 });
	s.channel(4, { name: "Listen", mode: "dmr", rx: 145000000, rxOnly: true, cc: 1, contact: 3, list: 3, scan: 1 });
	const A = Array.from({ length: 20 }, (_, i) => [1, 2, 4][i % 3]);
	s.zone(1, "Home", A);
	s.zone(2, "Both", [1], [2, 4, 3]);	// channel 3 is not in use
	return s;
}
