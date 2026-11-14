#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""tools/cps_fast.py against cpsSim (the fast codeplug protocol on the debug link, beside the CPS shim, on a simulated
DM-1701 flash): INFO, a read that matches the CPS's, delta reads from a cache, staged and direct writes of only the
changed blocks, bytes outside the regions kept, the command line, and the bytes on the wire against the CPS's. Run by
`make -C tests/host`."""
import os
import sys
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
import cps_client  # noqa: E402
import cps_fast  # noqa: E402
import cpsClientTest  # noqa: E402  (the settings area cases, shared)

failures = 0


def check(cond, what):
    global failures
    if not cond:
        print(f"FAIL {what}")
        failures += 1


def main():
    sim = os.path.join(HERE, "cpsSim")
    regions = cps_client.REGIONS["qdmr"]
    with tempfile.TemporaryDirectory() as d:
        dump = os.path.join(d, "flash.bin")
        # The flash with a byte outside the regions but inside one of their blocks (0x7400, before 0x7500)
        subprocess.run([sim, "--populate", "--dump", dump], stdin=subprocess.DEVNULL, check=True)
        with open(dump, "r+b") as f:
            f.seek(0x7400)
            f.write(b"\x5a")

        link = cps_fast.open_link(exec_cmd=f"{sim} --image {dump} --dump {dump}")
        info = link.info()
        check(info["version"] == 1 and info["staging"] and info["writable"] and info["stage_max"] == 63,
              f"info {info}")
        check(info["base"] == 0 and info["ee_size"] == 0x10000 and info["flash_start"] == 0x20000 and
              info["end"] == 0xE00000, f"layout {info}")

        # A read: the same bytes as the CPS's, in far fewer bytes on the wire
        img = link.read_image(regions)
        full_moved, full_wire, full_requests = link.moved, link.wire, link.requests
        check(cps_client.decode_channel(img, 2) == {"name": "Sim 2", "rx_hz": 446025000, "tx_hz": 446025000},
              "channel 2 read")
        cps = cps_client.Cps(link.t)        # the CPS shim on the same stream
        ref = cps.read_image(regions, block=1024)
        check(not cps_client.compare(ref, img, regions), "fast read = CPS read")
        cps_reads = sum((e - s + 31) // 32 for s, e in regions)     # 8-byte request, 3-byte header, 32 bytes each
        cps_wire = sum(e - s for s, e in regions) + 11 * cps_reads
        print(f"# full read: {full_moved} blocks moved, {full_wire} bytes on the wire in {full_requests} requests "
              f"(the qdmr regions in 32-byte CPS reads: about {cps_wire} bytes)")
        check(full_wire * 5 < cps_wire, "a read moves a fifth of the CPS's bytes or less")

        # A delta read from the cache: nothing moves; after an edit, one block
        link.wire = 0
        again = link.read_image(regions, cache=img)
        check(link.moved == 0 and again == img, f"cached read moved {link.moved}")
        edit = bytearray(img)
        cps_client.set_channel_name(edit, 3, "Fast three")
        n, sent = link.write_image(edit, regions)
        check(n == 1 and 0 < sent < 1500, f"one block written, {sent} stream bytes")
        st = link.status()
        check(st["state"] == cps_fast.ST_DONE and st["mode"] == cps_fast.MODE_STAGED and st["copied"] == 1,
              f"staged commit {st}")
        delta = link.read_image(regions, cache=img)
        check(link.moved == 1 and cps_client.decode_channel(delta, 3)["name"] == "Fast three", "delta read")
        check(cps_client.decode_channel(cps.read_image(regions), 3)["name"] == "Fast three", "the CPS sees the write")
        check(link.write_image(edit, regions) == (0, 0), "nothing to write the second time")

        # DATA pipelined (cps-fast.md 3.6): a block that doesn't compress, its 68 pieces sent 8 at a time; a piece
        # lost in a window sends the block again from BLOCK, one DATA at a time
        import random
        noise = bytearray(edit)
        noise[0x30000:0x31000] = random.Random(7).randbytes(cps_fast.BLOCK)
        write, sent_data = link.t.write, []

        def lossy(f, drop=[3]):
            if f[2] == cps_fast.CMD and f[6] == cps_fast.OP_DATA:
                sent_data.append(f[3])
                if len(sent_data) in drop:
                    drop.clear()
                    return              # lost on the way
            write(f)
        link.t.write = lossy
        n, sent = link.write_image(noise, [(0x30000, 0x31000)])
        link.t.write = write
        pieces = -(-sent // link.info()["data_max"])
        check(n == 1 and pieces >= 60 and len(sent_data) == cps_fast.WINDOW + pieces,
              f"a lost piece: {len(sent_data)} DATA sent for {pieces} pieces (the window, then one at a time)")
        check(link.read_block(0x30000) == bytes(noise[0x30000:0x31000]), "the block after a lost piece")

        # Two tries lost, then the third's window fails: the block goes again one DATA at a time (the fallback costs
        # no try) and is written, never reported written when it isn't
        ok, data, calls = link.ok, link.data, {"block": 0, "data": 0}

        def flaky_ok(op, *a, **kw):
            if op == cps_fast.OP_BLOCK:
                calls["block"] += 1
                if calls["block"] <= 2:
                    raise cps_fast.FastError("op 4: no answer")
            return ok(op, *a, **kw)

        def flaky_data(stream, window):
            calls["data"] += 1
            return False if calls["data"] == 1 else data(stream, window)
        link.ok, link.data = flaky_ok, flaky_data
        link.begin(cps_fast.MODE_DIRECT)
        noise[0x30000:0x31000] = random.Random(8).randbytes(cps_fast.BLOCK)
        got = link.write_block(0x30000, bytes(noise[0x30000:0x31000]))
        link.ok, link.data = ok, data
        link.commit()
        check(got > 0 and calls == {"block": 4, "data": 2}, f"tries lost, then a window: returned {got}, {calls}")
        check(link.read_block(0x30000) == bytes(noise[0x30000:0x31000]), "the block after tries lost and a window")

        # A block the regions cover only in part: the radio's bytes outside them stay
        edit2 = bytearray(edit)
        edit2[0x7400] = 0x00                                # outside the regions: not written
        edit2[0x7540:0x7550] = b"Fast boot line 1"         # boot text line 1 (inside 0x7500-0xB000)
        n, _ = link.write_image(edit2, regions)
        back = link.read_image([(0x7000, 0x8000)])
        check(n == 1 and back[0x7400] == 0x5A and back[0x7540:0x7550] == b"Fast boot line 1", "partial block merged")
        check(not link.verify(edit2, regions), "verify")

        # Patch blocks (3.7): with an image read earlier as the cache, a changed block goes as a patch of the radio's
        check(link.info()["patch"], "INFO lists patch blocks")
        edit3 = bytearray(edit2)
        cps_client.set_channel_name(edit3, 3, "Plain three")
        link.patched = 0
        n, plain = link.write_image(edit3, regions)                 # no cache: the whole block
        check(n == 1 and link.patched == 0, f"plain write {n} {link.patched}")
        held = link.read_image(regions)
        edit4 = bytearray(edit3)
        cps_client.set_channel_name(edit4, 3, "Patch three")
        n, patched = link.write_image(edit4, regions, cache=held)
        print(f"# one channel renamed: {plain} stream bytes as a block, {patched} as a patch")
        check(n == 1 and link.patched == 1 and patched * 5 < plain, f"patch write {n} {link.patched} {patched}")
        check(not link.verify(edit4, regions), "verify after the patch")
        # a cache that's stale for a block: that block goes whole (its CRC isn't the radio's)
        edit5 = bytearray(edit4)
        cps_client.set_channel_name(edit5, 3, "Stale three")
        link.patched = 0
        n, _ = link.write_image(edit5, regions, cache=held)
        check(n == 1 and link.patched == 0 and not link.verify(edit5, regions), "stale cache: plain")
        # a base the radio doesn't hold (a block changed behind the host's back): refused, sent whole
        a = 0x7000
        want = bytes(edit5[a:a + cps_fast.BLOCK])
        wrong = bytearray(want)
        wrong[100] ^= 1
        link.begin(cps_fast.MODE_DIRECT)
        link.patched = 0
        sent = link.write_block(a, want, base=bytes(wrong))
        link.commit()
        check(link.patched == 0 and sent > 0 and not link.verify(edit5, regions), "wrong base: sent whole")
        link.close()
        edit2 = edit5

        # The command line: verify, a write from a file, a read with a cache
        path = os.path.join(d, "img.bin")
        with open(path, "wb") as f:
            f.write(edit2)
        rc = cps_fast.main(["--exec", f"{sim} --image {dump}", "--quiet", "verify", path])
        check(rc == 0, "cps_fast verify")
        cps_client.set_channel_name(edit2, 4, "Cli four")
        with open(path, "wb") as f:
            f.write(edit2)
        rc = cps_fast.main(["--exec", f"{sim} --image {dump} --dump {dump}", "--quiet", "write", path])
        check(rc == 0, "cps_fast write")
        out = os.path.join(d, "out.bin")
        rc = cps_fast.main(["--exec", f"{sim} --image {dump}", "--quiet", "read", out, "--cache", path])
        check(rc == 0 and cps_client.decode_channel(cps_client.load_image(out), 4)["name"] == "Cli four", "cps_fast read")

        # A radio without a staging area: the write goes through the journal block by block
        link = cps_fast.open_link(exec_cmd=f"{sim} --image {dump} --no-staging --dump {dump}")
        check(not link.info()["staging"], "no staging")
        cps_client.set_channel_name(edit2, 5, "Direct five")
        cps_client.set_channel_name(edit2, 200, "Direct 200")     # in the flash image, bank 1 unused: a new block
        n, _ = link.write_image(edit2, regions)
        st = link.status()
        check(n == 2 and st["mode"] == cps_fast.MODE_DIRECT and st["state"] == cps_fast.ST_DONE, f"direct {n} {st}")
        link.close()
        listing = subprocess.run([sim, "--image", dump, "--list"], capture_output=True, text=True).stdout
        check('channel 5 "Direct five"' in listing and 'channel 4 "Cli four"' in listing, f"stored: {listing[:200]}")

        # A refusal comes back as one: the settings ring isn't the protocol's
        link = cps_fast.open_link(exec_cmd=f"{sim} --image {dump}")
        try:
            link.read_block(0x11000)
            check(False, "settings ring read refused")
        except cps_fast.FastError as e:
            check(e.reason == 1, f"refused as outside: {e}")
        link.close()

    # The settings area: written only when the file's is valid and the radio's isn't newer, as cps_client.py
    radio = cps_client.REGIONS["radio"]
    cpsClientTest.area_cases(sim, lambda cmd: cps_fast.open_link(exec_cmd=cmd), lambda k: k.read_image(radio),
                             lambda k, img: k.write_image(img, radio))
    global failures
    failures += cpsClientTest.failures
    print(f"# cpsFastClientTest {'FAILED' if failures else 'PASSED'} ({failures} failures)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
