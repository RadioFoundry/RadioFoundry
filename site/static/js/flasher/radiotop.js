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

// The keys held to enter update mode, by key name (board.toml [flash.bootloader] hold): [] when the data has none
export const holdKeys = (radio) => radio?.flash?.bootloader_hold || [];

// A key as the radio's label names it (flash.key.<name>), else its name in capitals
export function keyLabel(name, tr = t) {
	const k = `flash.key.${name}`, s = tr(k);
	return s && s !== k ? s : name.toUpperCase();
}

// "Hold PTT and SK1": the keys in words, in the page's language, or "" for none. The steps stay the instructions;
// this names what the marks on the picture show, and the keys the picture doesn't (a front view without side keys)
export function holdCaption(names, tr = t, lang = globalThis.document?.documentElement.lang || "en") {
	if (!names.length) return "";
	const labels = names.map((n) => keyLabel(n, tr));
	return tr("flash.keys.hold", { keys: new Intl.ListFormat(lang, { type: "conjunction" }).format(labels) });
}

// Where each of those keys is on the picture, as percentages of it (the site build's frame_geometry.keys from the
// drawing's key-<name> shapes): only the keys the picture shows
export function keyMarks(radio, names) {
	const boxes = radio?.frame_geometry?.keys || {};
	return names.filter((n) => boxes[n]).map((n) => ({ name: n, ...boxes[n] }));
}

// The radio's picture alone, for steps that say nothing about the LED or the screen (the bootloader entry: what a
// radio shows there is in its steps' words, not in the data), or null without one. keys: names of keys to mark on it
// (holdKeys), each ringed where the picture shows it, and named in a caption. The marks are decoration: the caption
// and the steps say the same in words.
export function radioPicture(radio, { keys = [] } = {}) {
	const src = pictureSrc(radio);
	if (!src) return null;
	const marks = keyMarks(radio, keys).map((m) => h("span", { class: "rt-key", "aria-hidden": "true",
		"data-key": m.name, style: `left:${m.left}%;top:${m.top}%;width:${m.width}%;height:${m.height}%` }));
	const caption = keys.length ? h("figcaption", { class: "rt-caption" }, h("span", {}, holdCaption(keys))) : null;
	return h("figure", { class: "radio-top radio-pic" },
		h("div", { class: "rt-pic" }, h("img", { src: ROOT + src, alt: radio.name || "", class: "rt-img" }), ...marks), caption);
}

// What RadioFoundry's own screens look like on this radio: the UI gallery's picture of a screen for the radio's display
// class (radios.json gallery; docs/ux/screens/<gallery>/<name>.png, made by the host tests' ui-gallery), so the
// picture shows the radio's real boot screen and the stub entry's splash, at any display size
const SCREEN_SHOTS = { rfoundry: "boot", loader: "stub_entry" };
const screenShot = (radio, sc) => (radio?.gallery && SCREEN_SHOTS[sc] ? `${ROOT}docs/ux/screens/${radio.gallery}/${SCREEN_SHOTS[sc]}.png` : null);

// led: off | wait (slow orange) | busy (fast orange) | done (three green flashes) | green (steady)
// screen: off (dark: a power-on stub load or the factory loader) | loader (RadioFoundry handed over to the stub: its
// "PC Loader Connected" splash) | rfoundry (RadioFoundry running: its boot screen) | on (another firmware running: lit,
// no picture of its screens) | any (the state doesn't matter yet: no screen line in the caption)
// knob: null | "cycle" (off, then on) | "on"
// note: a line for the caption in place of the LED's, while the LED says nothing
export function radioTop(radio, initial = {}) {
	const src = pictureSrc(radio);
	const pct = radio?.frame_geometry?.screen_pct;
	const shot = h("img", { alt: "", class: "rt-shot", hidden: true });
	// what the screen shows, laid over the picture's screen where the data says it is
	const screen = src && pct ? h("div", { class: "rt-screen", style: `left:${pct.left}%;top:${pct.top}%;width:${pct.width}%;height:${pct.height}%` }, shot) : null;
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
		const url = screenShot(radio, sc);
		shot.hidden = !url;
		if (url && shot.getAttribute("src") !== url) shot.setAttribute("src", url);
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
