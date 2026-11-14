#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""tools/cps_client.py against cpsSim (the CPS shim on a simulated DM-1701 flash): info, a qdmr-region read, an edited
write with save-and-reboot, read back, verify, and the flash dump checked byte for byte. Run by `make -C tests/host`."""
import os
import sys
import subprocess
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
import cps_client  # noqa: E402

failures = 0


def check(cond, what):
    global failures
    if not cond:
        print(f"FAIL {what}")
        failures += 1


def make_area(ars):
    """A valid codeplug settings area (codeplug-settings.md 2) holding the ARS server key, as a 4 KB sector"""
    data = (1).to_bytes(2, "little") + (4).to_bytes(2, "little") + ars.to_bytes(4, "little")
    head = b"RFKV" + bytes([1, 16]) + len(data).to_bytes(2, "little") + zlib.crc32(data).to_bytes(4, "little") + bytes(4)
    return (head + data).ljust(4096, b"\xff")


def area_cases(sim, open_fn, read_area, write):
    """The settings area through a client: a valid one in the file is written, an erased or damaged one leaves the
    radio's, and the radio's of a newer version is never written over"""
    A, B = cps_client.SETTINGS_AREA
    check(cps_client.area_state(make_area(5)) == "valid" and cps_client.area_state(b"\xff" * 4096) == "empty",
          "area_state valid / empty")
    newer = bytearray(make_area(5))
    newer[4] = 2
    check(cps_client.area_kept(b"\xff" * 4096, make_area(5)) and not cps_client.area_kept(make_area(6), make_area(5))
          and cps_client.area_kept(make_area(6), bytes(newer)),
          "area_kept: an empty file area or a newer radio area keeps the radio's (verify leaves it out)")
    with tempfile.TemporaryDirectory() as d:
        dump = os.path.join(d, "flash.bin")
        subprocess.run([sim, "--populate", "--dump", dump], stdin=subprocess.DEVNULL, check=True)
        link = open_fn(f"{sim} --image {dump} --dump {dump}")
        img = bytearray(read_area(link))
        check(bytes(img[A:B]) == b"\xff" * 4096, "no area on a new radio")
        img[A:B] = make_area(3100111)
        write(link, img)
        check(bytes(read_area(link)[A:B]) == make_area(3100111), "a valid area is written")
        img[A:B] = b"\xff" * 4096
        write(link, img)
        check(bytes(read_area(link)[A:B]) == make_area(3100111), "an erased area in the file keeps the radio's")
        bad = bytearray(make_area(3100222))
        bad[20] ^= 1
        img[A:B] = bad
        write(link, img)
        check(bytes(read_area(link)[A:B]) == make_area(3100111), "a damaged area in the file keeps the radio's")
        link.close()
        with open(dump, "r+b") as f:
            f.seek(A + 4)
            f.write(b"\x02")                           # a later firmware's area
        link = open_fn(f"{sim} --image {dump} --dump {dump}")
        img[A:B] = make_area(3100333)
        write(link, img)
        check(read_area(link)[A + 4] == 2, "a newer area is never written over")
        link.close()


def test_channels(sim):
    """cps_client.py test-channels: the image it makes, written through the shim into an empty flash, comes up as the
    FM and the DMR channel, the talk group contact and the zone (433.4500 MHz, TG 91, CC 1)"""
    regions = cps_client.REGIONS["qdmr"]
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "test.bin")
        dump = os.path.join(d, "flash.bin")
        rc = cps_client.main(["test-channels", path, "--rx", "433.45", "--tg", "91", "--cc", "1", "--id", "9990276",
                               "--call", "Q0TST"])
        check(rc == 0 and os.path.getsize(path) == cps_client.IMAGE_SIZE, "test-channels makes an image")
        img = cps_client.load_image(path)
        check(cps_client.decode_channel(img, 1) == {"name": "Test FM 433.4500", "rx_hz": 433450000, "tx_hz": 433450000},
              "FM channel in the image")
        check(cps_client.zones(img) == [(0, "Test zone", [1, 2])], f"zones {cps_client.zones(img)}")
        check(cps_client.channel_dmr(img, 1) == (False, 0, 0) and cps_client.channel_dmr(img, 2) == (True, 1, 1),
              "channel_dmr")
        moved = bytearray(img)          # a site moved onto another pair and color code (test_roaming's bench site)
        cps_client.set_channel_rx(moved, 2, 461462500)
        cps_client.set_channel_tx(moved, 2, 451875000)
        cps_client.set_channel_cc(moved, 2, 2)
        check(cps_client.decode_channel(moved, 2)["rx_hz"] == 461462500 and
              cps_client.decode_channel(moved, 2)["tx_hz"] == 451875000 and
              cps_client.channel_dmr(moved, 2) == (True, 2, 1), "set_channel_rx / _tx / _cc")
        cps = cps_client.open_cps(exec_cmd=f"{sim} --dump {dump}")
        cps.write_image(img, regions, reboot=False)
        check(not cps_client.compare(img, cps.read_image(regions), regions), "the image reads back")
        cps.close()
        out = subprocess.run([sim, "--image", dump, "--list"], capture_output=True, text=True).stdout.splitlines()
        check("source 2 channels 2" in out, f"two channels stored: {out[:2]}")
        check('channel 1 "Test FM 433.4500" rx 433450000 tx 433450000 mode 0 bw 0 cc 0 ts 1 contact 0' in out,
              f"FM channel: {out}")
        check('channel 2 "Test DMR TG91" rx 433450000 tx 433450000 mode 1 bw 0 cc 1 ts 1 contact 1' in out,
              f"DMR channel: {out}")
        check('zone 0 "Test zone" 2: 1 2' in out, f"zone: {out}")
        check('contact 1 "TG 91" id 91 type 0' in out, f"contact: {out}")

        # --repeater: channel 3, duplex (RX the repeater's output, TX --rx), in the zone after the other two
        path3 = os.path.join(d, "cp3.bin")
        rc = cps_client.main(["test-channels", path3, "--rx", "433.45", "--tg", "91", "--cc", "1", "--repeater", "438.5"])
        img3 = cps_client.load_image(path3)
        check(rc == 0 and cps_client.decode_channel(img3, 3) ==
              {"name": "DMR RPT 438.5000", "rx_hz": 438500000, "tx_hz": 433450000}, "repeater channel in the image")
        zone = img3[cps_client.ZONE_DATA + 16:cps_client.ZONE_DATA + 24]
        check(zone == bytes([1, 0, 2, 0, 3, 0, 0, 0]), f"zone holds channels 1-3: {zone.hex()}")

        # --watch: channel 4, FM receive only, last in the zone; --zone2: zone 2 holding channel 1
        path4 = os.path.join(d, "cp4.bin")
        rc = cps_client.main(["test-channels", path4, "--rx", "433.45", "--tg", "91", "--cc", "1", "--watch", "433.5",
                              "--zone2"])
        img4 = cps_client.load_image(path4)
        check(rc == 0 and cps_client.decode_channel(img4, 4) ==
              {"name": "Watch 433.5000", "rx_hz": 433500000, "tx_hz": 433500000}, "watch channel in the image")
        _, a4 = cps_client.channel_addr(4)
        check(img4[a4 + 0x33] & 0x04, "watch channel receive only")

        # --repeater with two frequencies: channel 5, the second site, after channel 4
        path5 = os.path.join(d, "cp5.bin")
        rc = cps_client.main(["test-channels", path5, "--rx", "433.45", "--tg", "91", "--cc", "1", "--repeater", "438.5",
                              "438.6", "--watch", "433.5"])
        img5 = cps_client.load_image(path5)
        check(rc == 0 and cps_client.decode_channel(img5, 5) ==
              {"name": "DMR RPT 438.6000", "rx_hz": 438600000, "tx_hz": 433450000}, "second site in the image")
        zone = img5[cps_client.ZONE_DATA + 16:cps_client.ZONE_DATA + 28]
        check(zone == bytes([1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 0, 0]), f"zone holds channels 1-5: {zone.hex()}")
        zone = img4[cps_client.ZONE_DATA + 16:cps_client.ZONE_DATA + 24]
        check(zone == bytes([1, 0, 2, 0, 4, 0, 0, 0]), f"zone holds channels 1, 2, 4: {zone.hex()}")
        z2 = img4[cps_client.ZONE_DATA + 176:cps_client.ZONE_DATA + 176 + 20]
        check(z2[:11] == b"Test zone 2" and z2[16:20] == bytes([1, 0, 0, 0]) and
              img4[cps_client.ZONE_BITMAP] & 3 == 3, f"zone 2: {z2.hex()}")


def firmware_power(n, count):
    """The level the firmware reads from power byte n (core/codeplug power_from_byte)"""
    step = n - 1
    if count == 0 or count >= 9:
        return count - 1 if count and step >= count else step
    return (min(step, 9) * (count - 1) + 4) // 9


def test_power_byte():
    """power_byte reads back as the same level on every level count, and set_channel_power writes it (test_roaming's
    sites at the bench level)"""
    for count in range(1, 17):
        for level in range(count):
            n = cps_client.power_byte(level, count)
            check(1 <= n <= max(10, count) and firmware_power(n, count) == level,
                  f"power_byte({level}, {count}) = {n} reads as {firmware_power(n, count)}")
    img = bytearray(b"\xff" * cps_client.IMAGE_SIZE)
    cps_client.set_channel_power(img, 2, 1, 5)
    _, a = cps_client.channel_addr(2)
    check(img[a + 0x19] == 3, f"set_channel_power: byte {img[a + 0x19]}")


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
        check(bytes(img[0xE0:0xE5]) == b"Q0SIM", "radio name")
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
        # ... and inside the first data block of a write: the sector is read back, differs, and is written again
        hit = [True]
        def noisy_data(b):
            if hit and b[:2] == b"X\x02":
                hit.pop()
                b = b[:12] + b"\xfc" + b[12:]
            real_write(b)
        cps.t.write = noisy_data
        noisy_img = bytearray(img)
        cps_client.set_channel_name(noisy_img, 3, "Noise")
        cps.write_image(noisy_img, regions, reboot=False, base=bytes(img))
        cps.t.write = real_write
        check(not hit, "the noise went in")
        check(cps_client.decode_channel(cps.read_image(regions), 3)["name"] == "Noise", "a write through noise lands")
        cps.write_image(img, regions, reboot=False, base=bytes(noisy_img))

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
    test_channels(sim)
    test_power_byte()
    radio = cps_client.REGIONS["radio"]
    area_cases(sim, lambda cmd: cps_client.open_cps(exec_cmd=cmd), lambda c: c.read_image(radio),
               lambda c, img: c.write_image(img, radio, reboot=False))
    print(f"# cpsClientTest {'FAILED' if failures else 'PASSED'} ({failures} failures)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
