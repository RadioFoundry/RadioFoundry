// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The Support page's download (docs/spec/support-info.md): the report text from what the radio says (static/js/support.js),
// the release match by image hash, and the debug link's SUPPORT requests (static/js/flasher/link.js, debuglink.md 3.29).
// Run with node --test site/tests/ (site/tests/run.sh).

import { test } from "node:test";
import assert from "node:assert/strict";

import { releaseMatch, supportReport } from "../static/js/support.js";
import { readFileSync } from "node:fs";

import { Link, LinkError, CMD, CABLES } from "../static/js/flasher/link.js";

const SHA = "ab".repeat(32);
const releases = [
	{ board: "radio1", variant: "full", version: "v26.11", sha256: SHA },
	{ board: "radio1", variant: "amateur", version: "v26.11", sha256: "cd".repeat(32) },
];

const data = (over = {}) => ({
	site: "site/build.py 1234abcd", browser: "test", at: new Date(Date.UTC(2026, 10, 18)),
	version: { board: "Radio-1", git: "5277f73a", source: "official", repo: "RadioFoundry/RadioFoundry", proto: "1" },
	chip: { family: 0, idcode: 0x10076413, flashKb: 1024, uid: "00112233445566778899aabb", token: 0x33221100 },
	support: { source: "official", "hw.mcu": "STM32F4 413 rev 1007, 1024 KB", "hw.dmr": "HR_C6000", "hw.rf": "AT1846S",
		"image.addr": "0x0800C000", "image.size": "639012" },
	hwopts: [{ name: "Board revision", value: -1, choices: ["V1", "V2"] }],
	sha256: SHA, ...over,
});

test("release match by image hash, any case; nothing for a bad or missing hash", () => {
	assert.deepEqual(releaseMatch(SHA.toUpperCase(), releases).map((r) => r.variant), ["full"]);
	assert.deepEqual(releaseMatch("ef".repeat(32), releases), []);
	assert.deepEqual(releaseMatch("abc", releases), []);
	assert.deepEqual(releaseMatch(null, releases), []);
});

test("report: build, hardware lines, options, and the release it matches", () => {
	const r = supportReport(data(), releases);
	assert.match(r, /^source: official$/m);
	assert.match(r, /^repo: RadioFoundry\/RadioFoundry$/m);
	assert.match(r, /^mcu\.idcode: 0x10076413$/m);
	assert.match(r, /^dmr: HR_C6000$/m);
	assert.match(r, /^rf: AT1846S$/m);
	assert.match(r, /^option Board revision: not set$/m);
	assert.match(r, /^release: one of our release images: v26\.11 radio1 full$/m);
	assert.match(r, /^size: 639012$/m);
	// never the chip UID: its low word is the REBOOT token
	assert.doesNotMatch(r, /00112233445566778899aabb|33221100/i);
});

test("report: a fork's image and an old firmware say so", () => {
	assert.match(supportReport(data({ sha256: "ef".repeat(32) }), releases), /^release: not a RadioFoundry release image/m);
	const old = supportReport(data({ support: null, sha256: null }), releases);
	assert.match(old, /^sha256: unavailable/m);
	assert.match(old, /update to the current release/);
});

// A Link whose requests answer from a script of replies (key=value text), recording what was asked
function fakeLink(replies) {
	const link = Object.create(Link.prototype);
	link.asked = [];
	link.request = async (cmd, payload) => {
		link.asked.push([cmd, ...payload]);
		const r = replies.shift();
		if (typeof r === "number") throw Object.assign(new LinkError(`status ${r}`), { status: r });
		return new TextEncoder().encode(r);
	};
	return link;
}

test("SUPPORT: INFO, then HASH repeated until done, with progress", async () => {
	const link = fakeLink(["source=pr\nhw.rf=AT1846S\n", "image.state=pending\nimage.done=32768\nimage.size=65537\n",
		"image.state=pending\nimage.done=65536\nimage.size=65537\n", `image.state=done\nimage.size=65537\nimage.sha256=${SHA}\n`]);
	assert.equal((await link.support())["hw.rf"], "AT1846S");
	const seen = [];
	assert.equal(await link.imageHash((d, s) => seen.push([d, s])), SHA);
	assert.deepEqual(seen, [[32768, 65537], [65536, 65537]]);
	assert.deepEqual(link.asked, [[CMD.SUPPORT, 0], [CMD.SUPPORT, 1], [CMD.SUPPORT, 1], [CMD.SUPPORT, 1]]);
});

test("SUPPORT: firmware without it (unknown command, not built in) gives null; other failures throw", async () => {
	assert.equal(await fakeLink([1]).support(), null);
	assert.equal(await fakeLink([4]).imageHash(), null);
	assert.equal(await fakeLink(["image.state=none\n"]).imageHash(), null);
	await assert.rejects(fakeLink([3]).support(), LinkError);
});

test("the download's port chooser offers the programming cables, from the one list in link.js", () => {
	const src = readFileSync(new URL("../static/js/supportpage.js", import.meta.url), "utf8");
	assert.match(src, /knownRadioPort\(\[RFOUNDRY_USB, \.\.\.CABLES\]\)/);
	assert.match(src, /requestPort\(\{ filters: \[RFOUNDRY_USB, \.\.\.CABLES\] \}\)/);
	// tools/serial_loader.py USB_SERIAL names the cable chips; every vendor there is offered
	const py = readFileSync(new URL("../../tools/serial_loader.py", import.meta.url), "utf8");
	const list = py.slice(py.indexOf("USB_SERIAL = ["), py.indexOf("]", py.indexOf("USB_SERIAL = [")));
	const vendors = new Set([...list.matchAll(/\(0x([0-9A-Fa-f]{4}),/g)].map((m) => parseInt(m[1], 16)));
	assert.deepEqual([...vendors].sort(), CABLES.map((c) => c.usbVendorId).sort());
});
