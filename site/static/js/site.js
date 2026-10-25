// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Shared page behavior: theme toggle, the narrow-screen menu, the features filter, the home page's carousel, the
// screenshots page's radio picker.

const root = document.documentElement;

document.querySelector(".theme-toggle")?.addEventListener("click", () => {
	const dark = root.dataset.theme ? root.dataset.theme === "dark" : matchMedia("(prefers-color-scheme: dark)").matches;
	root.dataset.theme = dark ? "light" : "dark";
	try { localStorage.setItem("rfoundry-theme", root.dataset.theme); } catch (e) { /* private mode: not remembered */ }
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

// the screenshots page: the radio links switch the section shown at once and keep the address (#<radio>) for links.
// Without the script CSS shows the section the address names (:target), else the first.
const screensPage = document.querySelector("[data-screens]");
if (screensPage) {
	const all = screensPage.querySelector(".rshots-all");
	const sections = [...all.querySelectorAll(".rshots")];
	const picks = [...screensPage.querySelectorAll(".radio-pick a")];
	const toggle = screensPage.querySelector(".variants-toggle");
	const pick = (id) => {
		const cur = sections.find((s) => s.id === id) ?? sections[0];
		for (const s of sections) s.classList.toggle("is-current", s === cur);
		for (const a of picks) {
			if (a.hash === "#" + cur.id) a.setAttribute("aria-current", "true"); else a.removeAttribute("aria-current");
		}
		// a radio with no other colors or themes (a monochrome display) has nothing to toggle
		toggle.hidden = !cur.querySelector(".gshot-variant");
		return cur;
	};
	all.classList.add("js");
	pick(decodeURIComponent(location.hash.slice(1)));
	for (const a of picks) {
		a.addEventListener("click", (ev) => {
			ev.preventDefault();
			const cur = pick(a.hash.slice(1));
			history.replaceState(null, "", "#" + cur.id);
		});
	}
	addEventListener("hashchange", () => pick(decodeURIComponent(location.hash.slice(1))));
}

// the home page's screenshot carousel. The row scrolls and snaps on its own (CSS); this adds previous and next,
// which move by the screens in view, and the position ("3–5 of 12"). Without the script the row still scrolls.
for (const box of document.querySelectorAll("[data-carousel]")) {
	const track = box.querySelector(".carousel-track");
	const slides = [...track.children];
	const controls = box.querySelector(".carousel-controls");
	const prev = box.querySelector(".carousel-prev");
	const next = box.querySelector(".carousel-next");
	const pos = box.querySelector(".carousel-pos");
	const still = matchMedia("(prefers-reduced-motion: reduce)");
	// slide edges relative to the row's visible area; a slide counts as in view when it shows nearly whole
	const edges = () => {
		const r = track.getBoundingClientRect();
		return slides.map((s) => { const b = s.getBoundingClientRect(); return { l: b.left - r.left, r: b.right - r.left }; });
	};
	const go = (dx) => track.scrollBy({ left: dx, behavior: still.matches ? "auto" : "smooth" });
	next.addEventListener("click", () => {
		const e = edges(), w = track.clientWidth;
		// the first slide cut off on the right comes to the left edge; one wider than the row moves on by one
		let i = e.findIndex((s) => s.r > w + 2);
		if (i < 0) i = slides.length - 1;
		if (e[i].l <= 2 && i + 1 < slides.length) i++;
		go(e[i].l);
	});
	prev.addEventListener("click", () => {
		const e = edges(), w = track.clientWidth;
		// a row's width back: the leftmost earlier slide that still fits with the first one in view
		const before = e.map((s, i) => i).filter((i) => e[i].l < -2);
		if (!before.length) return;
		const last = before[before.length - 1];
		const i = before.find((j) => e[last].r - e[j].l <= w + 2) ?? last;
		go(e[i].l);
	});
	let timer = 0;
	const update = () => {
		const e = edges(), w = track.clientWidth;
		const seen = e.map((s, i) => i).filter((i) => e[i].l >= -2 && e[i].r <= w + 2);
		const from = (seen[0] ?? e.findIndex((s) => s.r > 0)) + 1;
		const to = seen.length ? seen[seen.length - 1] + 1 : from;
		const fmt = from === to ? pos.dataset.one : pos.dataset.range;
		const text = fmt.replace("{from}", from).replace("{to}", to).replace("{n}", slides.length);
		if (pos.textContent !== text) pos.textContent = text;	// the live region speaks only on a change
		prev.disabled = track.scrollLeft <= 2;
		next.disabled = track.scrollLeft + w >= track.scrollWidth - 2;
	};
	const later = () => { clearTimeout(timer); timer = setTimeout(update, 80); };
	track.addEventListener("scroll", later, { passive: true });
	addEventListener("resize", later);
	// lazy images take their width from their attributes, but a late layout change still moves the edges
	for (const img of track.querySelectorAll("img")) img.addEventListener("load", later);
	controls.hidden = false;
	update();
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
