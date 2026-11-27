// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 RadioFoundry Theme Pack contributors

const MAX_PACK_THEMES = 10;
const MAX_NAME_BYTES = 23;
const STORE_THEMES = "rfoundry-theme-builder.themes.v1";
const STORE_PACK = "rfoundry-theme-builder.pack.v1";
const encoder = new TextEncoder();

const FIELDS = [
	["bg", "Screen background", "Main background behind menus and screens."],
	["fg", "Primary text", "Main text, icons, and screen titles."],
	["dim", "Secondary text", "Less prominent text such as details and labels."],
	["line", "Lines and borders", "Dividers, outlines, and subtle boundaries."],
	["tx", "Transmit", "Transmit status pill and transmit activity."],
	["rx", "Receive", "Receive status pill and receive activity."],
	["warn", "Warnings", "Alerts, denied transmit, and warning indicators."],
	["idle", "Idle status", "Background of the idle status pill."],
	["pill_fg", "Pill text: bright", "Text on bright status pills and indicators."],
	["pill_dark_fg", "Pill text: dark", "Text on dark pills, including receive and warning pills."],
	["focus", "Selected row", "Background of the selected menu row."],
	["hint_bg", "Hint background", "Background used for hints and toast messages."],
	["priv", "Private call", "Private-call status and indicator color."],
	["idle_fg", "Idle status text", "Text color on the idle status pill."],
	["focus_fg", "Selected row text", "Text color on the selected menu row."],
	["chnum", "Channel number", "Channel number shown on the home screen."],
	["strip_bg", "Status strip", "Background of the optional filled status strip."],
	["strip_warn", "Status strip warning", "Warning text and icons on the filled status strip."],
];

const PRESETS = [
	{ file: "radio-theme-presets.json", source: "Built-in themes" },
];

const DEFAULT_COLORS = {
	bg: "#000000", fg: "#FFFFFF", dim: "#A8A8A8", line: "#484848",
	tx: "#E02020", rx: "#28B848", warn: "#F0A800", idle: "#606060",
	pill_fg: "#FFFFFF", pill_dark_fg: "#000000", focus: "#2060C8", hint_bg: "#202020",
	priv: "#408CFF", idle_fg: "#FFFFFF", focus_fg: "#FFFFFF", chnum: "#A8A8A8",
	strip_bg: "#D8D8D8", strip_warn: "#884400",
};

const FIELD_PREVIEWS = {
	bg: '<div class="sample-screen-name">COUNTY DMR</div><div class="sample-screen-sub">TG 3100 USA</div>',
	fg: '<div class="sample-screen-name">COUNTY DMR</div><div class="sample-screen-sub sample-primary">TG 3100 USA</div>',
	dim: '<div class="sample-zone"><span>Local repeaters</span><b>3/16</b></div><div class="sample-screen-sub">No calls heard yet</div>',
	line: '<div class="sample-screen-name">COUNTY DMR</div><i class="sample-divider"></i><div class="sample-screen-sub">TG 3100 USA</div>',
	tx: '<span class="sample-pill sample-tx">TX</span>',
	rx: '<span class="sample-pill sample-rx">RX</span><span class="sample-signal"><i></i><i></i><i></i><i></i></span>',
	warn: '<span class="sample-warning"><b>!</b> LOW BATTERY</span>',
	idle: '<span class="sample-pill sample-idle">IDLE</span><span class="sample-screen-sub">DMR</span>',
	pill_fg: '<span class="sample-pill sample-bright">RX</span>',
	pill_dark_fg: '<span class="sample-pill sample-dark">TX</span>',
	focus: '<span class="sample-selected"><b>3</b> COUNTY DMR</span>',
	hint_bg: '<div class="sample-hint"><span>OK Menu</span><span>* Quick</span></div>',
	priv: '<span class="sample-priv">PRIV</span>',
	idle_fg: '<span class="sample-pill sample-idle sample-idle-text">IDLE</span>',
	focus_fg: '<span class="sample-selected sample-selected-text"><b>3</b> COUNTY DMR</span>',
	chnum: '<div class="sample-zone"><span>Local repeaters</span><b>3/16</b></div>',
	strip_bg: '<div class="sample-strip"><span>IDLE</span><span>DMR</span><span>▂▅█ 84%</span></div>',
	strip_warn: '<div class="sample-strip"><span>IDLE</span><span>DMR</span><b class="sample-strip-warning">! 18%</b></div>',
};

const byId = (id) => document.getElementById(id);
const libraryNode = byId("theme-library");
const colorGrid = byId("color-fields-grid");
const nameInput = byId("theme-name");
const packNameInput = byId("pack-name");
const packList = byId("pack-list");
const statusNode = byId("builder-status");
const downloadPackButton = byId("download-rftheme");
const downloadJsonButton = byId("download-json");
const previewZoom = byId("preview-zoom");
const previewZoomValue = byId("preview-zoom-value");
const radioSelect = byId("radio-model");
const radioPreview = byId("theme-preview");
const radioFrame = byId("radio-preview-image");
const radioScreen = byId("radio-screen");
const radioPreviewDetail = byId("radio-preview-detail");

let presets = [];
let customThemes = readStore(STORE_THEMES, []);
let packIds = readStore(STORE_PACK, []);
let editingId = null;
let radioModels = [];
let activeRadio = null;
let activePreviewGeometry = null;

function readStore(key, fallback) {
	try {
		const data = JSON.parse(localStorage.getItem(key));
		return Array.isArray(data) ? data : fallback;
	} catch {
		return fallback;
	}
}

function writeStore(key, value) {
	try {
		localStorage.setItem(key, JSON.stringify(value));
		return true;
	} catch {
		setStatus("This browser could not save to local storage. Export your editable JSON as a backup.", "bad");
		return false;
	}
}

function setStatus(message, kind = "info") {
	statusNode.textContent = message;
	statusNode.className = `notice ${kind}`;
}

function normalizeColor(value) {
	const color = String(value ?? "").trim().toUpperCase();
	return /^#[0-9A-F]{6}$/.test(color) ? color : null;
}

function safeName(name) {
	const text = String(name ?? "").trim();
	const byteLength = encoder.encode(text).length;
	if (!text || /[\u0000-\u001F\u007F]/u.test(text) || byteLength > MAX_NAME_BYTES) {
		throw new Error(`Enter a printable theme name that uses 1–${MAX_NAME_BYTES} UTF-8 bytes.`);
	}
	return text;
}

function themeById(id) {
	return customThemes.find((theme) => theme.id === id) || presets.find((theme) => theme.id === id) || null;
}

function themeColors(colors) {
	return Object.fromEntries(FIELDS.map(([key]) => [key, normalizeColor(colors?.[key]) || DEFAULT_COLORS[key]]));
}

function setPreviewPalette(element, palette) {
	const vars = {
		"--pv-bg": palette.bg, "--pv-fg": palette.fg, "--pv-dim": palette.dim, "--pv-line": palette.line,
		"--pv-tx": palette.tx, "--pv-rx": palette.rx, "--pv-warn": palette.warn, "--pv-idle": palette.idle,
		"--pv-pill-fg": palette.pill_fg, "--pv-pill-dark-fg": palette.pill_dark_fg, "--pv-focus": palette.focus,
		"--pv-hint-bg": palette.hint_bg, "--pv-priv": palette.priv, "--pv-idle-fg": palette.idle_fg,
		"--pv-focus-fg": palette.focus_fg, "--pv-chnum": palette.chnum, "--pv-strip": palette.strip_bg,
		"--pv-strip-warn": palette.strip_warn,
	};
	for (const [key, value] of Object.entries(vars)) element.style.setProperty(key, value);
}

function fieldPreview(key, label) {
	const figure = document.createElement("figure");
	figure.className = "color-preview";
	figure.dataset.field = key;
	figure.setAttribute("role", "img");
	figure.setAttribute("aria-label", `${label} color sample on the home screen`);
	const screen = document.createElement("div");
	screen.className = `color-preview-screen color-preview-${key}`;
	screen.innerHTML = FIELD_PREVIEWS[key];
	setPreviewPalette(screen, DEFAULT_COLORS);
	const caption = document.createElement("figcaption");
	caption.textContent = "Color sample";
	figure.append(screen, caption);
	return figure;
}

function loadThemeIntoEditor(theme) {
	if (!theme) return;
	editingId = theme.custom ? theme.id : null;
	nameInput.value = theme.name;
	for (const [key] of FIELDS) {
		byId(`hex-${key}`).value = theme.colors[key];
		byId(`color-${key}`).value = theme.colors[key];
	}
	renderPreview();
	byId("editor-title").textContent = theme.custom ? `Edit ${theme.name}` : `Copy ${theme.name}`;
	setStatus(theme.custom ? `Editing ${theme.name}. Save to update it.` : `${theme.name} loaded. Save it to create your own copy.`);
}

function createColorFields() {
	colorGrid.replaceChildren();
	for (const [key, label, help] of FIELDS) {
		const wrapper = document.createElement("div");
		wrapper.className = "color-control";
		const labelNode = document.createElement("label");
		labelNode.htmlFor = `hex-${key}`;
		labelNode.append(document.createTextNode(label));
		const code = document.createElement("code");
		code.textContent = key;
		labelNode.append(code);
		const helpNode = document.createElement("p");
		helpNode.textContent = help;
		const row = document.createElement("div");
		row.className = "color-editor-row";
		const preview = fieldPreview(key, label);
		const inputs = document.createElement("div");
		inputs.className = "color-input-row";
		const picker = document.createElement("input");
		picker.className = "color-picker";
		picker.type = "color";
		picker.id = `color-${key}`;
		picker.value = DEFAULT_COLORS[key];
		picker.setAttribute("aria-label", `${label} color picker`);
		const hex = document.createElement("input");
		hex.className = "hex-color";
		hex.type = "text";
		hex.id = `hex-${key}`;
		hex.value = DEFAULT_COLORS[key];
		hex.autocomplete = "off";
		hex.spellcheck = false;
		hex.inputMode = "text";
		hex.setAttribute("aria-label", `${label} hex color`);
		const updateFromPicker = () => { hex.value = picker.value.toUpperCase(); renderPreview(); };
		picker.addEventListener("input", updateFromPicker);
		picker.addEventListener("change", updateFromPicker);
		hex.addEventListener("input", () => {
			const color = normalizeColor(hex.value);
			if (!color) return;
			picker.value = color;
			renderPreview();
		});
		hex.addEventListener("blur", () => {
			const color = normalizeColor(hex.value);
			if (color) { hex.value = color; picker.value = color; }
		});
		inputs.append(picker, hex);
		row.append(preview, inputs);
		wrapper.append(labelNode, helpNode, row);
		colorGrid.append(wrapper);
	}
}

function readEditorTheme() {
	const name = safeName(nameInput.value);
	const colors = {};
	for (const [key] of FIELDS) {
		const color = normalizeColor(byId(`hex-${key}`).value);
		if (!color) throw new Error(`${key}: use a six-digit hex color such as #BB0000.`);
		colors[key] = color;
	}
	return { name, colors };
}

function renderPreview(colors = null) {
	const palette = colors || Object.fromEntries(FIELDS.map(([key]) => [key, normalizeColor(byId(`hex-${key}`)?.value) || DEFAULT_COLORS[key]]));
	setPreviewPalette(radioPreview, palette);
	for (const [key] of FIELDS) {
		const screen = colorGrid.querySelector(`.color-preview[data-field="${key}"] .color-preview-screen`);
		if (screen) setPreviewPalette(screen, { ...DEFAULT_COLORS, [key]: palette[key] });
	}
}

function renderPreviewZoom() {
	const zoom = Number(previewZoom.value);
	radioPreview.style.setProperty("--radio-zoom", String(zoom));
	const value = `${Math.round(zoom * 100)}%`;
	previewZoomValue.value = value;
	previewZoomValue.textContent = value;
}

function sizeRadioPreview(geometry) {
	const aspect = geometry.aspect;
	radioPreview.style.setProperty("--radio-preview-aspect", `${aspect} / 1`);
	const narrow = window.matchMedia("(max-width: 850px)").matches;
	const maxHeight = Math.min(window.innerHeight * (narrow ? 0.3 : 0.48), narrow ? 224 : 352);
	const maxWidth = narrow ? 112 : 240;
	radioPreview.style.setProperty("--radio-preview-max-width", `${Math.min(maxWidth, maxHeight * aspect)}px`);
}

function showRadioPreview(radio) {
	activeRadio = radio;
	const geometry = radio.id === "dm32" ? {
		aspect: 342 / 455,
		screen: { w: 240, h: 320 },
		screen_pct: { left: 25.7, top: 27.7, width: 48.5, height: 50.8 },
	} : radio.frame_geometry;
	activePreviewGeometry = geometry;
	const screen = geometry.screen_pct;
	const screenSize = geometry.screen;
	const color = /\bcolor\b/i.test(radio.display) &&
		(radio.config?.full ? radio.config.full.includes("UI_COLOR") : true);
	const screenAspect = screenSize ? screenSize.w / screenSize.h : 1;
	const frameImage = radio.id === "dm32" ? "static/img/radio-preview.jpg" : radio.frame_image;
	radioFrame.src = `${byId("theme-builder").dataset.radioRoot}${frameImage}?v=${encodeURIComponent(byId("theme-builder").dataset.assets || "")}`;
	radioScreen.style.setProperty("--screen-left", `${screen.left}%`);
	radioScreen.style.setProperty("--screen-top", `${screen.top}%`);
	radioScreen.style.setProperty("--screen-width", `${screen.width}%`);
	radioScreen.style.setProperty("--screen-height", `${screen.height}%`);
	radioScreen.style.setProperty("--screen-font", `${screen.width * 0.066}cqw`);
	radioScreen.dataset.layout = screenAspect > 1.1 ? "landscape" : "portrait";
	radioPreview.dataset.display = color ? "color" : "mono";
	radioPreview.style.aspectRatio = `${geometry.aspect} / 1`;
	const screenCenterX = screen.left + screen.width / 2;
	const screenCenterY = screen.top + screen.height / 2;
	byId("radio-preview-canvas").style.transformOrigin = `${screenCenterX}% ${screenCenterY}%`;
	radioPreview.setAttribute("aria-label", `${radio.name} with a live RadioFoundry theme preview on its display`);
	radioPreviewDetail.textContent = color
		? `${radio.name} · ${radio.display}. This mockup previews color themes.`
		: `${radio.name} · ${radio.display}. This monochrome display is shown in grayscale.`;
	sizeRadioPreview(geometry);
}

async function loadRadioModels() {
	try {
		const app = byId("theme-builder");
		const response = await fetch(app.dataset.radios);
		if (!response.ok) throw new Error(`${response.status} ${response.statusText}`);
		const data = await response.json();
		radioModels = (Array.isArray(data.radios) ? data.radios : [])
			.filter((radio) => radio.display && radio.display !== "none" && radio.frame_image &&
				radio.frame_geometry?.screen_pct && radio.frame_geometry?.aspect)
			.sort((a, b) => a.order - b.order);
		if (!radioModels.length) throw new Error("No supported radios with displays were found.");
		radioSelect.replaceChildren(...radioModels.map((radio) => {
			const option = document.createElement("option");
			option.value = radio.id;
			option.textContent = `${radio.name} — ${radio.display}`;
			return option;
		}));
		const initial = radioModels.find((radio) => radio.id === "dm32") || radioModels[0];
		radioSelect.value = initial.id;
		radioSelect.addEventListener("change", () => {
			const selected = radioModels.find((radio) => radio.id === radioSelect.value);
			if (selected) showRadioPreview(selected);
		});
		showRadioPreview(initial);
	} catch (error) {
		radioSelect.disabled = true;
		radioSelect.replaceChildren(new Option("Radio list unavailable", ""));
		radioPreviewDetail.textContent = "Radio mockups could not be loaded. Refresh the page or build the site data first.";
		console.warn("Could not load supported-radio mockups:", error);
	}
}

function swatches(colors) {
	const span = document.createElement("span");
	span.className = "swatches";
	for (const key of ["bg", "fg", "focus", "tx", "rx"]) {
		const dot = document.createElement("i");
		dot.className = "swatch";
		dot.style.backgroundColor = colors[key];
		dot.setAttribute("aria-hidden", "true");
		span.append(dot);
	}
	return span;
}

function renderLibrary() {
	libraryNode.replaceChildren();
	const all = [...presets, ...customThemes];
	if (!all.length) {
		const empty = document.createElement("p");
		empty.className = "muted";
		empty.textContent = "No palettes loaded. You can still create your own theme below.";
		libraryNode.append(empty);
		return;
	}
	for (const theme of all) {
		const row = document.createElement("div");
		row.className = "library-row";
		const summary = document.createElement("div");
		summary.className = "library-theme";
		summary.append(swatches(theme.colors));
		const titleWrap = document.createElement("span");
		const title = document.createElement("strong");
		title.textContent = theme.name;
		const origin = document.createElement("span");
		origin.className = "library-origin";
		origin.textContent = theme.custom ? " · saved here" : ` · ${theme.source}`;
		titleWrap.append(title, origin);
		summary.append(titleWrap);
		const actions = document.createElement("div");
		actions.className = "library-actions";
		const edit = document.createElement("button");
		edit.className = "btn btn-secondary";
		edit.type = "button";
		edit.textContent = theme.custom ? "Edit" : "Copy";
		edit.addEventListener("click", () => loadThemeIntoEditor(theme));
		const add = document.createElement("button");
		add.className = "btn";
		add.type = "button";
		add.textContent = packIds.includes(theme.id) ? "In pack" : "Add to pack";
		add.disabled = packIds.includes(theme.id) || packIds.length >= MAX_PACK_THEMES;
		add.addEventListener("click", () => addThemeToPack(theme.id));
		actions.append(edit, add);
		if (theme.custom) {
			const remove = document.createElement("button");
			remove.className = "btn btn-secondary";
			remove.type = "button";
			remove.textContent = "Delete";
			remove.addEventListener("click", () => deleteTheme(theme.id));
			actions.append(remove);
		}
		row.append(summary, actions);
		libraryNode.append(row);
	}
}

function renderPack() {
	packIds = packIds.filter((id) => themeById(id));
	writeStore(STORE_PACK, packIds);
	byId("pack-count").textContent = `${packIds.length} of ${MAX_PACK_THEMES} themes`;
	packList.replaceChildren();
	if (!packIds.length) {
		const empty = document.createElement("li");
		empty.className = "muted";
		empty.textContent = "Add themes from the library.";
		packList.append(empty);
	}
	const selected = packIds.map(themeById).filter(Boolean);
	const names = selected.map((theme) => theme.name.trim().toLocaleLowerCase());
	const duplicateNames = new Set(names.filter((name, index) => names.indexOf(name) !== index));
	for (let index = 0; index < packIds.length; index++) {
		const theme = themeById(packIds[index]);
		if (!theme) continue;
		const item = document.createElement("li");
		const line = document.createElement("div");
		line.className = "pack-item-row";
		const title = document.createElement("span");
		title.className = "pack-item-name";
		title.append(swatches(theme.colors), document.createTextNode(theme.name));
		const actions = document.createElement("div");
		actions.className = "pack-item-actions";
		for (const [label, delta] of [["Move up", -1], ["Move down", 1]]) {
			const move = document.createElement("button");
			move.className = "btn btn-secondary";
			move.type = "button";
			move.textContent = label === "Move up" ? "↑" : "↓";
			move.setAttribute("aria-label", `${label}: ${theme.name}`);
			move.disabled = index + delta < 0 || index + delta >= packIds.length;
			move.addEventListener("click", () => {
				const next = [...packIds];
				[next[index], next[index + delta]] = [next[index + delta], next[index]];
				packIds = next;
				writeStore(STORE_PACK, packIds);
				renderAll();
			});
			actions.append(move);
		}
		const remove = document.createElement("button");
		remove.className = "btn btn-secondary";
		remove.type = "button";
		remove.textContent = "Remove";
		remove.addEventListener("click", () => removeThemeFromPack(theme.id));
		actions.append(remove);
		line.append(title, actions);
		item.append(line);
		if (duplicateNames.has(theme.name.trim().toLocaleLowerCase())) {
			const warning = document.createElement("p");
			warning.className = "small";
			warning.textContent = "Theme names in a pack must be unique. Rename one before downloading.";
			item.append(warning);
		}
		packList.append(item);
	}
	const ready = selected.length > 0 && !duplicateNames.size && selected.every((theme) => {
		try { safeName(theme.name); return FIELDS.every(([key]) => normalizeColor(theme.colors[key])); } catch { return false; }
	});
	downloadPackButton.disabled = !ready;
	downloadJsonButton.disabled = !ready;
	renderLibrary();
}

function renderAll() {
	renderPack();
}

function addThemeToPack(id) {
	const theme = themeById(id);
	if (!theme || packIds.includes(id)) return;
	if (packIds.length >= MAX_PACK_THEMES) {
		setStatus(`A pack can contain up to ${MAX_PACK_THEMES} themes in this builder. Remove one before adding another.`, "warn");
		return;
	}
	if (packIds.map(themeById).some((item) => item?.name.trim().toLocaleLowerCase() === theme.name.trim().toLocaleLowerCase())) {
		setStatus(`A theme named “${theme.name}” is already in this pack. Theme names must be unique.`, "warn");
		return;
	}
	packIds.push(id);
	writeStore(STORE_PACK, packIds);
	setStatus(`${theme.name} added to your pack.`, "ok");
	renderAll();
}

function removeThemeFromPack(id) {
	packIds = packIds.filter((item) => item !== id);
	writeStore(STORE_PACK, packIds);
	setStatus("Theme removed from the pack.");
	renderAll();
}

function deleteTheme(id) {
	const theme = themeById(id);
	customThemes = customThemes.filter((item) => item.id !== id);
	packIds = packIds.filter((item) => item !== id);
	writeStore(STORE_THEMES, customThemes);
	writeStore(STORE_PACK, packIds);
	if (editingId === id) resetEditor();
	setStatus(`${theme?.name || "Theme"} deleted from this browser.`, "ok");
	renderAll();
}

function resetEditor() {
	editingId = null;
	nameInput.value = "";
	byId("editor-title").textContent = "Create or edit a theme";
	for (const [key] of FIELDS) {
		byId(`hex-${key}`).value = DEFAULT_COLORS[key];
		byId(`color-${key}`).value = DEFAULT_COLORS[key];
	}
	renderPreview();
}

function saveTheme(event) {
	event.preventDefault();
	try {
		const value = readEditorTheme();
		const existing = editingId ? customThemes.find((theme) => theme.id === editingId) : null;
		const sameName = customThemes.find((theme) => theme.name.trim().toLocaleLowerCase() === value.name.toLocaleLowerCase() && theme.id !== editingId);
		if (sameName && !window.confirm(`Replace your saved theme “${sameName.name}”?`)) return;
		const saved = { id: existing?.id || sameName?.id || `custom:${crypto.randomUUID()}`, name: value.name, colors: value.colors, custom: true };
		if (sameName) customThemes = customThemes.filter((theme) => theme.id !== sameName.id);
		const index = customThemes.findIndex((theme) => theme.id === saved.id);
		if (index >= 0) customThemes[index] = saved;
		else customThemes.push(saved);
		if (!writeStore(STORE_THEMES, customThemes)) return;
		editingId = saved.id;
		byId("editor-title").textContent = `Edit ${saved.name}`;
		setStatus(`Saved “${saved.name}” in this browser.`, "ok");
		renderAll();
	} catch (error) {
		setStatus(error.message, "bad");
	}
}

function currentPack() {
	const themes = packIds.map(themeById).filter(Boolean).map((theme) => ({
		name: safeName(theme.name),
		colors: Object.fromEntries(FIELDS.map(([key]) => [key, normalizeColor(theme.colors[key])])),
	}));
	if (!themes.length || themes.length > MAX_PACK_THEMES) throw new Error(`Choose 1–${MAX_PACK_THEMES} themes.`);
	const nameSet = new Set();
	for (const theme of themes) {
		const normalized = theme.name.toLocaleLowerCase();
		if (nameSet.has(normalized)) throw new Error(`Duplicate theme name: ${theme.name}.`);
		nameSet.add(normalized);
	}
	return themes;
}

function crc32(bytes) {
	let crc = 0xFFFFFFFF;
	for (const byte of bytes) {
		crc ^= byte;
		for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ (0xEDB88320 & -(crc & 1));
	}
	return (crc ^ 0xFFFFFFFF) >>> 0;
}

function rgb565(hex) {
	const red = Number.parseInt(hex.slice(1, 3), 16);
	const green = Number.parseInt(hex.slice(3, 5), 16);
	const blue = Number.parseInt(hex.slice(5, 7), 16);
	return ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3);
}

function encodePack(themes) {
	const payload = new Uint8Array(themes.length * 60);
	const payloadView = new DataView(payload.buffer);
	themes.forEach((theme, index) => {
		const offset = index * 60;
		const name = encoder.encode(safeName(theme.name));
		if (!name.length || name.length > MAX_NAME_BYTES) throw new Error(`Invalid name: ${theme.name}.`);
		payload[offset] = name.length;
		payload.set(name, offset + 1);
		FIELDS.forEach(([key], colorIndex) => payloadView.setUint16(offset + 24 + colorIndex * 2, rgb565(theme.colors[key]), true));
	});
	const output = new Uint8Array(20 + payload.length);
	output.set([0x52, 0x46, 0x54, 0x50], 0); // RFTP
	const header = new DataView(output.buffer);
	header.setUint16(4, 1, true);
	header.setUint16(6, 20, true);
	header.setUint32(8, payload.length, true);
	header.setUint16(12, themes.length, true);
	header.setUint16(14, 0, true);
	header.setUint32(16, crc32(payload), true);
	output.set(payload, 20);
	return output;
}

function download(filename, data, type) {
	const url = URL.createObjectURL(new Blob([data], { type }));
	const link = document.createElement("a");
	link.href = url;
	link.download = filename;
	document.body.append(link);
	link.click();
	link.remove();
	setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function fileStem() {
	const stem = packNameInput.value.trim().replace(/[^a-z0-9_-]+/gi, "-").replace(/^-+|-+$/g, "").toLowerCase();
	return stem || "my-theme-pack";
}

function exportBinary() {
	try {
		const themes = currentPack();
		const bytes = encodePack(themes);
		download(`${fileStem()}.rftheme`, bytes, "application/octet-stream");
		setStatus(`Downloaded a ${themes.length}-theme pack. Open the local uploader and select the .rftheme file.`, "ok");
	} catch (error) { setStatus(error.message, "bad"); }
}

function exportJson() {
	try {
		const themes = currentPack();
		const doc = {
			_spdx: "SPDX-License-Identifier: MIT",
			_copyright: "Copyright (c) 2026 The RadioFoundry Authors; Copyright (c) 2026 RadioFoundry Theme Pack contributors",
			format: "rfoundry-theme-pack",
			version: 1,
			themes,
		};
		download(`${fileStem()}.json`, `${JSON.stringify(doc, null, 2)}\n`, "application/json");
		setStatus("Downloaded editable JSON. Keep it as a backup or edit it later.", "ok");
	} catch (error) { setStatus(error.message, "bad"); }
}

async function importJson(file) {
	try {
		if (!file || file.size > 65536) throw new Error("Choose a JSON file smaller than 64 KB.");
		const doc = JSON.parse(await file.text());
		if (doc?.format !== "rfoundry-theme-pack" || doc?.version !== 1 || !Array.isArray(doc.themes) ||
		    doc.themes.length < 1 || doc.themes.length > MAX_PACK_THEMES) {
			throw new Error("This file is not a valid version 1 theme JSON pack.");
		}
		const importedNames = new Set();
		const imported = doc.themes.map((item, index) => {
			if (!item || typeof item !== "object" || Object.keys(item).sort().join(",") !== "colors,name" ||
			    !item.colors || typeof item.colors !== "object") {
				throw new Error(`Theme ${index + 1} must contain only a name and colors.`);
			}
			const name = safeName(item.name);
			const lowered = name.toLocaleLowerCase();
			if (importedNames.has(lowered)) throw new Error(`The file contains a duplicate theme name: ${name}.`);
			importedNames.add(lowered);
			const keys = Object.keys(item.colors).sort();
			const expected = FIELDS.map(([key]) => key).sort();
			if (keys.join(",") !== expected.join(",")) throw new Error(`${name}: the JSON must include all 18 color fields.`);
			const colors = {};
			for (const [key] of FIELDS) {
				const color = normalizeColor(item.colors[key]);
				if (!color) throw new Error(`${name}.${key}: use a six-digit hex color such as #BB0000.`);
				colors[key] = color;
			}
			return { name, colors };
		});
		const presetNames = new Set(presets.map((theme) => theme.name.toLocaleLowerCase()));
		const conflicts = imported.filter((theme) => presetNames.has(theme.name.toLocaleLowerCase()) ||
			customThemes.some((saved) => saved.name.toLocaleLowerCase() === theme.name.toLocaleLowerCase()));
		if (conflicts.length && !window.confirm(`Replace or save over these existing names? ${conflicts.map((theme) => theme.name).join(", ")}`)) return;
		for (const theme of imported) {
			const existing = customThemes.find((saved) => saved.name.toLocaleLowerCase() === theme.name.toLocaleLowerCase());
			const preset = presets.find((item) => item.name.toLocaleLowerCase() === theme.name.toLocaleLowerCase());
			const previousId = existing?.id || preset?.id;
			const wasInPack = previousId ? packIds.includes(previousId) : false;
			if (existing) {
				customThemes = customThemes.filter((saved) => saved.id !== existing.id);
			}
			const saved = { id: `custom:${crypto.randomUUID()}`, name: theme.name, colors: theme.colors, custom: true };
			customThemes.push(saved);
			if (wasInPack) packIds = packIds.map((id) => id === previousId ? saved.id : id);
		}
		writeStore(STORE_THEMES, customThemes);
		writeStore(STORE_PACK, packIds);
		setStatus(`Imported ${imported.length} theme(s) into this browser's library.`, "ok");
		renderAll();
	} catch (error) {
		setStatus(error instanceof SyntaxError ? "The selected file is not valid JSON." : error.message, "bad");
	} finally {
		byId("import-json-file").value = "";
	}
}

async function loadPresets() {
	const loaded = [];
	const manifestUrl = byId("theme-builder")?.dataset.presets;
	if (manifestUrl) {
		try {
			const response = await fetch(manifestUrl);
			if (!response.ok) throw new Error(`${response.status} ${response.statusText}`);
			const doc = await response.json();
			if (Array.isArray(doc.themes)) {
				for (const [index, theme] of doc.themes.entries()) {
					if (!theme || typeof theme.name !== "string" || !theme.colors) continue;
					loaded.push({
						id: typeof theme.id === "string" ? theme.id : `preset:site:${index}`,
						name: theme.name,
						colors: themeColors(theme.colors),
						custom: false,
						source: theme.source || "Included preset",
					});
				}
			}
		} catch (error) {
			console.warn("Could not load the site theme presets:", error);
		}
	} else {
		for (const preset of PRESETS) {
			try {
				const response = await fetch(`../themes/${preset.file}`);
				if (!response.ok) throw new Error(`${response.status} ${response.statusText}`);
				const doc = await response.json();
				if (doc.format !== "rfoundry-theme-pack" || doc.version !== 1 || !Array.isArray(doc.themes)) continue;
				for (const [index, theme] of doc.themes.entries()) {
					if (!theme || typeof theme.name !== "string" || !theme.colors) continue;
					loaded.push({
						id: `preset:${preset.file}:${index}`,
						name: theme.name,
						colors: themeColors(theme.colors),
						custom: false,
						source: preset.source,
					});
				}
			} catch (error) {
				console.warn(`Could not load preset ${preset.file}:`, error);
			}
		}
	}
	const seen = new Set();
	presets = loaded.filter((theme) => {
		const key = theme.name.toLocaleLowerCase();
		if (seen.has(key)) return false;
		seen.add(key);
		return true;
	});
	const packValid = packIds.every((id) => customThemes.some((theme) => theme.id === id) || presets.some((theme) => theme.id === id));
	if (!packValid) packIds = packIds.filter((id) => customThemes.some((theme) => theme.id === id) || presets.some((theme) => theme.id === id));
	renderAll();
}

createColorFields();
renderPreview();
previewZoom.addEventListener("input", renderPreviewZoom);
renderPreviewZoom();
window.addEventListener("resize", () => { if (activePreviewGeometry) sizeRadioPreview(activePreviewGeometry); });
byId("theme-editor").addEventListener("submit", saveTheme);
byId("new-theme").addEventListener("click", () => { resetEditor(); setStatus("Ready for a new theme."); });
byId("clear-pack").addEventListener("click", () => {
	packIds = [];
	writeStore(STORE_PACK, packIds);
	setStatus("Pack cleared.");
	renderAll();
});
byId("import-json-button").addEventListener("click", () => byId("import-json-file").click());
byId("import-json-file").addEventListener("change", () => importJson(byId("import-json-file").files?.[0]));
downloadPackButton.addEventListener("click", exportBinary);
downloadJsonButton.addEventListener("click", exportJson);
loadPresets();
loadRadioModels();
