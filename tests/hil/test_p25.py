# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""P25 conventional on the radio, with nothing sent (docs/spec/p25.md 7). Radio only: the radio never transmits.

test_p25_channel_switch: the channel on view becomes P25 by a temporary mode change (the debug link's mode request,
debuglink.md 3.8 op 31); the p25 view section must show the engine running (`p25.on=1`, no new `begin_fails`) and,
where the provider counts it, the receive load at idle (`p25.rx_work_us / p25.rx_span_us`, at most MAX_IDLE_LOAD).
Undoing the temporary change brings the channel's own mode back with the engine stopped, and a DMR channel's call
stack running again.

test_p25_settings: the P25 settings (op 39, RAM only) round trip through the p25 view and are put back.

Skipped without CONFIG_P25, on a channel in a listen-only band, and in hotspot mode."""
import os
import time

import pytest

from rfoundry_hil import as_int, skip

# Receive work over the signal it covered, P25 running with nothing on the channel. The soft demodulator runs on
# every sample whether a call is there or not, so idle is close to the busy figure
MAX_IDLE_LOAD = float(os.environ.get("RFOUNDRY_HIL_P25_MAX_LOAD", "0.5"))


def p25_ready(radio):
    if "P25" not in radio.features():
        skip("not_built", "built without CONFIG_P25")
    radio.go_home()
    if radio.core().get("hotspot") == "1":
        skip("state", "the radio is in hotspot mode")


def p25_view(radio):
    v = radio.view(["p25"])
    if v.get("p25.built") != "1":
        skip("not_built", "no p25 view section")
    return v


def load(radio):
    """(work_us, span_us) of the provider's receive since P25 started, or None when it doesn't count them."""
    v = radio.view(["p25"])
    w, s = as_int(v.get("p25.rx_work_us")), as_int(v.get("p25.rx_span_us"))
    return None if w is None or s is None else (w, s)


@pytest.mark.feature("p25")
def test_p25_channel_switch(radio):
    p25_ready(radio)
    snap = radio.snapshot()
    mode0 = radio.core().get("mode")
    if mode0 == "p25":
        skip("state", "the channel is P25 already")
    fails0 = as_int(p25_view(radio).get("p25.begin_fails")) or 0
    try:
        radio.link.dmr_fields(mode="p25")
        try:
            v = radio.wait_for(lambda x: x.get("mode") == "p25" and x.get("p25.on") == "1", 3.0,
                               sections=("core", "p25"), what="P25 running on the channel")
        except AssertionError:
            v = radio.view(["core", "p25"])
            if v.get("mode") != "p25":
                skip("state", f"the radio kept mode {v.get('mode')} (a listen-only band, or the VFO without "
                              "both modes)")
            raise
        assert (as_int(v.get("p25.begin_fails")) or 0) == fails0, "the engine did not start (begin_fails)"
        print(f"\np25: nac {v.get('p25.rx_nac')}/{v.get('p25.tx_nac')} tg {v.get('p25.rx_tgid')} "
              f"state {v.get('state')}")
        a = load(radio)
        if a is not None:
            time.sleep(3.0)
            b = load(radio)
            dw, ds = b[0] - a[0], b[1] - a[1]
            assert ds > 0, "the provider counted no receive in 3 s"
            share = dw / ds
            print(f"p25 receive load at idle: {dw} us of work over {ds} us = {share:.1%}")
            assert share <= MAX_IDLE_LOAD, f"receive takes {share:.1%} of the CPU at idle"
        assert radio.core().get("state") in ("idle", "rx")
    finally:
        radio.restore(snap)
    v = radio.wait_for(lambda x: x.get("mode") == mode0 and x.get("p25.on") == "0", 3.0,
                       sections=("core", "p25"), what=f"mode {mode0} back, P25 stopped")
    if mode0 == "dmr" and "DMR" in radio.features():
        radio.wait_for(lambda x: x.get("dmr.active") == "1", 3.0, sections=("dmr",), what="the DMR stack back")


@pytest.mark.feature("p25")
def test_p25_settings(radio):
    p25_ready(radio)
    v = p25_view(radio)
    old = {k: int(v[f"p25.{k}"]) for k in ("rx_nac", "tx_nac", "rx_tgid", "talkaround", "tx_dst", "unit_id")}
    old["unit_call"] = v.get("p25.unit_call") == "1"
    try:
        radio.link.p25_settings_update(rx_nac=0xF7E, rx_tgid=4321, talkaround="on")
        v = radio.wait_for(lambda x: x.get("p25.rx_tgid") == "4321", 2.0, sections=("p25",), what="settings taken")
        assert v.get("p25.rx_nac") == str(0xF7E) and v.get("p25.talkaround") == "1"
        assert v.get("p25.tx_nac") == str(old["tx_nac"]), "a field not given changed"
    finally:
        radio.link.p25_settings(**old)
    radio.wait_for(lambda x: x.get("p25.rx_tgid") == str(old["rx_tgid"]), 2.0, sections=("p25",),
                   what="settings put back")
