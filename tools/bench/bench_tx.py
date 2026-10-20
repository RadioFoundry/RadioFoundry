#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""
Adapted from an earlier bench tool of ours.

Drive the SDR bench's MMDVM_HS modem board by hand as a plain DMR subscriber (simplex) to send known traffic to the radio under
test, or to listen to what the radio sends. Runs on the SDR bench in a Python environment with the odr_sdrbench
package (default /opt/odr-sdrbench/.venv/bin/python): it uses its MMDVM modem driver and its burst and FEC code. The
bench's gateway service must not hold the bench modem while this runs; tools/bench/dmr_bench.py stops it for you.

    bench_tx.py [--freq 461.6875] [--cc 2] [--power 255] voice [--src ID] [--dst ID] [--private] [--superframes N]
              [--headers N] [--terminators N] [--late K] [--calls N] [--gap-ms MS] [--src2 ID]
              [--family ms|bs|ts1|ts2] [--ambe FILE] [--talker-alias TEXT] [--gps LAT,LON[,ERR]] [--pi]
              [--service-options HEX] [--emergency] [--burst-cc CC]
    bench_tx.py ... csbk  [--src ID] [--dst ID] [--count N] [--opcode HEX [--fid HEX] [--octet3 HEX]] [--family ...]
                                            (preamble CSBK train, then optionally one CSBK, e.g. 1F call alert, or
                                            24 --fid 10 --octet3 80 radio check request)
    bench_tx.py ... data  [--src ID] [--dst ID] [--confirmed] [--octets N | --tms TEXT [--tms-seq N] [--tms-no-ack]
                           | --etsi TEXT [--etsi-no-response] [--group] | --ip-text TEXT]
                          [--preambles N] [--family ...]
                                            (--tms: a Motorola TMS text message in IPv4 / UDP 4007 user data;
                                            --ip-text: an ETSI IP text, UTF-16BE in IPv4 / UDP 5016)
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
  --ambe FILE        voice content: a file of 9-byte AMBE+2 channel frames (FEC applied, as odr_sdrbench's recorder
                     writes them, *.ambe), or a DSD .amb file is NOT accepted (it has no FEC). Default: AMBE silence
  --talker-alias T   send talker alias (ISO-8, up to 27 characters) in the embedded LC of odd superframes
  --gps LAT,LON[,E]  send an ETSI GPS Info LC (FLCO 8, TS 102 361-2 7.1.1) in the odd superframes, after the talker
                     alias LCs when both are given; degrees (north / east positive), E the position error code 0-7
  --pi               set the EMB PI bit on voice bursts B-F (a privacy-protected call to a radio without the key)
  --service-options  LC service options byte (hex): 0x80 emergency, 0x40 privacy, 0x08 broadcast, 0x04 OVCM
  --burst-cc CC      color code written into the bursts (default --cc): a wrong-CC test with the bench modem on --cc

listen: logs what the bench modem hears (MS-synced DMO bursts only: the MMDVM_HS simplex receiver does not search for BS or
TDMA direct-mode sync). --json prints one line per burst with data type / voice index, the 12 info octets of CSBKs,
data headers and rate 1/2 blocks ("info", hex, after the BPTC's corrections; "info_raw" as received; "crc_ok" for CSBKs
and data headers), header and terminator LC,
EMB color code and PI, the embedded LC assembled per superframe (call LC, talker alias or GPS Info) and, at the end, a
summary per call (with "gps": the last GPS Info position heard). --ambe-out PREFIX writes each call's AMBE frames to PREFIX_<n>.ambe (9-byte FEC frames) and
PREFIX_<n>.amb (DSD .amb: 49 parameter bits per frame, for any AMBE+2 decoder you have; none is in this project).
"""
import argparse
import json
import logging
import time
import zlib

from odr_sdrbench.dmr import ambe as AMBE
from odr_sdrbench.dmr import fec, pdu
from odr_sdrbench import layer2 as L2
from odr_sdrbench import lcinfo
from odr_sdrbench import sim
from odr_sdrbench.mmdvm import bursts as B
from odr_sdrbench.mmdvm.modem import Modem, ModemConfig, SYNC_AUDIO, SYNC_DATA
from odr_sdrbench.mmdvm.radio import Radio, ModemTuner

SLOT_S = 0.060          # one DMO burst every 60 ms
FLCO_GROUP, FLCO_PRIVATE, FLCO_TA_HEADER = 0, 3, 4
TA_ISO8 = 1
DD_ISO8859_1 = 0x03     # defined data format (TS 102 361-1 Table 9.50); 0x02 is 7-bit characters


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


def gps_lc(lat: float, lon: float, err: int = 1) -> bytes:
    """ETSI GPS Info LC: FLCO 8, FID 0, reserved 4 bits, position error 3 bits, longitude 25 bits (360 / 2^25 degree
    steps), latitude 24 bits (180 / 2^24), both two's complement."""
    lo = max(-(1 << 24), min((1 << 24) - 1, round(lon * (1 << 25) / 360.0))) & 0x1FFFFFF
    la = max(-(1 << 23), min((1 << 23) - 1, round(lat * (1 << 24) / 180.0))) & 0xFFFFFF
    return bytes([8, 0, ((err & 7) << 1) | (lo >> 24)]) + (lo & 0xFFFFFF).to_bytes(3, "big") + la.to_bytes(3, "big")


def gps_decode(lc9: bytes):
    """(lat, lon, err) of a GPS Info LC, None for anything else."""
    lc9 = bytes(lc9)
    if lc9[0] & 0x3F != 8 or lc9[1] != 0:
        return None
    lo = ((lc9[2] & 1) << 24) | int.from_bytes(lc9[3:6], "big")
    la = int.from_bytes(lc9[6:9], "big")
    lo -= (1 << 25) if lo & (1 << 24) else 0
    la -= (1 << 24) if la & (1 << 23) else 0
    return round(la * 180.0 / (1 << 24), 6), round(lo * 360.0 / (1 << 25), 6), (lc9[2] >> 1) & 7


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
    if args.gps:
        g = [float(x) for x in args.gps.split(",")]
        ta.append(gps_lc(g[0], g[1], int(g[2]) if len(g) > 2 else 1))
    items = []
    if args.late is None:
        items.append((B.HEADER, L2.lc_burst(lc, pdu.DT_VOICE_LC_HEADER, cc, L2.data_sync(fam))))
    k = 0
    for sf in range(superframes):
        # Embedded LC: the call's in even superframes, a talker alias (or GPS Info) LC in odd ones (in turn)
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


def ip_checksum(data: bytes) -> int:
    if len(data) % 2:
        data += b"\0"
    s = sum(int.from_bytes(data[i:i + 2], "big") for i in range(0, len(data), 2))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF


def udp_user_data(src: int, dst: int, port: int, payload: bytes) -> bytes:
    """IPv4 / UDP port -> port with the payload, radio IDs as 12.x.x.x (the CAI network), checksums filled in."""
    sip, dip = bytes([12]) + src.to_bytes(3, "big"), bytes([12]) + dst.to_bytes(3, "big")
    udp_len = 8 + len(payload)
    udp = port.to_bytes(2, "big") * 2 + udp_len.to_bytes(2, "big") + b"\0\0" + payload
    csum = ip_checksum(sip + dip + bytes([0, 17]) + udp_len.to_bytes(2, "big") + udp) or 0xFFFF
    udp = udp[:6] + csum.to_bytes(2, "big") + udp[8:]
    total = 20 + udp_len
    ip = bytes([0x45, 0]) + total.to_bytes(2, "big") + b"\0\x01\0\0" + bytes([64, 17]) + b"\0\0" + sip + dip
    ip = ip[:10] + ip_checksum(ip).to_bytes(2, "big") + ip[12:]
    return ip + udp


def tms_user_data(src: int, dst: int, text: str, seq: int, ack: bool) -> bytes:
    """IPv4 / UDP 4007 -> 4007 carrying a TMS simple text message (header 0xE0 with an ACK requested, else 0xA0;
    sequence number with bit 7; encoding 4; UTF-16LE text)."""
    body = bytes([0xE0 if ack else 0xA0, 0x00, 0x80 | (seq & 0x1F), 0x04]) + text.encode("utf-16-le")
    return udp_user_data(src, dst, 4007, len(body).to_bytes(2, "big") + body)


def ip_text_user_data(src: int, dst: int, text: str) -> bytes:
    """An ETSI IP text (TS 102 361-3 5.7.1): the text as UTF-16BE in IPv4 / UDP 5016 -> 5016."""
    return udp_user_data(src, dst, 5016, text.encode("utf-16-be"))


def bptc_info(burst: bytes) -> bytes:
    """The 12 info octets of a BPTC(196,96) burst (CSBK, data header, rate 1/2 block; ETSI TS 102 361-1 B.1.1):
    the 196 bits around the sync, de-interleaved (index * 181 mod 196), data bits taken without correction."""
    bits = [(burst[i // 8] >> (7 - i % 8)) & 1 for i in range(264)]
    raw = bits[0:98] + bits[166:264]
    de = [raw[(i * 181) % 196] for i in range(196)]
    idx = list(range(4, 12)) + [a for r in range(1, 9) for a in range(1 + 15 * r, 12 + 15 * r)]
    data = [de[i] for i in idx]
    return bytes(int("".join(map(str, data[k:k + 8])), 2) for k in range(0, 96, 8))


def etsi_crc32(data: bytes) -> int:
    """The data block CRC-32 (ETSI TS 102 361-1 B.3.9): polynomial 0x04C11DB7, initial 0, over the octets taken in
    swapped pairs (1, 0, 3, 2, ...), the last odd octet paired with a zero; sent least significant octet first."""
    crc = 0
    n = len(data) + (len(data) & 1)
    for i in range(n):
        j = i ^ 1
        crc ^= (data[j] if j < len(data) else 0) << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) if crc & 0x80000000 else (crc << 1)
            crc &= 0xFFFFFFFF
    return crc


def etsi_text_infos(src: int, dst: int, text: str, group: bool, response: bool, preambles: int):
    """An ETSI text message (defined short data: DPF 13, SAP 10, DD format 0x03 ISO 8859-1; TS 102 361-1 V2.7.1
    Figure 8.9, 9.2.12, Table 9.50, where 0x02 is the 7-bit format) as (data type, 12 info octets): preamble CSBKs,
    the header, rate 1/2 blocks with the CRC-32 in the last block."""
    user = text.encode("latin-1", "replace")
    blocks = (len(user) + 4 + 11) // 12
    pad = blocks * 12 - 4 - len(user)
    h = bytes([(0x80 if group else 0) | (0x40 if response and not group else 0) | (((blocks >> 4) & 3) << 4) | 13,
               (10 << 4) | (blocks & 0x0F)]) + dst.to_bytes(3, "big") + src.to_bytes(3, "big") + \
        bytes([(DD_ISO8859_1 << 2) | 1, pad * 8])
    h += (fec.crc16_ccitt(h) ^ 0xCCCC).to_bytes(2, "big")
    body = user + bytes(pad)
    body += etsi_crc32(body).to_bytes(4, "little")
    infos = []
    for k in range(preambles):
        head = bytes([0x80 | 0x3D, 0, 0x80 | (0x40 if group else 0), blocks + 1 + preambles - 1 - k]) + \
            dst.to_bytes(3, "big") + src.to_bytes(3, "big")
        infos.append((pdu.DT_CSBK, head + (fec.crc16_ccitt(head) ^ fec.MASK_CSBK).to_bytes(2, "big")))
    infos.append((6, h))
    infos += [(7, body[i * 12:(i + 1) * 12]) for i in range(blocks)]
    return infos


def data_items(args):
    from odr_sdrbench.dmr import packet
    if args.etsi is not None:
        infos = etsi_text_infos(args.src, args.dst, args.etsi, args.group, not args.etsi_no_response, args.preambles)
        sync = L2.data_sync(family_sync(args.family))
        return [(SYNC_DATA | dt, L2.data_burst(info, dt, args.burst_cc, sync)) for dt, info in infos]
    if args.tms is not None:
        user = tms_user_data(args.src, args.dst, args.tms, args.tms_seq, not args.tms_no_ack)
    elif args.ip_text is not None:
        user = ip_text_user_data(args.src, args.dst, args.ip_text)
    else:
        user = bytes((i * 7 + 1) & 0xFF for i in range(args.octets))
    if args.confirmed:
        infos = packet.confirmed_packet(args.src, args.dst, 4, user, ns=1).bursts(preambles=args.preambles)
    else:
        infos = packet.unconfirmed_packet(args.src, args.dst, 4, user, preambles=args.preambles)
    sync = L2.data_sync(family_sync(args.family))
    return [(SYNC_DATA | dt, L2.data_burst(info, dt, args.burst_cc, sync)) for dt, info in infos]


def one_csbk(opcode: int, fid: int, src: int, dst: int, cc: int, octet3: int = 0) -> bytes:
    """A single CSBK (last block): opcode, FID, octet 2 zero, octet 3, target (4-6), source (7-9), CRC with the CSBK
    mask."""
    head = bytes([0x80 | (opcode & 0x3F), fid, 0, octet3 & 0xFF]) + dst.to_bytes(3, "big") + src.to_bytes(3, "big")
    info = head + (fec.crc16_ccitt(head) ^ fec.MASK_CSBK).to_bytes(2, "big")
    return L2.data_burst(info, pdu.DT_CSBK, cc, L2.data_sync(L2.MS_VOICE))


def csbk_items(args):
    bursts = B.csbk_preambles(args.src, args.dst, False, args.count, args.burst_cc)
    if args.opcode is not None:
        bursts.append(one_csbk(args.opcode, args.fid, args.src, args.dst, args.burst_cc, args.octet3))
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
                        "talker_alias": None, "gps": None, "ta": {}, "ambe": [], "cc": set(), "after_end": 0,
                        "lc_type": None, "lc_types": {}, "alias_asm": lcinfo.AliasAssembler(), "gps_info": None}
            self.calls.append(self.cur)
        return self.cur

    def on_rx(self, control: int, burst: bytes, rssi) -> None:
        rec = {"t": round(time.time(), 3), "control": control, "rssi": rssi}
        if control & SYNC_DATA:
            dt = control & 0x0F
            rec["dt"] = dt
            cc, _dt, _ = L2.decode_slot_type(burst)
            rec["cc"] = cc
            if dt in (pdu.DT_CSBK, 6, 7):     # CSBK, data header, rate 1/2 block: the info octets for tests
                # "info": after the BPTC's Hamming corrections (what a receiver acts on); "info_raw": as received;
                # "crc_ok": the CSBK's or data header's CRC-CCITT over the corrected octets (an overloaded or weak
                # burst can arrive with bit errors the FEC can't fix: tests count only CRC-good ones)
                corrected, fec_ok = fec.bptc196_decode(burst)
                rec["info"] = bytes(corrected[:12]).hex()
                rec["info_raw"] = bptc_info(burst).hex()
                if dt == pdu.DT_CSBK:
                    rec["crc_ok"] = bool(fec_ok and fec.check_crc16(corrected, fec.MASK_CSBK))
                elif dt == 6:
                    rec["crc_ok"] = bool(fec_ok and fec.check_crc16(corrected, fec.MASK_DATA_HEADER))
            if dt in (pdu.DT_VOICE_LC_HEADER, pdu.DT_TERMINATOR_WITH_LC):
                lc = pdu.decode_voice_lc(burst, dt)
                if lc is not None:
                    rec.update(src=lc.src_id, dst=lc.dst_id, flco=lc.flco, so=lc.service_options,
                               lc_type=lcinfo.lc_type(lc.flco, lc.fid))
                if dt == pdu.DT_VOICE_LC_HEADER:
                    c = self._call(rec.get("src"), rec.get("dst"), rec.get("flco"))
                    c["headers"] += 1
                    c["lc_type"] = c["lc_type"] or rec.get("lc_type")
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
                        # decoded by FLCO (lcinfo): type, call LC service options, alias fragment, GPS degrees
                        d = lcinfo.describe(lc9)
                        if d["type"] == "talker_alias_hdr" and self.cur is not None:
                            self.cur["ta_fmt"] = d["format"]
                        lcinfo.fill_fragment(d, (self.cur or {}).get("ta_fmt"))
                        rec["emb_lc"] = {"flco": lc.flco, "fid": lc.fid, "src": lc.src_id, "dst": lc.dst_id, **d}
                        g = gps_decode(lc9)
                        if g is not None:
                            rec["emb_lc"]["gps"] = g
                            if self.cur is not None:
                                self.cur["gps"] = g
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
        t = lcinfo.lc_type(lc.flco, lc.fid)
        c["lc_types"][t] = c["lc_types"].get(t, 0) + 1
        c["alias_asm"].push(lc.raw)
        if t == "gps_info":
            c["gps_info"] = {"lat": round(lc.lat, 6), "lon": round(lc.lon, 6), "position_error": lc.position_error,
                             "position_error_m": pdu.POSITION_ERROR_M.get(lc.position_error)}
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
            c.pop("ta_fmt", None)
            c["alias"] = c.pop("alias_asm").result()      # {text, format_name, length, complete, blocks_seen} or None
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
    ap.add_argument("--rx-offset", type=int, default=None, help="Hz, the bench modem's RX offset (default: the bench's calibration)")
    ap.add_argument("--tx-offset", type=int, default=None, help="Hz, the bench modem's TX offset (default: the bench's calibration)")
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
    v.add_argument("--gps", default=None, help="LAT,LON[,ERR]: an ETSI GPS Info LC in the odd superframes")
    v.add_argument("--pi", action="store_true")
    v.add_argument("--service-options", type=lambda s: int(s, 16), default=0)
    v.add_argument("--emergency", action="store_true")
    d = sub.add_parser("data")
    common(d, 3101276)
    d.add_argument("--confirmed", action="store_true")
    d.add_argument("--octets", type=int, default=40)
    d.add_argument("--preambles", type=int, default=2)
    d.add_argument("--tms", default=None, help="user data: a TMS text message with this text (else --octets bytes)")
    d.add_argument("--tms-seq", type=int, default=1, help="TMS sequence number (5 bits)")
    d.add_argument("--tms-no-ack", action="store_true", help="TMS without an ACK requested")
    d.add_argument("--etsi", default=None, help="an ETSI text message (defined short data, ISO 8859-1) with this text")
    d.add_argument("--etsi-no-response", action="store_true", help="ETSI text without a response requested")
    d.add_argument("--group", action="store_true", help="--etsi to a talkgroup (--dst)")
    d.add_argument("--ip-text", default=None, help="an ETSI IP text (UTF-16BE on UDP 5016) with this text")
    c = sub.add_parser("csbk")
    common(c, 3101276)
    c.add_argument("--count", type=int, default=4, help="preamble CSBKs (opcode 0x3D)")
    c.add_argument("--opcode", type=lambda x: int(x, 16), default=None,
                   help="then one CSBK with this opcode (hex), e.g. 1F call alert (FID 0)")
    c.add_argument("--fid", type=lambda x: int(x, 16), default=0)
    c.add_argument("--octet3", type=lambda x: int(x, 16), default=0, help="CSBK octet 3 (hex): 80 radio check request")
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
