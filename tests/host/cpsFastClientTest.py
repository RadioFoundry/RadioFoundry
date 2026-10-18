#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
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

        # A block the regions cover only in part: the radio's bytes outside them stay
        edit2 = bytearray(edit)
        edit2[0x7400] = 0x00                                # outside the regions: not written
        edit2[0x7540:0x7550] = b"Fast boot line 1"         # boot text line 1 (inside 0x7500-0xB000)
        n, _ = link.write_image(edit2, regions)
        back = link.read_image([(0x7000, 0x8000)])
        check(n == 1 and back[0x7400] == 0x5A and back[0x7540:0x7550] == b"Fast boot line 1", "partial block merged")
        check(not link.verify(edit2, regions), "verify")
        link.close()

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
    print(f"# cpsFastClientTest {'FAILED' if failures else 'PASSED'} ({failures} failures)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
