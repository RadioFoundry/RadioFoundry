#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""
A local DMR repeater on the SDR bench's MMDVM_HS dual modem board, with no network: the radio under test, on a repeater channel
(its TX = our --rx, its RX = our --tx), must wake it with the downlink activate CSBK (BS_Dwn_Act) and then talk
through it. This is how we test the radio's repeater mode (RMO): BS activation, slot alignment to the outbound (CACH
TC), BS sync reception, hang time, the radio hearing its own call repeated, and late entry on a BS outbound.

New tool (no earlier bench tool did this). It wraps odr_sdrbench's Repeater (odr_sdrbench.mmdvm.repeater: the
MMDVMHost DMRSlot behavior on a duplex modem: BS sync, CACH with short LC, 3 headers, terminators repeated for the
call hang, AMBE FEC regenerated) with no network link. Needs the dual modem (two ADF7021s; a single one can't run
duplex) and the odr_sdrbench package. The bench's gateway service must not hold the bench modem.

    bench_repeater.py --rx 466.6875 --tx 461.6875 [--cc 2] [--seconds 60] [--hang 4] [--call-hang 3]
                    [--slots 1,2] [--power 255] [--json] [--beacon S --beacon-len S [--beacon-phase S]]

--beacon S: an idle repeater's roaming beacon (docs/spec/dmr-roaming.md 4.8): every S seconds, while the transmitter is
down, the repeater keys its standard outbound (BS sync, CACH, idle bursts) for --beacon-len seconds and drops again,
without a call. Made by handing the repeater a local BS_Dwn_Act (as if a radio had asked) with the hang time set to
the beacon length, so nothing but the repeater's own outbound goes on air. The first beacon comes --beacon-phase
seconds after the start (default 2). Emits "beacon" events with the time keyed. Run on the bench 2026-10-15: each
beacon is on air for its full length (BS sync, CACH, idle bursts).

Frequencies: a split pair you are licensed for; there is no default on purpose. Prints one JSON line per event
(--json): "key" (transmitter up / down), "wakeup" (BS_Dwn_Act heard, with the source), "rx" (each burst heard on the
input: slot, data type or voice index, LC), "lost"; at the end a summary per slot.

A burst heard while the transmitter is down is only a wakeup candidate: an MMDVM duplex modem with its transmitter
off passes only data-synced bursts (CONTROL_IDLE), so a radio that keys straight into voice without a BS_Dwn_Act
never brings it up (that is the expected failure for a radio without BS activation).
"""
import argparse
import json
import logging
import threading
import time

from odr_sdrbench.dmr import pdu
from odr_sdrbench.mmdvm.modem import CONTROL_IDLE, Modem, ModemConfig, SYNC_AUDIO, SYNC_DATA
from odr_sdrbench.mmdvm.radio import ModemTuner, Radio
from odr_sdrbench.mmdvm.repeater import Repeater

BS_DWN_ACT = 0x38


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/ttyS0")
    ap.add_argument("--rx", type=float, required=True, help="MHz: repeater input (the radio's TX)")
    ap.add_argument("--tx", type=float, required=True, help="MHz: repeater output (the radio's RX)")
    ap.add_argument("--cc", type=int, default=2)
    ap.add_argument("--slots", default="1,2")
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--hang", type=float, default=4.0, help="s the transmitter stays up after the last traffic")
    ap.add_argument("--call-hang", type=float, default=3.0, help="s a call's terminator is repeated")
    ap.add_argument("--power", type=int, default=255)
    ap.add_argument("--rx-offset", type=int, default=None, help="Hz (default: the bench's calibration)")
    ap.add_argument("--tx-offset", type=int, default=None, help="Hz (default: the bench's calibration)")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--beacon", type=float, default=0, help="s between idle beacons (0: no beacons)")
    ap.add_argument("--beacon-len", type=float, default=4.0, help="s each beacon lasts (the hang time in this mode)")
    ap.add_argument("--beacon-phase", type=float, default=2.0, help="s from the start to the first beacon")
    args = ap.parse_args()
    if args.beacon:
        if not 0 < args.beacon_len < args.beacon:
            raise SystemExit("--beacon-len must be shorter than --beacon")
        args.hang = args.beacon_len	# a beacon is a wake-up with nothing to repeat: it lasts the hang time
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    if abs(args.rx - args.tx) < 0.0125:
        raise SystemExit("a duplex repeater needs separate RX and TX frequencies")

    stats = {1: {}, 2: {}}

    def emit(rec):
        rec = {"t": round(time.time(), 3), **rec}
        if args.json:
            print(json.dumps(rec), flush=True)
        else:
            logging.info("%s", rec)

    import serial
    modem = Modem(serial.Serial(args.port, 115200, timeout=0.1))
    modem.start()
    radio = Radio(modem, "mmdvm_hs", ModemConfig(tx_level=50, rx_level=100, tx_delay_ms=100),
                  tuner=ModemTuner(modem, args.rx_offset, args.tx_offset))
    rpt = Repeater(radio, 0, int(round(args.rx * 1e6)), int(round(args.tx * 1e6)), args.cc, power=args.power,
                   slots=tuple(int(s) for s in args.slots.split(",")), hang_s=args.hang, call_hang_s=args.call_hang)
    keyed_now = {"on": False}

    def on_key(keyed):
        keyed_now["on"] = bool(keyed)
        emit({"key": bool(keyed)})

    rpt.on_key = on_key
    rpt.start()
    logging.info("%s repeater RX %.4f TX %.4f MHz cc %d", modem.version, args.rx, args.tx, args.cc)

    heard, lost = modem.on_rx_slot, modem.on_lost

    def on_rx_slot(slot, control, burst, rssi):
        rec = {"slot": slot, "control": control, "rssi": rssi}
        st = stats[slot]
        if control & SYNC_DATA:
            dt = control & 0x0F
            rec["dt"] = dt
            if dt == pdu.DT_CSBK:
                c = pdu.decode_csbk(burst)
                if c is not None:
                    rec.update(csbk_opcode=c.opcode, crc_ok=c.crc_ok, src=c.src_id)
                    if c.opcode == BS_DWN_ACT and c.crc_ok:
                        st["wakeups"] = st.get("wakeups", 0) + 1
                        emit({"wakeup": c.src_id, "slot": slot, "while_down": bool(control & CONTROL_IDLE)})
            elif dt in (pdu.DT_VOICE_LC_HEADER, pdu.DT_TERMINATOR_WITH_LC):
                lc = pdu.decode_voice_lc(burst, dt)
                if lc is not None:
                    rec.update(src=lc.src_id, dst=lc.dst_id, flco=lc.flco)
            key = f"dt{dt}"
        else:
            rec["voice"] = 0 if control & SYNC_AUDIO else control & 0x0F
            key = "voice"
        st[key] = st.get(key, 0) + 1
        emit({"rx": rec})
        heard(slot, control, burst, rssi)

    def on_lost(slot):
        emit({"lost": slot})
        lost(slot)

    modem.on_rx_slot, modem.on_lost = on_rx_slot, on_lost
    stop = threading.Event()
    t = threading.Thread(target=rpt.run, args=(stop,), daemon=True)
    t.start()

    def beacons():
        # A local BS_Dwn_Act straight into the repeater's receive path (not on air): it keys its outbound for the
        # hang time. Skipped while the transmitter is up (traffic: the radios hear the repeater anyway)
        from bench_tx import one_csbk
        burst = one_csbk(BS_DWN_ACT, 0, 0xFFFFFF, 0, args.cc)
        start = time.monotonic()
        n = 0
        while not stop.is_set():
            due = start + args.beacon_phase + n * args.beacon
            if stop.wait(max(0.0, due - time.monotonic())):
                break
            n += 1
            if keyed_now["on"]:
                emit({"beacon": n, "skipped": "keyed"})
                continue
            heard(1, SYNC_DATA | pdu.DT_CSBK | CONTROL_IDLE, burst, -60)
            emit({"beacon": n, "len_s": args.beacon_len})

    if args.beacon:
        threading.Thread(target=beacons, daemon=True).start()
        logging.info("beacon every %.1f s for %.1f s, first after %.1f s", args.beacon, args.beacon_len,
                     args.beacon_phase)
    try:
        time.sleep(args.seconds)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        t.join(2)
        try:
            modem.dmr_start(False)
        except Exception:
            pass
        modem.close()
        emit({"summary": stats})


if __name__ == "__main__":
    main()
