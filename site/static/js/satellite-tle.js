// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors

export const SATELLITES = Object.freeze([
	{ id: 25544, name: "ISS", rxHz: 437800000, txHz: 145990000, toneHz: 670 },
	{ id: 27607, name: "SO-50", rxHz: 436795000, txHz: 145850000, toneHz: 670 },
	{ id: 40908, name: "LILACSAT-2", rxHz: 437200000, txHz: 144350000, toneHz: 0 },
	{ id: 40931, name: "IO-86", rxHz: 435880000, txHz: 145880000, toneHz: 885 },
	{ id: 43017, name: "AO-91", rxHz: 145960000, txHz: 435250000, toneHz: 0 },
	{ id: 43678, name: "PO-101", rxHz: 145900000, txHz: 437500000, toneHz: 1413 },
	{ id: 61781, name: "AO-123", rxHz: 435400000, txHz: 145850000, toneHz: 670 },
	{ id: 63213, name: "TEVEL2-4", rxHz: 436400000, txHz: 145970000, toneHz: 0 },
	{ id: 63214, name: "TEVEL2-5", rxHz: 436400000, txHz: 145970000, toneHz: 0 },
	{ id: 63215, name: "TEVEL2-6", rxHz: 436400000, txHz: 145970000, toneHz: 0 },
	{ id: 63217, name: "TEVEL2-1", rxHz: 436400000, txHz: 145970000, toneHz: 0 },
	{ id: 63218, name: "TEVEL2-3", rxHz: 436400000, txHz: 145970000, toneHz: 0 },
	{ id: 63219, name: "TEVEL2-2", rxHz: 436400000, txHz: 145970000, toneHz: 0 },
	{ id: 63237, name: "TEVEL2-9", rxHz: 436400000, txHz: 145970000, toneHz: 0 },
	{ id: 63238, name: "TEVEL2-7", rxHz: 436400000, txHz: 145970000, toneHz: 0 },
	{ id: 63239, name: "TEVEL2-8", rxHz: 436400000, txHz: 145970000, toneHz: 0 },
	{ id: 67291, name: "RS95S", rxHz: 436950000, txHz: 145920000, toneHz: 670 },
].map(Object.freeze));

export const DEFAULT_SELECTED_IDS = Object.freeze([25544, 27607, 43017]);
export const AMSAT_BULLETIN = "https://www.amsat.org/tle/daily-bulletin.txt";
export const MAX_SELECTED_PROFILES = 10;

export function filterFmSatellites(records) {
	const fmIds = new Set(SATELLITES.map((satellite) => satellite.id));
	return records.filter((record) => fmIds.has(record.catalog_id));
}

const DAY_MS = 86400000;
const MAX_AGE_DAYS = 30;

export function parseTleCatalog(text) {
	if (typeof text !== "string") throw new Error("TLE catalog must be text");
	const lines = text.replace(/^\uFEFF/, "").split(/\r?\n/);
	const records = [];
	const seen = new Set();
	let name = "";
	for (let i = 0; i < lines.length;) {
		const line = lines[i];
		if (!line.trim()) {
			i++;
			continue;
		}
		if (line.startsWith("2 ")) throw new Error("Catalog contains an unpaired TLE line 2");
		if (!line.startsWith("1 ")) {
			name = (line.startsWith("0 ") ? line.slice(2) : line).trim();
			i++;
			continue;
		}
		let line2Index = i + 1;
		while (line2Index < lines.length && !lines[line2Index].trim()) line2Index++;
		const line2 = lines[line2Index];
		if (!line2?.startsWith("2 ")) throw new Error("Catalog contains an incomplete TLE pair");
		const idText = line.slice(2, 7);
		if (!/^\d{5}$/.test(idText)) throw new Error("Catalog contains an invalid NORAD catalog ID");
		const catalogId = Number(idText);
		if (seen.has(catalogId)) throw new Error(`Catalog contains duplicate ID ${catalogId}`);
		if (!name) throw new Error(`Catalog ID ${catalogId} has no satellite name`);
		records.push({ catalog_id: catalogId, name, line1: line, line2 });
		seen.add(catalogId);
		name = "";
		i = line2Index + 1;
	}
	if (!records.length) throw new Error("No TLE records found in the selected file");
	return records;
}

function checksumValid(line) {
	if (line.length !== 69 || !/^\d$/.test(line[68])) return false;
	let sum = 0;
	for (const char of line.slice(0, 68)) {
		if (char >= "0" && char <= "9") sum += Number(char);
		else if (char === "-") sum++;
	}
	return sum % 10 === Number(line[68]);
}

export function tleEpochMilliseconds(line1, id) {
	const match = /^(\d{2})(\d{3})\.(\d{8})$/.exec(line1.slice(18, 32));
	if (!match) throw new Error(`NORAD ${id}: invalid TLE epoch`);
	const shortYear = Number(match[1]);
	const year = shortYear >= 57 ? 1900 + shortYear : 2000 + shortYear;
	const dayOfYear = Number(match[2]);
	const leap = year % 4 === 0 && (year % 100 !== 0 || year % 400 === 0);
	if (dayOfYear < 1 || dayOfYear > (leap ? 366 : 365)) throw new Error(`NORAD ${id}: invalid TLE epoch day`);
	return Date.UTC(year, 0, 1) + (dayOfYear - 1 + Number(`0.${match[3]}`)) * DAY_MS;
}

function validateRecord(record, now) {
	const { catalog_id: id, name, line1, line2 } = record;
	if (!Number.isInteger(id) || id < 1 || id > 99999 || !name || /[\x00-\x1f\x7f]/.test(name) ||
		new TextEncoder().encode(name).length > 48) throw new Error("Invalid satellite ID or name");
	if (line1.length !== 69 || line2.length !== 69 || !line1.startsWith("1 ") || !line2.startsWith("2 ") ||
		!checksumValid(line1) || !checksumValid(line2)) throw new Error(`NORAD ${id}: invalid TLE length or checksum`);
	if (line1.slice(2, 7) !== String(id).padStart(5, "0") || line2.slice(2, 7) !== String(id).padStart(5, "0"))
		throw new Error(`NORAD ${id}: TLE catalog IDs do not match`);
	if (!"UCS".includes(line1[7]) || line1[8] !== " " || line1[17] !== " " || line1[32] !== " " ||
		!/^[ 0-9A-Za-z]{8}$/.test(line1.slice(9, 17)) || !/^[ +-]\.\d{8}$/.test(line1.slice(33, 43)) ||
		!/^[ +-]\d{5}[+-]\d$/.test(line1.slice(44, 52)) || !/^[ +-]\d{5}[+-]\d$/.test(line1.slice(53, 61)) ||
		line1[43] !== " " || line1[52] !== " " || line1[61] !== " " || !/^\d$/.test(line1[62]) ||
		line1[63] !== " " || !/^\d{1,4}$/.test(line1.slice(64, 68).trim()))
		throw new Error(`NORAD ${id}: malformed TLE line 1`);
	if ([7, 16, 25, 33, 42, 51].some((index) => line2[index] !== " ") ||
		!/^(?: *\d{1,3}\.\d{4})$/.test(line2.slice(8, 16)) ||
		!/^(?: *\d{1,3}\.\d{4})$/.test(line2.slice(17, 25)) || !/^\d{7}$/.test(line2.slice(26, 33)) ||
		!/^(?: *\d{1,3}\.\d{4})$/.test(line2.slice(34, 42)) ||
		!/^(?: *\d{1,3}\.\d{4})$/.test(line2.slice(43, 51)) ||
		!/^(?:\d{2}| \d)\.\d{8}$/.test(line2.slice(52, 63)) || !/^ *\d{1,5}$/.test(line2.slice(63, 68)))
		throw new Error(`NORAD ${id}: malformed TLE line 2`);
	const epochMs = tleEpochMilliseconds(line1, id);
	const ageDays = (now - epochMs) / DAY_MS;
	if (ageDays < -1 || ageDays > MAX_AGE_DAYS) throw new Error(`NORAD ${id}: TLE is outside the 30-day age limit`);
	const angles = [line2.slice(8, 16), line2.slice(17, 25), line2.slice(34, 42), line2.slice(43, 51)].map(Number);
	const eccentricity = Number(`0.${line2.slice(26, 33)}`);
	const meanMotion = Number(line2.slice(52, 63));
	if (angles.some((value, i) => !Number.isFinite(value) || value < 0 || value >= (i === 0 ? 180 : 360)) ||
		eccentricity < 0 || eccentricity >= 1 || !Number.isFinite(meanMotion) || meanMotion <= 0)
		throw new Error(`NORAD ${id}: orbital value is outside its valid range`);
}

export function buildTleBundle(catalog, selectedIds,
	{ now = new Date(), provider = AMSAT_BULLETIN, profiles: suppliedProfiles = [] } = {}) {
	const selected = [...new Set(selectedIds)].sort((a, b) => a - b);
	if (!selected.length) throw new Error("Select at least one satellite");
	if (selected.length > MAX_SELECTED_PROFILES) throw new Error(`Select no more than ${MAX_SELECTED_PROFILES} satellites`);
	const byId = new Map(catalog.map((record) => [record.catalog_id, record]));
	const missing = selected.filter((id) => !byId.has(id));
	if (missing.length) throw new Error(`Catalog is missing selected IDs: ${missing.join(", ")}`);
	const catalogProfiles = new Map(SATELLITES.map((satellite) => [satellite.id, {
	catalog_id: satellite.id, rx_hz: satellite.rxHz, tx_hz: satellite.txHz, tx_tone_tenths: satellite.toneHz,
	}]));
	const profileOverrides = new Map(suppliedProfiles.map((profile) => [profile.catalog_id, profile]));
	const time = now instanceof Date ? now.getTime() : Number(now);
	if (!Number.isFinite(time)) throw new Error("Invalid bundle timestamp");
	const records = selected.map((id) => {
		const record = byId.get(id);
		validateRecord(record, time);
		return { catalog_id: record.catalog_id, name: record.name, line1: record.line1, line2: record.line2 };
	});
	const profiles = selected.map((id) => {
		const profile = profileOverrides.get(id) || catalogProfiles.get(id);
		if (!profile || profile.catalog_id !== id || !Number.isInteger(profile.rx_hz) || profile.rx_hz <= 0 ||
		    profile.rx_hz >= 1000000000 || !Number.isInteger(profile.tx_hz) || profile.tx_hz <= 0 ||
		    profile.tx_hz >= 1000000000 || !Number.isInteger(profile.tx_tone_tenths) ||
		    profile.tx_tone_tenths < 0 ||
		    (profile.tx_tone_tenths !== 0 && (profile.tx_tone_tenths < 600 || profile.tx_tone_tenths > 2600)))
			throw new Error(`NORAD ${id}: enter valid RX, TX, and tone frequencies`);
		return { catalog_id: id, rx_hz: profile.rx_hz, tx_hz: profile.tx_hz,
			tx_tone_tenths: profile.tx_tone_tenths };
	});
	return {
		schema_version: 3,
		provider,
		fetched_at: new Date(time).toISOString().replace(/\.\d{3}Z$/, "Z"),
		profile_ids: selected,
		profiles,
		records,
	};
}