// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Shared page behavior: theme toggle, the narrow-screen menu, the features filter.

const root = document.documentElement;

document.querySelector(".theme-toggle")?.addEventListener("click", () => {
	const dark = root.dataset.theme ? root.dataset.theme === "dark" : matchMedia("(prefers-color-scheme: dark)").matches;
	root.dataset.theme = dark ? "light" : "dark";
	try { localStorage.setItem("odr-theme", root.dataset.theme); } catch (e) { /* private mode: not remembered */ }
});

const navToggle = document.querySelector(".nav-toggle");
navToggle?.addEventListener("click", () => {
	const nav = document.getElementById("site-nav");
	const open = nav.classList.toggle("open");
	navToggle.setAttribute("aria-expanded", String(open));
});

const filter = document.getElementById("feature-filter");
filter?.addEventListener("input", () => {
	const q = filter.value.trim().toLowerCase();
	for (const body of document.querySelectorAll("#features-table tbody")) {
		let shown = 0;
		for (const tr of body.querySelectorAll("tr[data-search]")) {
			const hit = !q || tr.dataset.search.toLowerCase().includes(q);
			tr.hidden = !hit;
			shown += hit;
		}
		body.hidden = shown === 0;
	}
});

// radio pages: show a screenshot on the drawing of the radio (the overlay sits on the frame's screen rectangle)
const screen = document.getElementById("device-screen");
for (const b of document.querySelectorAll(".shot-show")) {
	b.addEventListener("click", () => {
		screen.src = b.dataset.src;
		for (const o of document.querySelectorAll(".shot-show")) o.setAttribute("aria-pressed", String(o === b));
		screen.closest(".radio-device").scrollIntoView({ behavior: "smooth", block: "nearest" });
	});
}

// glossary terms in the docs: <abbr class="gl" title="..."> becomes a popover on hover, focus and tap.
// Without this script the browser's own title tooltip still shows the definition.
const terms = document.querySelectorAll("abbr.gl");
if (terms.length) {
	const pop = document.createElement("div");
	pop.id = "gl-pop";
	pop.className = "gl-pop";
	pop.setAttribute("role", "tooltip");
	pop.hidden = true;
	const text = document.createElement("span");
	const more = document.createElement("a");
	more.textContent = "More";
	pop.append(text, " ", more);
	document.body.append(pop);

	let current = null;
	let timer = 0;
	const hide = () => {
		clearTimeout(timer);
		pop.hidden = true;
		current?.removeAttribute("aria-describedby");
		current = null;
	};
	const later = () => { clearTimeout(timer); timer = setTimeout(hide, 200); };
	const show = (el) => {
		clearTimeout(timer);
		if (current === el && !pop.hidden) return;
		current?.removeAttribute("aria-describedby");
		current = el;
		text.textContent = el.dataset.def;
		more.href = el.dataset.more;
		el.setAttribute("aria-describedby", pop.id);
		pop.hidden = false;
		const r = el.getBoundingClientRect();
		const w = Math.min(pop.offsetWidth, innerWidth - 16);
		const left = Math.max(8, Math.min(r.left + scrollX, scrollX + innerWidth - w - 8));
		const below = r.bottom + pop.offsetHeight + 8 < innerHeight || r.top < pop.offsetHeight + 8;
		pop.style.left = left + "px";
		pop.style.top = (below ? r.bottom + scrollY + 6 : r.top + scrollY - pop.offsetHeight - 6) + "px";
	};
	for (const el of terms) {
		el.dataset.def = el.title;
		el.removeAttribute("title");		// the popover replaces the native tooltip
		el.addEventListener("mouseenter", () => show(el));
		el.addEventListener("mouseleave", later);
		el.addEventListener("focus", () => show(el));
		el.addEventListener("blur", later);
		el.addEventListener("click", (ev) => {
			ev.preventDefault();
			if (current === el && !pop.hidden) hide(); else show(el);
		});
		el.addEventListener("keydown", (ev) => {
			if (ev.key === "Enter") location.href = el.dataset.more;	// keyboard: the definition is already shown
		});
	}
	pop.addEventListener("mouseenter", () => clearTimeout(timer));
	pop.addEventListener("mouseleave", later);
	pop.addEventListener("focusin", () => clearTimeout(timer));
	pop.addEventListener("focusout", later);
	document.addEventListener("keydown", (ev) => { if (ev.key === "Escape") hide(); });
	document.addEventListener("click", (ev) => { if (!ev.target.closest("abbr.gl, #gl-pop")) hide(); });
}
