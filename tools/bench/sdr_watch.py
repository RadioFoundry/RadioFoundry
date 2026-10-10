#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""
Adapted from our earlier bench tool sdr_watch.py.

Watch the bursts the bench gateway's SDR receiver decodes on one channel (MQTT DMR/<receiver>/sdr/raw) and print,
per burst: time, the slot the gateway assigned, the sync family read from the burst bits (BS / MS / TS1 / TS2, or EMB
for voice B-F), color code, data type and the gateway's source / destination. Needs no service stop: the gateway
keeps running and the SDR keeps its normal job. It sees what the gateway decodes (no idle bursts, no timing, no
power): use sdrmon.py for those.

    sdr_watch.py [--receiver SDR15] [--seconds 60] [--json] [--host HOST]

Runs on the SDR bench with the odr_sdrbench package (odr_sdrbench.layer2) and paho-mqtt. The broker comes from
--host or $ODR_SDR_BENCH_MQTT_HOST (default localhost), the login from $ODR_SDR_BENCH_MQTT_USER and
$ODR_SDR_BENCH_MQTT_PASSWORD. On the bench they are in $ODR_SDR_BENCH_ENV_FILE (default /etc/odr-sdrbench/env, root
only), which dmr_bench.py exports before it runs this.
SDR15 is the 461.6875 MHz receiver on the SDR bench (2026-10).
"""
import argparse
import base64
import json
import os
import time

import paho.mqtt.client as mqtt

from odr_sdrbench import layer2 as L2

FAMILIES = [(name, voice, L2.data_sync(voice)) for name, voice, _ in L2.FAMILIES]


def family(burst: bytes) -> str:
    best = ("emb", 49)
    for name, voice, data in FAMILIES:
        for kind, pattern in (("voice", voice), ("data", data)):
            e = L2.sync_errors(burst, pattern)
            if e < best[1]:
                best = (f"{name}-{kind}", e)
    return best[0] if best[1] <= 6 else "emb/none"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=os.environ.get("ODR_SDR_BENCH_MQTT_HOST", "localhost"))
    ap.add_argument("--receiver", default="SDR15")
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--json", action="store_true", help="one JSON object per burst")
    args = ap.parse_args()

    def on_message(client, userdata, msg):
        m = json.loads(msg.payload).get("DMR", {})
        b = base64.b64decode(m.get("data", ""))
        voice = m.get("type") in ("voice", "voice_sync")
        cc, dt, _ = (None, None, 0) if voice else L2.decode_slot_type(b)
        if m.get("type") == "voice":
            cc = L2.decode_emb(b)[0]
        rec = {"t": m.get("timestamp"), "slot": m.get("slot"), "family": family(b), "type": m.get("type"),
               "cc": cc, "dt": dt, "src": m.get("src_id"), "dst": m.get("dst_id")}
        if args.json:
            print(json.dumps(rec), flush=True)
        else:
            print(f"{rec['t']} slot {rec['slot']} {rec['family']:10s} type {rec['type']:12s} "
                  f"cc {cc} src {rec['src']} dst {rec['dst']}", flush=True)

    c = mqtt.Client()
    c.username_pw_set(os.environ["ODR_SDR_BENCH_MQTT_USER"], os.environ["ODR_SDR_BENCH_MQTT_PASSWORD"])
    c.on_message = on_message
    c.connect(args.host, 1883)
    c.subscribe(f"DMR/{args.receiver}/sdr/raw")
    c.loop_start()
    time.sleep(args.seconds)
    c.loop_stop()


if __name__ == "__main__":
    main()
