# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The flash stub route for the boards whose platform builds the STM32F4 flash stub: a radio that runs ODR built with
CONFIG_STUB_ENTRY=y takes the stub into RAM over its USB debug link (stub/stubtool.py --odr-enter), the stub writes the
sectors that differ and restarts the radio (docs/spec/stub-stm32f4.md). No bootloader key press
and no key file. The radio's codec stays: a sector that holds both application and codec bytes is saved first.

odrflash wraps stubtool instead of speaking the stub protocol again, as serialflash.py does for the HR_C7000 radios.
"""
import os
import sys
import tempfile

from . import boards, serialflash

# The boards whose platform builds the STM32F4 stub (the board registry)
BOARDS = tuple(b.id for b in boards.BOARDS.values() if "stub/stm32f4" in b.platform_stubs)


class StubRouteError(Exception):
    pass


def argv(board_id, image_path, port, backup_dir=None):
    out = [port, "--odr-enter", "--board", board_id, "--write-app", image_path, "--boot", "reset"]
    if backup_dir:
        out += ["--backup-dir", backup_dir]
    return out


def run(board, image, port=None, backup_dir=None, log=print, stubtool=None):
    """Write `image` (bytes from the board's application base) through the stub. Returns stubtool's exit code."""
    if board.id not in BOARDS:
        raise StubRouteError("the %s has no stub" % board.name)
    if port is None:
        from . import debuglink
        ports = debuglink.find_ports() or []
        if len(ports) != 1:
            raise StubRouteError("give --port: %s" % ("no ODR debug link found" if not ports else
                                                      "more than one found (%s)" % ", ".join(ports)))
        port = ports[0]
    if stubtool is None:
        stub_dir = os.path.join(serialflash._ROOT, "stub")
        if stub_dir not in sys.path:
            sys.path.insert(0, stub_dir)
        stubtool = serialflash.tool("stubtool")
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "odr_%s.bin" % board.id)
        with open(path, "wb") as f:
            f.write(image)
        log("stub     %s via %s: %d bytes from 0x%X" % (board.name, port, len(image), board.app_base))
        return stubtool.main(argv(board.id, path, port, backup_dir))
