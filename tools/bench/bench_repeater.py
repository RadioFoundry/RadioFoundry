#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""
A local DMR repeater on the SDR bench's MMDVM_HS dual modem board, with no network: the radio under test, on a repeater channel
(its TX = our --rx, its RX = our --tx), must wake it with the downlink activate CSBK (BS_Dwn_Act) and then talk
through it. This is how we test the radio's repeater mode (RMO): BS activation, slot alignment to the outbound (CACH
TC), BS sync reception, hang time, the radio hearing its own call repeated, and late entry on a BS outbound.

New tool (no earlier bench tool did this). It wraps RadioDesk rfgw's Repeater (radiodesk_rfgw.mmdvm.repeater: the
MMDVMHost DMRSlot behavior on a duplex modem: BS sync, CACH with short LC, 3 headers, terminators repeated for the
call hang, AMBE FEC regenerated) without its HBlink4 link. Needs the dual modem (two ADF7021s; a single one can't run
duplex) and rfgw's venv (/opt/radiodesk-rfgw/.venv/bin/python). rfgw must not hold the bench modem.

    bench_repeater.py --rx 466.6875 --tx 461.6875 [--cc 2] [--seconds 60] [--hang 4] [--call-hang 3]
                    [--slots 1,2] [--power 255] [--json]

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

from radiodesk.dmr import pdu
from radiodesk_rfgw.mmdvm.modem import CONTROL_IDLE, Modem, ModemConfig, SYNC_AUDIO, SYNC_DATA
from radiodesk_rfgw.mmdvm.radio import ModemTuner, Radio
from radiodesk_rfgw.mmdvm.repeater import Repeater

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
    ap.add_argument("--rx-offset", type=int, default=-150)
    ap.add_argument("--tx-offset", type=int, default=-175)
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
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
    rpt.on_key = lambda keyed: emit({"key": bool(keyed)})
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
