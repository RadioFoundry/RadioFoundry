// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The browser's Back button and the flasher's steps. Run with site/tests/run.sh.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { stepHistory } from "../static/js/flasher/stephistory.js";

// A browser's joint history; go() lands at once, as a popstate
function fakeWindow() {
	const entries = [{ state: null }];
	let at = 0, pop = null;
	const history = {
		replaceState(s) { entries[at] = { state: s }; },
		pushState(s) { entries.length = at + 1; entries.push({ state: s }); at++; },
		go(d) { at = Math.max(0, Math.min(entries.length - 1, at + d)); pop({ state: entries[at].state }); },
	};
	return { history, addEventListener: (ev, f) => { pop = f; }, at: () => at, count: () => entries.length };
}

function setup(busy = () => false) {
	const win = fakeWindow(), shown = [];
	const h = stepHistory(win, { busy, show: (p) => { shown.push(p); h.sync(p); } });
	return { win, shown, h };
}

test("Back goes one step at a time, then to the task list", () => {
	const { win, shown, h } = setup();
	for (const p of ["", "install/0", "install/1", "install/2"]) h.sync(p);
	win.history.go(-1);
	assert.deepEqual(shown, ["install/1"]);
	win.history.go(-1);
	win.history.go(-1);
	assert.deepEqual(shown, ["install/1", "install/0", ""]);
});

test("the page's own Back rewinds, so the browser's Back doesn't return to the step just left", () => {
	const { win, shown, h } = setup();
	for (const p of ["", "install/0", "install/1", "install/2", "install/1"]) h.sync(p);
	assert.equal(win.at(), 2);
	win.history.go(-1);
	assert.deepEqual(shown, ["install/0"]);
});

test("Start over rewinds to the task list", () => {
	const { win, shown, h } = setup();
	for (const p of ["", "backup/0", "backup/1", ""]) h.sync(p);
	assert.equal(win.at(), 0);
	assert.deepEqual(shown, []);
});

test("a new step after going back replaces what followed", () => {
	const { win, h } = setup();
	for (const p of ["", "install/0", "install/1"]) h.sync(p);
	win.history.go(-1);
	h.sync("install/1");
	assert.equal(win.at(), 2);
	assert.equal(win.count(), 3);
});

test("Forward is refused: it stays on the step", () => {
	const { win, shown, h } = setup();
	for (const p of ["", "install/0", "install/1"]) h.sync(p);
	win.history.go(-1);
	win.history.go(1);
	assert.deepEqual(shown, ["install/0"]);
	assert.equal(win.at(), 1);
});

test("Back is refused while a transfer runs", () => {
	let running = true;
	const { win, shown, h } = setup(() => running);
	for (const p of ["", "install/0", "install/1"]) h.sync(p);
	win.history.go(-1);
	assert.deepEqual(shown, []);
	assert.equal(win.at(), 2);
	running = false;
	win.history.go(-1);
	assert.deepEqual(shown, ["install/0"]);
});

test("an entry from an earlier load of the page is left alone", () => {
	const { win, shown, h } = setup();
	h.sync("");
	win.history.pushState({ rfStepTrail: "old", n: 5 }, "");
	win.history.go(0);
	assert.deepEqual(shown, []);
	assert.equal(win.count(), 2);
});

// The page's guard: a write counts as a transfer even with no bar moving (an update mode erase reports nothing for
// half a minute). A Back then once left the write running behind a fresh step, whose Install started a second one
test("the page holds Back during a write with no moving bar", () => {
	const app = readFileSync(new URL("../static/js/flasher/app.js", import.meta.url), "utf8");
	assert.match(app, /busy: \(\) => \(barsRunning\(\) \|\| !!S\.writing\) && transferStep\(\)/);
	const c7 = readFileSync(new URL("../static/js/flasher/c7000ui.js", import.meta.url), "utf8");
	assert.match(c7, /async function busy\(on\) \{[\s\S]{0,400}S\(\)\.writing = !!on;/);
});
