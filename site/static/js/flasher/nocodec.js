// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
import { h, t, button } from "./dom.js";
import * as diag from "../diag.js";

// Asked when Install is pressed on an install that leaves the radio without the DMR voice codec (no codec to write,
// or a kept codec known to be missing): the step's notice says it once, this says it again where the click is.
// Resolves true to write, false to stay on the step. back: goes to the step where a codec can be added (and the dialog
// offers it), or null where the route has none
export function confirmNoCodec(back = null) {
	return new Promise((done) => {
		const close = (answer) => {
			dlg.close();
			dlg.remove();
			diag.note("no codec confirm", { answer });
			done(answer === "go");
			if (answer === "back") back();
		};
		const dlg = h("dialog", { class: "confirm", "aria-labelledby": "nocodec-confirm-title", oncancel: (e) => { e.preventDefault(); close("stay"); } },
			h("h2", { id: "nocodec-confirm-title" }, t("flash.nocodec.title")),
			h("p", {}, t("flash.nocodec.text")),
			h("p", {}, t(back ? "flash.nocodec.fix_back" : "flash.nocodec.fix_later")),
			h("div", { class: "confirm-actions" },
				back ? button(t("flash.nocodec.back"), () => close("back"), { primary: true }) : null,
				button(t("flash.nocodec.go"), () => close("go"), { danger: true })));
		document.body.append(dlg);
		dlg.showModal();
	});
}
