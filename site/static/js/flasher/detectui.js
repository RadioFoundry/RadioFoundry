// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The Auto-detect button beside a variant chooser (detect.js): the user connects the radio in whatever state it is in,
// the page runs the backend's variant probes and says what it found. Any backend with [hardware] variants can use it.

import { h, t, notice, button } from "./dom.js";
import { detectVariant } from "./detect.js";
import * as diag from "../diag.js";

const key = (id) => id.replace(/-/g, "_");	// a probe id or a via word as an i18n key part

// opts: {radio, board, fb, ids (probe ids, in order), open(baud) (a stream on the radio's port; the first call asks
// for the port), onFound(found) (a probe named the variant: found as detectVariant returns it), nextHint(found) (an
// i18n key for what the user does next, or null), log(line), showBlocked (say here that a found variant has no image;
// false when the caller says so itself), showAllPorts() (optional: the next open lists every port; adds the link for a
// radio the filtered chooser doesn't list)}.
// Returns the parts for the caller to lay out: {go (the button), hint (what to do before pressing it), say (the outcome)}
export function detectControl({ radio, board, fb, ids, open, onFound, nextHint = () => null, log = () => {}, showBlocked = true, showAllPorts = null }) {
	const say = h("div");
	const fill = (...parts) => say.replaceChildren(...parts.filter(Boolean));
	let busy = false;
	const run = async () => {
		if (busy) return;
		busy = true;
		go.disabled = true;
		fill(notice(t("flash.detect.running"), "info"));
		try {
			const { found, tried } = await detectVariant({ ids, radio, board, fb, open, log });
			diag.note("variant detect", { found: found?.code || null, via: found?.via || null,
				tried: tried.map((x) => `${x.id}:${x.outcome}`).join(" ") });
			if (found?.code) {
				const v = (radio.variants || []).find((x) => x.code === found.code);
				fill(notice(t("flash.detect.found", { variant: v ? `${v.code} (${v.name})` : found.code, via: t(`flash.detect.via.${key(found.via)}`),
					version: found.version || "" }), "ok"),
					v?.blocked ? (showBlocked ? notice(t("flash.detect.blocked", { why: v.blocked }), "bad") : null)
						: (found.note || nextHint(found)) ? h("p", {}, t(found.note || nextHint(found))) : null);
				await onFound(found);
			} else if (found) {
				fill(notice(t("flash.detect.no_variant", { version: found.version || "", via: t(`flash.detect.via.${key(found.via)}`) }), "info"),
					h("p", {}, radio.variant_check || ""));
			} else {
				fill(notice(t("flash.detect.none"), "bad"), h("p", {}, radio.variant_check || ""),
					h("details", { class: "small" }, h("summary", {}, t("flash.problem.details")),
						h("ul", {}, tried.map((x) => h("li", {}, `${t(`flash.detect.probe.${key(x.id)}`)}: ${t(`flash.detect.outcome.${x.outcome}`)}${x.error ? ` (${x.error})` : ""}`)))));
			}
		} catch (e) {
			// the user closed the port chooser (the browser's NotFoundError), or the port couldn't be opened
			fill(notice(e?.name === "NotFoundError" ? t("flash.err.no_device") : t("flash.detect.failed", { why: String(e?.message || e) }), "bad"));
		} finally {
			busy = false;
			go.disabled = false;
		}
	};
	const go = button(t("flash.detect.go"), run);
	const all = showAllPorts && h("a", { href: "#", onclick: (e) => { e.preventDefault(); showAllPorts(); run(); } }, t("flash.detect.all_ports"));
	return { go, hint: h("p", { class: "small muted" }, t("flash.detect.hint"), all && " ", all && t("flash.c7.connect.no_cable"), all && " ", all), say };
}
