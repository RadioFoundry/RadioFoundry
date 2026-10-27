# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""MD-380 panel type (docs/spec/hardware-md380.md 7), radio only, never transmits: the display driver read the factory
byte from SPI flash security register 3 offset 0x1D and gave its low two bits to the ST7735 driver (type 3 for an
erased byte, 0xFF). The link section's display.panel_raw is the byte (none: not read), display.panel_type the type in
use. A build with MD380_PANEL_TYPE overrides the type and fails the second check unless it names the factory's. A
board's own test: skipped other_board on any other radio (tests/hil/hil_plugin.py)."""
from rfoundry_hil import skip


def panel(radio):
    v = radio.view(["link"])
    if "display.panel_type" not in v:
        skip("not_built", "firmware without display.panel_raw / display.panel_type")
    return v.get("display.panel_raw"), int(v["display.panel_type"])


def test_factory_panel_byte_read(radio):
    raw, _ = panel(radio)
    assert raw not in (None, "none"), "security register 3 byte 0x1D was not read"
    int(raw, 16)


def test_panel_type_is_factory_type(radio):
    raw, used = panel(radio)
    if raw in (None, "none"):
        skip("state", "the factory byte was not read")
    b = int(raw, 16)
    want = b & 3
    assert used == want, f"factory byte {raw}, type in use {used}, expected {want}"
