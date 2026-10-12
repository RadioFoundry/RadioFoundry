# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The AnyRoad (TYT/Baofeng STM32) DFU bootloader: finding it and running a sector plan.

Protocol from docs/spec/hardware-dm1701.md 15.1-15.3 (the same steps tools/stm32_dfu.py drives), with one change:
only the sectors in the plan are erased and written, instead of every application sector. Each block is XORed with
the 1024-byte model key; every sector starts a multiple of 1024 bytes after 0x0800C000, so every block starts at key
index 0 whichever sectors are skipped.
"""
import hashlib
import os
import struct
import sys
import time

from .plan import BLOCK, FIRST_BLOCK

KEY_LEN = 1024
EXIT_ADDR = 0x08000000

DFU_DNLOAD, DFU_GETSTATUS, DFU_CLRSTATUS, DFU_ABORT = 1, 3, 4, 6
ST_IDLE, ST_DNBUSY, ST_DNIDLE, ST_ERROR = 2, 4, 5, 10
STATE_NAMES = {0: "appIDLE", 1: "appDETACH", 2: "dfuIDLE", 3: "dfuDNLOAD-SYNC", 4: "dfuDNBUSY",
               5: "dfuDNLOAD-IDLE", 6: "dfuMANIFEST-SYNC", 7: "dfuMANIFEST", 8: "dfuMANIFEST-WAIT-RESET",
               9: "dfuUPLOAD-IDLE", 10: "dfuERROR"}


class DfuError(Exception):
    pass


# ---------------------------------------------------------------------------------------------------------------
# key

def check_key(key, board):
    if len(key) != KEY_LEN:
        raise DfuError("the key is %d bytes, expected %d" % (len(key), KEY_LEN))
    got = hashlib.sha256(key).hexdigest()
    if got != board.key_sha256:
        raise DfuError("this is not the %s bootloader key (SHA-256 %s, expected %s...)"
                       % (board.name, got, board.key_sha256[:16]))
    return key


def load_key(path, board):
    with open(os.path.expanduser(path), "rb") as f:
        return check_key(f.read(), board)


def xor_block(block, key):
    return bytes(a ^ b for a, b in zip(block, key))


# ---------------------------------------------------------------------------------------------------------------
# protocol, over anything with a pyusb-like ctrl_transfer()

class Dfu:
    def __init__(self, dev, intf=0, log=print, sleep=time.sleep):
        self.dev = dev
        self.intf = intf
        self.log = log
        self.sleep = sleep

    def _out(self, req, value, data):
        return self.dev.ctrl_transfer(0x21, req, value, self.intf, data, 5000)

    def get_status(self):
        r = bytes(self.dev.ctrl_transfer(0xA1, DFU_GETSTATUS, 0, self.intf, 6, 5000))
        if len(r) != 6:
            raise DfuError("short GETSTATUS reply (%d bytes)" % len(r))
        return r[0], r[1] | (r[2] << 8) | (r[3] << 16), r[4]

    def wait_idle(self):
        state = status = None
        for attempt in range(20):
            status, _, state = self.get_status()
            if state == ST_IDLE:
                return
            self._out(DFU_CLRSTATUS if attempt < 10 else DFU_ABORT, 0, None)
        raise DfuError("the bootloader stays in state %s (status %s), not dfuIDLE: power-cycle the radio into the "
                       "bootloader and try again" % (STATE_NAMES.get(state, state), status))

    def wait_dnload_done(self, what):
        for _ in range(4000):
            status, poll_ms, state = self.get_status()
            if state == ST_ERROR or status != 0:
                raise DfuError("%s failed: status %d, state %s" % (what, status, STATE_NAMES.get(state, state)))
            if state == ST_DNIDLE:
                return
            if state != ST_DNBUSY:
                raise DfuError("%s: unexpected state %s" % (what, STATE_NAMES.get(state, state)))
            self.sleep(max(poll_ms, 1) / 1000.0)
        raise DfuError("%s: the bootloader stayed busy" % what)

    def vendor(self, b0, b1):
        self._out(DFU_DNLOAD, 0, bytes([b0, b1]))
        self.get_status()
        self.sleep(0.1)
        self.get_status()
        self.wait_idle()

    def command(self, cmd, addr, what):
        self._out(DFU_DNLOAD, 0, bytes([cmd]) + struct.pack("<I", addr))
        self.wait_dnload_done(what)

    def run(self, plan, key, progress=None):
        """Erase the plan's sectors (in the bootloader's order), write each, exit (the radio resets)."""
        base = plan.board.app_base
        for st in plan.writes:
            if (st.sector.addr - base) % KEY_LEN:
                raise DfuError("sector 0x%08X is not key-aligned" % st.sector.addr)
        self.wait_idle()
        self.vendor(0x91, 0x01)
        self.vendor(0x91, 0x31)
        for s in plan.erase:
            self.log("erase sector %d at 0x%08X" % (s.index, s.addr))
            self.command(0x41, s.addr, "erase 0x%08X" % s.addr)
        total, done = plan.total_blocks, 0
        for st in plan.writes:
            addr = st.sector.addr
            self.command(0x21, addr, "set address 0x%08X" % addr)
            for num, blk in st.blocks:
                if len(blk) != BLOCK:
                    raise DfuError("block of %d bytes" % len(blk))
                self._out(DFU_DNLOAD, num, xor_block(blk, key))
                self.wait_dnload_done("block %d at 0x%08X" % (num, addr + (num - FIRST_BLOCK) * BLOCK))
                done += 1
                if progress:
                    progress(done, total)
        self.command(0x21, EXIT_ADDR, "set address 0x%08X" % EXIT_ADDR)
        self._out(DFU_DNLOAD, 0, None)
        try:
            self.get_status()	# the radio resets; it may not answer
        except Exception:
            pass


# ---------------------------------------------------------------------------------------------------------------
# USB

def import_usb():
    try:
        import usb.core
        import usb.util
    except ImportError:
        raise DfuError("pyusb is not installed: pip install pyusb libusb-package (or use the odrflash executable)")
    return usb


def backend():
    """libusb from the libusb-package wheel when present (the single-file executables bundle it), else pyusb's."""
    try:
        import libusb_package
        return libusb_package.get_libusb1_backend()
    except Exception:
        return None


def permission_hint():
    if sys.platform == "darwin":
        return ("macOS: close other programs that use the radio (a CPS in a virtual machine with USB passthrough), "
                "unplug and replug the cable")
    if sys.platform.startswith("linux"):
        return ("Linux: allow your user to open the bootloader, then replug the radio:\n"
                "  echo 'SUBSYSTEM==\"usb\", ATTRS{idVendor}==\"0483\", ATTRS{idProduct}==\"df11\", MODE=\"0666\"' "
                "| sudo tee /etc/udev/rules.d/50-odr-dfu.rules\n"
                "  sudo udevadm control --reload-rules && sudo udevadm trigger")
    return WINDOWS_DRIVER_STEPS


# Shown on Windows when the bootloader can't be opened. Same steps as docs/user/02-installing.md and the web flasher.
WINDOWS_DRIVER_STEPS = (
    "Windows: the bootloader needs the WinUSB driver, installed once with Zadig:\n"
    "  1. Download Zadig from https://zadig.akeo.ie and run it (it needs no install).\n"
    "  2. Put the radio in its bootloader and plug it in.\n"
    "  3. In Zadig's list pick \"Digital Radio in USB mode\" (USB ID 0483 DF11). Not listed: Options > List All Devices.\n"
    "  4. Choose WinUSB as the driver and click Install Driver (or Replace Driver). Wait until it says it succeeded.\n"
    "  5. Unplug the radio, put it back in its bootloader and run odrflash again.")


def find(usb, board):
    vid, pid = board.dfu_ids
    try:
        return list(usb.core.find(find_all=True, idVendor=vid, idProduct=pid, backend=backend()))
    except usb.core.NoBackendError:
        raise DfuError("no libusb found: macOS 'brew install libusb', Linux 'apt install libusb-1.0-0', or "
                       "pip install libusb-package")


def _string(usb, dev, index):
    if not index:
        return None
    try:
        return usb.util.get_string(dev, index)
    except (usb.core.USBError, ValueError) as e:
        if getattr(e, "errno", None) == 13:
            raise DfuError("access denied reading the bootloader's strings\n" + permission_hint())
        return None


def fingerprint(usb, dev):
    """The strings a bootloader shows: string 1, manufacturer, product, serial, DFU interface names."""
    fp = {"string1": _string(usb, dev, 1), "manufacturer": _string(usb, dev, dev.iManufacturer),
          "product": _string(usb, dev, dev.iProduct), "serial": _string(usb, dev, dev.iSerialNumber),
          "interfaces": []}
    for cfg in dev:
        for intf in cfg:
            if intf.bInterfaceClass == 0xFE and intf.bInterfaceSubClass == 0x01:
                fp["interfaces"].append(_string(usb, dev, intf.iInterface))
    return fp


def open_device(usb, dev):
    intf = None
    for cfg in dev:
        for i in cfg:
            if i.bInterfaceClass == 0xFE and i.bInterfaceSubClass == 0x01:
                intf = i.bInterfaceNumber
                break
    if intf is None:
        raise DfuError("the device has no DFU interface")
    try:
        try:
            if dev.is_kernel_driver_active(intf):
                dev.detach_kernel_driver(intf)
        except (NotImplementedError, usb.core.USBError):
            pass
        dev.set_configuration()
        usb.util.claim_interface(dev, intf)
    except usb.core.USBError as e:
        if e.errno == 13 or "Access denied" in str(e):
            raise DfuError("access denied opening the bootloader\n" + permission_hint())
        if e.errno == 16 or "busy" in str(e).lower():
            raise DfuError("the bootloader is busy (another program has it open)\n" + permission_hint())
        raise DfuError("cannot open the bootloader: %s\n%s" % (e, permission_hint()))
    return intf


def release(usb, dev):
    try:
        usb.util.dispose_resources(dev)
    except Exception:
        pass
