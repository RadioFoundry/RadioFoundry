// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Where the editor's optional parts plug in (CSV, qdmr YAML, contact sources): each registers what it adds to the
// Save menu, to a table's actions, to opening a text file, and to the DMR ID Database tab. The page asks this module
// and imports only parts.js, the list of parts, so a part can be added without touching the page.

const parts = [];

export function register(part) {
	parts.push(part);
}

export const extensions = {
	saveItems: (api) => parts.flatMap((p) => p.saveItems?.(api) || []),
	tableActions: (tab, api) => parts.flatMap((p) => p.tableActions?.(tab, api) || []),
	async openText(file, name, api) {
		for (const p of parts) if (await p.openText?.(file, name, api)) return true;
		return false;
	},
	iddbTab(api) {
		const p = parts.find((x) => x.iddbTab);
		if (p) return p.iddbTab(api);
		const { h, t, model } = api;
		const db = model.iddb;
		return h("p", {}, db ? t("editor.iddb.present", { n: db.count.toLocaleString(), len: db.textLen }) : t("editor.iddb.none"));
	},
};
