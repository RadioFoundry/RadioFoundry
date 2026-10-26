// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The manual install kit: when the page can't install (a browser without WebUSB or Web Serial, an install that keeps
// failing), a zip with what the user needs to install from the command line: rfoundry from the site's own commit
// (downloads/rfoundry-tool.zip, checked against site.json "tool"), the firmware built here with its manifest, the
// stock file the user gave, and a README.txt with this radio's commands, file names filled in. The README is English,
// as rfoundry's own output is. docs/dev/web-flasher.md "The manual install kit".

import { sha256 } from "./bytes.js";
import { zip, unzip } from "./zip.js";
import { h, t, button, download, filePicker } from "./dom.js";

export class KitError extends Error {}

export const COMMUNITY = "https://www.facebook.com/groups/radiofoundry/";

// How rfoundry installs a radio (registry.json family): "dfu" over USB, "serial" over the programming cable, null where
// rfoundry has no install (a radio installed through the RAM stub: the web flasher or stub/stubtool.py)
export function kitRoute(radio) {
	return { "usb-dfu": "dfu", "serial-loader": "serial" }[radio?.flash?.family] || null;
}

// The browser API an install step needs that this browser lacks: "usb" or "serial", else null. step: the step about
// to be drawn; stub: the install goes through the STM32F4 loader (serial) rather than the bootloader (USB)
export function missingApi(radio, task, step, { usb, serial, stub = false }) {
	if (task !== "install") return null;
	const backend = radio?.flash?.backend;
	if (backend === "anyroad-dfu" && step === "write") return stub ? (serial ? null : "serial") : usb ? null : "usb";
	if (backend === "c7000-stub" && step === "connect") return serial ? null : "serial";
	return null;
}

// The image's file name without .bin: the manifest's, else rfoundry_<board> (a local build's manifest says "local")
export function kitBase(manifest, boardId) {
	const f = manifest?.image?.file || "";
	return /^[\w.+-]+\.bin$/.test(f) ? f.slice(0, -4) : `rfoundry_${boardId}`;
}

const day = (d) => d.toISOString().slice(0, 10);
export const kitName = (radio, when = new Date()) =>
	`RadioFoundry_${radio.name.replace(/[^A-Za-z0-9-]+/g, "-").replace(/^-|-$/g, "")}_manual_install_${day(when)}.zip`;

// A path for a command line: always quoted (stock file names carry spaces and brackets)
const q = (p) => `"${p}"`;

// The stock files the radio's codec comes from, as the codec step names them; a program flash read is not one
const stockFiles = (board) => (board?.codec?.sources || []).filter((s) => /\.bin$/i.test(s));

// README.txt. o: {radio (radios.json), board (flash-boards.json), base, manifest, stock: file name or null, site (the
// page's address, ending in /), repo, git, date}
export function kitReadme(o) {
	const route = kitRoute(o.radio);
	if (!route) throw new KitError(`rfoundry does not install the ${o.radio.name}`);
	const name = o.radio.name, m = o.manifest || {};
	const img = `firmware/${o.base}.bin`;
	const files = stockFiles(o.board);
	const readout = route === "serial" && !files.length;	// the codec comes from the radio's own program flash
	const stock = o.stock ? `stock/${o.stock}` : readout ? "stock/progmem.bin" : null;
	const docs = (page, anchor = "") => `${o.site}docs/user/${page}.html${anchor ? "#" + anchor : ""}`;
	const L = [];
	const add = (...lines) => L.push(...lines);
	const both = (args) => [`  Windows:        venv\\Scripts\\python ${args}`, `  macOS, Linux:   venv/bin/python ${args}`];

	add(`RadioFoundry for the ${name}: manual install kit`, "",
		`Made ${o.date} by ${o.site}flash/ (RadioFoundry ${o.git || "?"}).`,
		"This kit installs RadioFoundry from the command line with rfoundry, the tool the web installer is built on.",
		"Use it where the web installer can't: a browser without WebUSB or Web Serial (Safari, Firefox), or an install",
		"that keeps failing.", "",
		"WHAT'S INSIDE", "",
		`  ${img.padEnd(40)} the firmware you chose (${[m.version, m.variant].filter(Boolean).join(", ") || "RadioFoundry"})`,
		`  ${`firmware/${o.base}.json`.padEnd(40)} its manifest: rfoundry checks the image against it`);
	if (o.stock) add(`  ${stock.padEnd(40)} your stock firmware file: ${route === "dfu" ? "the codec and the bootloader key" : "the codec"}`);
	add(`  ${"rfoundry.py".padEnd(40)} starts rfoundry from this folder, nothing to install`,
		`  ${"tools/, stub/".padEnd(40)} rfoundry and the tools it uses`,
		`  ${"requirements.txt".padEnd(40)} the Python packages rfoundry needs`,
		`  ${"LICENSE".padEnd(40)} MIT`, "",
		"Nothing here writes the radio's bootloader, so the radio's own update mode always stays available.", "");

	add("1. SET UP (ONCE)", "",
		"Unpack the zip into a folder and open a terminal in it.", "",
		"Windows",
		"  - Install Python 3 from https://www.python.org/downloads/ (tick \"Add python.exe to PATH\").",
		"  - In the folder's address bar type cmd and press Enter. Then:",
		"      py -m venv venv",
		"      venv\\Scripts\\python -m pip install -r requirements.txt");
	if (route === "dfu") add(
		"  - The radio's bootloader needs the WinUSB driver, once:",
		"      1. Download Zadig from https://zadig.akeo.ie and run it.",
		"      2. Put the radio in its bootloader (the steps under INSTALL below) and plug it in.",
		`      3. In Zadig's list pick "${o.board?.dfu_product || "Digital Radio in USB mode"}" (USB ID 0483 DF11).`,
		"         Not listed: Options > List All Devices.",
		"      4. Choose WinUSB and click Install Driver (or Replace Driver).",
		"      5. Unplug the radio, put it back in its bootloader and connect again.");
	else add("  - The cable's port is COM3, COM4, ...: Device Manager lists it under Ports.");
	add("", "macOS",
		"  - Python 3 from https://www.python.org/downloads/ (or Homebrew: brew install python). In Terminal:",
		"      cd FOLDER   (type cd and a space, then drag the folder onto the window)",
		"      python3 -m venv venv",
		"      venv/bin/python -m pip install -r requirements.txt");
	if (route === "serial") add("  - The cable's port is /dev/cu.usbserial-... (or /dev/cu.wchusbserial-...).");
	add("", "Linux",
		"  - In a terminal, in the folder (Debian, Ubuntu: sudo apt install python3-venv first):",
		"      python3 -m venv venv",
		"      venv/bin/python -m pip install -r requirements.txt");
	if (route === "dfu") add(
		"  - Let your user reach the bootloader (0483:DF11), once:",
		"      echo 'SUBSYSTEM==\"usb\", ATTRS{idVendor}==\"0483\", ATTRS{idProduct}==\"df11\", MODE=\"0666\"' | sudo tee /etc/udev/rules.d/50-rfoundry-dfu.rules",
		"      sudo udevadm control --reload-rules && sudo udevadm trigger");
	add("  - Join the dialout group for the serial port, then log out and in again:",
		"      sudo usermod -aG dialout $USER");
	if (route === "serial") add("  - The cable's port is /dev/ttyUSB0 (or /dev/ttyACM0).");
	add("");

	// the codec source the commands use, and what to do when the kit has none
	let n = 2;
	if (readout) {
		add(`${n++}. READ THE CODEC FROM THE RADIO`, "",
			`The ${name}'s codec comes from its own program flash. If the radio runs its stock firmware, read it first`,
			"(it is also a backup: keep the file). Switch the radio off, connect the cable, run this and switch the",
			"radio on when the tool says (PORT: the cable's port, see step 1):", "",
			...both(`stub/stubtool.py PORT --board ${o.board?.id || o.radio.flash.board} --progmem ${q(stock)}`), "",
			"It takes about a minute; three green flashes end it. Then switch the radio off and on again.",
			"A radio that runs RadioFoundry with working DMR voice keeps its codec: leave out --stock below.", "");
	} else if (!o.stock) {
		add("THE STOCK FILE", "",
			`The kit has no stock firmware file, which gives the codec${route === "dfu" ? " and the bootloader key" : ""}. Copy yours into the stock folder`,
			`(${files.slice(0, 3).join(", ")}${files.length > 3 ? ", ..." : ""}) and put its name in the commands below.`,
			`A radio that runs RadioFoundry with working DMR voice keeps its codec${route === "dfu" ? " and gives the key" : ""}: leave out --stock.`, "");
	}
	const stockArg = ` --stock ${q(stock || `stock/${files[0] || "STOCK.bin"}`)}`;
	const port = route === "serial" ? " --port PORT" : "";
	const cmd = `rfoundry.py flash ${q(img)}${stockArg}${port}`;

	add(`${n++}. CHECK FIRST (WRITES NOTHING)`, "", ...both(`${cmd} --dry-run`), "",
		"rfoundry checks the image against its manifest and prints exactly what it would erase and write.");
	if (route === "serial") add("Only one USB-serial cable plugged in? rfoundry finds it: you can leave out --port PORT.",
		"List the ports: venv/bin/python -m serial.tools.list_ports (Windows: venv\\Scripts\\python ...).");
	add("");

	const steps = o.radio.flash.bootloader || [];
	add(`${n++}. INSTALL`, "", ...both(cmd), "");
	if (route === "dfu") add(
		"Leave the radio as it is when you start: if it runs RadioFoundry, rfoundry asks it about its codec first.",
		"Then rfoundry asks you to put the radio in its bootloader:");
	else add(
		"A radio that runs RadioFoundry stays on: rfoundry installs over the cable with no keys to hold.",
		"Any other radio: rfoundry asks you to put it in update mode:");
	if (steps.length) steps.forEach((s, i) => add(`  ${i + 1}. ${s}`));
	else add(`  rfoundry names the keys to hold. Your radio's section: ${docs("23-radios")}`);
	add("", "rfoundry writes, waits for the radio to restart and checks what it reports. Keep the cable in until it",
		"says it is done.", "");

	add("IF IT FAILS", "",
		"  - Run the same command again. The radio's update mode is never written, so it can always be reached.",
		`  - Command line troubleshooting: ${docs("20-troubleshooting", "command-line-installs-advanced")}`,
		`  - rfoundry on the command line: ${docs("02-installing", "advanced-rfoundry-on-the-command-line")}`,
		`  - Ask the community: ${COMMUNITY}`);
	if (o.repo) add(`  - Report a bug: ${o.repo}/issues (paste what rfoundry printed)`);
	return L.join("\n") + "\n";
}

// rfoundry from this site: downloads/rfoundry-tool.zip, refused unless its size and SHA-256 are site.json's (a
// cached or damaged copy of another build would refuse this firmware's manifest, or worse, accept a wrong board)
export async function toolEntries(site, root = "", fetchFn = globalThis.fetch) {
	const tool = site?.tool;
	if (!tool?.zip) throw new KitError("this site was built without the rfoundry tool (site.json has no \"tool\")");
	const r = await fetchFn(`${root}${tool.zip}`);
	if (!r.ok) throw new KitError(`could not download the rfoundry tool (${r.status || "network error"})`);
	const data = new Uint8Array(await r.arrayBuffer());
	if (data.length !== tool.size || await sha256(data) !== tool.sha256)
		throw new KitError("the rfoundry tool download does not match this page (size or SHA-256): reload the page and try again");
	return unzip(data);
}

// Every file of the kit, in order: [{name, data}]. tool: toolEntries(); fw: {manifest, image}; stock: {name, data} or
// null; text: kitReadme()
export function kitEntries(tool, fw, base, stock, text) {
	const out = [{ name: "README.txt", data: text.replace(/\n/g, "\r\n") }];	// CRLF: Notepad of any age
	out.push({ name: `firmware/${base}.bin`, data: fw.image }, { name: `firmware/${base}.json`, data: JSON.stringify(fw.manifest, null, 1) + "\n" });
	if (stock) out.push({ name: `stock/${stock.name}`, data: stock.data });
	else out.push({ name: "stock/PUT-YOUR-STOCK-FILE-HERE.txt", data: "Copy your stock firmware file (or program flash read) here. README.txt says which.\r\n" });
	for (const [name, data] of tool) out.push({ name, data });
	return out;
}

// The kit's zip. o: {radio, board, firmware: {manifest, image}, stock: {name, data} | null, site (site.json), root (the
// page's path to the site root), url (the site's address), when}
export async function makeKit(o, fetchFn = globalThis.fetch) {
	if (!o.firmware?.image) throw new KitError("no firmware yet: choose and build it first");
	const when = o.when || new Date();
	const base = kitBase(o.firmware.manifest, o.radio.flash.board);
	const stock = o.stock?.data ? { name: o.stock.name.split(/[\\/]/).pop(), data: o.stock.data } : null;
	const text = kitReadme({ radio: o.radio, board: o.board, base, manifest: o.firmware.manifest, stock: stock?.name || null,
		site: o.url, repo: o.site?.repo, git: o.site?.git, date: day(when) });
	const tool = await toolEntries(o.site, o.root, fetchFn);
	return { name: kitName(o.radio, when), bytes: await zip(kitEntries(tool, o.firmware, base, stock, text), when) };
}

// ---------------------------------------------------------------------------------------------------------------
// on the page

// The download button: builds the kit and saves it, or says why not. args(): makeKit's o at the time of the click
export function kitButton(args, { primary = false, label = t("flash.kit.download"), status = null } = {}) {
	const b = button(label, async () => {
		b.disabled = true;
		status?.replaceChildren(h("span", { class: "muted" }, t("flash.kit.making")));
		try {
			const kit = await makeKit(args());
			download(kit.name, kit.bytes, "application/zip");
			status?.replaceChildren(h("span", {}, "✓ ", t("flash.kit.saved", { name: kit.name })));
		} catch (e) {
			status?.replaceChildren(h("span", { class: "kit-error" }, t("flash.kit.failed", { error: e.message })));
		}
		b.disabled = false;
	}, { primary });
	return b;
}

// A step the browser can't run (missingApi): what is missing, and the kit in its place. args(): makeKit's o;
// pickStock(f): takes the user's stock file (app.js pickedStock) and redraws. Returns {el, action}: the panel, and the
// download button for the nav bar (null where rfoundry can't install the radio)
export function kitPanel(args, api, { pickStock = null } = {}) {
	const o = args();
	const route = kitRoute(o.radio);
	const why = t(api === "usb" ? "flash.err.no_usb" : "flash.err.no_serial");
	if (!route) {
		return { el: h("section", { class: "panel kit" }, h("h2", {}, t("flash.kit.title")),
			h("p", { class: "notice bad" }, why), h("p", {}, t("flash.kit.none", { radio: o.radio.name }))), action: null };
	}
	const status = h("p", { class: "small", "aria-live": "polite" });
	const files = stockFiles(o.board);
	const stockPart = o.stock?.data
		? h("li", {}, t("flash.kit.has_stock", { name: o.stock.name }))
		: files.length ? h("li", {}, t("flash.kit.no_stock", { files: files.slice(0, 3).join(", ") }),
			pickStock ? filePicker(t("flash.codec.stock_pick"), ".bin,.zip", pickStock) : null)
			: h("li", {}, t("flash.kit.readout"));
	const el = h("section", { class: "panel kit" },
		h("h2", {}, t("flash.kit.title")),
		h("p", { class: "notice info" }, why, " ", t("flash.kit.lead")),
		h("ul", { class: "kit-list" },
			h("li", {}, t("flash.kit.has_firmware", { version: o.firmware?.manifest?.version || "", radio: o.radio.name })),
			stockPart,
			h("li", {}, t("flash.kit.has_tool")),
			h("li", {}, t("flash.kit.has_readme"))),
		h("p", { class: "small muted" }, t("flash.kit.needs")),
		status);
	return { el, action: kitButton(args, { primary: true, status }) };
}

// Under a failed install: one line offering the kit. null where rfoundry can't install the radio or there is no
// firmware to put in it
export function kitLine(args) {
	const o = args();
	if (!kitRoute(o.radio) || !o.firmware?.image) return null;
	const status = h("span", { class: "small", "aria-live": "polite" });
	return h("div", { class: "notice info kit-line" },
		h("p", {}, h("strong", {}, t("flash.kit.line_title")), " ", t("flash.kit.line")),
		h("div", { class: "actions" }, kitButton(args, { status }), status));
}
