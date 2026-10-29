# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""A vendor bootloader's update protocol over USB HID, taking an SGL update file (updatefile.py,
docs/spec/update-gd77.md). The radio is started in update mode (the board's keys held at power-on); it shows up as HID
device 15A2:0073. The host sends the file's names, erases,
and sends the body in short blocks with a running sum; every step gets a one-byte answer.

Unverified on a radio. FakeBootloader models the protocol as the spec reads it, for the host tests.
"""
import struct
import time

VID, PID = 0x15A2, 0x0073
REPORT = 42			# report id + 41 data bytes
ERASE_TIMEOUT = 10.0
STEP_TIMEOUT = 2.0


class BootError(Exception):
    pass


def frame(payload):
    if len(payload) > REPORT - 4:
        raise ValueError("payload longer than one report")
    return bytes([1, 0, len(payload) & 255, len(payload) >> 8]) + payload + b"\0" * (REPORT - 4 - len(payload))


def parse(report):
    if len(report) < 4:
        raise BootError("short report from the radio")
    n = report[2] | (report[3] << 8)
    return bytes(report[4:4 + n])


class HidLink:
    """The radio through hidapi (pip install hidapi). write(report) and read(timeout_s) -> bytes or None."""

    def __init__(self):
        try:
            import hid
        except ImportError:
            raise BootError("the USB HID update needs the hidapi package (pip install hidapi)")
        self.dev = hid.device()
        try:
            self.dev.open(VID, PID)
        except (OSError, ValueError):
            raise BootError("no radio in update mode found (USB 15A2:0073): switch it off, hold both side keys, "
                            "switch it on and connect the USB cable")

    def write(self, report):
        if self.dev.write(report) < 0:
            raise BootError("write to the radio failed")

    def read(self, timeout_s):
        r = self.dev.read(64, int(timeout_s * 1000))
        return bytes(r) if r else None

    def close(self):
        self.dev.close()


def present():
    """True when a radio in update mode is on the bus."""
    try:
        import hid
    except ImportError:
        return False
    return any(d["vendor_id"] == VID and d["product_id"] == PID for d in hid.enumerate())


class Updater:
    def __init__(self, link, sleep=time.sleep):
        self.link, self.sleep = link, sleep

    def xfer(self, payload, timeout=STEP_TIMEOUT):
        self.link.write(frame(payload))
        r = self.link.read(timeout)
        if r is None:
            raise BootError("the radio did not answer within %.0f s" % timeout)
        return parse(r)

    def step(self, payload, what, timeout=STEP_TIMEOUT):
        r = self.xfer(payload, timeout)
        if r[:1] != b"A":
            raise BootError("%s: the radio answered %r, not an acknowledgement" % (what, r))

    def flash(self, u, body=None, progress=lambda done, total: None):
        """Send the update file u (updatefile.UpdateFile) to the radio. body: the scrambled body to send (default u.body)."""
        body = u.body if body is None else body
        for _ in range(5):
            r = self.xfer(b"DOWNLOAD")
            if r[:8] in (b"#UPDATE?", b"#UPDATA?"):
                break
            self.sleep(0.2)
        else:
            raise BootError("the radio is not in update mode (it did not answer DOWNLOAD)")
        self.step(b"A", "start")
        if u.send_tag:
            r = self.xfer(u.tag)
            if r[:8] != u.tag:
                raise BootError("the radio did not echo the file's tag (%r)" % r)
        if u.use_fprog:
            self.sleep(0.005)
            self.step(b"F-PROG\xff\xff", "program mode")
        for part, what in ((u.company, "company"), (u.model, "model"), (u.version, "version")):
            self.sleep(0.005)
            self.step(part, "the file's %s was refused" % what)
        self.sleep(0.1)
        self.step(b"F-ERASE\xff" if u.use_fprog else b"ERASEROM", "erase", ERASE_TIMEOUT)
        self.sleep(0.005)
        self.step(b"A", "erase done")
        self.sleep(0.005)
        self.step(b"PROGRAM\xff", "program")
        total, bs = u.length, u.block_size
        addr, since, acc = u.start, 0, 0
        while addr < total:
            n = min(bs - addr % bs, total - addr)
            data = body[addr:addr + n]
            self.step(addr.to_bytes(u.addr_width, "big") + n.to_bytes(u.len_width, "big") + data, "block at 0x%X" % addr)
            acc = (acc + sum(data)) & 0xFFFFFFFF
            addr += n
            since += n
            progress(addr, total)
            if since % 1024 == 0 or addr >= total:
                self.step(b"END\xff" + struct.pack("<I", acc), "checksum after 0x%X" % addr)
                acc = 0


class FakeBootloader:
    """A model of the loader for the host tests: checks every step in order and the running sums, keeps what it received."""

    def __init__(self, u, reject=None):
        self.u, self.q, self.state, self.mem, self.sum, self.reject = u, [], "idle", {}, 0, reject

    def write(self, report):
        r = self.respond(parse(report))
        self.q.append(bytes([3, 0, len(r) & 255, len(r) >> 8]) + r + b"\0" * (REPORT - 4 - len(r)))

    def read(self, timeout_s):
        return self.q.pop(0) if self.q else None

    def respond(self, p):
        u, st = self.u, self.state
        if p == self.reject:
            return b"N"
        if st == "idle" and p == b"DOWNLOAD":
            self.state = "start"
            return b"#UPDATE?"
        order = [("start", b"A", "tag" if u.send_tag else "fprog"), ("tag", u.tag, "fprog"),
                 ("fprog", b"F-PROG\xff\xff", "company"), ("company", u.company, "model"), ("model", u.model, "version"),
                 ("version", u.version, "erase"), ("erase", b"F-ERASE\xff", "erased"), ("erased", b"A", "program"),
                 ("program", b"PROGRAM\xff", "data")]
        for s, want, nxt in order:
            if st == s and p == want:
                self.state = nxt
                return p if s == "tag" else b"A"
        if st == "data":
            if p[:4] == b"END\xff":
                if struct.unpack("<I", p[4:8])[0] != self.sum:
                    return b"N"
                self.sum = 0
                return b"A"
            aw, lw = u.addr_width, u.len_width
            addr, n = int.from_bytes(p[:aw], "big"), int.from_bytes(p[aw:aw + lw], "big")
            data = p[aw + lw:]
            if len(data) != n or addr % u.block_size + n > u.block_size:
                return b"N"
            self.mem[addr] = data
            self.sum = (self.sum + sum(data)) & 0xFFFFFFFF
            return b"A"
        return b"N"

    def received(self):
        out = bytearray(self.u.length)
        for a, d in self.mem.items():
            out[a:a + len(d)] = d
        return bytes(out)
