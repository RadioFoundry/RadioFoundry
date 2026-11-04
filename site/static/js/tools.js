// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The Tools page: codeplug conversion (the framework in flasher/codeplug.js). Files are read in the page only.

import { detect, convertersFor } from "./flasher/codeplug.js";

import { page } from "./i18n.js";

const { t, opt } = page;
const input = document.getElementById("convert-file");
const out = document.getElementById("convert-result");

function el(tag, text, cls) {
	const e = document.createElement(tag);
	if (text) e.textContent = text;
	if (cls) e.className = cls;
	return e;
}

input?.addEventListener("change", async () => {
	const f = input.files?.[0];
	if (!f) return;
	const data = new Uint8Array(await f.arrayBuffer());
	const fmt = detect(data, f.name);
	out.replaceChildren();
	if (!fmt) return out.append(el("p", t("tools.convert.unknown"), "notice"));
	out.append(el("p", t("tools.convert.format", { format: opt(`tools.format.${fmt.id}`, fmt.name) })));
	const convs = convertersFor(fmt.id);
	if (!convs.length) return out.append(el("p", t("tools.convert.none"), "muted"));
	out.append(el("p", t("tools.convert.available")));
	const ul = el("ul");
	for (const c of convs) {
		const li = el("li");
		if (c.status === "ready") {
			const b = el("button", opt(`tools.conv.${c.id}`, c.name), "btn");
			b.type = "button";
			b.addEventListener("click", async () => {
				try {
					const r = await c.convert(data, f.name);
					const url = URL.createObjectURL(new Blob([r.data]));
					const a = Object.assign(document.createElement("a"), { href: url, download: r.name });
					a.click();
					setTimeout(() => URL.revokeObjectURL(url), 10000);
				} catch (e) {
					li.append(el("p", e.code ? t(`tools.convert.err.${e.code}`, e.params || {}) : e.message, "notice bad"));
				}
			});
			li.append(b);
		} else {
			li.append(el("span", opt(`tools.conv.${c.id}`, c.name) + " "), el("span", t("tools.convert.planned"), "badge badge-planned"));
		}
		ul.append(li);
	}
	out.append(ul);
});
