// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The Support page's "Download support info": reads the radio over its USB port or programming cable and saves a text
// report (support.js, docs/spec/support-info.md). Only reads: VERSION, INFO_CHIP, SUPPORT, HWOPT LIST, then the image hash a step per
// request, so the radio keeps receiving while it runs. Nothing is sent anywhere: the file stays on the computer.

import { Link, SerialStream, serialSupported, serialApi, RFOUNDRY_USB, CABLES, knownRadioPort, noDeviceChosen } from "./flasher/link.js";
import { toSerial, CLICK_WAIT_MS } from "./flasher/netmode.js";
import { h, t, ROOT, download } from "./flasher/dom.js";
import { supportReport } from "./support.js";

const go = document.getElementById("support-download");
const out = document.getElementById("support-result");

function say(text, cls = "") {
	out.replaceChildren(h("p", { class: cls }, text));
}

async function releases() {
	try {
		return (await (await fetch(`${ROOT}data/releases.json`)).json()).releases || [];
	} catch (e) {
		return [];
	}
}

async function collect(link) {
	const version = await link.version();
	let chip = null, support = null, hwopts = null, sha256 = null;
	try { chip = await link.chip(); } catch (e) { /* older firmware */ }
	support = await link.support();
	try { hwopts = await link.hwopts(); } catch (e) { /* older firmware */ }
	if (support) {
		sha256 = await link.imageHash((done, size) => say(t("support.dl.hashing", { pct: size ? Math.floor(done * 100 / size) : 0 })));
	}
	return { version, chip, support, hwopts, sha256 };
}

go?.addEventListener("click", async () => {
	if (!serialSupported()) return say(t("support.dl.no_serial"), "notice bad");
	go.disabled = true;
	let stream = null;
	try {
		// a radio in USB network mode is asked for its serial port first; the wait is short, the chooser below must
		// still open in this click
		await toSerial({ http: false, ms: CLICK_WAIT_MS });
		const known = await knownRadioPort([RFOUNDRY_USB, ...CABLES]);
		stream = known?.stream || await SerialStream.open(await serialApi().requestPort({ filters: [RFOUNDRY_USB, ...CABLES] }));
		say(t("support.dl.reading"));
		const data = await collect(new Link(stream));
		const text = supportReport({ ...data, site: document.querySelector('meta[name="generator"]')?.content || "",
			browser: navigator.userAgent, at: new Date() }, await releases());
		const board = String(data.version.board || "radio").replace(/[^A-Za-z0-9_-]+/g, "-");
		download(`rfoundry-support-${board}.txt`, new TextEncoder().encode(text), "text/plain");
		say(t(data.support ? "support.dl.done" : "support.dl.done_old"), data.support ? "" : "notice");
	} catch (e) {
		if (noDeviceChosen(e)) say(t("support.dl.none"), "notice");
		else say(t("support.dl.failed", { error: String(e?.message || e) }), "notice bad");
	} finally {
		try { await stream?.close(); } catch (e) { /* gone */ }
		go.disabled = false;
	}
});
