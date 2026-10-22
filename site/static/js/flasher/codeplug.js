// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Codeplug formats and conversions: the framework the Tools page and the flasher share. A format knows how to
// recognize a file; a converter turns one format into another. Converters with status "planned" are listed so users
// see what's coming; only "ready" ones run. Adding a conversion is adding an entry here with a convert() function.

import { openBundle, CODEPLUG_SPAN } from "./backup.js";
import { startsWith } from "./bytes.js";
import { classify } from "../codeplug/detect.js";
import { STOCK, RDT_MD380, RDT_UV380 } from "../codeplug/stock_tyt.js";

// A stock TYT-family codeplug in a dump or memory image (docs/spec/codeplug-stock-tyt.md): its family, or null
const stockLayout = (b) => {
	const { layout } = classify(b);
	return STOCK[layout] ? layout : null;
};

export const FORMATS = [
	{ id: "odr-backup", name: "OpenDigitalRadio backup bundle (.zip)", detect: (b) => startsWith(b, "PK\u0003\u0004") },
	{ id: "tyt-stock", name: "Stock TYT-family codeplug: DM-1701, RT84, MD-UV380/390, MD-380 (SPI flash dump or memory image)", detect: (b) => b.length > 0x40000 && b.length !== RDT_UV380 && !!stockLayout(b) },
	{ id: "openfw-image", name: "Codeplug image in the open-firmware layout (SPI flash dump)", detect: (b) => b.length === CODEPLUG_SPAN || b.length === 0x1000000 },
	{ id: "tyt-rdt", name: "TYT MD-380 CPS codeplug (.rdt)", detect: (b) => b.length === RDT_MD380 },
	{ id: "tyt-uv380-rdt", name: "TYT MD-UV380 family CPS codeplug (.rdt)", detect: (b) => b.length === RDT_UV380 },
	{ id: "tyt-raw", name: "TYT MD-380 codeplug memory image (256 KB)", detect: (b) => b.length === 262144 },
	{ id: "dfuse", name: "DfuSe file (qdmr binary codeplug)", detect: (b) => startsWith(b, "DfuSe") },
	{ id: "yaml", name: "qdmr YAML codeplug", detect: (b, name) => /\.ya?ml$/i.test(name || "") },
	{ id: "stock-firmware", name: "Stock firmware file (not a codeplug)", detect: (b) => startsWith(b, "OutSecurityBin") },
];

export const CONVERTERS = [
	{
		id: "backup-to-image", from: "odr-backup", to: "openfw-image", status: "ready",
		name: "Codeplug image from a backup bundle (the open-firmware layout, 1.1 MB)",
		async convert(bytes) {
			const { meta, parts } = await openBundle(bytes);
			const spi = parts.get("spi-flash");
			if (!spi || spi.data.length < CODEPLUG_SPAN) throw new Error("this backup has no SPI flash read covering the codeplug");
			const name = `codeplug-${meta.radio.board}.bin`;
			const { layout } = classify(spi.data);
			// a backup taken right after ODR replaced the stock firmware still holds the stock codeplug
			if (STOCK[layout]) return { name, data: await fromStock(spi.data, layout) };
			if (layout === "unknown") throw new Error("the codeplug in this backup is in a layout this page cannot read");
			return { name, data: spi.data.slice(0, CODEPLUG_SPAN) };
		},
	},
	{
		id: "stock-to-odr", from: "tyt-stock", to: "openfw-image", status: "ready",
		name: "Stock codeplug to an OpenDigitalRadio codeplug image (open-firmware layout)",
		async convert(bytes, name) {
			return { name: `${base(name)}-odr.bin`, data: await fromStock(bytes, stockLayout(bytes)) };
		},
	},
	{
		id: "stock-to-yaml", from: "tyt-stock", to: "yaml", status: "ready",
		name: "Stock codeplug to qdmr YAML (through the OpenDigitalRadio layout)",
		async convert(bytes, name) {
			const { exportQdmr } = await import("../codeplug/qdmr.js");
			const { decode, CodeplugImage } = await codeplugModules();
			const data = await fromStock(bytes, stockLayout(bytes));
			const { text } = exportQdmr(decode(CodeplugImage.from(data)));
			return { name: `${base(name)}.yaml`, data: new TextEncoder().encode(text) };
		},
	},
	{
		id: "rdt-to-odr", from: "tyt-rdt", to: "openfw-image", status: "ready", name: "TYT MD-380 CPS file to an OpenDigitalRadio codeplug image",
		async convert(bytes, name) {
			return { name: `${base(name)}-odr.bin`, data: await fromStock(bytes, "tyt-md380") };
		},
	},
	{
		id: "raw-to-odr", from: "tyt-raw", to: "openfw-image", status: "ready", name: "TYT MD-380 memory image to an OpenDigitalRadio codeplug image",
		async convert(bytes, name) {
			return { name: `${base(name)}-odr.bin`, data: await fromStock(bytes, "tyt-md380") };
		},
	},
	{ id: "dfuse-to-odr", from: "dfuse", to: "openfw-image", status: "planned", name: "qdmr binary codeplug to an OpenDigitalRadio codeplug" },
	{
		id: "yaml-to-odr", from: "yaml", to: "openfw-image", status: "ready",
		name: "qdmr YAML to an OpenDigitalRadio codeplug image (open-firmware layout)",
		async convert(bytes, name) {
			// the codeplug modules load only when a conversion runs
			const { importQdmr } = await import("../codeplug/qdmr.js");
			const { empty, encode } = await import("../codeplug/model.js");
			const m = empty();
			try {
				importQdmr(m, new TextDecoder().decode(bytes));
			} catch (e) {
				throw new Error(e.code === "not_qdmr" ? "this file is not a qdmr codeplug" : `this file is not valid YAML (${e.params?.error || e.message})`);
			}
			return { name: `${base(name)}.bin`, data: encode(m).bytes.slice(0, CODEPLUG_SPAN) };
		},
	},
	{
		id: "odr-to-yaml", from: "openfw-image", to: "yaml", status: "ready",
		name: "OpenDigitalRadio codeplug image to qdmr YAML",
		async convert(bytes, name) {
			const { exportQdmr } = await import("../codeplug/qdmr.js");
			const { decode } = await import("../codeplug/model.js");
			const { CodeplugImage } = await import("../codeplug/image.js");
			const { text } = exportQdmr(decode(CodeplugImage.from(bytes.subarray(0, CODEPLUG_SPAN))));
			return { name: `${base(name)}.yaml`, data: new TextEncoder().encode(text) };
		},
	},
];

async function codeplugModules() {
	const { empty, encode, decode } = await import("../codeplug/model.js");
	const { importStock, detectStock } = await import("../codeplug/stock_tyt.js");
	const { CodeplugImage } = await import("../codeplug/image.js");
	return { empty, encode, decode, importStock, detectStock, CodeplugImage };
}

// An open-firmware codeplug image converted from a stock one
async function fromStock(bytes, layout) {
	const { empty, encode, importStock, detectStock } = await codeplugModules();
	if (!layout || detectStock(bytes) !== layout) throw new Error("this file holds no stock codeplug this page can read");
	const m = empty();
	importStock(m, bytes, layout);
	return encode(m).bytes.slice(0, CODEPLUG_SPAN);
}

const base = (name) => String(name || "codeplug").replace(/^.*[\\/]/, "").replace(/\.[^.]+$/, "") || "codeplug";

export function detect(bytes, name) {
	return FORMATS.find((f) => f.detect(bytes, name)) || null;
}

export function convertersFor(formatId) {
	return CONVERTERS.filter((c) => c.from === formatId);
}
