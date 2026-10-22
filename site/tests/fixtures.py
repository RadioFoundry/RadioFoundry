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
    made-up addresses, its profile, what hrc7000_codec.locate() finds and the slot hrc7000_codec.merge() builds from it
  - for the GD-77: a synthetic stock update file with a made-up codec (the odrflash tests' builders), what
    updatefile.py and gd77codec.py make of it with an application image, and every frame gd77boot's updater sends to
    its model loader, so site/tests/gd77.test.mjs checks gd77.js and gd77codec.js byte for byte
  - for the GD-77's K22F stub: its board record from stub/k22stub.py config_record(), the HELLO replies its simulated
    stub (FakeF4 with k22stub.K22) gives before and after CONFIG, and two installs on a simulated flash (the
    application alone, then an image merged with the codec) with the sectors k22stub.py erases and the flash it leaves,
    so site/tests/k22stub.test.mjs checks k22stub.js and the fake radio against the Python
  - for the codeplug editor: the radio's own codeplug code on the host (tests/host/cpsSim), a codeplug it stored
    and what it reads back, so site/tests/codeplug.test.mjs checks the JavaScript decoder and encoder against it

    python3 site/tests/fixtures.py OUT_DIR
"""
import collections
import json
import os
import random
import struct
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "odrflash"))

sys.path.insert(0, os.path.join(ROOT, "site"))

from odrflash import boards, codecmap, manifest, plan, stockfw  # noqa: E402
from odrsite import repo  # noqa: E402
from odrflash.fakedfu import synthetic_key  # noqa: E402


def codec_map_vectors(out):
	"""odrflash's shared codec map vectors with the map normalized, as data/flash-boards.json carries maps"""
	with open(os.path.join(ROOT, "tools", "odrflash", "tests", "codecmap_vectors.json")) as f:
		v = json.load(f)
	v["map"] = codecmap.normalize(v["map"])
	v["new_run"] = int(v["new_run"], 0)
	v["table_moves"] = {int(k, 0): int(x, 0) for k, x in v["table_moves"].items()}
	t = v["thumb"]
	t["map"] = codecmap.normalize(t["map"])
	t["new_run"], t["window_addr"] = int(t["new_run"], 0), int(t["window_addr"], 0)
	with open(os.path.join(out, "codecmap.json"), "w") as f:
		json.dump(v, f, indent=1)


def md380_like(out):
	"""A stock file whose plain text is like Thumb code, 0x00 common but not dominant, as in the MD-380 image: the column
	vote gets hundreds of key columns wrong and only the byte-pair refinement finds the key (odrflash's
	test_derive_key_refines_a_bad_vote). Returns its fixture entry."""
	rng = random.Random(1)
	key = synthetic_key(380)
	vocab = [rng.randrange(1, 65536) for _ in range(48)] + [0]
	weights = [1.0 / (i + 1) for i in range(48)] + [1.0]
	n = 0xF2C00
	nxt = {w: rng.choices(vocab, weights, k=3) for w in vocab}
	pool = rng.choices(vocab, weights, k=n // 2)
	hw, cur = [], 0
	for i in range(n // 2):
		cur = nxt[cur][i % 3] if rng.random() < 0.6 else pool[i]
		hw.append(cur)
	plain = struct.pack("<%dH" % (n // 2), *hw)
	hdr = bytearray(stockfw.HEADER_LEN)
	hdr[:len(stockfw.MAGIC)] = stockfw.MAGIC
	struct.pack_into("<I", hdr, 0x7C, 0xFFFFFFFF)
	struct.pack_into("<II", hdr, 0x80, 0x0800C000, n)
	stream = (key * (n // len(key) + 1))[:n]
	stock = bytes(hdr) + bytes(a ^ b for a, b in zip(plain, stream)) + bytes(0x100)
	with open(os.path.join(out, "stock-md380like.bin"), "wb") as f:
		f.write(stock)
	payload = stock[stockfw.HEADER_LEN:stockfw.HEADER_LEN + n]
	vote = bytes(collections.Counter(payload[j::1024]).most_common(1)[0][0] for j in range(1024))
	return {"key": key.hex(), "derived_key": stockfw.derive_key(stock, stockfw.sha256(key)).hex(),
		"vote_wrong": sum(a != b for a, b in zip(vote, key))}


def main(out):
	os.makedirs(out, exist_ok=True)
	codec_map_vectors(out)
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
		# the codec as the merge places it (code moved, tables cut, descriptor): what stockfw.js must give
		"placed": [[a, stockfw.sha256(d), len(d)] for a, d in codec],
		"md380like": md380_like(out),
	}
	for k in ("code", "tables", "extra"):
		doc["profile"].pop(k, None)
	with open(os.path.join(out, "fixtures.json"), "w") as f:
		json.dump(doc, f, indent=1)
	# the site's data files the flasher reads (what site/build.py writes to data/)
	feats = repo.features()
	variants = repo.variants()
	radios, _ = repo.boards([f["name"] for f in feats], [v["id"] for v in variants])
	# the synthetic codecs' maps go into the flash-boards data, as a real build's would (docs/spec/codec-placement.md)
	test_maps = {**c7000(out, rng), **db25d(out, rng)}
	gd77(out)
	k22(out)
	codeplug(out)
	fb = repo.flash_boards()
	fb["codec_maps"].update(test_maps)
	fb["c7000"]["codec_maps"].update(test_maps)
	for name, obj in (("c7000-stub.json", repo.c7000_stub()), ("f4-stub.json", repo.f4_stub()), ("k22-stub.json", repo.k22_stub()),
			  ("flash-boards.json", fb), ("features.json", {"features": feats}),
			  ("variants.json", {"variants": variants}), ("radios.json", {"radios": radios})):
		with open(os.path.join(out, name), "w") as f:
			json.dump(obj, f)


def c7000(out, rng):
	import struct
	import zlib
	st, dc = repo._tool("stub", "stubtool.py"), repo._tool("tools", "hrc7000_codec.py")
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
	maps = dc.synthetic_map(found, bytes(code))
	merged = dc.merge(image, found, *dc.carve(bytes(dump), found), maps)
	for name, data in (("lz4_plain.bin", sector), ("lz4_block.bin", st.lz4_encode(sector)), ("c7000_progmem.bin", dump),
			   ("c7000_image.bin", image), ("c7000_merged.bin", merged)):
		with open(os.path.join(out, name), "wb") as f:
			f.write(data)
	doc = {"profile": {"name": "synthetic", "board": "dm1801pro", "tag": prof["tag"], "code_len": len(code), "run": prof["code"][2],
			   "code_sha256": prof["code_sha256"], "tables_len": len(tables), "tables_ram": prof["tables"][2],
			   "tables_sha256": prof["tables_sha256"], "tables_head_crc": prof["tables_head_crc"],
			   "entries": [prof["entries"][k] for k in dc.ENTRIES], "stock_file": None},
	       "found": {"code": found["code"][0] - dc.PROGMEM_BASE, "tables": found["tables"][0] - dc.PROGMEM_BASE}}
	# a slot (hrc7000_codec.test_slot: a synthetic DB25-D codec with a map), the code window it was made for
	slot2, windows, moved_crc, m = dc.test_slot()
	with open(os.path.join(out, "c7000_slot_v2.bin"), "wb") as f:
		f.write(slot2)
	run_lo, run_hi = struct.unpack_from("<2I", windows, 0)
	doc["v2"] = {"map": m, "run": [run_lo, run_hi], "moved_crc": moved_crc}
	with open(os.path.join(out, "c7000.json"), "w") as f:
		json.dump(doc, f, indent=1)
	return maps


def db25d(out, rng):
	"""The DB25-D's plain stock image (no header, no container), a synthetic one with the real profile's addresses, and
	the merged image hrc7000_codec.merge() builds from it. With ODR_DB25D_STOCK=<one of the real 018 files> the expected
	result for that file is made as well, so the JavaScript can be checked on the real bytes (never kept in the repo)."""
	import struct
	dc = repo._tool("tools", "hrc7000_codec.py")
	real = dc.PROFILES["341536cbac0c1b2471f8198ebc456946e5fe026d96d153df14d6aa5566304f8b"]
	plain = bytearray(rng.getrandbits(8) for _ in range(real["size"]))
	caddr, clen, _ = real["code"]
	taddr, tlen, _ = real["tables"]
	struct.pack_into("<2I", plain, dc.file_offset(caddr, 0), *dc.CODEC_SIG)
	plain = bytes(plain)
	prof = dict(real)
	found, code, tables = dc.extract(plain, {dc.sha256(plain): prof}, board="db25d")
	image = bytes(rng.getrandbits(8) for _ in range(0x1000 + 20000))
	maps = dc.synthetic_map(found, code)
	merged = dc.merge(image, found, code, tables, maps)
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
	return maps


def gd77(out):
	"""The GD-77 update file round: a synthetic stock file (tools/odrflash/tests: test_gd77codec.synthetic,
	test_gd77flash.make_update_file), an application image, the update file odrflash writes with the codec merged and
	without it, the placed codec's CRC-32s and descriptor, and the frames the updater sends the model loader. With
	ODR_GD77_STOCK=<a real stock update file> the expected result for that file is made as well (never kept in the repo)."""
	import hashlib
	sys.path.insert(0, os.path.join(ROOT, "tools", "odrflash", "tests"))
	try:
		import test_gd77codec as tc
		import test_gd77flash as tf
	finally:
		sys.path.pop(0)
	from odrflash import gd77boot as gb, gd77codec as gc, updatefile as uf
	sha = lambda b: hashlib.sha256(bytes(b)).hexdigest()
	stock, spec = tc.synthetic(random.Random(77))
	donor = tf.make_update_file(stock)
	app = bytes(random.Random(9).getrandbits(8) for _ in range(20000))
	tag = 0x1234
	pk = gc.pack(stock, spec)
	merged = gc.merge(app, pk, tag)
	sgl = uf.pack(merged, donor)
	keep = uf.pack(app, donor)
	u = uf.UpdateFile(sgl)
	fake = gb.FakeBootloader(u)
	frames = []

	class Recorder:
		def write(self, report):
			frames.append(bytes(report))
			fake.write(report)

		def read(self, timeout_s):
			return fake.read(timeout_s)
	gb.Updater(Recorder(), sleep=lambda s: None).flash(u)
	if fake.received() != u.body[:u.length]:
		raise SystemExit("gd77 fixture: the model loader received something else than the body")
	d = uf.UpdateFile(donor)
	doc = {
		"spec": dict(spec, words=[list(w) for w in spec["words"]], branches=[list(b) for b in spec["branches"]]),
		"tag": tag,
		"info": {"start": d.start, "length": d.length, "company": d.company.hex(), "model": d.model.hex(),
			 "version": d.version.hex(), "tag": d.tag.hex(), "addr_width": d.addr_width, "len_width": d.len_width,
			 "block_size": d.block_size, "use_fprog": d.use_fprog, "send_tag": d.send_tag, "room": d.room,
			 "label": d.label(), "pad": d.pad, "info_at": d.info_at},
		"unpacked_sha256": sha(uf.unpack(donor)),
		"stock_sha256": sha(stock),
		"placed": {"main_new": pk.main_new, "low_new": pk.low_new, "code_len": len(pk.code), "code_crc": gc.crc32(pk.code),
			   "tab_crc": gc.crc32(pk.tab), "descriptor": pk.descriptor(tag).hex()},
		"merged_sha256": sha(merged), "merged_len": len(merged),
		"out_sha256": sha(sgl), "keep_sha256": sha(keep),
		"frames": {"count": len(frames), "sha256": sha(b"".join(frames)), "head": [f.hex() for f in frames[:12]],
			   "tail": [f.hex() for f in frames[-3:]]},
		"real": None,
	}
	files = {"gd77_donor.sgl": donor, "gd77_app.bin": app, "gd77_out.sgl": sgl}
	path = os.environ.get("ODR_GD77_STOCK")
	if path:
		with open(path, "rb") as f:
			real = f.read()
		ru = uf.UpdateFile(real)
		flat = uf.unpack(ru)
		rspec = gc.find_spec(flat)
		out_real = uf.pack(gc.merge(app, gc.pack(flat, rspec)) if rspec else app, ru)
		doc["real"] = {"path": os.path.abspath(path), "label": ru.label(), "profile": rspec and rspec["profile"],
			       "out_sha256": sha(out_real)}
	for name, data in files.items():
		with open(os.path.join(out, name), "wb") as f:
			f.write(data)
	with open(os.path.join(out, "gd77.json"), "w") as f:
		json.dump(doc, f, indent=1)


def k22(out):
	"""stub/k22stub.py's board record, HELLO from its fake stub before and after CONFIG (and a refused record), the stack
	pointers its chip accepts, and installs on a simulated flash: what the tool erases and the flash it leaves"""
	import hashlib
	import tempfile
	k22 = repo._tool("stub", "k22stub.py")
	f4 = k22.f4stub
	st = f4.stubtool
	sha = lambda b: hashlib.sha256(bytes(b)).hexdigest()

	def ask(fake, op, payload=b""):
		fake.write(bytes([0x02, op, len(payload) >> 8, len(payload) & 0xFF, st.sum8(payload), 0x03]) + payload)
		r = bytes(fake.out)
		fake.out.clear()
		return {"status": r[1], "body": r[6:].hex()}

	rec = k22.config_record("gd77")
	bad = bytearray(rec)
	bad[4:8] = (0x2000).to_bytes(4, "big")		# an application base below the loader's end
	fake = f4.FakeF4(b"\xff" * (k22.FLASH_KB * 1024), spi=b"\xff" * 0x200000, chip=k22.K22)
	doc = {"record": rec.hex(), "hello": {"before": ask(fake, f4.OP_HELLO), "refused": ask(fake, f4.OP_CONFIG, bytes(bad)),
					      "erase_before": ask(fake, f4.OP_ERASE, bytes([9]) + k22.MAGIC.to_bytes(4, "big")),
					      "config": ask(fake, f4.OP_CONFIG, rec), "after": ask(fake, f4.OP_HELLO)},
	       "sp": {hex(a): k22.K22.sp_ok(a) for a in (0x1FFF0000, 0x1FFF0004, 0x2000FEF0, 0x20010000, 0x20010004, 0x2000FEF2,
							   0x20000000, 0x0800C000)}}
	rnd = random.Random(22)
	flash = bytearray(rnd.getrandbits(8) for _ in range(k22.FLASH_KB * 1024))
	app = bytearray(rnd.getrandbits(8) for _ in range(300000))
	app[:4] = (0x2000FEF0).to_bytes(4, "little")
	b = k22.BOARDS["gd77"]
	merged = bytearray(b"\xff" * (b["codec_image_end"] - k22.APP_BASE))
	merged[:len(app) - 5000] = app[:-5000]
	merged[b["codec"][0] - k22.APP_BASE:] = bytes(rnd.getrandbits(8) for _ in range(b["codec_image_end"] - b["codec"][0]))
	files = {"k22_flash.bin": bytes(flash), "k22_app.bin": bytes(app), "k22_merged.bin": bytes(merged)}
	for name, data in files.items():
		with open(os.path.join(out, name), "wb") as f:
			f.write(data)
	fake = f4.FakeF4(bytes(flash), chip=k22.K22)
	radio = f4.F4Radio(fake, log=lambda *a: None, chip=k22.K22)
	radio.hello()
	radio.configure("gd77")
	doc["installs"] = []
	with tempfile.TemporaryDirectory(prefix="odr-k22-fixture-") as tmp:
		for name in ("k22_app.bin", "k22_merged.bin"):
			fake.erased = []
			segs = f4.load_segments(os.path.join(out, name), "gd77", k22.K22)
			nw, nk = radio.write_image("gd77", segs, backup_dir=tmp)
			doc["installs"].append({"image": name, "segments": [[a, len(d), sha(d)] for a, d in segs], "erased": fake.erased,
					       "written": nw, "same": nk, "flash_sha256": sha(fake.flash)})
	with open(os.path.join(out, "k22.json"), "w") as f:
		json.dump(doc, f, indent=1)


def codeplug(out):
	host = os.path.join(ROOT, "tests", "host")
	subprocess.run(["make", "-s", "-C", host, "cpsSim"], check=True)
	sim = os.path.join(host, "cpsSim")
	image = os.path.join(out, "codeplug_sim.bin")
	subprocess.run([sim, "--populate", "--dump", image], stdin=subprocess.DEVNULL, check=True)
	detail = subprocess.run([sim, "--image", image, "--detail"], capture_output=True, text=True, check=True).stdout
	with open(os.path.join(out, "codeplug.json"), "w") as f:
		json.dump({"cpssim": sim, "detail": detail}, f, indent=1)


if __name__ == "__main__":
	main(sys.argv[1])
