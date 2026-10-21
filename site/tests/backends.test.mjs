// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The radio access backend registry (backends.js): what a backend must declare, the fallback for a radio without
// one, and the HR_C7000 backend's steps. Run with site/tests/run.sh, or: node --test site/tests/

import { test } from "node:test";
import assert from "node:assert/strict";

import { register, backendOf, bundleBackend, allBackends } from "../static/js/flasher/backends.js";
import "../static/js/flasher/c7000ui.js";
import "../static/js/flasher/gd77ui.js";

const radio = (backend, board = "x") => ({ flash: { backend, board } });

test("a backend that lacks part of the contract is refused", () => {
	assert.throws(() => register({ id: "half", available: () => true }), /half lacks steps, views, recovery/);
	assert.ok(!allBackends().some((b) => b.id === "half"));
});

test("a radio without a backend the page knows offers nothing and keeps the shared steps", () => {
	for (const r of [radio("none"), radio("some-future-protocol"), radio(undefined), null]) {
		const b = backendOf(r);
		assert.equal(b.id, "none");
		assert.equal(b.available(r, {}, "install"), false);
		assert.equal(b.steps("install", { recovering: false }), null);
	}
});

test("a new mechanism is one register() call: its steps, views and recovery take over for its radios", () => {
	const view = () => "drawn";
	register({ id: "test-proto", available: (r, fb, task) => task === "backup", steps: (task) => (task === "backup" ? ["radio", "read"] : null),
		views: { read: view }, recovery: { first: "fix", stock: "restore", reach: null, stockText: "k" }, ownsBundle: (b) => b.meta?.tag === "test" });
	const r = radio("test-proto");
	assert.equal(backendOf(r).available(r, {}, "backup"), true);
	assert.equal(backendOf(r).available(r, {}, "install"), false);
	assert.deepEqual(backendOf(r).steps("backup", {}), ["radio", "read"]);
	assert.equal(backendOf(r).views.read(), "drawn");
	assert.equal(bundleBackend({ meta: { tag: "test" }, parts: new Map() }).id, "test-proto");
	assert.equal(bundleBackend(null), null);
});

test("HR_C7000 backend: connect and check replace now and codec; its bundles are its own", () => {
	const fb = { c7000: { boards: { dm32: {} } } };
	const b = backendOf(radio("c7000-stub", "dm32"));
	assert.equal(b.id, "c7000-stub");
	assert.ok(b.available(radio("c7000-stub", "dm32"), fb, "install"));
	assert.ok(!b.available(radio("c7000-stub", "dm99"), fb, "install"));
	assert.deepEqual(b.steps("install", { recovering: false }), ["radio", "build", "connect", "check", "write", "done"]);
	assert.deepEqual(b.steps("install", { recovering: true }), ["build", "connect", "check", "write", "done"]);
	assert.equal(b.steps("backup", { recovering: false }), null);
	assert.equal(b.stepLabel("done", "install"), "c7_done");
	assert.equal(bundleBackend({ parts: new Map([["c7000-progmem", {}]]) }).id, "c7000-stub");
	for (const s of ["connect", "check", "write", "done", "backup", "restore"]) assert.equal(typeof b.views[s], "function", s);
});

test("GD-77 backend: install only, over the cable or through its update file and update mode; the help task's own way back", () => {
	const fb = { gd77: { boards: ["gd77"] } };
	const r = radio("gd77-hid", "gd77");
	const b = backendOf(r);
	assert.equal(b.id, "gd77-hid");
	assert.ok(b.available(r, fb, "install"));
	assert.ok(!b.available(r, fb, "backup"));
	assert.ok(!b.available(radio("gd77-hid", "gd99"), fb, "install"));
	assert.ok(!b.available(r, {}, "install"));
	assert.deepEqual(b.steps("install", { recovering: false }), ["radio", "build", "connect", "update_file", "write", "done"]);
	assert.deepEqual(b.steps("install", { recovering: true }), ["build", "update_file", "write", "done"]);
	assert.equal(b.steps("backup", { recovering: false }), null);
	assert.equal(b.stepLabel("done", "install"), "gd77_done");
	assert.deepEqual(b.recovery.choices, ["again"]);
	assert.deepEqual(b.recovery.extra().map((x) => x.id), ["original"]);
	for (const s of ["connect", "update_file", "write", "done"]) assert.equal(typeof b.views[s], "function", s);
	assert.equal(bundleBackend({ parts: new Map([["internal-flash", {}]]) }), null);
});
