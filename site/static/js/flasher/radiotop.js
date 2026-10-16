// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The radio as the page wants the user to see it: a handheld with its antenna, power knob, LED on top, screen and
// keypad. The LED blinks the way the real one does in each state (the stub's status blink: half-second phases while
// waiting, an eighth of a second while data moves, three green flashes when done), and a caption says the same in
// words, so nobody has to read the animation. Colors follow the radio's frame in radio.json.

import { h, svg, t } from "./dom.js";

// led: off | wait (slow yellow) | busy (fast yellow) | done (three green flashes) | green (steady)
// screen: off | loader ("PC Loader Connected") | on | any (the state doesn't matter yet: no screen line in the caption)
// knob: null | "cycle" (off, then on) | "on"
// note: a line for the caption in place of the LED's, while the LED says nothing
export function radioTop(radio, initial = {}) {
	const body = radio?.frame?.body || "#22252a", accent = radio?.frame?.accent || "#e0a526";
	const led = svg("circle", { cx: 108, cy: 82, r: 6, class: "rt-led" });
	const glow = svg("circle", { cx: 108, cy: 82, r: 14, class: "rt-glow" });
	const knobTurn = svg("g", { class: "rt-knob-turn" },
		svg("path", { d: "M 168 34 A 19 19 0 1 1 142 34", fill: "none", "stroke-width": 3, class: "rt-arrow" }),
		svg("path", { d: "M 136 38 L 142 30 L 148 38 Z", class: "rt-arrow-head" }));
	const screenText = svg("text", { x: 110, y: 170, "text-anchor": "middle", class: "rt-screen-text" });
	const screenText2 = svg("text", { x: 110, y: 188, "text-anchor": "middle", class: "rt-screen-text small" });
	const screen = svg("rect", { x: 52, y: 138, width: 116, height: 70, rx: 5, class: "rt-screen" });
	const key = (x, y, w, hgt) => svg("rect", { x, y, width: w, height: hgt, rx: 4, fill: "rgb(255 255 255 / 12%)" });
	const keys = [];
	for (let r = 0; r < 4; r++) for (let k = 0; k < 3; k++) keys.push(key(56 + 38 * k, 252 + 28 * r, 32, 22));
	const pic = svg("svg", { viewBox: "0 0 220 384", class: "rt-svg", role: "img" },
		// antenna, power knob, side PTT, body
		svg("rect", { x: 52, y: 4, width: 20, height: 76, rx: 9, fill: "#111317" }),
		svg("rect", { x: 140, y: 40, width: 30, height: 36, rx: 6, fill: "#2b2f35" }),
		svg("rect", { x: 144, y: 34, width: 22, height: 10, rx: 3, fill: accent }),
		svg("rect", { x: 28, y: 150, width: 10, height: 56, rx: 3, fill: "#2b2f35" }),
		svg("rect", { x: 36, y: 72, width: 148, height: 304, rx: 20, fill: body }),
		svg("rect", { x: 36, y: 72, width: 148, height: 20, rx: 10, fill: "rgb(255 255 255 / 8%)" }),
		// speaker grille
		[104, 112, 120].map((y) => svg("rect", { x: 66, y, width: 88, height: 3, rx: 1.5, fill: "rgb(0 0 0 / 35%)" })),
		knobTurn,
		glow, led,
		svg("line", { x1: 108, y1: 74, x2: 108, y2: 30, class: "rt-callout" }),
		svg("text", { x: 108, y: 24, "text-anchor": "middle", class: "rt-label" }, t("flash.c7.visual.led")),
		svg("line", { x1: 171, y1: 58, x2: 178, y2: 58, class: "rt-callout" }),
		svg("text", { x: 181, y: 62, class: "rt-label" }, t("flash.c7.visual.knob")),
		screen, screenText, screenText2,
		// navigation keys, keypad
		key(52, 220, 34, 20), key(93, 220, 34, 20), key(134, 220, 34, 20),
		keys);
	const caption = h("p", { class: "rt-caption" });
	const el = h("figure", { class: "radio-top" }, pic, caption);
	const set = ({ led: l = "off", screen: sc = "off", knob = null, note = null } = {}) => {
		el.dataset.led = l;
		el.dataset.screen = sc;
		el.dataset.knob = knob || "";
		screenText.textContent = sc === "loader" ? "PC Loader" : sc === "on" ? "OpenDigitalRadio" : "";
		screenText2.textContent = sc === "loader" ? "Connected" : "";
		const words = [knob === "cycle" ? t("flash.c7.visual.cycle") : knob === "on" ? t("flash.c7.visual.on") : null,
			note || t(`flash.c7.visual.led_${l}`), sc === "any" ? null : t(`flash.c7.visual.screen_${sc}`)].filter(Boolean);
		caption.replaceChildren(...words.map((w) => h("span", {}, w)));
		pic.setAttribute("aria-label", words.join(" "));
		// restart the animation on every change, so "done" plays its three flashes from the start
		led.style.animation = "none";
		void led.getBBox?.();
		led.style.animation = "";
	};
	set(initial);
	return { el, set };
}
