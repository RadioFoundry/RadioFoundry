// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Codeplug formats and conversions: the framework the Tools page and the flasher share. A format knows how to
// recognize a file; a converter turns one format into another. Converters with status "planned" are listed so users
// see what's coming; only "ready" ones run. Adding a conversion is adding an entry here with a convert() function.

import { openBundle, CODEPLUG_SPAN } from "./backup.js";
import { startsWith } from "./bytes.js";

export const FORMATS = [
	{ id: "odr-backup", name: "OpenDigitalRadio backup bundle (.zip)", detect: (b) => startsWith(b, "PK\u0003\u0004") },
	{ id: "openfw-image", name: "Codeplug image in the open-firmware layout (SPI flash dump)", detect: (b) => b.length === CODEPLUG_SPAN || b.length === 0x1000000 },
	{ id: "tyt-rdt", name: "TYT CPS codeplug (.rdt)", detect: (b) => b.length === 262709 },
	{ id: "tyt-raw", name: "TYT codeplug memory image (256 KB)", detect: (b) => b.length === 262144 },
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
			return { name: `codeplug-${meta.radio.board}.bin`, data: spi.data.slice(0, CODEPLUG_SPAN) };
		},
	},
	{ id: "rdt-to-odr", from: "tyt-rdt", to: "openfw-image", status: "planned", name: "TYT CPS file to an OpenDigitalRadio codeplug" },
	{ id: "raw-to-odr", from: "tyt-raw", to: "openfw-image", status: "planned", name: "TYT memory image to an OpenDigitalRadio codeplug" },
	{ id: "dfuse-to-odr", from: "dfuse", to: "openfw-image", status: "planned", name: "qdmr binary codeplug to an OpenDigitalRadio codeplug" },
	{ id: "yaml-to-odr", from: "yaml", to: "openfw-image", status: "planned", name: "qdmr YAML to an OpenDigitalRadio codeplug" },
];

export function detect(bytes, name) {
	return FORMATS.find((f) => f.detect(bytes, name)) || null;
}

export function convertersFor(formatId) {
	return CONVERTERS.filter((c) => c.from === formatId);
}
