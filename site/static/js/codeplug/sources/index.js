// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The contacts and channels sources the editor can draw on. Each module exports ID, its loaders and parsers; output.js
// turns any source's users into the ID database or contacts. Repeater sources (channels and zones) come later.

import * as radioid from "./radioid.js";

export const SOURCES = { [radioid.ID]: radioid };
