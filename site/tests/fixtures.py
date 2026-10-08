#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Fixtures for the web flasher's tests, made with odrflash itself, so the JavaScript port is checked against the
Python it was ported from (site/tests/flasher.test.mjs):

  - a synthetic DM-1701 stock file and its key (odrflash's stockfw.build_synthetic_stock), with its profile
  - plans for a synthetic image in keep and merge mode, as plan.describe() prints them
  - a release manifest for that image (manifest.generate)
  - the site's data files (radios, features, variants, flash boards), as site/build.py writes them

    python3 site/tests/fixtures.py OUT_DIR
"""
import json
import os
import random
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "odrflash"))

sys.path.insert(0, os.path.join(ROOT, "site"))

from odrflash import boards, manifest, plan, stockfw  # noqa: E402
from odrsite import repo  # noqa: E402
from odrflash.fakedfu import synthetic_key  # noqa: E402


def main(out):
	os.makedirs(out, exist_ok=True)
	rng = random.Random(42)
	key = synthetic_key(1701)
	stock, profiles, code, tables = stockfw.build_synthetic_stock(key, rng)
	(sha, prof), = profiles.items()
	board = boards.DM1701
	image = bytes(rng.getrandbits(8) for _ in range(150 * 1024 + 123))
	_, codec = stockfw.extract(stock, key, profiles)
	keep = plan.make_plan(board, [(board.app_base, image)], plan.KEEP)
	merge = plan.make_plan(board, [(board.app_base, image)], plan.MERGE, stockfw.codec_segments(prof, codec))
	m = manifest.generate(board, image, "v0.0.0-test", "ab" * 20, features="FM,DMR")
	with open(os.path.join(out, "stock.bin"), "wb") as f:
		f.write(stock)
	with open(os.path.join(out, "image.bin"), "wb") as f:
		f.write(image)
	doc = {
		"key": key.hex(),
		"stock_sha256": sha,
		"profile": dict(prof, segments=[list(s) for s in prof["segments"]],
				windows=[list(w) for w in stockfw.windows(prof)]),
		"derived_key": stockfw.derive_key(stock, stockfw.sha256(key)).hex(),
		"keep": keep.describe(),
		"merge": merge.describe(),
		"manifest": m,
	}
	for k in ("code", "tables", "extra"):
		doc["profile"].pop(k, None)
	with open(os.path.join(out, "fixtures.json"), "w") as f:
		json.dump(doc, f, indent=1)
	# the site's data files the flasher reads (what site/build.py writes to data/)
	feats = repo.features()
	variants = repo.variants()
	radios, _ = repo.boards([f["name"] for f in feats], [v["id"] for v in variants])
	for name, obj in (("flash-boards.json", repo.flash_boards()), ("features.json", {"features": feats}),
			  ("variants.json", {"variants": variants}), ("radios.json", {"radios": radios})):
		with open(os.path.join(out, name), "w") as f:
			json.dump(obj, f)


if __name__ == "__main__":
	main(sys.argv[1])
