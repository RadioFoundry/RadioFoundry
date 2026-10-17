#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Fixtures for the web flasher's tests, made with odrflash itself, so the JavaScript port is checked against the
Python it was ported from (site/tests/flasher.test.mjs):

  - a synthetic DM-1701 stock file and its key (odrflash's stockfw.build_synthetic_stock), with its profile
  - plans for a synthetic image in keep and merge mode, as plan.describe() prints them
  - a release manifest for that image (manifest.generate)
  - the site's data files (radios, features, variants, flash boards, the C7000 stub), as site/build.py writes them
  - for the HR_C7000 radios: an LZ4 block from stubtool's encoder, and a synthetic program flash with a codec at
    made-up addresses, its profile, what dm32_codec.locate() finds and the slot dm32_codec.merge() builds from it

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
	data = bytes(rng.getrandbits(8) for _ in range(20 * 1024 + 7))	# the build's sector 9 image
	keep9 = plan.make_plan(board, [(board.app_base, image), (0x080A0000, data)], plan.KEEP)
	m9 = manifest.generate(board, image, "v0.0.0-test", "ab" * 20, features="FM,DMR", data=data)
	with open(os.path.join(out, "stock.bin"), "wb") as f:
		f.write(stock)
	with open(os.path.join(out, "image.bin"), "wb") as f:
		f.write(image)
	with open(os.path.join(out, "image9.bin"), "wb") as f:
		f.write(image + data)
	doc = {
		"key": key.hex(),
		"stock_sha256": sha,
		"profile": dict(prof, segments=[list(s) for s in prof["segments"]],
				windows=[list(w) for w in stockfw.windows(prof)]),
		"derived_key": stockfw.derive_key(stock, stockfw.sha256(key)).hex(),
		"keep": keep.describe(),
		"merge": merge.describe(),
		"manifest": m,
		"keep9": keep9.describe(),
		"manifest9": m9,
	}
	for k in ("code", "tables", "extra"):
		doc["profile"].pop(k, None)
	with open(os.path.join(out, "fixtures.json"), "w") as f:
		json.dump(doc, f, indent=1)
	# the site's data files the flasher reads (what site/build.py writes to data/)
	feats = repo.features()
	variants = repo.variants()
	radios, _ = repo.boards([f["name"] for f in feats], [v["id"] for v in variants])
	c7000(out, rng)
	db25d(out, rng)
	for name, obj in (("c7000-stub.json", repo.c7000_stub()), ("f4-stub.json", repo.f4_stub()), ("flash-boards.json", repo.flash_boards()), ("features.json", {"features": feats}),
			  ("variants.json", {"variants": variants}), ("radios.json", {"radios": radios})):
		with open(os.path.join(out, name), "w") as f:
			json.dump(obj, f)


def c7000(out, rng):
	import struct
	import zlib
	st, dc = repo._tool("stub", "stubtool.py"), repo._tool("tools", "dm32_codec.py")
	sector = bytes(rng.choice(b"ODR flash  \x00\xff") for _ in range(st.SECTOR))
	real = next(p for p in dc.PROFILES.values() if p["board"] == "dm1801pro")
	dump = bytearray(rng.getrandbits(8) for _ in range(dc.PROGMEM_SIZE))
	code = bytearray(rng.getrandbits(8) for _ in range(real["code"][1]))
	struct.pack_into("<2I", code, 0, *dc.CODEC_SIG)
	tables = bytes(rng.getrandbits(8) for _ in range(real["tables"][1]))
	dump[0xB1234:0xB1234 + len(code)] = code
	dump[0x60010:0x60010 + len(tables)] = tables
	prof = dict(real, code_sha256=dc.sha256(bytes(code)), tables_sha256=dc.sha256(tables),
		    tables_head_crc=zlib.crc32(tables[:64]))
	found = dc.locate(bytes(dump), {prof["code_sha256"]: prof}, "dm1801pro")
	image = bytes(rng.getrandbits(8) for _ in range(300 * 1024 + 5))
	merged = dc.merge(image, found, *dc.carve(bytes(dump), found))
	for name, data in (("lz4_plain.bin", sector), ("lz4_block.bin", st.lz4_encode(sector)), ("c7000_progmem.bin", dump),
			   ("c7000_image.bin", image), ("c7000_merged.bin", merged)):
		with open(os.path.join(out, name), "wb") as f:
			f.write(data)
	doc = {"profile": {"name": "synthetic", "board": "dm1801pro", "tag": prof["tag"], "code_len": len(code), "run": prof["code"][2],
			   "code_sha256": prof["code_sha256"], "tables_len": len(tables), "tables_ram": prof["tables"][2],
			   "tables_sha256": prof["tables_sha256"], "tables_head_crc": prof["tables_head_crc"],
			   "entries": [prof["entries"][k] for k in dc.ENTRIES], "stock_file": None},
	       "found": {"code": found["code"][0] - dc.PROGMEM_BASE, "tables": found["tables"][0] - dc.PROGMEM_BASE}}
	with open(os.path.join(out, "c7000.json"), "w") as f:
		json.dump(doc, f, indent=1)


def db25d(out, rng):
	"""The DB25-D's plain stock image (no header, no container), a synthetic one with the real profile's addresses, and
	the merged image dm32_codec.merge() builds from it. With ODR_DB25D_STOCK=<one of the real 018 files> the expected
	result for that file is made as well, so the JavaScript can be checked on the real bytes (never kept in the repo)."""
	import struct
	dc = repo._tool("tools", "dm32_codec.py")
	real = dc.PROFILES["341536cbac0c1b2471f8198ebc456946e5fe026d96d153df14d6aa5566304f8b"]
	plain = bytearray(rng.getrandbits(8) for _ in range(real["size"]))
	caddr, clen, _ = real["code"]
	taddr, tlen, _ = real["tables"]
	struct.pack_into("<2I", plain, dc.file_offset(caddr, 0), *dc.CODEC_SIG)
	plain = bytes(plain)
	prof = dict(real)
	found, code, tables = dc.extract(plain, {dc.sha256(plain): prof}, board="db25d")
	image = bytes(rng.getrandbits(8) for _ in range(0x1000 + 20000))
	merged = dc.merge(image, found, code, tables)
	files = {"db25d_stock.bin": plain, "db25d_image.bin": image, "db25d_merged.bin": merged}
	doc = {"sha256": dc.sha256(plain), "size": len(plain), "code": caddr, "tables": taddr, "real": None}
	path = os.environ.get("ODR_DB25D_STOCK")
	if path:
		with open(path, "rb") as f:
			data = f.read()
		p, c, t = dc.extract(data, board="db25d")
		files["db25d_real_slot.bin"] = dc.slot_bytes(p, c, t)
		doc["real"] = {"path": os.path.abspath(path), "name": p["name"]}
	for name, data in files.items():
		with open(os.path.join(out, name), "wb") as f:
			f.write(data)
	with open(os.path.join(out, "db25d.json"), "w") as f:
		json.dump(doc, f, indent=1)


if __name__ == "__main__":
	main(sys.argv[1])
