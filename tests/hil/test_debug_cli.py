# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""rfoundry_debug.py's radio command: what it parses and sends. No radio: these run anywhere, and tools/ci.sh runs them.
`radio ysf` had a handler but no place in the parser's choices, so every call failed before it reached the radio, and
bench A/Bs that switched the soft modem's options over it measured with them on all along."""
import re
from pathlib import Path
from types import SimpleNamespace

import rfoundry_debug

SOURCE = (Path(__file__).resolve().parents[2] / "tools" / "rfoundry_debug.py").read_text()


def radio_choices():
    m = re.search(r'p = sub\.add_parser\("radio"\)\n    p\.add_argument\("op", choices=\[(.*?)\]\)', SOURCE, re.S)
    assert m, "the radio subparser's choices"
    return set(re.findall(r'"([\w-]+)"', m.group(1)))


def radio_handlers():
    body = SOURCE[SOURCE.index("def cmd_radio(link, a):"):]
    body = body[:body.index("\ndef ", 1)]
    return set(re.findall(r'op == "([\w-]+)"', body))


def test_every_radio_handler_is_a_choice():
    missing = radio_handlers() - radio_choices()
    assert not missing, f"handled in cmd_radio but refused by the parser: {sorted(missing)}"


class FakeLink:
    def __init__(self):
        self.kw = None

    def ysf_settings_update(self, **kw):
        self.kw = kw


def ysf(*args):
    link = FakeLink()
    rfoundry_debug.cmd_radio(link, SimpleNamespace(op="ysf", args=list(args)))
    return link.kw


def test_ysf_bench_bits():
    assert ysf("eq=off") == {"bench": 1}
    assert ysf("tone=off") == {"bench": 2}
    assert ysf("dcs=off") == {"bench": 4}
    assert ysf("eq=off", "tone=off", "dcs=off") == {"bench": 7}
    assert ysf("eq=on", "tone=on", "dcs=on") == {"bench": 0}
    assert ysf("dgid=5") == {"tx_dgid": 5, "rx_dgid": 5}


def test_ysf_settings_sends_the_bench_byte():
    sent = []
    link = rfoundry_debug.DebugLink.__new__(rfoundry_debug.DebugLink)
    link.radio = lambda op, args=b"": sent.append((op, args))
    link.ysf_settings(1, 2, bench=7)
    assert sent == [(rfoundry_debug.OP_YSF, bytes([1, 2, 7]))]
