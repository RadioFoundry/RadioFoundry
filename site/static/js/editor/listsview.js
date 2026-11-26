// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The Codeplug Editor's Contact Lists tab (docs/spec/contact-lists.md): the named groups a contact can be in, one row
// each with its name and how many contacts it holds, and the ID database as a fixed row after them. Which contacts are
// in a list is set on the Contacts tab; which lists name a system's calls, in the system's form.

import { LIST_MAX, LIST_NAME_MAX, LIST_IDDB, newContactList, removeLists, listNameProblem, listUsers } from "../codeplug/contactlists.js";

// What is wrong with a list name as typed, as text, or null when it can be stored
function nameError(name, t) {
	const bad = listNameProblem(name);
	return bad === null ? null : t(bad === "list_name_chars" ? "editor.problem.list_name_chars" : `editor.err.${bad}`, { max: LIST_NAME_MAX });
}

// An unused name for a new list: "List 3"
function newName(m, t) {
	const names = new Set((m.contactLists ?? []).map((l) => l.name));
	let i = (m.contactLists ?? []).length + 1;
	while (names.has(`${t("editor.lists.new_name")} ${i}`)) i++;
	return `${t("editor.lists.new_name")} ${i}`;
}

export function contactListsTab(api) {
	const { model: m, t, h, button } = api;
	const lists = [...(m.contactLists ?? [])].sort((a, b) => a.number - b.number);
	const full = lists.length >= LIST_MAX;
	// the systems that select a list (once each, though it may be both their primary and secondary), and its members
	const systemsOf = (list) => [...new Set(listUsers(m, list).map((u) => u.system.name))].join(", ");
	const members = new Map();
	for (const c of m.contacts) if (c.list != null) members.set(c.list, (members.get(c.list) ?? 0) + 1);

	const add = () => {
		let made = null;
		api.change(t("editor.undo.add"), () => {
			made = newContactList(m, newName(m, t));
			return made ? undefined : false;
		});
		// the tab was drawn again: the new list's name is where to type next
		if (made) document.getElementById(`list-name-${made.number}`)?.focus();
	};

	const rename = (l, input, err) => {
		const name = input.value.trim();
		const bad = nameError(name, t);
		err.textContent = bad ?? "";
		if (bad) return input.setAttribute("aria-invalid", "true");
		input.removeAttribute?.("aria-invalid");
		if (name !== l.name) api.change(t("editor.undo.edit"), () => (l.name = name));
	};

	const remove = (l) => {
		if (listUsers(m, l).length) {
			api.say(t("editor.lists.in_use", { name: l.name, systems: systemsOf(l) }), "warn");
			return;
		}
		api.confirmDialog(t("editor.lists.delete_confirm", { name: l.name, n: members.get(l.number) ?? 0 }), t("editor.delete"),
			() => api.change(t("editor.undo.delete"), () => removeLists(m, [l])));
	};

	const rows = lists.map((l) => {
		const id = `list-name-${l.number}`;
		const err = h("span", { id: `${id}-err`, class: "small ed-err", role: "alert" });
		const input = h("input", { type: "text", id, value: l.name, spellcheck: "false", "aria-label": t("editor.lists.name"), "aria-describedby": `${id}-err` });
		input.addEventListener("change", () => rename(l, input, err));
		return h("tr", {},
			h("td", {}, input, err),
			h("td", {}, String(members.get(l.number) ?? 0)),
			h("td", {}, systemsOf(l)),
			h("td", {}, h("div", { class: "ed-actions" }, button(t("editor.delete"), () => remove(l), { quiet: true, danger: true }))));
	});
	// the ID database: not a list of the codeplug's contacts, and not editable here
	rows.push(h("tr", { class: "muted" },
		h("th", { scope: "row" }, t("editor.lists.iddb")),
		h("td", {}, "–"),
		h("td", {}, systemsOf({ number: LIST_IDDB })),
		h("td", {}, t("editor.lists.iddb_note"))));

	const head = [t("editor.col.name"), t("editor.col.contacts"), t("editor.lists.used_by"), ""];
	return h("div", {},
		h("p", { class: "small muted" }, t("editor.lists.intro")),
		h("div", { class: "ed-bar" }, h("div", { class: "ed-actions" }, button(t("editor.add"), add, { disabled: full }))),
		full ? h("p", { class: "small muted" }, t("editor.lists.full", { max: LIST_MAX })) : null,
		!lists.length ? h("p", { class: "grid-empty muted" }, t("editor.empty.contactLists")) : null,
		h("div", { class: "table-wrap" }, h("table", { class: "data" },
			h("thead", {}, h("tr", {}, ...head.map((x) => h("th", {}, x)))), h("tbody", {}, ...rows))));
}
