// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The radio as the page wants the user to see it while entering or using update mode: the radio's own picture, the
// one its radio page shows (data/radios.json frame_image: its illustration with the screen dark, or the generic drawing
// per shape the site build makes for a radio without one, docs/dev/website.md "Radio images"), so a new board gets it
// with no change here. Over the picture's screen (frame_geometry) what the screen shows; under it an LED that blinks
// the way the real one does in each state (the stub's status blink: half-second phases while waiting, an eighth of a
// second while data moves, three green flashes when done), and a caption that says the same in words, so nobody has to
// read the animation.

import { h, svg, t, ROOT } from "./dom.js";

// The picture's address in the site, or null when the data has none
export const pictureSrc = (radio) => radio?.frame_image || null;

// The radio's picture alone, for steps that say nothing about the LED or the screen (the bootloader entry: what a
// radio shows there is in its steps' words, not in the data), or null without one
export function radioPicture(radio) {
	const src = pictureSrc(radio);
	return src ? h("figure", { class: "radio-top radio-pic" }, h("img", { src: ROOT + src, alt: radio.name || "", class: "rt-img" })) : null;
}

// led: off | wait (slow orange) | busy (fast orange) | done (three green flashes) | green (steady)
// screen: off | loader ("PC Loader Connected") | on | any (the state doesn't matter yet: no screen line in the caption)
// knob: null | "cycle" (off, then on) | "on"
// note: a line for the caption in place of the LED's, while the LED says nothing
export function radioTop(radio, initial = {}) {
	const src = pictureSrc(radio);
	const pct = radio?.frame_geometry?.screen_pct;
	const line1 = h("span"), line2 = h("span", { class: "small" });
	// what the screen shows, laid over the picture's screen where the data says it is
	const screen = src && pct ? h("div", { class: "rt-screen", style: `left:${pct.left}%;top:${pct.top}%;width:${pct.width}%;height:${pct.height}%` }, line1, line2) : null;
	const pic = h("div", { class: "rt-pic", role: "img" }, src ? h("img", { src: ROOT + src, alt: "", class: "rt-img" }) : null, screen);
	const led = svg("circle", { cx: 12, cy: 12, r: 5, class: "rt-led" });
	const glow = svg("circle", { cx: 12, cy: 12, r: 11, class: "rt-glow" });
	const label = h("strong", {});
	const lamp = h("div", { class: "rt-lamp" }, svg("svg", { viewBox: "0 0 24 24", width: 24, height: 24, "aria-hidden": "true" }, glow, led), label);
	const caption = h("p", { class: "rt-caption" });
	const el = h("figure", { class: "radio-top" }, pic, lamp, caption);
	const set = ({ led: l = "off", screen: sc = "off", knob = null, note = null } = {}) => {
		el.dataset.led = l;
		el.dataset.screen = sc;
		el.dataset.knob = knob || "";
		label.textContent = l === "off" ? "" : t(`flash.c7.visual.tag_${l}`);
		line1.textContent = sc === "loader" ? "PC Loader" : sc === "on" ? "RadioFoundry" : "";
		line2.textContent = sc === "loader" ? "Connected" : "";
		const words = [knob === "cycle" ? t("flash.c7.visual.cycle") : knob === "on" ? t("flash.c7.visual.on") : null,
			note || t(`flash.c7.visual.led_${l}`), sc === "any" ? null : t(`flash.c7.visual.screen_${sc}`)].filter(Boolean);
		caption.replaceChildren(...words.map((w) => h("span", {}, w)));
		pic.setAttribute("aria-label", [radio?.name, ...words].filter(Boolean).join(" "));
		// restart the animation on every change, so "done" plays its three flashes from the start
		led.style.animation = "none";
		void led.getBBox?.();
		led.style.animation = "";
	};
	set(initial);
	return { el, set };
}
