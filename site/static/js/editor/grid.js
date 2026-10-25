// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The codeplug editor's table: one row per record, cells shown as text and edited in place (double-click, Enter or
// typing on a focused cell), sorting by a column, a filter, row selection for bulk edits, and keyboard movement
// between cells. Only the cell being edited holds an input, so a table of 1024 channels stays light.

import { h } from "../flasher/dom.js";

// The row's edit button icon: a pencil drawn in the text color, so it follows the theme.
const SVG = "http://www.w3.org/2000/svg";
function pencil() {
	const svg = document.createElementNS(SVG, "svg");
	svg.setAttribute("viewBox", "0 0 16 16");
	svg.setAttribute("aria-hidden", "true");
	svg.setAttribute("class", "icon");
	const path = document.createElementNS(SVG, "path");
	path.setAttribute("d", "M11.5 1.5l3 3L5 14H2v-3l9.5-9.5zM10 3l3 3");
	path.setAttribute("fill", "none");
	path.setAttribute("stroke", "currentColor");
	path.setAttribute("stroke-width", "1.4");
	path.setAttribute("stroke-linejoin", "round");
	svg.append(path);
	return svg;
}

// A column: {key, label, type: "text" | "number" | "select" | "bool", get(row), set(row, value) -> error string or
// undefined, format(value), options(row) -> [[value, label]], width, readOnly}
export class Grid {
	constructor({ columns, rows, onChange, onSelect, onOpen, t, empty = "" }) {
		Object.assign(this, { columns, rows, onChange, onSelect, onOpen, t, empty });
		this.filter = "";
		this.sort = null;	// {key, dir}
		this.selected = new Set();
		this.focus = { row: 0, col: 0 };
		this.el = h("div", { class: "grid-wrap" });
		this.table = h("table", { class: "grid", role: "grid" });
		this.el.append(this.table);
		this.table.addEventListener("keydown", (e) => this.key(e));
		this.table.addEventListener("dblclick", (e) => {
			const td = e.target.closest("td[data-col]");
			if (td) this.edit(td);
		});
		this.table.addEventListener("click", (e) => {
			const td = e.target.closest("td[data-col]");
			if (td && !td.querySelector("input, select")) this.moveTo(Number(td.parentNode.dataset.i), Number(td.dataset.col));
		});
		this.render();
	}

	setRows(rows) {
		this.rows = rows;
		for (const r of [...this.selected]) if (!rows.includes(r)) this.selected.delete(r);
		this.render();
	}

	shown() {
		let list = this.rows;
		if (this.filter) {
			const f = this.filter.toLowerCase();
			list = list.filter((r) => this.columns.some((c) => this.text(c, r).toLowerCase().includes(f)));
		}
		if (this.sort) {
			const c = this.columns.find((x) => x.key === this.sort.key);
			const val = (r) => {
				const v = c.sortValue ? c.sortValue(r) : c.get(r);
				return typeof v === "number" ? v : String(this.text(c, r)).toLowerCase();
			};
			list = [...list].sort((a, b) => {
				const x = val(a), y = val(b);
				return (x < y ? -1 : x > y ? 1 : 0) * this.sort.dir;
			});
		}
		return list;
	}

	text(c, r) {
		const v = c.get(r);
		if (c.format) return c.format(v, r);
		if (c.type === "bool") return v ? "✓" : "";
		if (c.type === "select") {
			const o = c.options(r).find(([k]) => k === v);
			return o ? o[1] : String(v ?? "");
		}
		return v == null ? "" : String(v);
	}

	render() {
		this.view = this.shown();
		const all = h("input", {
			type: "checkbox", "aria-label": this.t("editor.grid.select_all"),
			onchange: (e) => {
				for (const r of this.view) e.target.checked ? this.selected.add(r) : this.selected.delete(r);
				this.render();
				this.onSelect?.(this.selection());
			},
		});
		all.checked = this.view.length > 0 && this.view.every((r) => this.selected.has(r));
		const head = h("tr", {}, h("th", { class: "grid-check" }, all), this.onOpen ? h("th", { class: "grid-open" }) : null, ...this.columns.map((c) => {
			const dir = this.sort?.key === c.key ? (this.sort.dir > 0 ? "ascending" : "descending") : "none";
			return h("th", { scope: "col", "aria-sort": dir, style: c.width ? `width:${c.width}` : null },
				h("button", { type: "button", class: "grid-sort", onclick: () => this.sortBy(c.key) }, c.label));
		}));
		const body = h("tbody");
		this.view.forEach((r, i) => {
			const check = h("input", {
				type: "checkbox", "aria-label": this.t("editor.grid.select_row"),
				onchange: (e) => {
					e.target.checked ? this.selected.add(r) : this.selected.delete(r);
					tr.classList.toggle("selected", e.target.checked);
					this.onSelect?.(this.selection());
				},
			});
			check.checked = this.selected.has(r);
			const tr = h("tr", { "data-i": i, class: this.selected.has(r) ? "selected" : "" }, h("td", { class: "grid-check" }, check));
			if (this.onOpen) tr.append(h("td", { class: "grid-open" }, h("button", { type: "button", class: "btn-quiet", "aria-label": this.t("editor.grid.open"), title: this.t("editor.grid.open"), onclick: () => this.onOpen(r) }, pencil())));
			this.columns.forEach((c, j) => {
				const cls = [c.type === "bool" ? "c-bool" : "", c.type === "number" || c.numeric ? "c-num" : "", this.locked(c, r) ? "c-ro" : ""].join(" ");
				tr.append(h("td", { "data-col": j, tabindex: -1, class: cls.trim(), title: c.title ? c.title(r) : null }, this.text(c, r)));
			});
			body.append(tr);
		});
		const cols = this.columns.length + 1 + (this.onOpen ? 1 : 0);
		if (!this.view.length) body.append(h("tr", {}, h("td", { colspan: cols, class: "muted grid-empty" }, this.rows.length ? this.t("editor.grid.no_match") : this.empty)));
		this.table.replaceChildren(h("thead", {}, head), body);
		const cell = this.cell(this.focus.row, this.focus.col);
		if (cell) cell.tabIndex = 0;
		else this.cell(0, 0)?.setAttribute("tabindex", "0");
	}

	sortBy(key) {
		this.sort = this.sort?.key === key ? (this.sort.dir > 0 ? { key, dir: -1 } : null) : { key, dir: 1 };
		this.render();
	}

	setFilter(text) {
		this.filter = text;
		this.render();
	}

	selection() {
		return this.rows.filter((r) => this.selected.has(r));
	}

	// The rows an action applies to: the selection, or the focused row
	targets() {
		const s = this.selection();
		if (s.length) return s;
		const r = this.view[this.focus.row];
		return r ? [r] : [];
	}

	// readOnly: a column's, or per row (a function)
	locked(c, r) {
		return typeof c.readOnly === "function" ? c.readOnly(r) : !!c.readOnly;
	}

	cell(i, j) {
		return this.table.querySelector(`tr[data-i="${i}"] td[data-col="${j}"]`);
	}

	moveTo(i, j) {
		i = Math.max(0, Math.min(this.view.length - 1, i));
		j = Math.max(0, Math.min(this.columns.length - 1, j));
		this.cell(this.focus.row, this.focus.col)?.setAttribute("tabindex", "-1");
		this.focus = { row: i, col: j };
		const td = this.cell(i, j);
		if (td) {
			td.tabIndex = 0;
			td.focus();
		}
	}

	focusRow(row) {
		const i = this.view.indexOf(row);
		if (i >= 0) this.moveTo(i, 0);
	}

	key(e) {
		if (e.target.closest("input, select")) return;
		const td = e.target.closest("td[data-col]");
		if (!td) return;
		const i = Number(td.parentNode.dataset.i), j = Number(td.dataset.col);
		const moves = { ArrowUp: [-1, 0], ArrowDown: [1, 0], ArrowLeft: [0, -1], ArrowRight: [0, 1], PageUp: [-15, 0], PageDown: [15, 0] };
		if (moves[e.key]) {
			e.preventDefault();
			this.moveTo(i + moves[e.key][0], j + moves[e.key][1]);
		} else if (e.key === "Enter" || e.key === "F2") {
			e.preventDefault();
			this.edit(td);
		} else if (e.key === " " && this.columns[j].type === "bool" && !this.locked(this.columns[j], this.view[i])) {
			e.preventDefault();
			this.commit(this.view[i], this.columns[j], !this.columns[j].get(this.view[i]));
			this.moveTo(i, j);
		} else if (e.key.length === 1 && !e.ctrlKey && !e.metaKey && !e.altKey && this.columns[j].type !== "bool") {
			this.edit(td, e.key);
			e.preventDefault();
		}
	}

	edit(td, typed = null) {
		const j = Number(td.dataset.col), i = Number(td.parentNode.dataset.i);
		const c = this.columns[j], r = this.view[i];
		if (!c || !r || this.locked(c, r)) return;
		if (c.type === "bool") {
			this.commit(r, c, !c.get(r));
			this.moveTo(i, j);
			return;
		}
		let input;
		if (c.type === "select") {
			// options by position: a value can be a record, which has no text form
			const opts = c.options(r);
			input = h("select", {}, ...opts.map(([, label], k) => h("option", { value: String(k) }, label)));
			input.selectedIndex = Math.max(0, opts.findIndex(([v]) => v === c.get(r)));
			input._opts = opts;
		} else {
			input = h("input", { type: "text", value: typed ?? (c.editValue ? c.editValue(c.get(r), r) : this.text(c, r)), spellcheck: "false" });
		}
		td.replaceChildren(input);
		td.classList.add("editing");
		input.focus();
		if (input.select && typed === null) input.select();
		let done = false;
		const finish = (save, move = null) => {
			if (done) return;
			done = true;
			if (save) {
				let v = input.value;
				if (c.type === "select") v = input._opts[input.selectedIndex]?.[0];
				const err = this.commit(r, c, v);
				if (err) {
					done = false;
					input.setCustomValidity?.(err);
					input.reportValidity?.();
					input.title = err;
					return;
				}
			}
			this.render();
			if (move) this.moveTo(i + move[0], j + move[1]);
			else this.moveTo(i, j);
		};
		input.addEventListener("keydown", (e) => {
			if (e.key === "Enter") {
				e.preventDefault();
				finish(true, [e.shiftKey ? -1 : 1, 0]);
			} else if (e.key === "Escape") {
				e.preventDefault();
				finish(false);
			} else if (e.key === "Tab") {
				e.preventDefault();
				finish(true, [0, e.shiftKey ? -1 : 1]);
			}
		});
		input.addEventListener("input", () => input.setCustomValidity?.(""));
		input.addEventListener("blur", () => finish(true));
		if (c.type === "select") input.addEventListener("change", () => finish(true));
	}

	commit(row, c, value) {
		const err = this.onChange(row, c, value);
		return err || null;
	}
}
