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
