// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CHIRP CSV in the Codeplug Editor: Open File takes a channel list CHIRP saved (CSV, or an image of a supported radio), replaces the codeplug's channels and
// zones with its FM channels, and Save offers the FM channels as a CHIRP CSV. Both list what it left out. One undoable change.

import { register } from "./extensions.js";
import { importChirp, exportChirp, ChirpError } from "../codeplug/chirp.js";
import { importChirpImg } from "../codeplug/chirp_img.js";
import { datedName } from "./files.js";

register({
	saveItems(api) {
		return [[api.t("editor.chirp.save"), () => save(api)]];
	},

	async openText(f, name, api) {
		if (f.kind !== "chirp" && f.kind !== "chirp-img") return false;
		open(api, name, f);
		return true;
	},
});

function save(api) {
	const { text, notes } = exportChirp(api.model);
	api.download(`${datedName(api.fileName)}-chirp.csv`, new TextEncoder().encode(text));
	if (notes.length) report(api, api.t("editor.chirp.saved_title"), api.t("editor.chirp.saved_notes"), notes);
}

function open(api, name, f) {
	const { t, h } = api;
	let result = null, error = null;
	const zone = name.replace(/\.[^.]*$/, "");
	api.change(t("editor.chirp.undo", { name }), () => {
		try {
			result = f.kind === "chirp-img" ? importChirpImg(api.model, f.bytes, zone) : importChirp(api.model, f.text, zone);
		} catch (e) {
			if (!(e instanceof ChirpError)) throw e;
			error = e;
			return false;
		}
	});
	if (error) {
		api.say(t(`editor.chirp.err.${error.code}`, { name, ...error.params }), "bad");
		// nothing was imported: say why, row by row kind
		if (error.params.notes?.length) report(api, t("editor.chirp.report_title", { name }), t("editor.chirp.empty_notes"), error.params.notes);
		return;
	}
	const { channels, zones } = result.counts;
	api.say(t("editor.chirp.imported", { name, channels, zones }), result.notes.length ? "warn" : "ok");
	if (!result.notes.length) return;
	report(api, t("editor.chirp.report_title", { name }), t("editor.chirp.import_notes"), result.notes, true);
}

function report(api, title, lead, notes, undo = false) {
	const { h, t } = api;
	const body = [h("p", {}, lead), h("ul", {}, ...notes.map((x) => h("li", {}, t(`editor.chirp.note.${x.code}`, { n: x.n }))))];
	if (undo) body.push(h("p", { class: "small muted" }, t("editor.chirp.undo_hint")));
	api.dialog(title, body, null, null, () => {}, { cancel: t("editor.close") });
}
