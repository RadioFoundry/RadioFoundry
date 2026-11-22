// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors

import { tleEpochMilliseconds } from "./satellite-tle.js";

const COMMAND = 0x26;
const OP = Object.freeze({ INFO: 0, LIST: 1, DATA: 3, COMMIT: 4, ABORT: 5,
	REPLACE_BEGIN: 9, PROFILE_STAGE: 10, PROFILE_VALUES_GET: 11, PROFILE_STAGE_CHUNK: 12 });

function put16(view, offset, value) { view.setUint16(offset, value, true); }
function put32(view, offset, value) { view.setUint32(offset, value, true); }
function get16(view, offset) { return view.getUint16(offset, true); }
function get32(view, offset) { return view.getUint32(offset, true); }

export function satelliteCrc32(bytes) {
	let crc = 0xFFFFFFFF;
	for (const byte of bytes) {
		crc ^= byte;
		for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ (0xEDB88320 & -(crc & 1));
	}
	return (crc ^ 0xFFFFFFFF) >>> 0;
}

export function encodeSatelliteRecords(records) {
	const encoder = new TextEncoder();
	const encoded = records.map((record) => {
		const name = encoder.encode(record.name);
		if (!name.length || name.length > 48 || record.line1.length !== 69 || record.line2.length !== 69)
			throw new Error(`NORAD ${record.catalog_id}: invalid TLE record`);
		const bytes = new Uint8Array(5 + name.length + 138);
		const view = new DataView(bytes.buffer);
		put32(view, 0, record.catalog_id);
		bytes[4] = name.length;
		bytes.set(name, 5);
		bytes.set(encoder.encode(record.line1), 5 + name.length);
		bytes.set(encoder.encode(record.line2), 5 + name.length + 69);
		return bytes;
	});
	const result = new Uint8Array(encoded.reduce((sum, record) => sum + record.length, 0));
	let offset = 0;
	for (const record of encoded) { result.set(record, offset); offset += record.length; }
	return result;
}

function profilesPayload(transaction, profiles) {
	const payload = new Uint8Array(4 + profiles.length * 14);
	const view = new DataView(payload.buffer);
	payload[0] = OP.PROFILE_STAGE;
	put16(view, 1, transaction);
	payload[3] = profiles.length;
	profiles.forEach((profile, index) => {
		const offset = 4 + index * 14;
		put32(view, offset, profile.catalog_id);
		put32(view, offset + 4, profile.rx_hz);
		put32(view, offset + 8, profile.tx_hz);
		put16(view, offset + 12, profile.tx_tone_tenths);
	});
	return payload;
}

function profilesChunkPayload(transaction, total, start, profiles) {
	const payload = new Uint8Array(6 + profiles.length * 14);
	const view = new DataView(payload.buffer);
	payload[0] = OP.PROFILE_STAGE_CHUNK;
	put16(view, 1, transaction);
	payload[3] = total;
	payload[4] = start;
	payload[5] = profiles.length;
	profiles.forEach((profile, index) => {
		const offset = 6 + index * 14;
		put32(view, offset, profile.catalog_id);
		put32(view, offset + 4, profile.rx_hz);
		put32(view, offset + 8, profile.tx_hz);
		put16(view, offset + 12, profile.tx_tone_tenths);
	});
	return payload;
}

export async function pushSatelliteBundle(link, bundle, onProgress = () => {}) {
	if (bundle?.schema_version !== 3 || !Array.isArray(bundle.records) || !Array.isArray(bundle.profiles) ||
	    bundle.records.length < 1 || bundle.records.length > 10 || bundle.profiles.length !== bundle.records.length)
		throw new Error("Invalid satellite bundle");
	const info = await link.request(COMMAND, new Uint8Array([OP.INFO]));
	if (info.length !== 5 || info[0] < 5) throw new Error("Radio firmware needs satellite protocol version 5 or newer");
	if (bundle.profiles.length > 4 && info[0] < 6)
		throw Object.assign(new Error("Radio firmware needs satellite protocol version 6 or newer for more than four profiles"),
			{ translationKey: "tools.satellite.status.profile_version" });
	if (get16(new DataView(info.buffer, info.byteOffset, info.byteLength), 1) < bundle.records.length)
		throw new Error("The radio cannot store this many satellites");

	const data = encodeSatelliteRecords(bundle.records);
	const crc = satelliteCrc32(data);
	const begin = new Uint8Array(9);
	const beginView = new DataView(begin.buffer);
	begin[0] = OP.REPLACE_BEGIN;
	put16(beginView, 1, bundle.records.length);
	put16(beginView, 3, data.length);
	put32(beginView, 5, crc);
	const started = await link.request(COMMAND, begin);
	if (started.length !== 2) throw new Error("Radio returned an invalid satellite transaction ID");
	const transaction = get16(new DataView(started.buffer, started.byteOffset, started.byteLength), 0);
	let committed = false;
	try {
		onProgress("writing");
		for (let offset = 0; offset < data.length;) {
			const size = Math.min(59, data.length - offset);
			const payload = new Uint8Array(5 + size);
			const view = new DataView(payload.buffer);
			payload[0] = OP.DATA;
			put16(view, 1, transaction);
			put16(view, 3, offset);
			payload.set(data.subarray(offset, offset + size), 5);
			const reply = await link.request(COMMAND, payload);
			if (reply.length !== 2 || get16(new DataView(reply.buffer, reply.byteOffset, reply.byteLength), 0) !== offset + size)
				throw new Error("Radio did not acknowledge the complete TLE data block");
			offset += size;
		}
		if (info[0] >= 6) {
			for (let start = 0; start < bundle.profiles.length; start += 4) {
				await link.request(COMMAND, profilesChunkPayload(transaction, bundle.profiles.length, start,
					bundle.profiles.slice(start, start + 4)));
			}
		} else {
			await link.request(COMMAND, profilesPayload(transaction, bundle.profiles));
		}
		onProgress("committing");
		const commit = new Uint8Array(7);
		const commitView = new DataView(commit.buffer);
		commit[0] = OP.COMMIT;
		put16(commitView, 1, transaction);
		put32(commitView, 3, crc);
		await link.request(COMMAND, commit);
		committed = true;

		onProgress("verifying");
		const list = await link.request(COMMAND, new Uint8Array([OP.LIST, 0, 0]));
		if (list.length < 1 || list[0] !== bundle.records.length || list.length !== 1 + list[0] * 8)
			throw new Error("Radio TLE inventory readback does not match the selection");
		const listView = new DataView(list.buffer, list.byteOffset, list.byteLength);
		const installed = Array.from({ length: list[0] }, (_, index) => get32(listView, 1 + index * 8));
		const expected = bundle.records.map(({ catalog_id }) => catalog_id);
		if (installed.some((id, index) => id !== expected[index]))
			throw new Error("Radio TLE inventory readback does not match the selection");
		for (let index = 0; index < bundle.records.length; index++) {
			const record = bundle.records[index];
			if (get32(listView, 5 + index * 8) !== Math.floor(tleEpochMilliseconds(record.line1, record.catalog_id) / 1000))
				throw new Error(`Radio TLE epoch readback does not match NORAD ${record.catalog_id}`);
		}

		const values = await link.request(COMMAND, new Uint8Array([OP.PROFILE_VALUES_GET]));
		if (values.length < 1 || values[0] !== bundle.profiles.length || values.length !== 1 + values[0] * 14)
			throw new Error("Radio frequency-profile readback does not match the selection");
		const valuesView = new DataView(values.buffer, values.byteOffset, values.byteLength);
		for (let index = 0; index < bundle.profiles.length; index++) {
			const offset = 1 + index * 14;
			const expectedProfile = bundle.profiles[index];
			if (get32(valuesView, offset) !== expectedProfile.catalog_id ||
			    get32(valuesView, offset + 4) !== expectedProfile.rx_hz ||
			    get32(valuesView, offset + 8) !== expectedProfile.tx_hz ||
			    get16(valuesView, offset + 12) !== expectedProfile.tx_tone_tenths)
				throw new Error("Radio frequency-profile readback does not match the selection");
		}
		onProgress("done");
	} catch (error) {
		if (!committed) {
			const abort = new Uint8Array(3);
			abort[0] = OP.ABORT;
			put16(new DataView(abort.buffer), 1, transaction);
			try { await link.request(COMMAND, abort); } catch { /* transaction may already have expired */ }
		}
		throw error;
	}
}
