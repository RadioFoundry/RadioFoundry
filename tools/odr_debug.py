#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""odr_debug: talk to a radio's USB debug link (docs/spec/debuglink.md). Command line tool and Python module
(tests/hil uses the DebugLink class).

    odr_debug.py [--port DEV] ping
    odr_debug.py version
    odr_debug.py diag [--watch N] [--raw]       radio summary + voice path + board diagnostics lines, with rates
    odr_debug.py view [--section NAME] [--json] the radio state as key=value (all sections)
    odr_debug.py reset                          zero the diagnostics counters
    odr_debug.py screenshot -o FILE.png [--scale N] [--crop] [--tries N]
    odr_debug.py key NAME [--long | --hold MS | --repeat N | --down | --up | --steps N]
    odr_debug.py key --mask 0xBITS --hold MS    physical key bits
    odr_debug.py wake                           backlight on without a key event
    odr_debug.py ptt --seconds S                key PTT for S seconds (at most 10; the firmware also times out)
    odr_debug.py radio OP [ARG...]              channel +1, pos 3, zone 0, vfo on, freq 162.550, power +1, ...
    odr_debug.py log [--follow]                 the firmware's dlog() ring
    odr_debug.py chip                           chip ID, flash size, UID, option bytes (RDP, write protection)
    odr_debug.py dump --addr A --len N [-o F]   read internal flash / system memory / OTP / option bytes to a file
                                                outside the git tree (default the external reference folder)
    odr_debug.py reboot [--rom-dfu]             reset; --rom-dfu: into ST's ROM USB DFU (RDP level 0 only)

Key names: up down left right select (ok) back (exit) quick (star) context (hash) 0-9 ptt monitor tt (talk target)
emergency enc user1-user4.

The port is found by its USB ID 1FC9:0094 (the only one present); --port or $ODR_PORT picks one. Needs pyserial
(pip install pyserial). Exit status 0 on success, 1 on a radio error, 2 on a usage or connection problem.
"""
import argparse
import json
import os
import struct
import sys
import time
import zlib

try:
    import serial
    from serial.tools import list_ports
except ImportError:          # the module is still importable for its pure helpers (PNG, parsing)
    serial = None
    list_ports = None

VID, PID = 0x1FC9, 0x0094
SOF1, SOF2 = 0xA5, 0x5A
HDR = 6
REQ_MAX = 64
INJECT_MAX_S = 10.0

PING, VERSION, GET_DIAG, RESET_COUNTERS, SCREENSHOT, KEY, PTT, GET_LOG, GET_VIEW, RADIO, INFO_CHIP, READ_MEM, \
    REBOOT = range(1, 14)
DMR_DATA = 0x0E
DATA_OP_SMS, DATA_OP_CALL_ALERT, DATA_OP_RADIO_CHECK, DATA_OP_SETTINGS, DATA_OP_INBOX, DATA_OP_MARK_READ = range(1, 7)
SMS_TEXT_MAX = REQ_MAX - 6          # op, u32 destination, flags
CMD_ERROR = 0xFF
STATUS = {0: "ok", 1: "unknown command", 2: "bad argument", 3: "busy", 4: "unavailable", 5: "bad CRC",
          6: "request too long", 7: "refused"}
ST_REFUSED = 7
MEM_READ_MAX = 1000
REBOOT_NORMAL, REBOOT_ROM_DFU = 0, 1
REBOOT_REASONS = {1: "wrong confirmation token (not the chip UID's low word)",
                  2: "RDP is not level 0: the ROM bootloader would refuse flash access and its unprotect mass-erases "
                     "everything, the stock bootloader included",
                  3: "transmitting", 4: "chip info unreadable", 5: "could not arm the ROM DFU flag"}
# STM32F405 sectors: (start, size)
F4_SECTORS = [(0x08000000 + i * 0x4000, 0x4000) for i in range(4)] + [(0x08010000, 0x10000)] + \
    [(0x08020000 + i * 0x20000, 0x20000) for i in range(7)]
MEM_WHITELIST = [(0x08000000, 0x08100000), (0x1FFF0000, 0x1FFF7800), (0x1FFF7800, 0x1FFF7A10),
                 (0x1FFFC000, 0x1FFFC010)]
DEFAULT_DUMP_DIR = os.path.expanduser("the external reference folder")
ST_OK, ST_BUSY = 0, 3

ACTIONS = {"up": 1, "down": 2, "left": 3, "right": 4, "select": 5, "ok": 5, "green": 5, "back": 6, "exit": 6,
           "red": 6, "quick": 7, "star": 7, "context": 8, "hash": 8, "ptt": 19, "monitor": 20, "tt": 21,
           "talk_target": 21, "emergency": 22, "enc": 23, "user1": 24, "user2": 25, "user3": 26, "user4": 27}
ACTIONS.update({str(d): 9 + d for d in range(10)})
PRESS, LONG, REPEAT, DOWN, UP = range(5)

VIEW_SECTIONS = ["core", "channel", "dmr", "dmr_call", "dmr_stats", "hotspot", "link", "features", "dmr_data"]

OP_CHANNEL_STEP, OP_ZONE_POS, OP_ZONE, OP_VFO, OP_VFO_SET, OP_VFO_STEP, OP_POWER, OP_SQUELCH, OP_BANDWIDTH, \
    OP_TONE, OP_MONITOR, OP_CLEAR_TEMP, OP_REPLY_CANCEL, OP_HOTSPOT_EXIT, OP_DMR_TARGET, OP_DMR_CONTACT, \
    OP_DMR_CALL_BACK, OP_DMR_TARGET_CLEAR, OP_DMR_PRIVACY = range(1, 20)
PRIVACY_TYPES = {"off": 0, "moto": 1, "anytone": 2}
TONE_TYPES = {"none": 0, "ctcss": 1, "dcs": 2, "dcs_inv": 3}
# The radio features (docs/spec/radio-features.md, debuglink.md 3.8)
OP_SCAN, OP_SCAN_VFO, OP_BEEP, OP_TONE_BURST, OP_SETTING = range(20, 25)
SCAN_CMDS = {"stop": 0, "zone": 1, "vfo": 2, "dual": 3, "cancel": 4, "nuisance": 5, "toggle": 6}
BEEPS = {"key": 1, "error": 2, "tx_permit": 3, "tx_deny": 4, "call_end": 5, "tot_warn": 6, "battery_low": 7,
         "battery_crit": 8, "scan_hold": 9}
SETTINGS = {"tot_s": 1, "tot_warn_s": 2, "bcl": 3, "roger_beep": 4, "tx_inhibit": 5, "scan_resume": 6,
            "scan_hold_s": 7, "scan_delay_ms": 8, "scan_dwell_ms": 9, "scan_priority": 10, "beep_level": 11,
            "key_beep": 12, "vox": 13, "vox_level": 14, "vox_tail_ms": 15, "rx_save": 16, "rx_save_idle_s": 17,
            "battery_off_s": 18, "scan_beep": 19}


class DebugLinkError(Exception):
    """The radio answered with an error status, or didn't answer."""

    def __init__(self, msg, status=None, data=b""):
        super().__init__(msg)
        self.status = status
        self.data = data


def crc16(data, crc=0xFFFF):
    """CRC-16/CCITT-FALSE."""
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def frame(cmd, seq, payload=b""):
    body = bytes([cmd, seq]) + struct.pack("<H", len(payload)) + bytes(payload)
    return bytes([SOF1, SOF2]) + body + struct.pack("<H", crc16(body))


def parse_kv(text):
    """key=value lines into a dict (values as strings; see as_int)."""
    out = {}
    for line in text.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v
    return out


def as_int(v, default=None):
    try:
        return int(v, 0)
    except (TypeError, ValueError):
        return default


def find_ports():
    if list_ports is None:
        return []
    return sorted(p.device for p in list_ports.comports() if p.vid == VID and p.pid == PID)


def pick_port(port=None):
    port = port or os.environ.get("ODR_PORT")
    if port:
        return port
    ports = find_ports()
    if len(ports) == 1:
        return ports[0]
    if not ports:
        raise DebugLinkError("no radio found (USB 1FC9:0094): is it plugged in and powered on? Or give --port")
    raise DebugLinkError(f"several radios found {ports}: give --port")


class DebugLink:
    """One open debug link. All methods raise DebugLinkError on an error status or a time-out."""

    def __init__(self, port=None, timeout=1.0):
        if serial is None:
            raise DebugLinkError("needs pyserial: pip install pyserial")
        self.port = pick_port(port)
        self.timeout = timeout
        self.seq = 0
        try:
            self.s = serial.Serial(self.port, 115200, timeout=0.02)
        except serial.SerialException as e:
            raise DebugLinkError(f"can't open {self.port}: {e}")
        self.buf = bytearray()

    def close(self):
        self.s.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # ── transport ──
    def _read_frame(self, deadline):
        while time.monotonic() < deadline:
            self.buf += self.s.read(max(1, self.s.in_waiting or 1))
            while True:
                i = self.buf.find(bytes([SOF1, SOF2]))
                if i < 0:
                    del self.buf[:-1]       # other protocols' bytes (MMDVM frames heard on air) are skipped
                    break
                del self.buf[:i]
                if len(self.buf) < HDR:
                    break
                n = self.buf[4] | (self.buf[5] << 8)
                if n > 1024:                # can't be ours: resync past this SOF
                    del self.buf[:1]
                    continue
                if len(self.buf) < HDR + n + 2:
                    break
                f = bytes(self.buf[:HDR + n + 2])
                body = f[2:HDR + n]
                if crc16(body) != (f[HDR + n] | (f[HDR + n + 1] << 8)):
                    del self.buf[:1]
                    continue
                del self.buf[:HDR + n + 2]
                return f[2], f[3], f[HDR:HDR + n]
        return None

    def request(self, cmd, payload=b"", timeout=None, busy_wait=2.0):
        """(status, data) of a command; retries a busy answer for up to busy_wait seconds. Raises on errors other
        than busy after the wait."""
        if len(payload) > REQ_MAX:
            raise DebugLinkError(f"request payload {len(payload)} > {REQ_MAX}")
        timeout = self.timeout if timeout is None else timeout
        end_busy = time.monotonic() + busy_wait
        while True:
            self.seq = (self.seq + 1) & 0xFF
            seq = self.seq
            self.s.write(frame(cmd, seq, payload))
            deadline = time.monotonic() + timeout
            while True:
                r = self._read_frame(deadline)
                if r is None:
                    raise DebugLinkError(f"no answer to command 0x{cmd:02X} within {timeout} s on {self.port} "
                                         "(firmware without the debug link? another program has the port?)")
                rcmd, rseq, data = r
                if rseq != seq:
                    continue                # a late answer to an earlier request
                if rcmd == CMD_ERROR:
                    st = data[0] if data else None
                    raise DebugLinkError(f"command 0x{cmd:02X}: {STATUS.get(st, st)}", st)
                if rcmd == (cmd | 0x80):
                    break
            st = data[0] if data else None
            if st == ST_BUSY and time.monotonic() < end_busy:
                time.sleep(0.03)
                continue
            if st != ST_OK:
                raise DebugLinkError(f"command 0x{cmd:02X}: {STATUS.get(st, st)}", st, bytes(data[1:]))
            return st, data[1:]

    # ── commands ──
    def ping(self, data=b"odr"):
        t = time.monotonic()
        _, r = self.request(PING, data)
        if r != data:
            raise DebugLinkError(f"ping echo mismatch: {r!r}")
        return time.monotonic() - t

    def chip(self):
        """INFO_CHIP decoded (dict)."""
        return decode_chip(self.request(INFO_CHIP)[1])

    def read_mem(self, addr, n):
        _, r = self.request(READ_MEM, struct.pack("<IH", addr, n))
        (raddr,) = struct.unpack_from("<I", r)
        if raddr != addr or len(r) - 4 != n:
            raise DebugLinkError(f"READ_MEM answer for 0x{raddr:08X} with {len(r) - 4} bytes, asked 0x{addr:08X} {n}")
        return bytes(r[4:])

    def dump(self, addr, n, progress=None):
        out = bytearray()
        while len(out) < n:
            k = min(MEM_READ_MAX, n - len(out))
            out += self.read_mem(addr + len(out), k)
            if progress:
                progress(len(out), n)
        return bytes(out)

    def reboot(self, mode, token):
        """Ask for a reboot; raises DebugLinkError (status 7, data = reason) when the firmware refuses."""
        self.request(REBOOT, struct.pack("<BI", mode, token))

    def version(self):
        return parse_kv(self.request(VERSION)[1].decode("ascii", "replace"))

    def diag(self):
        """(firmware ms, [lines])"""
        lines, first, ms = [], 0, None
        while True:
            _, r = self.request(GET_DIAG, bytes([first]))
            t, nxt = struct.unpack_from("<IB", r)
            ms = t if ms is None else ms
            text = r[5:].decode("ascii", "replace")
            lines += text.split("\n")[:-1] if text else []
            if nxt == 0xFF or nxt <= first:
                return ms, lines
            first = nxt

    def reset(self):
        """Zero the counters; returns the firmware ms at the reset."""
        return struct.unpack_from("<I", self.request(RESET_COUNTERS)[1])[0]

    def view_section(self, section):
        """(firmware ms, next section, dict)"""
        idx = VIEW_SECTIONS.index(section) if isinstance(section, str) else section
        _, r = self.request(GET_VIEW, bytes([idx]))
        ms, nxt = struct.unpack_from("<IB", r)
        return ms, nxt, parse_kv(r[5:].decode("ascii", "replace"))

    def view(self, sections=None):
        """All sections (or the named ones) merged into one dict; 'section' keys dropped, 'ms' added."""
        out = {}
        idxs = range(len(VIEW_SECTIONS)) if sections is None else [VIEW_SECTIONS.index(s) for s in sections]
        for i in idxs:
            ms, _, kv = self.view_section(i)
            kv.pop("section", None)
            out.update(kv)
            out.setdefault("ms", ms)
        return out

    def screenshot_raw(self):
        """(info dict, bytes) of one pass over the frame buffer (may tear, see the spec)"""
        _, r = self.request(SCREENSHOT)
        w, h, bpp, fmt, it, ib, il, ir, total, _ = struct.unpack_from("<HHBBBBBBII", r)
        info = {"width": w, "height": h, "bpp": bpp, "format": fmt, "insets": (it, ib, il, ir), "total": total}
        data = bytearray()
        while len(data) < total:
            _, r = self.request(SCREENSHOT, struct.pack("<IH", len(data), 1000))
            off = struct.unpack_from("<I", r, 14)[0]     # r: the payload after the status byte
            if off != len(data):
                raise DebugLinkError(f"screenshot chunk at {off}, expected {len(data)}")
            chunk = r[18:]
            if not chunk:
                raise DebugLinkError("screenshot: empty chunk")
            data += chunk
        return info, bytes(data)

    def screenshot(self, tries=4):
        """(info, bytes, stable): reads until two passes agree (stable False if they never did)."""
        info, prev = self.screenshot_raw()
        for _ in range(max(1, tries) - 1):
            info, cur = self.screenshot_raw()
            if cur == prev:
                return info, cur, True
            prev = cur
        return info, prev, tries <= 1

    def key(self, name, gesture=PRESS, value=0):
        a = ACTIONS[name] if isinstance(name, str) else name
        self.request(KEY, struct.pack("<BBBb", 0, a, gesture, value))

    def key_hold(self, name, ms):
        a = ACTIONS[name] if isinstance(name, str) else name
        self.request(KEY, struct.pack("<BBH", 3, a, int(ms)))

    def keys_mask(self, mask, ms):
        self.request(KEY, struct.pack("<BIH", 1, mask, int(ms)))

    def wake(self):
        self.request(KEY, bytes([2]))

    def wait_keys_idle(self, timeout=12.0):
        """Wait until no injected key is held (link section input.injected == 0)."""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if as_int(self.view(["link"]).get("input.injected"), 0) == 0:
                return
            time.sleep(0.05)
        raise DebugLinkError("injected keys still held")

    def ptt(self, down, max_ms=10000):
        _, r = self.request(PTT, struct.pack("<BH", 1 if down else 0, int(max_ms)))
        return struct.unpack_from("<H", r)[0]

    def ptt_for(self, seconds):
        """Key PTT for seconds (<= 10), then release. The firmware's time-out is set just past the hold, so a lost
        host can't leave it keyed; the release is sent in any case."""
        if seconds <= 0 or seconds > INJECT_MAX_S:
            raise DebugLinkError(f"PTT hold {seconds} s refused: 0 < S <= {INJECT_MAX_S:g}")
        timeout_ms = min(int(seconds * 1000) + 300, 10000)
        try:
            self.ptt(True, timeout_ms)
            time.sleep(seconds)
        finally:
            self.ptt(False)

    def log(self):
        """(dropped lines, text) of everything in the ring"""
        text, dropped = "", 0
        while True:
            _, r = self.request(GET_LOG)
            d, more = struct.unpack_from("<IB", r)
            dropped += d
            text += r[5:].decode("ascii", "replace")
            if not more:
                return dropped, text

    def radio(self, op, args=b""):
        self.request(RADIO, bytes([op]) + bytes(args))

    # radio request helpers
    def channel_step(self, d):
        self.radio(OP_CHANNEL_STEP, struct.pack("<b", d))

    def zone_pos(self, pos):
        self.radio(OP_ZONE_POS, struct.pack("<H", pos))

    def zone(self, index):
        self.radio(OP_ZONE, struct.pack("<H", index))

    def vfo(self, on):
        self.radio(OP_VFO, bytes([1 if on else 0]))

    def vfo_set(self, hz):
        self.radio(OP_VFO_SET, struct.pack("<I", int(hz)))

    def vfo_step(self, d):
        self.radio(OP_VFO_STEP, struct.pack("<b", d))

    def power_step(self, d):
        self.radio(OP_POWER, struct.pack("<b", d))

    def squelch_step(self, d):
        self.radio(OP_SQUELCH, struct.pack("<b", d))

    def bandwidth(self, wide):
        self.radio(OP_BANDWIDTH, bytes([1 if wide else 0]))

    def tone(self, which, kind, value):
        self.radio(OP_TONE, struct.pack("<BBH", which, TONE_TYPES[kind] if isinstance(kind, str) else kind, value))

    def monitor(self, on):
        self.radio(OP_MONITOR, bytes([1 if on else 0]))

    def clear_temporary(self):
        self.radio(OP_CLEAR_TEMP)

    def reply_cancel(self):
        self.radio(OP_REPLY_CANCEL)

    def hotspot_exit(self):
        self.radio(OP_HOTSPOT_EXIT)

    def dmr_target(self, dmr_id, private=False):
        """Talk to a typed ID / talkgroup until the channel changes."""
        self.radio(OP_DMR_TARGET, struct.pack("<I", (int(dmr_id) & 0xFFFFFF) | (1 << 24 if private else 0)))

    def dmr_contact(self, index):
        self.radio(OP_DMR_CONTACT, struct.pack("<H", index))

    def dmr_call_back(self):
        self.radio(OP_DMR_CALL_BACK)

    def dmr_target_clear(self):
        self.radio(OP_DMR_TARGET_CLEAR)

    def dmr_privacy(self, kind, key=0):
        """Load a basic privacy key in RAM: kind off | moto (key 1-255) | anytone (key 0-65535)."""
        self.radio(OP_DMR_PRIVACY, struct.pack("<BH", PRIVACY_TYPES[kind] if isinstance(kind, str) else kind, key))

    def scan(self, cmd, pos=None):
        """Scan: stop | zone | vfo | dual (pos = the second channel) | cancel | nuisance | toggle."""
        c = SCAN_CMDS[cmd] if isinstance(cmd, str) else cmd
        self.radio(OP_SCAN, bytes([c]) + (struct.pack("<H", pos) if c == SCAN_CMDS["dual"] else b""))

    def scan_vfo(self, lo_hz, hi_hz):
        """Band scan of the VFO from lo to hi in its step."""
        self.radio(OP_SCAN_VFO, struct.pack("<II", int(lo_hz), int(hi_hz)))

    def beep(self, which):
        self.radio(OP_BEEP, bytes([BEEPS[which] if isinstance(which, str) else which]))

    def tone_burst(self, on):
        """The 1750 Hz burst (FM): keys up and sends the tone while on."""
        self.radio(OP_TONE_BURST, bytes([1 if on else 0]))

    def setting(self, name, value):
        """One radio setting in RAM (not stored): a SETTINGS name or id, value u16."""
        self.radio(OP_SETTING, struct.pack("<BH", SETTINGS[name] if isinstance(name, str) else name, int(value)))

    # DMR data service (docs/spec/debuglink.md 3.12): requests go through the service's mailbox, so a busy answer
    # means one of the kind is still queued (request() retries it for 2 s). Results: the dmr_data view section
    def data_request(self, op, args=b""):
        return self.request(DMR_DATA, bytes([op]) + bytes(args))[1]

    def sms(self, dst, text, group=False, ack=True):
        """Send a text message (Motorola TMS); text is Latin-1, at most SMS_TEXT_MAX characters here."""
        t = text.encode("latin-1", "replace")[:SMS_TEXT_MAX]
        self.data_request(DATA_OP_SMS, struct.pack("<IB", int(dst) & 0xFFFFFF, (1 if group else 0) |
                                                   (2 if ack else 0)) + t)

    def call_alert(self, dst):
        self.data_request(DATA_OP_CALL_ALERT, struct.pack("<I", int(dst) & 0xFFFFFF))

    def radio_check(self, dst):
        self.data_request(DATA_OP_RADIO_CHECK, struct.pack("<I", int(dst) & 0xFFFFFF))

    def data_settings(self, ars_server=0, radio_check_reply=False, call_alert_ack=True, lrrp_reply=True):
        """The data service settings in RAM (not saved): ARS server ID (0 off) and the answer switches."""
        flags = (1 if radio_check_reply else 0) | (2 if call_alert_ack else 0) | (4 if lrrp_reply else 0)
        self.data_request(DATA_OP_SETTINGS, struct.pack("<IB", int(ars_server) & 0xFFFFFF, flags))

    def inbox(self, index=0):
        """Inbox message index (0 newest) as a dict, None when there is none."""
        try:
            r = self.data_request(DATA_OP_INBOX, bytes([index]))
        except DebugLinkError as e:
            if e.status == 4:
                return None
            raise
        src, dst, group, unread = struct.unpack_from("<IIBB", r)
        return {"src": src, "dst": dst, "group": bool(group), "unread": bool(unread),
                "text": r[10:].decode("latin-1")}

    def mark_read(self, index=0):
        self.data_request(DATA_OP_MARK_READ, bytes([index]))


# ── pictures ────────────────────────────────────────────────────────────────

def to_rgb(info, data, crop=False):
    """(width, height, RGB888 rows as bytes) from the frame buffer"""
    w, h = info["width"], info["height"]
    rows = []
    if info["format"] == 1:
        for y in range(h):
            row = bytearray()
            for x in range(w):
                o = (y * w + x) * 2
                if o + 1 >= len(data):
                    row += b"\0\0\0"
                    continue
                v = data[o] | (data[o + 1] << 8)
                r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
                row += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
            rows.append(bytes(row))
    else:
        stride = (w + 7) // 8
        for y in range(h):
            row = bytearray()
            for x in range(w):
                o = y * stride + x // 8
                on = o < len(data) and data[o] & (0x80 >> (x & 7))
                row += b"\0\0\0" if on else b"\xff\xff\xff"
            rows.append(bytes(row))
    if crop:
        t, b, lft, rgt = info["insets"]
        rows = [r[lft * 3:(w - rgt) * 3] for r in rows[t:h - b]]
        w, h = w - lft - rgt, h - t - b
    return w, h, rows


def write_png(path, w, h, rows, scale=1):
    """RGB888 rows to a PNG with only the standard library"""
    if scale > 1:
        big = []
        for r in rows:
            px = b"".join(r[i:i + 3] * scale for i in range(0, len(r), 3))
            big += [px] * scale
        rows, w, h = big, w * scale, h * scale
    raw = b"".join(b"\0" + r for r in rows)

    def chunk(kind, payload):
        c = kind + payload
        return struct.pack(">I", len(payload)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def parse_counters(lines):
    """Counters from the diagnostics lines as a flat dict: 'k=v' tokens (prefixed by the line's first word, e.g.
    call.rx, voice.play), the DM-1701 board tokens (V H T I C D crc rc, as modem.voice etc.) and its audio line
    (audio.blk, audio.ov, audio.dr)."""
    import re
    out = {}
    board = {"V": "modem.voice", "H": "modem.headers", "T": "modem.terminators", "I": "modem.idle",
             "C": "modem.csbk", "D": "modem.data", "crc": "modem.crc", "rc": "modem.rc", "tk": "modem.ticks",
             "si": "modem.sys_ints"}
    for line in lines:
        words = line.split()
        if not words:
            continue
        if words[0] == "aud":           # "aud blk N ov N dr N": the DM-1701 DMR audio counters
            for k, v in zip(words[1::2], words[2::2]):
                if as_int(v) is not None:
                    out[f"audio.{k}"] = as_int(v)
            continue
        if any("=" in w for w in words[1:]):
            for w in words[1:]:
                if "=" in w:
                    k, v = w.split("=", 1)
                    if as_int(v) is not None:
                        out[f"{words[0]}.{k}"] = as_int(v)
            continue
        for w in words:
            m = re.fullmatch(r"([A-Za-z]+)(\d+)", w)
            if m and m.group(1) in board:
                out.setdefault(board[m.group(1)], int(m.group(2)))
    return out


# ── command line ────────────────────────────────────────────────────────────

def cmd_diag(link, a):
    prev = None
    while True:
        ms, lines = link.diag()
        if a.raw:
            print("\n".join(lines))
        else:
            print(f"--- {link.port}  t={ms / 1000:.3f} s")
            for ln in lines:
                print("  " + ln)
            cur = parse_counters(lines)
            if prev is not None:
                dt = (ms - prev[0]) / 1000 or 1e-9
                rates = [f"{k} +{cur[k] - prev[1][k]} ({(cur[k] - prev[1][k]) / dt:.1f}/s)"
                         for k in sorted(cur) if k in prev[1] and cur[k] != prev[1][k]]
                if rates:
                    print("  changed: " + ", ".join(rates))
            prev = (ms, cur)
        if not a.watch:
            return 0
        time.sleep(a.watch)


def cmd_screenshot(link, a):
    info, data, stable = link.screenshot(a.tries)
    w, h, rows = to_rgb(info, data, a.crop)
    write_png(a.output, w, h, rows, a.scale)
    note = "" if stable else f" (the screen kept changing over {a.tries} reads: may be torn)"
    print(f"{a.output}: {w}x{h} from {info['width']}x{info['height']}x{info['bpp']}{note}")
    return 0


def cmd_key(link, a):
    if a.mask is not None:
        link.keys_mask(int(a.mask, 0), a.hold or 80)
        return 0
    if a.name is None:
        raise DebugLinkError("give a key name or --mask")
    name = a.name.lower()
    if name not in ACTIONS:
        raise DebugLinkError(f"unknown key {a.name!r}; one of: {' '.join(sorted(ACTIONS))}")
    if name == "enc":
        link.key(name, DOWN, a.steps or 1)
    elif a.hold:
        if a.hold > INJECT_MAX_S * 1000:
            raise DebugLinkError("holds are limited to 10000 ms")
        link.key_hold(name, a.hold)
    elif a.down or a.up:
        link.key(name, DOWN if a.down else UP)
    elif a.long:
        link.key(name, LONG)
    elif a.repeat:
        link.key(name, REPEAT, a.repeat)
    elif name in ("ptt", "monitor"):
        raise DebugLinkError(f"{name}: give --hold MS or --down / --up (or use the ptt command)")
    else:
        link.key(name, PRESS)
    return 0


def cmd_radio(link, a):
    op, args = a.op, a.args

    def num(i, f=int):
        if len(args) <= i:
            raise DebugLinkError(f"radio {op}: missing argument")
        return f(args[i])
    if op == "channel":
        link.channel_step(num(0))
    elif op == "pos":
        link.zone_pos(num(0))
    elif op == "zone":
        link.zone(num(0))
    elif op == "vfo":
        link.vfo(args[:1] != ["off"])
    elif op == "freq":
        link.vfo_set(round(num(0, float) * 1e6))
    elif op == "step":
        link.vfo_step(num(0))
    elif op == "power":
        link.power_step(num(0))
    elif op == "squelch":
        link.squelch_step(num(0))
    elif op == "bw":
        link.bandwidth(args[:1] == ["25"])
    elif op == "tone":                # tone tx|rx|both none|ctcss|dcs|dcs_inv VALUE
        which = {"tx": 1, "rx": 2, "both": 3}[args[0]]
        link.tone(which, args[1], int(args[2]) if len(args) > 2 else 0)
    elif op == "monitor":
        link.monitor(args[:1] != ["off"])
    elif op == "clear":
        link.clear_temporary()
    elif op == "reply-cancel":
        link.reply_cancel()
    elif op == "hotspot-exit":
        link.hotspot_exit()
    elif op == "target":              # target ID [private]
        link.dmr_target(num(0), args[1:2] == ["private"])
    elif op == "contact":
        link.dmr_contact(num(0))
    elif op == "call-back":
        link.dmr_call_back()
    elif op == "target-clear":
        link.dmr_target_clear()
    elif op == "privacy":             # privacy off | moto KEY | anytone KEY
        link.dmr_privacy(args[0], int(args[1], 0) if len(args) > 1 else 0)
    elif op == "scan":                # scan stop|zone|vfo|dual POS|cancel|nuisance|toggle
        link.scan(args[0] if args else "toggle", num(1) if args[:1] == ["dual"] else None)
    elif op == "scan-vfo":            # scan-vfo LO_MHZ HI_MHZ
        link.scan_vfo(round(num(0, float) * 1e6), round(num(1, float) * 1e6))
    elif op == "beep":                # beep key|error|tx_permit|...
        link.beep(args[0] if args else "key")
    elif op == "burst":               # burst on|off
        link.tone_burst(args[:1] != ["off"])
    elif op == "set":                 # set NAME VALUE
        link.setting(args[0], num(1))
    else:
        raise DebugLinkError(f"unknown radio op {op!r}")
    return 0


def cmd_data(link, a):
    args = a.args
    if a.op == "sms":
        if len(args) < 2:
            print("data sms: ID TEXT", file=sys.stderr)
            return 2
        link.sms(int(args[0], 0), " ".join(args[1:]), group=a.group, ack=not a.no_ack)
    elif a.op in ("alert", "check"):
        if len(args) != 1:
            print(f"data {a.op}: ID", file=sys.stderr)
            return 2
        (link.call_alert if a.op == "alert" else link.radio_check)(int(args[0], 0))
    elif a.op == "settings":
        if not args:
            print("data settings: ARS_ID [check-reply] [no-alert-ack] [no-lrrp]", file=sys.stderr)
            return 2
        link.data_settings(int(args[0], 0), "check-reply" in args, "no-alert-ack" not in args, "no-lrrp" not in args)
    elif a.op == "inbox":
        m = link.inbox(int(args[0]) if args else 0)
        print("empty" if m is None else f"from {m['src']} to {m['dst']}{' (group)' if m['group'] else ''}"
              f"{' unread' if m['unread'] else ''}: {m['text']}")
    elif a.op == "read":
        link.mark_read(int(args[0]) if args else 0)
    return 0


def decode_chip(r):
    idcode, flash_kb, u0, u1, u2, optcr, rdp, rdp_byte, wrp, bor, user = struct.unpack_from("<IH3IIBBHBB", r)
    return {"idcode": idcode, "dev_id": idcode & 0xFFF, "rev_id": idcode >> 16, "flash_kb": flash_kb,
            "uid": (u0, u1, u2), "token": u0, "optcr": optcr, "rdp": rdp, "rdp_byte": rdp_byte, "wrp": wrp,
            "wrp_sectors": [i for i in range(12) if wrp & (1 << i)], "bor": bor, "user": user}


def mem_allowed(addr, n):
    return n > 0 and any(lo <= addr and addr + n <= hi for lo, hi in MEM_WHITELIST)


def git_toplevel(path):
    """The git work tree holding path (or its nearest existing parent), None if none."""
    import subprocess
    d = os.path.abspath(path)
    while not os.path.isdir(d):
        d = os.path.dirname(d)
    try:
        r = subprocess.run(["git", "-C", d, "rev-parse", "--show-toplevel"], capture_output=True, text=True)
    except OSError:
        return None
    return os.path.realpath(r.stdout.strip()) if r.returncode == 0 and r.stdout.strip() else None


def dump_path_ok(path):
    """(ok, reason): refuse any path inside a git work tree (the vendor bootloader is proprietary: never commit)."""
    top = git_toplevel(path)
    if top is not None:
        return False, f"{path} is inside the git work tree {top}; dumps must stay out of any repository"
    return True, ""


def print_chip(c):
    rdp_names = {0: "level 0 (0xAA, no protection)", 1: f"LEVEL 1 (0x{c['rdp_byte']:02X}: read protection)",
                 2: "LEVEL 2 (0xCC: permanent, ROM bootloader and debug disabled)"}
    idc = f"0x{c['idcode']:08X}"
    if c["idcode"] == 0:
        idc += " (reads 0 without a debugger on some STM32F40x revisions)"
    else:
        idc += f" (dev 0x{c['dev_id']:03X}{' STM32F405/407' if c['dev_id'] == 0x413 else ''}, rev 0x{c['rev_id']:04X})"
    print(f"idcode   {idc}")
    print(f"flash    {c['flash_kb']} KB")
    print(f"uid      {c['uid'][2]:08X}{c['uid'][1]:08X}{c['uid'][0]:08X} (token 0x{c['token']:08X})")
    print(f"optcr    0x{c['optcr']:08X}")
    print(f"rdp      {rdp_names[c['rdp']]}")
    print(f"wrp      {'sectors ' + ','.join(map(str, c['wrp_sectors'])) if c['wrp_sectors'] else 'none'}")
    print(f"bor      level {['3 (2.7 V)', '2 (2.4 V)', '1 (2.1 V)', 'off'][c['bor']]}")
    u = c["user"]
    print(f"user     WDG_SW={u & 1} nRST_STOP={(u >> 1) & 1} nRST_STDBY={(u >> 2) & 1}")
    if c["rdp"] != 0:
        print("\n*** WARNING: RDP is not level 0. Do NOT use the ROM bootloader (reboot --rom-dfu is refused): it would\n"
              "*** refuse flash writes, and its read-unprotect command mass-erases ALL flash including the stock\n"
              "*** bootloader. Report this to the owner before doing anything else. ***", file=sys.stderr)
    app_wrp = [s for s in c["wrp_sectors"] if s >= 3]
    if app_wrp:
        print(f"\n*** WARNING: application sectors {app_wrp} are write protected: no loader can rewrite them ***",
              file=sys.stderr)


def cmd_dump(link, a):
    addr, n = int(a.addr, 0), int(a.len, 0)
    if not mem_allowed(addr, n):
        print(f"odr_debug: 0x{addr:08X}+0x{n:X} is outside the readable ranges", file=sys.stderr)
        return 2
    out = a.output or os.path.join(DEFAULT_DUMP_DIR, f"dump_{addr:08X}_{n:X}.bin")
    ok, why = dump_path_ok(out)
    if not ok:
        print(f"odr_debug: refusing: {why}", file=sys.stderr)
        return 2
    data = link.dump(addr, n, lambda d, t: print(f"\r{d}/{t} bytes", end="", file=sys.stderr))
    print(file=sys.stderr)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "wb") as f:
        f.write(data)
    import hashlib
    print(f"{out}: {len(data)} bytes from 0x{addr:08X}, sha256 {hashlib.sha256(data).hexdigest()}")
    if addr < 0x0800C000 and addr >= 0x08000000:
        print("note: this holds the proprietary stock bootloader: never commit or publish it")
    return 0


def cmd_reboot(link, a):
    c = link.chip()
    print_chip(c)
    mode = REBOOT_ROM_DFU if a.rom_dfu else REBOOT_NORMAL
    if mode == REBOOT_ROM_DFU and c["rdp"] != 0:
        print("odr_debug: refusing ROM DFU: RDP is not level 0", file=sys.stderr)
        return 1
    try:
        link.reboot(mode, c["token"])
    except DebugLinkError as e:
        if e.status == ST_REFUSED and e.data:
            print(f"odr_debug: the radio refused: {REBOOT_REASONS.get(e.data[0], e.data[0])}", file=sys.stderr)
            return 1
        raise
    if mode == REBOOT_ROM_DFU:
        print("rebooting into the ROM bootloader: look for USB 0483:DF11 'STM32 BOOTLOADER' "
              "(then tools/stm32_dfu.py --rom IMAGE.bin)")
    else:
        print("rebooting")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port (default: the radio with USB ID 1FC9:0094, or $ODR_PORT)")
    ap.add_argument("--timeout", type=float, default=1.0, help="seconds to wait for each answer")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("ping")
    sub.add_parser("version")
    p = sub.add_parser("diag")
    p.add_argument("--watch", type=float, metavar="N", help="repeat every N seconds, with counter rates")
    p.add_argument("--raw", action="store_true", help="the lines only")
    p = sub.add_parser("view")
    p.add_argument("--section", choices=VIEW_SECTIONS, action="append")
    p.add_argument("--json", action="store_true")
    sub.add_parser("reset")
    p = sub.add_parser("screenshot")
    p.add_argument("-o", "--output", required=True)
    p.add_argument("--scale", type=int, default=1)
    p.add_argument("--crop", action="store_true", help="leave out the insets hidden by the bezel")
    p.add_argument("--tries", type=int, default=4, help="reads until two agree (1: one read, may tear)")
    p = sub.add_parser("key")
    p.add_argument("name", nargs="?")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--long", action="store_true")
    g.add_argument("--hold", type=int, metavar="MS")
    g.add_argument("--repeat", type=int, metavar="N", help="long press, then N repeats")
    g.add_argument("--down", action="store_true", help="raw edge keys (monitor): press and keep (10 s at most)")
    g.add_argument("--up", action="store_true")
    g.add_argument("--steps", type=int, help="enc: detents (negative: the other way)")
    p.add_argument("--mask", help="physical key bits (board specific), with --hold")
    sub.add_parser("wake")
    p = sub.add_parser("ptt")
    p.add_argument("--seconds", type=float, required=True)
    p = sub.add_parser("radio")
    p.add_argument("op", choices=["channel", "pos", "zone", "vfo", "freq", "step", "power", "squelch", "bw", "tone",
                                  "monitor", "clear", "reply-cancel", "hotspot-exit", "target", "contact", "call-back",
                                  "target-clear", "privacy", "scan", "scan-vfo", "beep", "burst", "set"])
    p.add_argument("args", nargs="*")
    p = sub.add_parser("data", help="DMR data: sms, call alert, radio check, settings, inbox")
    p.add_argument("op", choices=["sms", "alert", "check", "settings", "inbox", "read"])
    p.add_argument("args", nargs="*", help="sms: ID TEXT [--group] [--no-ack]; alert / check: ID; settings: "
                   "ARS_ID [check-reply] [no-alert-ack] [no-lrrp]; inbox / read: [INDEX]")
    p.add_argument("--group", action="store_true")
    p.add_argument("--no-ack", action="store_true")
    p = sub.add_parser("log")
    p.add_argument("--follow", action="store_true")
    sub.add_parser("chip")
    p = sub.add_parser("dump")
    p.add_argument("--addr", required=True)
    p.add_argument("--len", required=True)
    p.add_argument("-o", "--output", help=f"file outside any git tree (default {DEFAULT_DUMP_DIR}/dump_ADDR_LEN.bin)")
    p = sub.add_parser("reboot")
    p.add_argument("--rom-dfu", action="store_true", help="into ST's ROM USB DFU (refused unless RDP level 0)")
    a = ap.parse_args(argv)

    if a.cmd == "ptt" and not 0 < a.seconds <= INJECT_MAX_S:
        ap.error(f"--seconds must be > 0 and <= {INJECT_MAX_S:g}")
    try:
        with DebugLink(a.port, a.timeout) as link:
            if a.cmd == "ping":
                print(f"{link.port}: pong in {link.ping() * 1000:.1f} ms")
            elif a.cmd == "version":
                for k, v in link.version().items():
                    print(f"{k}={v}")
            elif a.cmd == "diag":
                return cmd_diag(link, a)
            elif a.cmd == "view":
                v = link.view(a.section)
                print(json.dumps(v, indent=1) if a.json else "\n".join(f"{k}={x}" for k, x in v.items()))
            elif a.cmd == "reset":
                print(f"counters reset at t={link.reset() / 1000:.3f} s")
            elif a.cmd == "screenshot":
                return cmd_screenshot(link, a)
            elif a.cmd == "key":
                return cmd_key(link, a)
            elif a.cmd == "wake":
                link.wake()
            elif a.cmd == "ptt":
                link.ptt_for(a.seconds)
                print(f"PTT held {a.seconds:g} s, released")
            elif a.cmd == "radio":
                return cmd_radio(link, a)
            elif a.cmd == "data":
                return cmd_data(link, a)
            elif a.cmd == "chip":
                print_chip(link.chip())
            elif a.cmd == "dump":
                return cmd_dump(link, a)
            elif a.cmd == "reboot":
                return cmd_reboot(link, a)
            elif a.cmd == "log":
                while True:
                    dropped, text = link.log()
                    if dropped:
                        print(f"({dropped} lines dropped)")
                    sys.stdout.write(text)
                    sys.stdout.flush()
                    if not a.follow:
                        break
                    time.sleep(0.5)
    except DebugLinkError as e:
        print(f"odr_debug: {e}", file=sys.stderr)
        return 1 if e.status is not None else 2
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
