# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""An incoming private voice call forced through the RadioDesk sandbox alert API (opt-in: --allow-rf
--allow-radiodesk, and the sandbox broker's credentials in the environment; never in the repository).

The test publishes a plain-text alert to ESS/RadioDesk/Alert/<radio id>/voice on the sandbox MQTT broker; RadioDesk
turns it into speech and calls the radio (a private call to its own ID) through its gateway on the SDR bench, which
keeps running (no lease). First the radio keys 1 s at its lowest power on the channel so the gateway hears it and
RadioDesk has a route to it (marker radio_tx). The test listens on ESS/RadioDesk/AlertStatus/<radio id>/voice for RadioDesk's status
reports while it watches the radio over the debug link.

Environment (all from the shell, nothing stored): ODR_RADIODESK_MQTT_HOST, ODR_RADIODESK_MQTT_PORT (default 1883, or
8883 with TLS), ODR_RADIODESK_MQTT_USER, ODR_RADIODESK_MQTT_PASSWORD, ODR_RADIODESK_MQTT_TLS (1: TLS with the
system's CA store). Needs the paho-mqtt package (skipped without it).

Pass criteria:
- the radio shows the call: within 60 s the dmr_call view has a call with call.group=0 and call.to_us=1 (a private
  call to us), from --radiodesk-src when given, and the UI is on Home (the call is drawn there); the call has ended
  (dmr.state idle, call.ms 0) within 60 s more: the call.* keys keep the last caller after the end, so they don't
  show it;
- the radio plays it: the call stack counts one more call played (call.rx_calls_played), the voice layer plays at
  least 25 frames (0.5 s), and at most 5 % of the frames it took in were concealed;
- RadioDesk reports on AlertStatus at least once, and no report says "fail" or "error".
"""
import json
import os
import queue
import time

import pytest

from odr_hil import counter_deltas

TIMEOUT_S = 60


def mqtt_client():
    try:
        import paho.mqtt.client as mqtt
    except ImportError:
        pytest.skip("needs paho-mqtt (pip install paho-mqtt)")
    env = {k: os.environ.get(f"ODR_RADIODESK_MQTT_{k}") for k in ("HOST", "PORT", "USER", "PASSWORD", "TLS")}
    if not env["HOST"] or not env["USER"] or not env["PASSWORD"]:
        pytest.skip("set ODR_RADIODESK_MQTT_HOST, _USER and _PASSWORD (the sandbox broker; never commit them)")
    tls = env["TLS"] in ("1", "yes", "true")
    try:
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    except AttributeError:          # paho-mqtt 1.x
        c = mqtt.Client()
    c.username_pw_set(env["USER"], env["PASSWORD"])
    if tls:
        c.tls_set()
    return c, env["HOST"], int(env["PORT"] or (8883 if tls else 1883))


@pytest.mark.rf
@pytest.mark.radio_tx
@pytest.mark.radiodesk
def test_radiodesk_alert_private_call(radio, pytestconfig):
    if "DMR" not in radio.features():
        pytest.skip("built without DMR")
    freq = pytestconfig.getoption("--radiodesk-freq") or pytestconfig.getoption("--dmr-freq")
    want_src = pytestconfig.getoption("--radiodesk-src")
    client, host, port = mqtt_client()
    radio.go_home()
    radio.find_channel(round(freq * 1e6), "dmr")
    d = radio.view(["dmr"])
    rid = int(d["dmr.own_id"])
    if rid == 0:
        pytest.skip("the radio has no DMR ID (dmr.own_id=0)")
    if d.get("dmr.codec") != "1":
        pytest.skip("no vocoder merged in this firmware (dmr.codec=0): the call can't play")

    status = queue.Queue()
    alert_topic = f"ESS/RadioDesk/Alert/{rid}/voice"
    status_topic = f"ESS/RadioDesk/AlertStatus/{rid}/voice"
    client.on_message = lambda c, u, m: status.put((time.monotonic(), m.payload.decode("utf-8", "replace")))
    client.connect(host, port, keepalive=30)
    client.loop_start()
    try:
        client.subscribe(status_topic, qos=1)
        time.sleep(1.0)
        while not status.empty():                       # a retained report from an earlier run
            print("earlier status:", status.get()[1])
        # RadioDesk routes a private call to where it last heard the radio (HBlink4 needs a recent route): a 1 s key-up
        # at the lowest power, heard by the gateway, gives it one
        radio.lowest_power()
        radio.ptt_for(1.0)
        radio.wait_for(lambda x: x.get("state") != "tx", timeout=3, what="TX ended")
        time.sleep(2.0)
        radio.reset_counters()
        _, before = radio.counters()
        t0 = time.monotonic()
        client.publish(alert_topic, pytestconfig.getoption("--radiodesk-text"), qos=1).wait_for_publish(10)
        shown = None
        while time.monotonic() - t0 < TIMEOUT_S:
            v = radio.view(["dmr_call", "link", "dmr"])
            # dmr.state rx: a call is running (the call.* keys alone could still show an earlier run's call)
            if v.get("dmr.state") == "rx" and v.get("call.group") == "0" and v.get("call.to_us") == "1" and \
                    v.get("call.src", "0") != "0":
                shown = v
                break
            time.sleep(0.1)
        if not shown:
            while not status.empty():
                print("AlertStatus:", status.get()[1])
        assert shown, f"no private call to {rid} shown within {TIMEOUT_S} s (last view {v})"
        print(f"call shown after {time.monotonic() - t0:.1f} s: src {shown['call.src']} dst {shown['call.dst']} "
              f"name {shown.get('call.src_name')!r} screen {shown.get('ui.screen')}")
        assert shown.get("ui.screen") == "home", f"the call is drawn on Home; the UI is on {shown.get('ui.screen')}"
        assert int(shown["call.dst"]) == rid, shown
        if want_src:
            assert int(shown["call.src"]) == want_src, f"caller {shown['call.src']} != --radiodesk-src {want_src}"
        # The call view keeps the last caller after the call (Home shows it), so the end is the DMR state going idle
        # (dmr.state; call.ms, the running call's length, back to 0)
        radio.wait_for(lambda x: x.get("dmr.state") == "idle" and x.get("call.ms") == "0", timeout=TIMEOUT_S,
                       sections=("dmr", "dmr_call"), what="the call to end (dmr.state idle)")
        time.sleep(2.0)                                  # playback drains, the last status report arrives
        _, after = radio.counters()
    finally:
        client.loop_stop()
        client.disconnect()
    reports = []
    while not status.empty():
        reports.append(status.get()[1])
    keys = ["call.rx_calls", "call.rx_calls_played", "voice.rx_in", "voice.rx_frames", "voice.rx_concealed",
            "voice.rx_lost", "call.rx_lost_frames"]
    dl = counter_deltas(before, after, keys)
    print("radio deltas:", dl)
    print("AlertStatus reports:", json.dumps(reports))
    assert dl["call.rx_calls_played"] >= 1, f"the call was not played: {dl}"
    assert dl["voice.rx_frames"] >= 25, f"only {dl['voice.rx_frames']} voice frames played: {dl}"
    assert dl["voice.rx_concealed"] <= 0.05 * max(1, dl["voice.rx_in"]), f"too many concealed frames: {dl}"
    assert reports, f"no report on {status_topic}"
    bad = [r for r in reports if "fail" in r.lower() or "error" in r.lower()]
    assert not bad, f"RadioDesk reported a problem: {bad}"
