// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors

import {
	AMSAT_BULLETIN, buildTleBundle, DEFAULT_SELECTED_IDS, MAX_SELECTED_PROFILES, filterFmSatellites,
	parseTleCatalog, SATELLITES,
} from "./satellite-tle.js";
import { pushSatelliteBundle } from "./satellite-radio.js";
import {
	Link, SerialStream, RFOUNDRY_USB, CABLES, browserErrorKey, probe, serialApi, serialSupported,
} from "./flasher/link.js";
import { page } from "./i18n.js";

const { t } = page;
const fileInput = document.getElementById("satellite-tle-file");
const amsatButton = document.getElementById("tle-amsat");
const options = document.getElementById("satellite-tle-options");
const buildButton = document.getElementById("satellite-tle-build");
const pushButton = document.getElementById("satellite-tle-push");
const status = document.getElementById("satellite-tle-status");
const providerInput = document.getElementById("tle-provider");
const searchInput = document.getElementById("satellite-tle-search");
const profileById = new Map(SATELLITES.map((satellite) => [satellite.id, satellite]));
const RADIO_PORT_FILTERS = [RFOUNDRY_USB, ...CABLES];
let catalog = null;
let usingAmsat = false;
const selected = new Set(DEFAULT_SELECTED_IDS);

function selectedIds() {
	return [...selected];
}

function profileValues(selected) {
	return selected.map((id) => {
		const satellite = profileById.get(id);
		if (!satellite) throw new Error(`NORAD ${id}: no FM frequency profile is available`);
		return { catalog_id: id, rx_hz: satellite.rxHz, tx_hz: satellite.txHz, tx_tone_tenths: satellite.toneHz };
	});
}

function makeBundle(source = catalog) {
	const provider = new URL(providerInput.value.trim());
	if (provider.protocol !== "https:" || provider.username || provider.password || provider.hash)
		throw new Error("Catalog URL must use HTTPS and contain no credentials or fragment");
	return buildTleBundle(source, selectedIds(), { provider: provider.href, profiles: profileValues(selectedIds()) });
}

function showReadyState() {
	const selected = selectedIds();
	buildButton.disabled = true;
	pushButton.disabled = true;
	if (!catalog) {
		status.textContent = t("tools.satellite.status.pick");
		return;
	}
	if (!selected.length) {
		status.textContent = t("tools.satellite.status.none");
		return;
	}
	if (selected.length > MAX_SELECTED_PROFILES) {
		status.textContent = t("tools.satellite.status.limit");
		return;
	}
	try {
		makeBundle();
		buildButton.disabled = false;
		pushButton.disabled = false;
		status.textContent = t("tools.satellite.status.ready", { n: selected.length });
	} catch (error) {
		status.textContent = t("tools.satellite.status.invalid", { error: error.message });
	}
}

function renderOptions() {
	const records = filterFmSatellites(catalog || SATELLITES.map((satellite) => ({
		catalog_id: satellite.id, name: satellite.name,
	})));
	const query = searchInput.value.trim().toLocaleLowerCase();
	const matching = records.filter((record) => `${record.name} ${record.catalog_id}`.toLocaleLowerCase().includes(query));
	for (const record of matching) {
		const satellite = profileById.get(record.catalog_id);
		const row = document.createElement("div");
		row.className = "tle-profile";
		row.dataset.id = String(record.catalog_id);
		const checkbox = document.createElement("input");
		checkbox.type = "checkbox";
		checkbox.value = String(record.catalog_id);
		checkbox.id = `tle-satellite-${record.catalog_id}`;
		checkbox.checked = selected.has(record.catalog_id);
		checkbox.addEventListener("change", () => {
			if (checkbox.checked && selected.size >= MAX_SELECTED_PROFILES) {
				checkbox.checked = false;
				status.textContent = t("tools.satellite.status.limit");
				return;
			}
			if (checkbox.checked) selected.add(record.catalog_id);
			else selected.delete(record.catalog_id);
			showReadyState();
		});
		const name = document.createElement("label");
		name.className = "tle-profile-name";
		name.htmlFor = checkbox.id;
		name.textContent = record.name;
		const detail = document.createElement("small");
		detail.className = "tle-profile-detail";
		detail.textContent = t("tools.satellite.option", { id: record.catalog_id, rx: (satellite.rxHz / 1e6).toFixed(3),
			tx: (satellite.txHz / 1e6).toFixed(3) });
		row.append(checkbox, name, detail);
		options.append(row);
	}
	if (!matching.length) {
		const empty = document.createElement("p");
		empty.className = "small muted";
		empty.textContent = t("tools.satellite.no_matches");
		options.append(empty);
	}
}

function loadText(text, provider, fromAmsat) {
	catalog = parseTleCatalog(text);
	usingAmsat = fromAmsat;
	providerInput.value = provider;
	const available = new Set(filterFmSatellites(catalog).map((record) => record.catalog_id));
	for (const id of selected) if (!available.has(id)) selected.delete(id);
	options.replaceChildren();
	renderOptions();
	showReadyState();
}

async function fetchAmsatText() {
	let response;
	try {
		response = await fetch("/api/amsat-tle", { cache: "no-store", headers: { Accept: "text/plain" } });
	} catch (error) {
		throw new Error(t("tools.satellite.status.fetch_failed", { status: error.message }));
	}
	if (!response.ok) {
		throw new Error(response.status === 404
			? t("tools.satellite.status.api_not_found")
			: t("tools.satellite.status.fetch_failed", { status: response.status }));
	}
	const text = await response.text();
	if (new TextEncoder().encode(text).length > 1000000)
		throw new Error(t("tools.satellite.status.too_large"));
	return text;
}

async function refreshAmsat() {
	if (!usingAmsat) return;
	status.textContent = t("tools.satellite.status.fetching");
	const refreshed = parseTleCatalog(await fetchAmsatText());
	makeBundle(refreshed);
	catalog = refreshed;
}

function setPushProgress(step) {
	status.textContent = t(`tools.satellite.status.radio_${step}`);
}

function setBusy(busy) {
	for (const control of [amsatButton, fileInput, providerInput, searchInput, buildButton, pushButton,
		...options.querySelectorAll("input")])
		control.disabled = busy;
}

amsatButton.addEventListener("click", async () => {
	setBusy(true);
	status.textContent = t("tools.satellite.status.fetching");
	try {
		loadText(await fetchAmsatText(), AMSAT_BULLETIN, true);
	} catch (error) {
		catalog = null;
		buildButton.disabled = true;
		pushButton.disabled = true;
		status.textContent = error.message;
	} finally {
		setBusy(false);
		const result = status.textContent;
		showReadyState();
		status.textContent = result;
	}
});

fileInput.addEventListener("change", async () => {
	const file = fileInput.files?.[0];
	if (!file) return;
	if (file.size > 1000000) {
		catalog = null;
		buildButton.disabled = true;
		pushButton.disabled = true;
		status.textContent = t("tools.satellite.status.too_large");
		return;
	}
	try {
		loadText(await file.text(), providerInput.value.trim() || AMSAT_BULLETIN, false);
	} catch (error) {
		catalog = null;
		buildButton.disabled = true;
		pushButton.disabled = true;
		status.textContent = t("tools.satellite.status.read_failed", { error: error.message });
	}
});

providerInput.addEventListener("input", showReadyState);

buildButton.addEventListener("click", async () => {
	setBusy(true);
	try {
		await refreshAmsat();
		const bundle = makeBundle();
		const filename = `satellites-${new Date().toISOString().slice(0, 10)}.json`;
		const url = URL.createObjectURL(new Blob([`${JSON.stringify(bundle, null, 2)}\n`], { type: "application/json" }));
		const link = Object.assign(document.createElement("a"), { href: url, download: filename });
		link.click();
		setTimeout(() => URL.revokeObjectURL(url), 10000);
		status.textContent = t("tools.satellite.status.done", { name: filename });
	} catch (error) {
		status.textContent = t("tools.satellite.status.invalid", { error: error.message });
	} finally {
		setBusy(false);
		const result = status.textContent;
		showReadyState();
		status.textContent = result;
	}
});

renderOptions();

pushButton.addEventListener("click", async () => {
	let stream;
	let succeeded = false;
	setBusy(true);
	try {
		setPushProgress("connecting");
		if (!serialSupported()) throw new Error(t("flash.err.no_serial"));
		const port = await serialApi().requestPort({ filters: RADIO_PORT_FILTERS });
		await refreshAmsat();
		const bundle = makeBundle();
		stream = await SerialStream.open(port);
		const link = new Link(stream);
		setPushProgress("checking");
		if (!await probe(link)) throw new Error(t("tools.satellite.status.radio_not_found"));
		await pushSatelliteBundle(link, bundle, setPushProgress);
		succeeded = true;
	} catch (error) {
		const browserError = browserErrorKey(error);
		status.textContent = t("tools.satellite.status.push_failed", {
			error: error.translationKey ? t(error.translationKey) : browserError ? t(browserError) : error.message,
		});
	} finally {
		if (stream) await stream.close();
		setBusy(false);
		if (succeeded) {
			buildButton.disabled = false;
			pushButton.disabled = false;
		} else {
			const failure = status.textContent;
			showReadyState();
			status.textContent = failure;
		}
	}
});
showReadyState();
searchInput.addEventListener("input", () => {
	options.replaceChildren();
	renderOptions();
	showReadyState();
});
