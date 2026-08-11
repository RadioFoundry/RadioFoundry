#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""
Adapted from an earlier bench tool of ours.

Drive the SDR bench's MMDVM_HS modem board by hand as a plain DMR subscriber (simplex) to send known traffic to the radio under
test, or to listen to what the radio sends. Runs on the SDR bench with the RadioDesk rfgw venv
(/opt/radiodesk-rfgw/.venv/bin/python): it uses rfgw's MMDVM modem driver and RadioDesk's burst / FEC code, so bursts
are framed exactly as rfgw's roamer frames them. rfgw must not hold the bench modem while this runs (stop
radiodesk-rfgw@<bench host>, or take ROAM1 out of its config); tools/bench/dmr_bench.py does that for you.

    bench_tx.py [--freq 461.6875] [--cc 2] [--power 255] voice [--src ID] [--dst ID] [--private] [--superframes N]
              [--headers N] [--terminators N] [--late K] [--calls N] [--gap-ms MS] [--src2 ID]
              [--family ms|bs|ts1|ts2] [--ambe FILE] [--talker-alias TEXT] [--pi] [--service-options HEX]
              [--emergency] [--burst-cc CC]
    bench_tx.py ... csbk  [--src ID] [--dst ID] [--count N] [--opcode HEX [--fid HEX]] [--family ...]
                                            (preamble CSBK train, then optionally one CSBK, e.g. 1F call alert)
    bench_tx.py ... data  [--src ID] [--dst ID] [--confirmed] [--octets N] [--preambles N] [--family ...]
    bench_tx.py [--ambe-out PREFIX] ... listen [--seconds N] [--json]
    bench_tx.py --listen-after S [--ambe-out PREFIX] voice|data|csbk ...  (send, then listen S seconds, JSON lines)

voice:
  --late K           no header and the first K voice bursts left out (late entry: the receiver joins at burst K)
  --calls N          N calls back to back, --gap-ms apart (last terminator to the next header), source alternating
                     between --src and --src2
  --terminators 0    the call just stops (the receiver must time out)
  --family           sync family of every burst (default ms). bs gives BS-synced bursts in DMO timing (one burst per
                     60 ms, no CACH): enough to see that the radio recognizes BS sync, not a real repeater outbound
                     (use bench_repeater.py for that)
  --ambe FILE        voice content: a file of 9-byte AMBE+2 channel frames (FEC applied, as RadioDesk's recorder
                     writes them, *.ambe), or a DSD .amb file is NOT accepted (it has no FEC). Default: AMBE silence
  --talker-alias T   send talker alias (ISO-8, up to 27 characters) in the embedded LC of odd superframes
  --pi               set the EMB PI bit on voice bursts B-F (a privacy-protected call to a radio without the key)
  --service-options  LC service options byte (hex): 0x80 emergency, 0x40 privacy, 0x08 broadcast, 0x04 OVCM
  --burst-cc CC      color code written into the bursts (default --cc): a wrong-CC test with the bench modem on --cc

listen: logs what the bench modem hears (MS-synced DMO bursts only: the MMDVM_HS simplex receiver does not search for BS or
TDMA direct-mode sync). --json prints one line per burst with data type / voice index, header and terminator LC,
EMB color code and PI, the embedded LC assembled per superframe (call LC or talker alias) and, at the end, a
summary per call. --ambe-out PREFIX writes each call's AMBE frames to PREFIX_<n>.ambe (9-byte FEC frames) and
PREFIX_<n>.amb (DSD .amb: 49 parameter bits per frame, for any AMBE+2 decoder you have; none is in this project).
"""
import argparse
import json
import logging
import time
import zlib

from radiodesk.dmr import ambe as AMBE
from radiodesk.dmr import fec, pdu
from radiodesk_rfgw import layer2 as L2
from radiodesk_rfgw import sim
from radiodesk_rfgw.mmdvm import bursts as B
from radiodesk_rfgw.mmdvm.modem import Modem, ModemConfig, SYNC_AUDIO, SYNC_DATA
from radiodesk_rfgw.mmdvm.radio import Radio, ModemTuner

SLOT_S = 0.060          # one DMO burst every 60 ms
FLCO_GROUP, FLCO_PRIVATE, FLCO_TA_HEADER = 0, 3, 4
TA_ISO8 = 1


def family_sync(family: str) -> int:
    pattern, = [p for name, p, _ in L2.FAMILIES if name == family]
    return pattern


# ── bursts ───────────────────────────────────────────────────────────────────

def call_lc(src: int, dst: int, group: bool, service_options: int) -> bytes:
    return bytes([FLCO_GROUP if group else FLCO_PRIVATE, 0, service_options]) + dst.to_bytes(3, "big") \
        + src.to_bytes(3, "big")


def talker_alias_lcs(text: str):
    """Talker alias header + blocks (TS 102 361-2 7.1.1.4), ISO-8: 6 characters in the header, 7 per block."""
    data = text.encode("latin-1", "replace")[:27]
    head_bits = fec.int_to_bits(TA_ISO8, 2) + fec.int_to_bits(len(data), 5) + [0]   # 8-bit formats skip one bit
    payload = fec.bytes_to_bits(data.ljust(27, b"\x00"))
    out = [bytes([FLCO_TA_HEADER, 0]) + fec.bits_to_bytes(head_bits + payload[:48])]
    rest = payload[48:]
    for i in range(3):
        if len(data) <= 6 + 7 * i:
            break
        out.append(bytes([FLCO_TA_HEADER + 1 + i, 0]) + fec.bits_to_bytes(rest[i * 56:(i + 1) * 56]))
    return out


def read_ambe(path: str):
    raw = open(path, "rb").read()
    if raw[:4] == b".amb":
        raise SystemExit(f"{path}: a DSD .amb file has no FEC; give a 9-byte-per-frame .ambe recording")
    return [raw[i:i + 9] for i in range(0, len(raw) - len(raw) % 9, 9)]


def voice_call(args, src: int):
    """[(control, 33-byte burst)] of one call: header, superframes, terminator (expanded later)."""
    fam = family_sync(args.family)
    cc = args.burst_cc
    so = args.service_options | (0x80 if args.emergency else 0)
    lc = call_lc(src, args.dst, not args.private, so)
    frames = read_ambe(args.ambe) if args.ambe else [sim.AMBE_SILENCE] * (18 * args.superframes)
    superframes = max(1, -(-len(frames) // 18))
    frames += [sim.AMBE_SILENCE] * (18 * superframes - len(frames))
    ta = talker_alias_lcs(args.talker_alias) if args.talker_alias else []
    items = []
    if args.late is None:
        items.append((B.HEADER, L2.lc_burst(lc, pdu.DT_VOICE_LC_HEADER, cc, L2.data_sync(fam))))
    k = 0
    for sf in range(superframes):
        # Embedded LC: the call's in even superframes, a talker alias LC in odd ones (in turn)
        emb_lc = lc if (not ta or sf % 2 == 0) else ta[(sf // 2) % len(ta)]
        emb = fec.embedded_lc_encode(emb_lc)
        for n in range(6):
            bits = fec.bytes_to_bits(b"".join(frames[k:k + 3]))
            k += 3
            if n == 0:
                mid = fec.int_to_bits(fam, 48)
            elif n <= 4:
                mid = L2.voice_middle(cc, (1, 3, 3, 2)[n - 1], emb[(n - 1) * 32:n * 32], pi=int(args.pi))
            else:
                mid = L2.voice_middle(cc, 0, [0] * 32, pi=int(args.pi))
            items.append((SYNC_AUDIO if n == 0 else n, L2.voice_burst(bits, mid)))
    if args.late:
        items = items[args.late:]
    sent = [f for c, b in items if not c & SYNC_DATA for f in fec.ambe_frames(b)]
    logging.info("call from %d: %d AMBE frames, CRC-32 %08X (the radio's RX content check)", src, len(sent),
                 zlib.crc32(b"".join(sent)))
    if args.terminators != 0:
        items.append((B.TERMINATOR, L2.lc_burst(lc, pdu.DT_TERMINATOR_WITH_LC, cc, L2.data_sync(fam))))
    return items


def data_items(args):
    from radiodesk.dmr import packet
    user = bytes((i * 7 + 1) & 0xFF for i in range(args.octets))
    if args.confirmed:
        infos = packet.confirmed_packet(args.src, args.dst, 4, user, ns=1).bursts(preambles=args.preambles)
    else:
        infos = packet.unconfirmed_packet(args.src, args.dst, 4, user, preambles=args.preambles)
    sync = L2.data_sync(family_sync(args.family))
    return [(SYNC_DATA | dt, L2.data_burst(info, dt, args.burst_cc, sync)) for dt, info in infos]


def one_csbk(opcode: int, fid: int, src: int, dst: int, cc: int) -> bytes:
    """A single CSBK (last block): opcode, FID, octets 2-3 zero, target (4-6), source (7-9), CRC with the CSBK mask."""
    head = bytes([0x80 | (opcode & 0x3F), fid, 0, 0]) + dst.to_bytes(3, "big") + src.to_bytes(3, "big")
    info = head + (fec.crc16_ccitt(head) ^ fec.MASK_CSBK).to_bytes(2, "big")
    return L2.data_burst(info, pdu.DT_CSBK, cc, L2.data_sync(L2.MS_VOICE))


def csbk_items(args):
    bursts = B.csbk_preambles(args.src, args.dst, False, args.count, args.burst_cc)
    if args.opcode is not None:
        bursts.append(one_csbk(args.opcode, args.fid, args.src, args.dst, args.burst_cc))
    if args.family != "ms":
        bursts = [tb.burst for tb in sim.resync(bursts, args.family, args.burst_cc)]
    return [(B.CSBK, b) for b in bursts]


def send(modem: Modem, items, headers: int, terminators: int) -> int:
    out = []
    for control, burst in items:
        out += [(control, b) for b in B.expand(control, burst, headers, terminators)]
    # Flow control as MMDVMHost does it: only write while the bench modem reports free DMR space, then wait until it has
    # sent everything (space back to where it started, not transmitting) before closing the port
    space0 = modem.status()["space"]
    i, deadline = 0, time.monotonic() + len(out) * SLOT_S + 10
    last = None
    while time.monotonic() < deadline:
        st = modem.status()
        free = st["space"]
        if st != last:
            logging.debug("status %s, written %d/%d", st, i, len(out))
            last = st
        # Done only by a status read after the last write (else a bench modem that was idle with an empty buffer looks
        # finished right after we fill it, and closing the port leaves the call queued for the next run)
        if i == len(out) and not st["transmitting"] and st["space"] >= space0:
            break
        while free > 0 and i < len(out):
            modem.write_dmr(*out[i])
            i, free = i + 1, free - 1
        time.sleep(0.02)
    time.sleep(0.5)
    logging.info("sent %d bursts", len(out))
    return len(out)


# ── listening ────────────────────────────────────────────────────────────────

class Listener:
    """Decodes what the bench modem hears; JSON lines out, per-call summary and AMBE files at the end."""

    def __init__(self, as_json: bool, ambe_out: str = None):
        self.as_json, self.ambe_out = as_json, ambe_out
        self.calls = []
        self.cur = None
        self.frags = {}
        self.t0 = time.time()

    def emit(self, rec):
        if self.as_json:
            print(json.dumps(rec), flush=True)
        else:
            logging.info("%s", rec)

    def _call(self, src=None, dst=None, flco=None, how="header"):
        if self.cur is None or self.cur.get("ended"):
            self.cur = {"src": src, "dst": dst, "flco": flco, "start": how, "t0": round(time.time(), 3),
                        "headers": 0, "voice": 0, "terminators": 0, "ended": None, "emb_lc": 0, "pi": 0,
                        "talker_alias": None, "ta": {}, "ambe": [], "cc": set(), "after_end": 0}
            self.calls.append(self.cur)
        return self.cur

    def on_rx(self, control: int, burst: bytes, rssi) -> None:
        rec = {"t": round(time.time(), 3), "control": control, "rssi": rssi}
        if control & SYNC_DATA:
            dt = control & 0x0F
            rec["dt"] = dt
            cc, _dt, _ = L2.decode_slot_type(burst)
            rec["cc"] = cc
            if dt in (pdu.DT_VOICE_LC_HEADER, pdu.DT_TERMINATOR_WITH_LC):
                lc = pdu.decode_voice_lc(burst, dt)
                if lc is not None:
                    rec.update(src=lc.src_id, dst=lc.dst_id, flco=lc.flco, so=lc.service_options)
                if dt == pdu.DT_VOICE_LC_HEADER:
                    c = self._call(rec.get("src"), rec.get("dst"), rec.get("flco"))
                    c["headers"] += 1
                elif self.cur is not None:
                    self.cur["terminators"] += 1
                    self.cur["ended"] = self.cur["ended"] or "terminator"
            elif dt == pdu.DT_CSBK:
                cs = pdu.decode_csbk(burst)
                rec["csbk"] = str(cs) if cs is not None else None
            elif dt == 6:
                try:
                    rec["data_header"] = str(pdu.decode_data_header(burst))
                except Exception:
                    rec["data_header"] = None
        else:
            n = 0 if control & SYNC_AUDIO else control & 0x0F
            rec["voice"] = n
            if n:
                cc, pi, lcss, _ = L2.decode_emb(burst)
                rec.update(cc=cc, pi=pi, lcss=lcss)
                if 1 <= n <= 4:
                    self.frags[n] = fec.embedded_signalling(burst)
                if n == 4 and all(k in self.frags for k in (1, 2, 3, 4)):
                    lc9 = fec.embedded_lc_decode(sum((self.frags[k] for k in (1, 2, 3, 4)), []))
                    if lc9 is not None:
                        lc = pdu.parse_full_lc(lc9)
                        rec["emb_lc"] = {"flco": lc.flco, "fid": lc.fid, "src": lc.src_id, "dst": lc.dst_id}
                        self._embedded(lc)
            else:
                self.frags = {}
            c = self.cur
            if c is None or c.get("ended"):
                if c is not None and c["ended"] == "terminator" and time.time() - c["t_last"] < 0.3:
                    c["after_end"] += 1
                else:
                    c = self._call(how="late entry")
            if not c.get("ended"):
                c["voice"] += 1
                c["ambe"] += fec.ambe_frames(burst)
                if rec.get("pi"):
                    c["pi"] += 1
                if rec.get("cc") is not None:
                    c["cc"].add(rec["cc"])
        if self.cur is not None:
            self.cur["t_last"] = time.time()
        self.emit(rec)

    def _embedded(self, lc) -> None:
        c = self.cur
        if c is None:
            return
        if lc.flco in (FLCO_GROUP, FLCO_PRIVATE):
            c["emb_lc"] += 1
            if c["src"] is None:
                c.update(src=lc.src_id, dst=lc.dst_id, flco=lc.flco)
        elif lc.fid == 0 and 4 <= lc.flco <= 7:
            c["ta"][lc.flco] = lc
            hdr = c["ta"].get(4)
            if hdr is not None:
                need = pdu.ta_blocks_needed(hdr)
                blocks = [c["ta"].get(5 + i) for i in range(need)]
                if all(blocks):
                    c["talker_alias"] = pdu.decode_talker_alias(hdr, blocks)

    def on_lost(self, slot) -> None:
        if self.cur is not None and not self.cur.get("ended"):
            self.cur["ended"] = "lost"
        self.emit({"t": round(time.time(), 3), "lost": slot})

    def summary(self) -> None:
        for i, c in enumerate(self.calls, 1):
            frames = c.pop("ambe")
            c.pop("ta")
            c.pop("t_last", None)
            c["cc"] = sorted(c["cc"])
            c["ambe_frames"] = len(frames)
            if frames:
                _, n, errs = AMBE.to_amb(frames)
                c["ambe_fec_bits_corrected"] = errs
                c["ambe_silence_frames"] = sum(f == sim.AMBE_SILENCE for f in frames)
                c["ambe_crc32"] = f"{zlib.crc32(b''.join(frames)):08X}"
                if self.ambe_out:
                    with open(f"{self.ambe_out}_{i}.ambe", "wb") as f:
                        f.write(b"".join(frames))
                    with open(f"{self.ambe_out}_{i}.amb", "wb") as f:
                        f.write(AMBE.to_amb(frames)[0])
                    c["files"] = [f"{self.ambe_out}_{i}.ambe", f"{self.ambe_out}_{i}.amb"]
            self.emit({"call": i, **c})


# ── main ─────────────────────────────────────────────────────────────────────

def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/ttyS0")
    ap.add_argument("--freq", type=float, default=461.6875)
    ap.add_argument("--cc", type=int, default=2)
    ap.add_argument("--power", type=int, default=255, help="MMDVM_HS RF level 0-255")
    ap.add_argument("--rx-offset", type=int, default=-150, help="Hz, the bench modem's calibration (rfgw rx_offset_hz)")
    ap.add_argument("--tx-offset", type=int, default=-175, help="Hz, the bench modem's calibration (rfgw tx_offset_hz)")
    ap.add_argument("-v", "--verbose", action="store_true", help="log every bench modem status change")
    ap.add_argument("--listen-after", type=float, default=0,
                    help="voice / data / csbk: then listen this many seconds (JSON lines), for the radio's answer "
                         "(confirmed data response, call alert ack, a reply call)")
    ap.add_argument("--ambe-out", default=None, help="with --listen-after: AMBE files of what was heard")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p, dst):
        p.add_argument("--src", type=int, default=9990301)
        p.add_argument("--dst", type=int, default=dst)
        p.add_argument("--family", choices=("ms", "bs", "ts1", "ts2"), default="ms")
        p.add_argument("--burst-cc", type=int, default=None)

    v = sub.add_parser("voice")
    common(v, 215)
    v.add_argument("--private", action="store_true")
    v.add_argument("--superframes", type=int, default=3)
    v.add_argument("--headers", type=int, default=None, help="voice LC headers (default: the modem profile's, 8)")
    v.add_argument("--terminators", type=int, default=None, help="0: none, the call just stops")
    v.add_argument("--late", type=int, default=None, help="no header, the first K voice bursts left out")
    v.add_argument("--calls", type=int, default=1)
    v.add_argument("--gap-ms", type=int, default=120)
    v.add_argument("--src2", type=int, default=9990302)
    v.add_argument("--ambe", default=None)
    v.add_argument("--talker-alias", default=None)
    v.add_argument("--pi", action="store_true")
    v.add_argument("--service-options", type=lambda s: int(s, 16), default=0)
    v.add_argument("--emergency", action="store_true")
    d = sub.add_parser("data")
    common(d, 3101276)
    d.add_argument("--confirmed", action="store_true")
    d.add_argument("--octets", type=int, default=40)
    d.add_argument("--preambles", type=int, default=2)
    c = sub.add_parser("csbk")
    common(c, 3101276)
    c.add_argument("--count", type=int, default=4, help="preamble CSBKs (opcode 0x3D)")
    c.add_argument("--opcode", type=lambda x: int(x, 16), default=None,
                   help="then one CSBK with this opcode (hex), e.g. 1F call alert (FID 0)")
    c.add_argument("--fid", type=lambda x: int(x, 16), default=0)
    li = sub.add_parser("listen")
    li.add_argument("--seconds", type=float, default=20)
    li.add_argument("--json", action="store_true")
    args = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO, format="%(asctime)s %(message)s")
    if getattr(args, "burst_cc", None) is None:
        args.burst_cc = args.cc

    import serial
    modem = Modem(serial.Serial(args.port, 115200, timeout=0.1))
    lis = None
    if args.cmd == "listen":
        lis = Listener(args.json, args.ambe_out)
        modem.on_rx = lis.on_rx
        modem.on_lost = lis.on_lost
    else:
        modem.on_rx = lambda control, burst, rssi: logging.info("rx control %02X rssi %s", control, rssi)
    modem.on_nak = lambda cmd, err: logging.warning("NAK cmd %02X err %d", cmd, err)
    modem.start()
    radio = Radio(modem, "mmdvm_hs", ModemConfig(tx_level=50, rx_level=100, tx_delay_ms=100),
                  tuner=ModemTuner(modem, args.rx_offset, args.tx_offset))
    radio.tune(int(round(args.freq * 1e6)), args.cc, args.power)
    time.sleep(1.0)   # MMDVM_HS applies the frequency at SET_CONFIG; data written right after it can be dropped
    logging.info("%s on %.4f MHz cc %d", modem.version, args.freq, args.cc)   # dmr_bench.py waits for "MHz cc"

    try:
        if args.cmd == "voice":
            headers = radio.headers if args.headers is None else args.headers
            terminators = radio.terminators if args.terminators is None else max(1, args.terminators)
            for n in range(args.calls):
                if n:
                    time.sleep(args.gap_ms / 1000)
                src = args.src if n % 2 == 0 else args.src2
                send(modem, voice_call(args, src), headers, terminators)
                logging.info("call %d from %d done", n + 1, src)
        elif args.cmd == "data":
            send(modem, data_items(args), 1, 1)
        elif args.cmd == "csbk":
            send(modem, csbk_items(args), 1, 1)
        else:
            time.sleep(args.seconds)
            lis.summary()
        if args.cmd != "listen" and args.listen_after > 0:
            lis = Listener(True, args.ambe_out)
            modem.on_rx, modem.on_lost = lis.on_rx, lis.on_lost
            time.sleep(args.listen_after)
            lis.summary()
    finally:
        modem.close()


if __name__ == "__main__":
    main()
