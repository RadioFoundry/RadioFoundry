// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A simulated RadioFoundry radio as the codeplug editor sees it: the debug link VERSION answer and the open-firmware CPS
// (reads, sector writes, C 5), on a data flash with the codeplug at a base address. Writes outside the codeplug's
// room are refused as the radio refuses them (cps-protocol.md 2.3). For the tests and the page's #sim mode.

import { concat } from "../flasher/bytes.js";
import { frame, parseFrames } from "../flasher/link.js";

export class FakeCpsRadio {
	// foreign: the settings region still holds another firmware's data until a STORAGE CLAIM (settings-store.md 5)
	constructor({ board = "DM-1701", base = 0, end = 0xE00000, size = 0x1000000, image = null, cable = false, foreign = false } = {}) {
		Object.assign(this, { board, base, end, cable, foreign });
		this.spi = new Uint8Array(size).fill(0xFF);
		if (image) this.spi.set(image.subarray(0, Math.min(image.length, end - base)), base);
		this.writes = [];		// [addr, length] of every X 2
		this.commits = 0;
		this.reloads = 0;
		this.readBytes = 0;
	}

	writable(a, n) {
		const ee = a >= this.base && a + n <= this.base + 0x10000;
		const fl = a >= this.base + 0x20000 && a + n <= this.end;
		return ee || fl;
	}

	answer(req) {
		if (req[0] === 0xA5) {
			const f = parseFrames(req)[0];
			if (f?.cmd === 0x02) {
				const text = `proto=1\ngit=0123abcd\nboard=${this.board}\nfeatures=FM,DMR\ncodec=present\n`;
				return frame(0x82, f.seq, concat(new Uint8Array([0]), new TextEncoder().encode(text)));
			}
			const ok = (...b) => frame(f.cmd | 0x80, f.seq, new Uint8Array([0, ...b]));
			if (f?.cmd === 0x0B) return ok(...new Uint8Array(28).fill(0x11));	// INFO_CHIP: the UID's low word 0x11111111
			if (f?.cmd === 0x0F && f.payload[0] === 0) return ok(this.foreign ? 2 : 1, 0, ...new Uint8Array(24));
			if (f?.cmd === 0x0F && f.payload[0] === 2) {
				if (!this.foreign || f.payload[1] !== 0x11) return frame(0x8F, f.seq, new Uint8Array([this.foreign ? 7 : 4, 1]));
				this.foreign = false;
				this.claims = (this.claims || 0) + 1;
				return ok();
			}
			return frame((f?.cmd ?? 0) | 0x80, f?.seq ?? 0, new Uint8Array([1]));
		}
		if (req[0] === 0x52) {
			const addr = ((req[2] << 24) | (req[3] << 16) | (req[4] << 8) | req[5]) >>> 0, len = (req[6] << 8) | req[7];
			if (req[1] !== 1 || len === 0 || addr + len > this.spi.length) return new Uint8Array([0x2D]);
			this.readBytes += len;
			return concat(new Uint8Array([0x52, len >> 8, len & 0xFF]), this.spi.subarray(addr, addr + len));
		}
		if (req[0] === 0x58) {
			if (req[1] === 1) {
				if (this.foreign || this.sector) {
					this.sector = null;	// a sector still pending is dropped with the refusal, as the radio does
					return new Uint8Array([0x2D]);
				}
				this.pending = ((req[2] << 16) | (req[3] << 8) | req[4]) * 4096;
				this.sector = this.spi.slice(this.pending, this.pending + 4096);
				return req.subarray(0, 2);
			}
			if (req[1] === 2 && this.sector) {
				const addr = ((req[2] << 24) | (req[3] << 16) | (req[4] << 8) | req[5]) >>> 0, len = (req[6] << 8) | req[7];
				if (addr < this.pending || addr + len > this.pending + 4096 || !this.writable(addr, len)) {
					this.sector = null;	// a refusal drops the pending sector (open-firmware-cps.md 2.2)
					return new Uint8Array([0x2D]);
				}
				this.sector.set(req.subarray(8, 8 + len), addr - this.pending);
				this.writes.push([addr, len]);
				return req.subarray(0, 2);
			}
			if (req[1] === 3 && this.sector) {
				this.spi.set(this.sector, this.pending);
				this.sector = null;
				this.commits++;
				return req.subarray(0, 2);
			}
			return new Uint8Array([0x2D]);
		}
		if (req[0] === 0x43) {
			if (req[1] === 5) this.reloads++;
			return new Uint8Array([0x2D]);
		}
		return new Uint8Array([0x2D]);
	}

	// A stream as SerialStream offers it (buf, clear, take, write, close)
	stream() {
		const radio = this;
		const s = {
			buf: new Uint8Array(0),
			clear() { s.buf = new Uint8Array(0); },
			take(n) {
				const o = s.buf.slice(0, n);
				s.buf = s.buf.slice(n);
				return o;
			},
			async write(data) {
				s.buf = concat(s.buf, radio.answer(data));
			},
			async close() {},
		};
		return s;
	}
}
