// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The support download's report (docs/spec/support-info.md): what the radio says of its build and hardware, and whether
// its firmware image is one of our releases. Pure: the Support page (supportpage.js) reads the radio and saves the text.
//
//   releaseMatch(sha256, releases)   the releases.json entries with this image hash ([] when none)
//   supportReport(data, releases)    the report text
//
// The text is for the developers reading an issue, so it is English, as the debug link's is. It never carries the chip
// UID: the debug link uses its low word as the REBOOT token.

export function releaseMatch(sha256, releases) {
	const h = String(sha256 || "").toLowerCase();
	return /^[0-9a-f]{64}$/.test(h) ? (releases || []).filter((r) => String(r.sha256 || "").toLowerCase() === h) : [];
}

const FAMILY = ["STM32F4", "HR_C7000"];

// data: {site, browser, at (Date), version (VERSION kv), chip (Link.chip(), or null), support (SUPPORT INFO kv, or null:
// firmware without it), hwopts (Link.hwopts(), or null), sha256 (hex, or null)}
export function supportReport(data, releases) {
	const out = ["RadioFoundry support info", ""];
	const line = (k, v) => out.push(`${k}: ${v ?? "-"}`);
	line("generated", (data.at || new Date()).toISOString());
	line("site", data.site);
	line("browser", data.browser);
	out.push("", "[firmware]");
	const v = data.version || {};
	for (const k of ["board", "release", "git", "git_time", "source", "repo", "preset", "start", "codec", "features", "off", "display", "proto"]) {
		if (v[k] !== undefined) line(k, v[k]);
	}
	out.push("", "[image]");
	const matches = releaseMatch(data.sha256, releases);
	if (!data.sha256) {
		line("sha256", "unavailable (firmware without the support info)");
	} else {
		line("sha256", data.sha256);
		line("release", matches.length
			? `one of our release images: ${[...new Set(matches.map((m) => `${m.version} ${m.board} ${m.variant}`))].join(", ")}`
			: "not a RadioFoundry release image (a fork, a local or build-service build, or a changed image)");
	}
	const s = data.support;
	if (s) {
		if (s["image.size"]) line("size", s["image.size"]);
		if (s["image.addr"]) line("addr", s["image.addr"]);
	}
	out.push("", "[hardware]");
	const c = data.chip;
	if (c) {
		line("mcu.family", FAMILY[c.family] || `family ${c.family}`);
		line("mcu.idcode", `0x${(c.idcode >>> 0).toString(16).padStart(8, "0")}`);
		line("mcu.flash_kb", c.flashKb);
	}
	if (s) {
		for (const [k, val] of Object.entries(s)) {
			if (k.startsWith("hw.")) line(k.slice(3), val);
		}
	} else {
		out.push("(this firmware has no support info: update to the current release)");
	}
	for (const o of data.hwopts || []) {
		line(`option ${o.name}`, o.value >= 0 && o.value < o.choices.length ? o.choices[o.value] : "not set");
	}
	return out.join("\n") + "\n";
}
