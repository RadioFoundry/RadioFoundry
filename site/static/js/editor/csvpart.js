// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// CSV in the Codeplug Editor: Save offers the open table as a CSV file and every table as a .zip of CSV files; Open
// File takes either back. An import is one undoable change, and its problems are listed by row and column.

import { register } from "./extensions.js";
import { exportTable, exportAll, importTable, importAll, TABLES, CsvError } from "../codeplug/csv.js";
import { zip } from "../flasher/zip.js";

const TABLE_TABS = Object.keys(TABLES);

register({
	saveItems(api) {
		const { t } = api;
		const out = [];
		if (TABLE_TABS.includes(api.tab)) {
			out.push([t("editor.csv.save_table", { table: t(`editor.tab.${api.tab}`) }), () => saveTable(api, api.tab)]);
		}
		out.push([t("editor.csv.save_all"), () => saveAll(api)]);
		return out;
	},

	async openText(f, name, api) {
		if (f.kind === "csv") {
			run(api, name, (m) => [{ name, ...importTable(m, f.text) }]);
			return true;
		}
		if (f.kind === "csv-set") {
			run(api, name, (m) => {
				const { results, skipped } = importAll(m, f.files);
				if (!results.length) throw new CsvError("not_a_table");
				return [...results, ...skipped.map((n) => ({ name: n, skipped: true }))];
			});
			return true;
		}
		return false;
	},
});

const base = (api) => api.fileName || "codeplug";

function saveTable(api, table) {
	// a byte order mark, so spreadsheet programs read the names as UTF-8
	api.download(`${base(api)}-${TABLES[table].file}`, new TextEncoder().encode("﻿" + exportTable(api.model, table)));
}

async function saveAll(api) {
	api.download(`${base(api)}-csv.zip`, await zip(exportAll(api.model)));
}

function run(api, name, fn) {
	const { t, h } = api;
	let results = null, error = null;
	api.change(t("editor.csv.undo", { name }), () => {
		try {
			results = fn(api.model);
		} catch (e) {
			if (!(e instanceof CsvError)) throw e;
			error = e;
			return false;
		}
	});
	if (error) return api.say(t(`editor.csv.err.${error.code}`, { name }), "bad");
	const done = results.filter((r) => !r.skipped);
	const lines = done.map((r) => t("editor.csv.result", {
		table: t(`editor.tab.${r.table}`), added: r.added, updated: r.updated, removed: r.removed,
	}));
	const problems = done.flatMap((r) => r.problems.map((p) => ({ ...p, file: r.name })));
	const unknown = [...new Set(done.flatMap((r) => r.unknown))];
	const skipped = results.filter((r) => r.skipped).map((r) => r.name);
	api.say(t("editor.csv.imported", { name, summary: lines.join(" ") }) + (problems.length ? " " + t("editor.csv.with_problems", { n: problems.length }) : ""),
		problems.length ? "warn" : "ok");
	if (!problems.length && !unknown.length && !skipped.length) return;
	const multi = done.length > 1;
	const body = [];
	if (problems.length) {
		body.push(h("div", { class: "table-wrap ed-csv-problems" }, h("table", {},
			h("thead", {}, h("tr", {}, multi ? h("th", {}, t("editor.csv.file")) : null, h("th", {}, t("editor.csv.row")),
				h("th", {}, t("editor.csv.column")), h("th", {}, t("editor.csv.problem")))),
			h("tbody", {}, ...problems.slice(0, 200).map((p) => h("tr", {},
				multi ? h("td", {}, p.file) : null, h("td", {}, p.row ? String(p.row) : ""), h("td", {}, p.column),
				h("td", {}, t(`editor.csv.err.${p.code}`, p.params) + (p.skipped ? " " + t("editor.csv.row_skipped") : ""))))))));
		if (problems.length > 200) body.push(h("p", { class: "small muted" }, t("editor.csv.more", { n: problems.length - 200 })));
	}
	if (unknown.length) body.push(h("p", { class: "small" }, t("editor.csv.unknown", { columns: unknown.join(", ") })));
	if (skipped.length) body.push(h("p", { class: "small" }, t("editor.csv.skipped", { files: skipped.join(", ") })));
	body.push(h("p", { class: "small muted" }, t("editor.csv.undo_hint")));
	api.dialog(t("editor.csv.report_title", { name }), body, null, null, () => {}, { cancel: t("editor.close") });
}
