# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The radio's original firmware version, read over USB before an install, and the hardware option it settles.

Some radios come in board revisions RadioFoundry can't tell apart while it runs, but the firmware they shipped with
follows the revision. Such a board declares [flash.original_version] in its board.toml (docs/dev/board-registry.md,
"Original firmware version"): how the version is read (`read`), the DFU interface string that marks the original
firmware running (`interface`), the hardware option it settles (`hwopt`, the debug link's HWOPT,
docs/spec/debuglink.md 3.25) and the option's choice for each major version (`major`). The installer reads the version
before it asks for the bootloader and stores the choice over the debug link once RadioFoundry runs. A version not in
the table, or no answer, settles nothing: the radio then uses its own fallback.

Read "dfu-info" (docs/spec/hardware-md9600.md 15.1): the original firmware, running normally, is a DFU device with the
bootloader's USB IDs. Its interface string tells it from the bootloader without a request, so nothing is sent to a
device whose strings aren't the original firmware's. Then these requests, all on the DFU interface, none of which
changes anything on the radio:

    GETSTATUS; CLRSTATUS when the state is dfuERROR, ABORT when it is not dfuIDLE
    DNLOAD block 0 `A2 02`; GETSTATUS (dfuDNBUSY); GETSTATUS (dfuDNLOAD-IDLE); ABORT
    UPLOAD block 0, 1024 bytes: `56 0M m0 mm`, version Mm.mm (`56 00 60 09` = 06.09)

The bootloader answers the same command with `54 ...` ('T'): the radio is in its bootloader already. Any other answer,
a stall included: unknown.
"""
from . import dfu as dfumod

READS = ("dfu-info",)
ORIGINAL, BOOTLOADER = "original", "bootloader"

DFU_UPLOAD = 2
INFO_CMD = bytes([0xA2, 0x02])
UPLOAD_LEN = 1024
ST_DNBUSY, ST_DNIDLE = 4, 5


def parse_version(data):
    """The version text ("06.09") in a dfu-info answer, or None for anything else."""
    data = bytes(data or b"")
    if len(data) != 4 or data[0] != 0x56:
        return None
    nib = [data[1] >> 4, data[1] & 15, data[2] >> 4, data[2] & 15, data[3] >> 4, data[3] & 15]
    # 0M m0 mm: the major's tens in byte 1, its units in byte 2's high nibble, the minor in byte 3, all decimal
    if nib[0] or nib[3] or any(n > 9 for n in nib):
        return None
    return "%d%d.%d%d" % (nib[1], nib[2], nib[4], nib[5])


def answer_kind(data):
    """(kind, version) of a dfu-info answer: (ORIGINAL, "06.09"), (BOOTLOADER, None) for the bootloader's 'T' answer,
    (None, None) for anything else."""
    data = bytes(data or b"")
    v = parse_version(data)
    if v is not None:
        return ORIGINAL, v
    if len(data) == 4 and data[0] == 0x54:
        return BOOTLOADER, None
    return None, None


def _norm(s):
    return " ".join((s or "").split())


def runs_original(spec, interfaces):
    """Whether a DFU device's interface strings (fingerprint interfaces) are the original firmware's, running normally:
    one of them is spec["interface"] (spaces not counted). False for the bootloader's and for unreadable strings."""
    want = _norm(spec.get("interface")) if spec else ""
    return bool(want) and any(_norm(i) == want for i in interfaces or [])


def choice_for(spec, version):
    """The hardware option choice a version settles (spec: the board's original_version), or None."""
    if not spec or not version:
        return None
    return spec["major"].get(version.split(".")[0])


def read_dfu_info(dev, intf=0, timeout=2000, log=None):
    """(kind, version) from a device (dev: anything with pyusb's ctrl_transfer): (ORIGINAL, version) from the original
    firmware, (BOOTLOADER, None) from the bootloader, (None, None) for anything else. The caller checks the interface
    strings first (runs_original). Never raises for a device that doesn't answer the way that firmware does."""
    def out(req, data=None):
        dev.ctrl_transfer(0x21, req, 0, intf, data, timeout)

    def status():
        r = bytes(dev.ctrl_transfer(0xA1, dfumod.DFU_GETSTATUS, 0, intf, 6, timeout))
        if len(r) != 6:
            raise dfumod.DfuError("short GETSTATUS reply")
        return r[0], r[4]

    def idle():
        _, state = status()
        if state == dfumod.ST_ERROR:
            out(dfumod.DFU_CLRSTATUS)
        elif state != dfumod.ST_IDLE:
            out(dfumod.DFU_ABORT)

    try:
        idle()
        out(dfumod.DFU_DNLOAD, INFO_CMD)
        st1, s1 = status()
        st2, s2 = status()
        if (st1, s1, st2, s2) != (0, ST_DNBUSY, 0, ST_DNIDLE):
            if log:
                log("no version: the device answered states %d, %d" % (s1, s2))
            try:
                idle()
            except Exception:
                pass
            return None, None
        out(dfumod.DFU_ABORT)
        data = bytes(dev.ctrl_transfer(0xA1, DFU_UPLOAD, 0, intf, UPLOAD_LEN, timeout))
    except Exception as e:		# a stall: not the original firmware
        if log:
            log("no version: %s" % e)
        return None, None
    kind, v = answer_kind(data)
    if kind is None and log:
        log("no version: the answer is %s" % data[:16].hex())
    return kind, v


def hwopt_action(opts, name, choice):
    """What to do with a choice for the hardware option `name`, given the radio's HWOPT list (opts: [{name, value,
    choices}]). Returns (action, option index, choice index, the stored choice's label or None):
      store     nothing stored yet: store it
      same      the radio already holds this choice
      mismatch  the radio holds another choice: leave it, tell the user
      missing   the radio has no such option, or not this choice (another RadioFoundry version): nothing to do"""
    for i, o in enumerate(opts):
        if o["name"] != name:
            continue
        if choice not in o["choices"]:
            return "missing", i, None, None
        c = o["choices"].index(choice)
        v = o["value"]
        if v < 0:
            return "store", i, c, None
        if v >= len(o["choices"]):
            return "missing", i, c, None
        return ("same" if v == c else "mismatch"), i, c, o["choices"][v]
    return "missing", None, None, None
