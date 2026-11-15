// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Picking features "a la carte": a set of switches for one radio that respects the dependencies in
// config/features.mk (data/features.json "needs") and the radio's limits (BOARD_UNSUPPORTED). A selection that equals
// a prebuilt variant can be flashed right away; any other is a custom build, which the build API makes on request (or
// the user builds with make).

export class Selection {
	constructor(radio, features, variants) {
		this.radio = radio;
		this.features = features;
		this.byName = Object.fromEntries(features.map((f) => [f.name, f]));
		this.variants = variants;
		this.on = new Set();
		this.base = "full";
	}

	// A switch whose needs the radio can't meet isn't offered either (USB_SERIAL_REQUEST without USB_NETWORK): turned
	// on, enable() would skip the need and the build would stop at the features.mk check. The answer is fixed for the
	// radio, so it is kept (the build step asks for every feature on each draw). path: the needs being followed, so a
	// need cycle ends; a need already answered no stays no on another path
	supported(name, path = new Set()) {
		const memo = (this.supportMemo ??= new Map());
		if (memo.has(name)) return memo.get(name);
		if (path.has(name)) return true;
		path.add(name);
		const { all = [], any = [] } = this.byName[name]?.needs || {};
		const ok = this.allowed(name) && all.every((n) => this.supported(n, path))
			&& (!any.length || any.some((n) => this.supported(n, path)));
		path.delete(name);
		memo.set(name, ok);
		return ok;
	}

	// the radio itself can have it, whatever it needs (supported: and the radio can have what it needs too)
	allowed(name) {
		return !this.radio.unsupported.includes(name) && !this.notWritten(name) && name !== "AUDIO_PROBE";
	}

	// what keeps name off on this radio, followed down to the switches the radio itself can't have (a need that is
	// only blocked in turn says nothing to the user): [] when it can be on
	blockers(name, seen = new Set()) {
		if (!this.allowed(name)) return [name];
		if (seen.has(name)) return [];
		seen.add(name);
		const { all = [], any = [] } = this.byName[name]?.needs || {};
		const unmet = all.filter((n) => !this.supported(n));
		if (any.length && !any.some((n) => this.supported(n))) unmet.push(...any);
		return [...new Set(unmet.flatMap((n) => this.blockers(n, seen)))];
	}

	// the hardware can, but the radio's code isn't written yet (config.mk BOARD_NOT_WRITTEN): not offered either
	notWritten(name) {
		return (this.radio.not_written || []).includes(name);
	}

	required(name) {
		return this.radio.required.includes(name);
	}

	useVariant(id) {
		this.base = id;
		this.on = new Set(this.radio.config[id]);
	}

	// Turning a switch on turns on what it needs; off turns off what needs it. Returns the switches that changed.
	set(name, value) {
		const before = new Set(this.on);
		if (value) this.enable(name);
		else this.disable(name);
		return [...new Set([...before, ...this.on])].filter((n) => before.has(n) !== this.on.has(n));
	}

	enable(name) {
		if (this.on.has(name) || !this.supported(name)) return;
		this.on.add(name);
		const needs = this.byName[name]?.needs || { all: [], any: [] };
		for (const n of needs.all) this.enable(n);
		if (needs.any.length && !needs.any.some((n) => this.on.has(n))) {
			const pick = needs.any.find((n) => this.supported(n));
			if (pick) this.enable(pick);
		}
	}

	disable(name) {
		if (!this.on.has(name) || this.required(name)) return;
		this.on.delete(name);
		for (const f of this.features) {
			if (!this.on.has(f.name)) continue;
			const { all = [], any = [] } = f.needs || {};
			if (all.includes(name) || (any.length && !any.some((n) => this.on.has(n)))) this.disable(f.name);
		}
	}

	// The prebuilt variant this selection is, or null. skip(name): switches left out of the comparison
	matches(skip = () => false) {
		const on = [...this.on].filter((n) => !skip(n));
		for (const v of this.variants) {
			const cfg = this.radio.config[v.id].filter((n) => !skip(n));
			if (cfg.length === on.length && cfg.every((n) => this.on.has(n))) return v.id;
		}
		return null;
	}

	diff(variantId = this.base) {
		const cfg = new Set(this.radio.config[variantId]);
		return { added: [...this.on].filter((n) => !cfg.has(n)), removed: [...cfg].filter((n) => !this.on.has(n)) };
	}

	// For a custom selection: the make command that builds it
	// options: build options such as the MD-380's band ({BAND: "vhf"}), radios.json build_options
	makeCommand(options = {}) {
		const full = new Set(this.radio.config.full);
		const args = Object.entries(options).map(([k, v]) => `${k}=${v}`).concat(
			this.features.filter((f) => this.supported(f.name) && full.has(f.name) !== this.on.has(f.name))
				.map((f) => `CONFIG_${f.name}=${this.on.has(f.name) ? "y" : "n"}`));
		return `make BOARD=${this.radio.id}${args.length ? " " + args.join(" ") : ""}`;
	}
}
