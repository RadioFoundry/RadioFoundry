# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Planned tests, skipped with the reason until they are written."""
import os

import pytest


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.skip(reason="planned: radio DMR TX checked by a capture on the SDR bench's receiver (dmr_bench.py's "
                         "T cases: sdrmon.py capture + analyze); needs that wrapper in SdrBench")
def test_dmr_tx_via_sdr(radio, sdr_bench):
    pass


@pytest.mark.skip(reason="planned: an incoming private voice call forced through the RadioDesk sandbox alert API "
                         "(MQTT ESS/RadioDesk/Alert/<radio id>/voice on the sandbox broker), the radio must show "
                         "and play it; credentials from $ODR_RADIODESK_MQTT_USER / $ODR_RADIODESK_MQTT_PASSWORD, "
                         "never in the repo")
def test_radiodesk_alert_voice(radio):
    assert os.environ.get("ODR_RADIODESK_MQTT_USER")
