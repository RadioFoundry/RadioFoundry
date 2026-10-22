// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The Tools page: codeplug conversion (the framework in flasher/codeplug.js). Files are read in the page only.

import { detect, convertersFor } from "./flasher/codeplug.js";

const STRINGS = JSON.parse(document.getElementById("i18n")?.textContent || "{}");
const t = (key, fmt = {}) => (STRINGS[key] || key).replace(/\{(\w+)\}/g, (_, k) => fmt[k] ?? `{${k}}`);
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
	out.append(el("p", t("tools.convert.format", { format: fmt.name })));
	const convs = convertersFor(fmt.id);
	if (!convs.length) return out.append(el("p", t("tools.convert.none"), "muted"));
	out.append(el("p", t("tools.convert.available")));
	const ul = el("ul");
	for (const c of convs) {
		const li = el("li");
		if (c.status === "ready") {
			const b = el("button", c.name, "btn");
			b.type = "button";
			b.addEventListener("click", async () => {
				try {
					const r = await c.convert(data, f.name);
					const url = URL.createObjectURL(new Blob([r.data]));
					const a = Object.assign(document.createElement("a"), { href: url, download: r.name });
					a.click();
					setTimeout(() => URL.revokeObjectURL(url), 10000);
				} catch (e) {
					li.append(el("p", e.message, "notice bad"));
				}
			});
			li.append(b);
		} else {
			li.append(el("span", c.name + " "), el("span", t("tools.convert.planned"), "badge badge-planned"));
		}
		ul.append(li);
	}
	out.append(ul);
});
