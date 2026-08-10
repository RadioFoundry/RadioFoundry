# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The radio is up and the debug link works: version and features, screenshot geometry, no stalls, error replies."""
import struct
import time

import pytest

from odr_hil import DebugLinkError, as_int


def test_version_and_features(radio):
    v = radio.version(fresh=True)
    assert v.get("proto") == "1"
    assert v.get("board"), "board name"
    assert v.get("git") and v["git"] != "unknown", "firmware built from a git tree"
    feats = radio.features()
    assert {"HOSTLINK", "DEBUGLINK"} <= feats, feats    # USB too on radios that have it (not the DM-32)
    assert "FM" in feats or "DMR" in feats


def test_ping_latency(radio):
    times = [radio.ping() for _ in range(20)]
    assert max(times) < 0.15, f"slowest ping {max(times) * 1000:.0f} ms"


def test_screenshot_geometry(radio, shots):
    w, h, bpp = (int(x) for x in radio.version()["display"].split("x"))
    info, data, stable = radio.screenshot()
    assert (info["width"], info["height"], info["bpp"]) == (w, h, bpp)
    stride = w * 2 if bpp == 16 else (w + 7) // 8
    assert len(data) == info["total"] == stride * h
    assert stable, "the screen kept changing during the capture"
    radio.save_screenshot(f"{shots}/boot.png")


def test_no_stalls(radio):
    """The firmware clock advances with wall time, the link answers throughout, nothing timed out."""
    link0 = radio.view(["link"])
    t0, m0 = time.monotonic(), as_int(link0["uptime_ms"])
    for _ in range(10):
        radio.ping()
        time.sleep(0.2)
    link1 = radio.view(["link"])
    dt_fw = (as_int(link1["uptime_ms"]) - m0) / 1000
    dt = time.monotonic() - t0
    assert abs(dt_fw - dt) < 0.3, f"firmware clock {dt_fw:.2f} s vs wall {dt:.2f} s"
    assert link1["dl.timeouts"] == link0["dl.timeouts"], "partial frames timed out"
    assert link1["dl.bad_crc"] == link0["dl.bad_crc"]


def test_view_sections(radio):
    v = radio.view()
    for k in ("view", "state", "rx_hz", "ch.name", "dmr.built", "hostlink", "ui.screen", "input.idle_ms"):
        assert k in v, k
    assert v["view"] == "1"


def test_diag_has_clock_and_lines(radio):
    ms0, lines = radio.link.diag()
    assert len(lines) >= 2 and lines[0].startswith("radio ")
    time.sleep(0.5)
    ms1, _ = radio.link.diag()
    assert 300 < ms1 - ms0 < 1500


def test_reset_counters_returns_clock(radio):
    t = radio.reset_counters()
    assert abs(t - as_int(radio.view(["link"])["uptime_ms"])) < 1000


def test_log_readable(radio):
    dropped, text = radio.log()
    assert dropped >= 0
    assert all(line.split(" ", 1)[0].isdigit() for line in text.splitlines()), text[:200]


def test_unknown_command_and_bad_crc(radio):
    with pytest.raises(DebugLinkError) as e:
        radio.link.request(0x7E)
    assert e.value.status == 1
    # A frame with a broken CRC gets an error frame (command 0xFF, status 5)
    from odr_debug import frame
    f = bytearray(frame(0x01, 0x33, b"x"))
    f[-1] ^= 0xFF
    radio.link.s.write(bytes(f))
    r = radio.link._read_frame(time.monotonic() + 1.0)
    assert r is not None and r[0] == 0xFF and r[1] == 0x33 and r[2][0] == 5, r
    radio.ping()   # still fine afterwards


def test_oversized_request_rejected(radio):
    hdr = bytes([0xA5, 0x5A, 0x01, 0x44]) + struct.pack("<H", 65)
    radio.link.s.write(hdr)
    r = radio.link._read_frame(time.monotonic() + 1.0)
    assert r is not None and r[0] == 0xFF and r[2][0] == 6, r
    radio.ping()
