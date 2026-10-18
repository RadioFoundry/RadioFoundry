// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// qdmr YAML in the Codeplug Editor: Save offers the codeplug as a qdmr YAML file, and Open File takes one. An import
// replaces the channels, zones, contacts, lists and APRS configurations as one undoable change; what neither side can
// hold is listed afterwards.

import { register } from "./extensions.js";
import { importQdmr, exportQdmr, QdmrError } from "../codeplug/qdmr.js";

register({
	saveItems(api) {
		return [[api.t("editor.qdmr.save"), () => save(api)]];
	},

	async openText(f, name, api) {
		if (f.kind !== "yaml") return false;
		open(api, name, f.text);
		return true;
	},
});

function save(api) {
	const { text, notes } = exportQdmr(api.model);
	api.download(`${api.fileName || "codeplug"}.yaml`, new TextEncoder().encode(text));
	if (notes.length) report(api, api.t("editor.qdmr.saved_title"), api.t("editor.qdmr.saved_notes"), notes);
}

function open(api, name, text) {
	const { t } = api;
	let result = null, error = null;
	api.change(t("editor.qdmr.undo", { name }), () => {
		try {
			result = importQdmr(api.model, text);
		} catch (e) {
			if (!(e instanceof QdmrError)) throw e;
			error = e;
			return false;
		}
	});
	if (error) return api.say(t(`editor.qdmr.err.${error.code}`, { name, ...error.params }), "bad");
	const c = result.counts;
	api.say(t("editor.qdmr.imported", { name, channels: c.channels, zones: c.zones, contacts: c.contacts + c.dtmfContacts, lists: c.rxLists, aprs: c.aprs }),
		result.notes.length ? "warn" : "ok");
	if (result.notes.length) report(api, t("editor.qdmr.report_title", { name }), t("editor.qdmr.import_notes"), result.notes, true);
}

function report(api, title, lead, notes, undo = false) {
	const { h, t } = api;
	const body = [h("p", {}, lead), h("ul", {}, ...notes.map((x) => h("li", {}, t(`editor.qdmr.note.${x.code}`, { n: x.n }))))];
	if (undo) body.push(h("p", { class: "small muted" }, t("editor.qdmr.undo_hint")));
	api.dialog(title, body, null, null, () => {}, { cancel: t("editor.close") });
}
