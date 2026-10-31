// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The first-visit language hint: a page in one language, a browser that asks first for another the site has. The
// header's language links carry each language's own words for the hint (build.py), so it is shown in the language
// it offers. Never a redirect. A language picked or the hint dismissed is remembered and ends it. No DOM in
// suggestLanguage, so the host tests check it.

export const KEY = "rfoundry-lang";

// The site language to offer: the first of the browser's languages (primary subtags, in its order) the site has,
// when it isn't the page's. null when the page is in it, the site has none of them, or a choice is remembered.
export function suggestLanguage(browser, available, page, chosen) {
	if (chosen) return null;
	for (const l of browser || []) {
		const code = String(l).split("-")[0].toLowerCase();
		if (code === page) return null;
		if (available.includes(code)) return code;
	}
	return null;
}

function remember(code) {
	try { localStorage.setItem(KEY, code); } catch (e) { /* private mode: asked again next time */ }
}

export function initLanguageHint(doc = document) {
	const links = [...doc.querySelectorAll(".lang-switch a[hreflang]")];
	if (!links.length) return;
	const page = doc.documentElement.lang;
	for (const a of links) a.addEventListener("click", () => remember(a.hreflang));
	// the menu closes on a click anywhere else
	const menu = doc.querySelector("details.lang-switch");
	doc.addEventListener("click", (e) => { if (menu?.open && !menu.contains(e.target)) menu.open = false; });
	let chosen = null;
	try { chosen = localStorage.getItem(KEY); } catch (e) { /* private mode: nothing remembered */ }
	const code = suggestLanguage(globalThis.navigator?.languages, links.map((a) => a.hreflang), page, chosen);
	const a = links.find((x) => x.hreflang === code);
	if (!a) return;
	const bar = doc.createElement("div");
	bar.className = "lang-hint";
	bar.lang = code;
	bar.setAttribute("role", "region");
	bar.setAttribute("aria-label", a.dataset.hint);
	const p = doc.createElement("p");
	p.textContent = a.dataset.hint;
	const go = doc.createElement("a");
	go.href = a.href;
	go.className = "btn btn-primary";
	go.textContent = a.dataset.go;
	go.addEventListener("click", () => remember(code));
	const no = doc.createElement("button");
	no.type = "button";
	no.className = "btn btn-quiet";
	no.textContent = a.dataset.no;
	no.addEventListener("click", () => { remember(page); bar.remove(); });
	const wrap = doc.createElement("div");
	wrap.className = "wrap";
	wrap.append(p, go, no);
	bar.append(wrap);
	doc.querySelector(".site-header")?.after(bar);
}
