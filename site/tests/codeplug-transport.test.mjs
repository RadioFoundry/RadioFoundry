// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors

import assert from "node:assert/strict";
import { test } from "node:test";

import { FakeCpsRadio } from "../static/js/codeplug/fakecps.js";
import * as iddb from "../static/js/codeplug/iddb.js";
import { CodeplugImage } from "../static/js/codeplug/image.js";
import { CpsSerial, TransportError } from "../static/js/codeplug/transport.js";

test("a canceled CPS write keeps the radio's existing ID database", async () => {
	const have = CodeplugImage.from(new Uint8Array(0));
	const records = [{ id: 1234567, text: "W1OLD" }];
	iddb.write(have, records);
	const radio = new FakeCpsRadio({ image: have.bytes });
	const transport = new CpsSerial(radio.stream());
	await transport.identify([{ id: "dm1701", codeplug: { base: 0, end: 0xE00000, names: ["DM-1701"] } }]);
	const want = have.clone();
	iddb.write(want, [{ id: 1234567, text: "W1NEW" }]);

	await assert.rejects(transport.write(want, have, { iddb: true, signal: AbortSignal.abort() }),
		(e) => e instanceof TransportError && e.code === "stopped");
	assert.deepEqual(radio.writes, []);
	assert.deepEqual(iddb.read(radio.spi), records);
});
