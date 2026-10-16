// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The radio as the page wants the user to see it: a handheld with its antenna, knobs, LED on top, PTT, speaker grille,
// screen and keypad. The LED blinks the way the real one does in each state (the stub's status blink: half-second
// phases while waiting, an eighth of a second while data moves, three green flashes when done), a callout names the
// blink, and a caption says the same in words, so nobody has to read the animation. Colors follow the radio's frame
// in radio.json.

import { h, svg, t } from "./dom.js";

// led: off | wait (slow orange) | busy (fast orange) | done (three green flashes) | green (steady)
// screen: off | loader ("PC Loader Connected") | on | any (the state doesn't matter yet: no screen line in the caption)
// knob: null | "cycle" (off, then on) | "on"
// note: a line for the caption in place of the LED's, while the LED says nothing
export function radioTop(radio, initial = {}) {
	const body = radio?.frame?.body || "#22252a", accent = radio?.frame?.accent || "#e0a526";
	const dark = "#15171b", trim = "#2b2f35";
	// LED on the top face, between the antenna and the channel knob; the callout rises from it to the label
	const lx = 96, ly = 92;
	const led = svg("circle", { cx: lx, cy: ly, r: 5, class: "rt-led" });
	const glow = svg("circle", { cx: lx, cy: ly, r: 12, class: "rt-glow" });
	const label = svg("text", { x: lx - 6, y: 20, class: "rt-label" });
	const callout = svg("g", { class: "rt-led-callout" },
		svg("line", { x1: lx, y1: ly - 8, x2: lx, y2: 27, class: "rt-callout" }), label);
	// the arrow around the volume knob, which is also the power switch
	const knobTurn = svg("g", { class: "rt-knob-turn" },
		svg("path", { d: "M 168 58 A 20 20 0 1 0 132 58", fill: "none", "stroke-width": 3, class: "rt-arrow" }),
		svg("path", { d: "M 126 54 L 132 63 L 138 54 Z", class: "rt-arrow-head" }));
	const screenText = svg("text", { x: 116, y: 186, "text-anchor": "middle", class: "rt-screen-text" });
	const screenText2 = svg("text", { x: 116, y: 201, "text-anchor": "middle", class: "rt-screen-text small" });
	const screen = svg("rect", { x: 74, y: 166, width: 84, height: 44, rx: 4, class: "rt-screen" });
	const key = (x, y, w = 24, ht = 12) => svg("rect", { x, y, width: w, height: ht, rx: 3, fill: "rgb(255 255 255 / 14%)" });
	const keys = [key(78, 220, 22), key(105, 220, 22), key(132, 220, 22)];
	for (let r = 0; r < 4; r++) for (let k = 0; k < 3; k++) keys.push(key(78 + 27 * k, 240 + 15 * r, 22, 10));
	// a knob: a cylinder with grip ridges
	const knob = (x, y, w, ht, cap) => svg("g", {},
		svg("rect", { x, y, width: w, height: ht, rx: 4, fill: trim }),
		Array.from({ length: Math.floor(w / 5) - 1 }, (_, i) => svg("rect", { x: x + 4 + 5 * i, y: y + 3, width: 2, height: ht - 6, rx: 1, fill: "rgb(0 0 0 / 35%)" })),
		cap ? svg("rect", { x, y: y - 3, width: w, height: 6, rx: 3, fill: cap }) : null);
	const pic = svg("svg", { viewBox: "0 0 230 312", class: "rt-svg", role: "img" },
		// antenna with its collar, the channel knob, the volume knob (power), side keys and PTT, body, belt-clip edge
		svg("rect", { x: 64, y: 30, width: 16, height: 60, rx: 8, fill: dark }),
		svg("rect", { x: 60, y: 80, width: 24, height: 14, rx: 3, fill: trim }),
		knob(106, 72, 18, 20),
		knob(136, 64, 26, 28, accent),
		svg("rect", { x: 42, y: 128, width: 10, height: 44, rx: 3, fill: trim }),
		svg("rect", { x: 44, y: 178, width: 8, height: 14, rx: 2, fill: trim }),
		svg("rect", { x: 44, y: 196, width: 8, height: 14, rx: 2, fill: trim }),
		svg("rect", { x: 50, y: 88, width: 132, height: 214, rx: 16, fill: body }),
		svg("rect", { x: 50, y: 88, width: 132, height: 14, rx: 8, fill: "rgb(255 255 255 / 8%)" }),
		svg("rect", { x: 176, y: 110, width: 6, height: 170, rx: 3, fill: "rgb(0 0 0 / 25%)" }),
		// speaker grille: the upper half of the front, as on a handheld
		svg("rect", { x: 66, y: 108, width: 100, height: 50, rx: 8, fill: "rgb(0 0 0 / 22%)" }),
		[116, 124, 132, 140, 148].map((y) => svg("rect", { x: 74, y, width: 84, height: 3, rx: 1.5, fill: "rgb(0 0 0 / 45%)" })),
		knobTurn,
		glow, led, callout,
		screen, screenText, screenText2,
		keys);
	const caption = h("p", { class: "rt-caption" });
	const el = h("figure", { class: "radio-top" }, pic, caption);
	const set = ({ led: l = "off", screen: sc = "off", knob = null, note = null } = {}) => {
		el.dataset.led = l;
		el.dataset.screen = sc;
		el.dataset.knob = knob || "";
		label.textContent = l === "off" ? "" : t(`flash.c7.visual.tag_${l}`);
		screenText.textContent = sc === "loader" ? "PC Loader" : sc === "on" ? "ODR" : "";
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
