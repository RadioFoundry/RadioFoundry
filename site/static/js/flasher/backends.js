// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Radio access backends: how the page reaches one family of radios. radios.json flash.backend names the backend; app.js
// runs the tasks (install, backup, restore, help) and asks the radio's backend which steps each task takes and which
// views draw the steps it owns. The steps every radio shares (radio, build, bundle, symptom, recover) stay in app.js.
// A new way to install or back up a radio (another bootloader protocol, a stock read-back protocol) is a new module
// that calls register(); the task flow doesn't change. docs/dev/web-flasher.md "Radio access backends" has the contract.
//
//   id                      radios.json flash.backend
//   available(radio, fb, task)  the page can run task ("install" | "backup") on this radio
//   steps(task, ctx)        the step ids of a task, or null for the shared ones (STEPS in app.js). ctx: {recovering}
//   views                   {step id: () => element} for the steps the backend owns
//   stepLabel(step, task)   optional: the i18n key suffix of a step's name (flash.step.<suffix>)
//   recovery                the help task: {first: "updatemode" | "fix", stock: "file" | "restore", reach: i18n key
//                           or null, stockText: i18n key}. "updatemode": the page first checks the radio reaches its
//                           bootloader; stock "file": stock firmware comes back from the user's stock file, "restore":
//                           from a backup through the restore task. Optional extra(radio, fb): more reinstall choices,
//                           [{id, title, text, prepare()}] (string keys; prepare() runs once the install task is set up)
//   ownsBundle(bundle)      optional: this backend restores the backup bundle with its own view
//   ownImage(fb, board, data)  optional: splits a user's own image file into {image, codec} (a merged codec slot)
//   vendorRestore(radio, file, fb)  optional: a restore bundle from the radio's original firmware file (an
//                           openVendorFile entry for this radio), so recovery "restore" also takes that file
//   release()               optional: let go of a port or loader the backend still holds (start over, other radio)
//
// Bulk memory reads for backups go through readers (readers.js), so a backend that shares a radio family's backup
// steps can still bring its own way of reading.

const BACKENDS = new Map();
const REQUIRED = ["id", "available", "steps", "views", "recovery"];

const NONE = Object.freeze({
	id: "none", available: () => false, steps: () => null, views: {},
	recovery: { first: "fix", stock: "restore", reach: null, stockText: "flash.help.fix.stock_text" },
});

export function register(b) {
	const missing = REQUIRED.filter((k) => !(k in b));
	if (missing.length) throw new Error(`backend ${b.id || "?"} lacks ${missing.join(", ")}`);
	BACKENDS.set(b.id, b);
	return b;
}

// The backend of a radio (radios.json entry); a radio without one gets NONE, which offers nothing
export const backendOf = (radio) => BACKENDS.get(radio?.flash?.backend) || NONE;

// The backend that restores a bundle with a view of its own, else null (the radio's backend decides)
export const bundleBackend = (bundle) => (bundle ? [...BACKENDS.values()].find((b) => b.ownsBundle?.(bundle)) || null : null);

export const allBackends = () => [...BACKENDS.values()];
