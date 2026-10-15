#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""tools/cps_client.py against cpsSim (the CPS shim on a simulated DM-1701 flash): info, a qdmr-region read, an edited
write with save-and-reboot, read back, verify, and the flash dump checked byte for byte. Run by `make -C tests/host`."""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
import cps_client  # noqa: E402

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
        cps = cps_client.open_cps(exec_cmd=f"{sim} --populate --dump {dump}")
        info = cps.info()
        check(info["struct"] == 3 and info["radio_type"] == 8 and info["flash_id"] == 0x4018, f"info {info}")
        img = cps.read_image(regions)
        name = bytes(img[0x3790:0x37A0]).rstrip(b"\xff")
        check(name == b"Sim 1", f"channel 1 name {name!r}")
        check(bytes(img[0xE0:0xE5]) == b"N0SIM", "radio name")
        ch = cps_client.decode_channel(img, 2)
        check(ch == {"name": "Sim 2", "rx_hz": 446025000, "tx_hz": 446025000}, f"decode_channel {ch}")
        check(cps_client.decode_channel(img, 41) is None, "channel 41 unused")

        # Line noise (the DM-1801 Pro's cable): a burst in front of a read is dropped by the radio, one inside a read
        # garbles it; the client drains and repeats the read
        real_write = cps.t.write
        noise = [b"\xfc\xfe\xfc", None]
        def noisy(b):
            if noise and b[:1] == b"R":
                n = noise.pop(0)
                if n is None:
                    b = b[:4] + b"\xfc" + b[4:]      # inside: the address and length shift
                else:
                    b = n + b
            real_write(b)
        cps.t.write = noisy
        check(cps.read(1, 0x3790, 16)[:5] == b"Sim 1", "a read with noise in front")
        check(cps.read(1, 0x3790, 16)[:5] == b"Sim 1", "a read garbled by noise, repeated")
        cps.t.write = real_write

        # Only the sectors that differ from what the radio holds (the HIL write test's way)
        before = bytes(img)
        edit = bytearray(img)
        cps_client.set_channel_name(edit, 2, "Two")
        commits = []
        cps.write_image(edit, regions, reboot=False, base=before, progress=lambda i, n: commits.append(n))
        check(commits and commits[-1] == 1, f"one sector written ({commits[-1:]})")
        check(cps_client.decode_channel(cps.read_image(regions), 2)["name"] == "Two", "base write landed")
        cps.write_image(before, regions, reboot=False, base=edit)
        img = cps.read_image(regions)
        check(not cps_client.compare(before, img, regions), "base write undone")

        # Edit: rename channel 1, set the quick-key slots (the side key assignment); write with save-and-reboot;
        # read back
        img[0x3790:0x37A0] = b"Edited".ljust(16, b"\xff")
        quick = bytes(range(0x10, 0x24))
        img[0x7524:0x7538] = quick
        cps.write_image(img, regions, reboot=True)
        back = cps.read_image(regions)
        diffs = cps_client.compare(img, back, regions)
        check(not diffs, f"read back differs at {len(diffs)} bytes: {diffs[:4]}")
        check(bytes(back[0x7524:0x7538]) == quick, "quick-key slots written")
        # The same through larger blocks (the open-firmware CPS reads up to 2 KB at a time)
        big = cps.read_image(regions, block=1024)
        check(not cps_client.compare(back, big, regions, skip=()), "1 KB block read = 32-byte block read")
        cps.close()

        with open(dump, "rb") as f:
            flash = f.read()
        check(flash[0x3790:0x3796] == b"Edited", "flash holds the edit")
        for s, e in regions:
            if s <= 0x7524 < e:
                continue
            check(flash[s:e] == bytes(back[s:e]), f"flash = image in {s:#x}-{e:#x}")

        # The command line, on the dumped flash: verify against a file read by the module
        path = os.path.join(d, "img.bin")
        with open(path, "wb") as f:
            f.write(back)
        rc = cps_client.main(["--exec", f"{sim} --image {dump}", "--quiet", "verify", path])
        check(rc == 0, "cps_client verify")
    print(f"# cpsClientTest {'FAILED' if failures else 'PASSED'} ({failures} failures)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
