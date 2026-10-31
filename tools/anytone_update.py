#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""anytone_update: write a firmware image through an AnyTone radio's factory update mode, over its USB serial port
(boards with [flash] family = "anytone-update"). The protocol is docs/spec/hardware-d578uv.md 8; the logic is
rfoundry's (tools/rfoundry/rfoundry/anytone.py), which `rfoundry flash` uses too.

    anytone_update.py --board BOARD IMAGE --stock FILE.CDD      RadioFoundry (the boot block merged from the stock file)
    anytone_update.py --board BOARD IMAGE_modem.bin             an image with the boot block merged in already
    anytone_update.py --board BOARD --restore FILE.CDD          back to the stock firmware (its .CDI and .spi beside it)
    anytone_update.py --board BOARD --probe                     ask the radio which model and MCU it is, write nothing
    anytone_update.py --board BOARD IMAGE --dry-run             check the image and print the plan, touch nothing
    anytone_update.py --self-test                               against a simulated radio, no radio

The radio names its model when asked (byte 02), and the name differs by MCU: a GD32 radio and an Artery radio of the
same model answer different names, as the vendor's files carry them (board.toml [flash.anytone]). The tool writes only
an image built for the MCU the radio reports, as the vendor's updater refuses a file for the other one.

Needs Python 3.8+ and pyserial (pip install pyserial).
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "rfoundry"))
from rfoundry import anytone as at, boards as rfoundryboards, modemboot  # noqa: E402


def self_test():
    f = at.frame(0x08006000, b"\x01\x02")
    assert len(f) == at.FRAME_LEN and f[0] == 1 and f[-1] == at.ACK and f[1:5] == bytes.fromhex("00600008"), f.hex()
    assert at.parse_identify(b"IMODEL1\x00\x01V100\x00\x00\x06") == ("MODEL1", 1, "V100")
    for bad in (b"", b"X" * 16, b"IMODEL1\x00\x01V100\x00\x00\x15"):
        try:
            at.parse_identify(bad)
        except at.UpdateError:
            pass
        else:
            raise AssertionError("accepted %r" % bad)
    fake = at.FakeRadio(nak_every=7)
    r = at.Radio(fake, log=lambda m: None, timeout=0.01)
    r.hello()
    assert r.identify()[0] == "MODEL1"
    data = bytes(range(256)) * 3 + b"\xab"
    r.write(0x08006000, data)
    got = b"".join(fake.mem[a] for a in sorted(fake.mem))
    assert fake.finished and got[:len(data)] == data and len(fake.mem) == (len(data) + 31) // 32, len(fake.mem)
    # The variant checks against each board of the family: tools/rfoundry/tests/test_anytone.py
    print("anytone_update self-test passed")


def progress_bar(done, total):
    if done == total or done % 256 == 0:
        sys.stdout.write("\r  %3d %% (%d of %d frames)" % (done * 100 // total, done, total))
        if done == total:
            sys.stdout.write("\n")
        sys.stdout.flush()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("image", nargs="?", help="the RadioFoundry image to write")
    ap.add_argument("--board", help="the radio (boards/<id>)")
    ap.add_argument("--stock", help="the radio's stock firmware (.CDD) or a program flash read: the boot block for "
                    "the image")
    ap.add_argument("--restore", metavar="CDD", help="a vendor firmware .CDD to write back (its .CDI and .spi beside it)")
    ap.add_argument("--variant", help="the variant you know the radio is (refused if the radio says otherwise)")
    ap.add_argument("--port", help="the radio's USB serial port (default: the only one found)")
    ap.add_argument("--probe", action="store_true", help="identify the radio, write nothing")
    ap.add_argument("--dry-run", action="store_true", help="check the image and print the plan, touch nothing")
    ap.add_argument("--yes", action="store_true", help="don't ask before writing")
    ap.add_argument("--self-test", action="store_true", help="check the tool against a simulated radio")
    a = ap.parse_args(argv)
    if a.self_test:
        self_test()
        return 0
    boards = {b.id: b for b in rfoundryboards.by(family="anytone-update")}
    if a.board not in boards:
        ap.error("--board is one of: %s" % ", ".join(sorted(boards)))
    board = boards[a.board]
    try:
        spi = None
        if a.restore:
            addr, data, spi = at.read_stock(a.restore)
        elif a.image:
            with open(a.image, "rb") as f:
                app, block = at.split_image(board, f.read())
            if block is None:
                if not a.stock:
                    raise at.UpdateError("the image has no modem boot block: pass --stock with the radio's stock "
                                         "firmware (.CDD) or a read of its program flash")
                with open(a.stock, "rb") as f:
                    block = modemboot.find(f.read())
            addr, data = board.app_base, at.install_image(board, app, block)
        elif not a.probe:
            ap.error("give an image, --restore or --probe")
        if not a.probe:
            at.check_fits(board, addr, data)
            print("plan     %d bytes to 0x%08X in %d frames, %s" % (len(data), addr, at.frames_needed(len(data)),
                  "stock firmware for %s" % " or ".join(sorted(spi)) if spi else "RadioFoundry with the boot block"))
            if a.dry_run:
                print("dry run: nothing was written")
                return 0
        radio = at.Radio(at.open_port(a.port))
        radio.hello()
        model, hw, ver = radio.identify()
        got = at.variant_of(board, model)
        print("radio    model %s, hardware %d, protocol %s: %s" % (model, hw, ver, got and board.variant_name(got)
                                                                    or "not a %s this tool knows" % board.name))
        if a.probe:
            return 0
        if spi:
            at.check_restore(board, model, spi, a.variant)
        else:
            at.check_install(board, model, a.variant)
        if not a.yes and input("Write it now? [y/N] ").strip().lower() != "y":
            print("nothing written")
            return 1
        radio.write(addr, data, progress=progress_bar)
        print("done     the radio installs the image and restarts")
    except (at.UpdateError, modemboot.ModemBootError, OSError) as e:
        print("anytone_update: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
