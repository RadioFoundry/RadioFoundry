// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors

import assert from "node:assert/strict";
import test from "node:test";

import { encodeSatelliteRecords, pushSatelliteBundle, satelliteCrc32 } from "../static/js/satellite-radio.js";

const record = {
	catalog_id: 99001,
	name: "Manual satellite",
	line1: "1 99001U 26001A   26317.20662175  .00008559  00000+0  16132-3 0  9998",
	line2: "2 99001  51.6325 282.6584 0007349  20.2588 339.8690 15.49346920594858",
};
const profile = { catalog_id: 99001, rx_hz: 145900000, tx_hz: 435100000, tx_tone_tenths: 885 };

class FakeLink {
	constructor(version = 5, records = [record], profiles = [profile]) {
		this.version = version;
		this.records = records;
		this.expectedProfiles = profiles;
		this.calls = [];
		this.recordBytes = new Uint8Array(0);
		this.profiles = [];
		this.reportedEpoch = 1794545852 - 8 * 86400;
		this.keepOldEpoch = false;
	}

	async request(command, payload) {
		assert.equal(command, 0x26);
		this.calls.push([...payload]);
		switch (payload[0]) {
		case 0: {
			const reply = new Uint8Array(5);
			reply[0] = this.version;
			new DataView(reply.buffer).setUint16(1, 10, true);
			return reply;
		}
		case 1: {
			const reply = new Uint8Array(1 + this.records.length * 8);
			reply[0] = this.records.length;
			const view = new DataView(reply.buffer);
			this.records.forEach((item, index) => {
				view.setUint32(1 + index * 8, item.catalog_id, true);
				view.setUint32(5 + index * 8, this.reportedEpoch, true);
			});
			return reply;
		}
		case 3: {
			const offset = payload[3] | payload[4] << 8;
			const data = payload.slice(5);
			const next = new Uint8Array(Math.max(this.recordBytes.length, offset + data.length));
			next.set(this.recordBytes);
			next.set(data, offset);
			this.recordBytes = next;
			return new Uint8Array([next.length & 0xFF, next.length >> 8]);
		}
		case 4:
			if (!this.keepOldEpoch) this.reportedEpoch = 1794545852;
			return new Uint8Array(0);
		case 6:
			return new Uint8Array(0);
		case 9:
			this.recordBytes = new Uint8Array(0);
			return new Uint8Array([73, 0]);
		case 10:
			this.profiles = this.expectedProfiles;
			return new Uint8Array(0);
		case 12: {
			const total = payload[3], start = payload[4], count = payload[5];
			if (start === 0) this.profiles = [];
			for (let index = 0; index < count; index++) {
				const offset = 6 + index * 14;
				const view = new DataView(payload.buffer, payload.byteOffset + offset, 14);
				this.profiles.push({ catalog_id: view.getUint32(0, true), rx_hz: view.getUint32(4, true),
					tx_hz: view.getUint32(8, true), tx_tone_tenths: view.getUint16(12, true) });
			}
			assert.equal(this.profiles.length, Math.min(start + count, total));
			return new Uint8Array(0);
		}
		case 11: {
			const reply = new Uint8Array(1 + this.profiles.length * 14);
			reply[0] = this.profiles.length;
			const view = new DataView(reply.buffer);
			this.profiles.forEach((item, index) => {
				const offset = 1 + index * 14;
				view.setUint32(offset, item.catalog_id, true);
				view.setUint32(offset + 4, item.rx_hz, true);
				view.setUint32(offset + 8, item.tx_hz, true);
				view.setUint16(offset + 12, item.tx_tone_tenths, true);
			});
			return reply;
		}
		default:
			throw new Error(`unexpected operation ${payload[0]}`);
		}
	}
}

const bundle = { schema_version: 3, records: [record], profiles: [profile] };

test("serializes validated record bytes and IEEE CRC-32", () => {
	const bytes = encodeSatelliteRecords([record]);
	assert.equal(bytes.length, 5 + new TextEncoder().encode(record.name).length + 138);
	assert.equal(new DataView(bytes.buffer).getUint32(0, true), record.catalog_id);
	assert.equal(satelliteCrc32(new TextEncoder().encode("123456789")), 0xCBF43926);
});

test("pushes, commits, verifies, and synchronizes a full replacement", async () => {
	const link = new FakeLink();
	const progress = [];
	await pushSatelliteBundle(link, bundle, (step) => progress.push(step));
	assert.deepEqual(link.calls.map((call) => call[0]), [0, 9, 3, 3, 3, 10, 4, 1, 11]);
	const chunks = link.calls.filter((call) => call[0] === 3);
	assert.deepEqual(chunks.map((call) => [call[3] | call[4] << 8, call.length]), [[0, 64], [59, 64], [118, 46]]);
	assert.deepEqual(progress, ["writing", "committing", "verifying", "done"]);
	assert.equal(link.reportedEpoch, 1794545852);
	assert.equal(satelliteCrc32(link.recordBytes), new DataView(Uint8Array.from(link.calls[1]).buffer).getUint32(5, true));
});

test("supports consecutive full replacements without resetting the link", async () => {
	const link = new FakeLink();
	await pushSatelliteBundle(link, bundle);
	await pushSatelliteBundle(link, bundle);
	assert.deepEqual(link.calls.map((call) => call[0]), [0, 9, 3, 3, 3, 10, 4, 1, 11, 0, 9, 3, 3, 3, 10, 4, 1, 11]);
	assert.equal(link.recordBytes.length, encodeSatelliteRecords(bundle.records).length);
});

test("refuses success when an existing satellite still has an old epoch", async () => {
	const link = new FakeLink();
	link.keepOldEpoch = true;
	const progress = [];
	await assert.rejects(() => pushSatelliteBundle(link, bundle, (step) => progress.push(step)),
		/epoch readback does not match NORAD 99001/);
	assert.ok(!progress.includes("done"));
	assert.equal(link.calls.at(-1)[0], 1);
});

test("stages ten profiles in bounded protocol-v6 chunks", async () => {
	const records = Array.from({ length: 10 }, (_, index) => ({ ...record, catalog_id: 99001 + index,
		name: `Satellite ${index}` }));
	const profiles = records.map(({ catalog_id }) => ({ catalog_id, rx_hz: 145900000, tx_hz: 435100000,
		tx_tone_tenths: 885 }));
	const link = new FakeLink(6, records, profiles);
	await pushSatelliteBundle(link, { schema_version: 3, records, profiles });
	const chunks = link.calls.filter((call) => call[0] === 12);
	assert.deepEqual(chunks.map((call) => call.length), [62, 62, 34]);
	assert.ok(link.calls.every((call) => call.length <= 64));
});

test("rejects ten profiles on protocol-v5 before starting a transaction", async () => {
	const records = Array.from({ length: 10 }, (_, index) => ({ ...record, catalog_id: 99001 + index,
		name: `Satellite ${index}` }));
	const profiles = records.map(({ catalog_id }) => ({ catalog_id, rx_hz: 145900000, tx_hz: 435100000,
		tx_tone_tenths: 885 }));
	const link = new FakeLink(5, records, profiles);
	await assert.rejects(() => pushSatelliteBundle(link, { schema_version: 3, records, profiles }),
		{ translationKey: "tools.satellite.status.profile_version" });
	assert.deepEqual(link.calls.map((call) => call[0]), [0]);
});

test("rejects older firmware before starting a transaction", async () => {
	const link = new FakeLink(4);
	await assert.rejects(() => pushSatelliteBundle(link, bundle), /protocol version 5/);
	assert.deepEqual(link.calls.map((call) => call[0]), [0]);
});
