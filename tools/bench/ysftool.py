#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""
ysftool: YSF DN voice on the bench, through the firmware's own code (core/ysf, core/air, core/fec, built for the
host from tools/bench/ysf: `make -C tools/bench/ysf`, done here when the program is missing or older than its sources).

    ysftool.py decode IN [--unpacked | --records] [--json]
    ysftool.py gen OUT.dibits --call N0CALL --dgid 0 --frames 40
    ysftool.py selftest

decode reads symbols through the framer and the call layer and prints the call's start, updates and end, then a
summary: frames, FICH and DCH failures, voice frames. The input is a dibit file (four symbols a byte, the first in
bits 7-6, as `fsk4_bench.py rx --out` writes it), one symbol a byte with --unpacked, or the bench receiver's records
(`fsk4_bench.py rx --records`) with --records. Exit code 0 when a call was heard, else 1.

gen writes a DN voice transmission as the firmware's transmit path builds it (made-up voice frames, radio ID K7Q2Z).
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SRC = os.path.join(HERE, "ysf")
BIN = os.path.join(ROOT, "build", "bench", "ysftool")


def build():
    srcs = [os.path.join(SRC, "ysftool.c"), os.path.join(SRC, "Makefile")]
    for d in ("core/ysf", "core/air", "core/fec"):
        p = os.path.join(ROOT, d)
        srcs += [os.path.join(p, f) for f in os.listdir(p) if f.endswith((".c", ".h"))]
    if not os.path.exists(BIN) or os.path.getmtime(BIN) < max(os.path.getmtime(s) for s in srcs):
        r = subprocess.run(["make", "-s", "-C", SRC], capture_output=True, text=True)
        if r.returncode:
            sys.exit("ysftool: build failed\n" + r.stderr)
    return BIN


def run(args):
    r = subprocess.run([build()] + [str(a) for a in args], capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stderr.strip() or "ysftool: failed")
    return [json.loads(line) for line in r.stdout.splitlines() if line.startswith("{")]


def decode(path, unpacked=False, records=False):
    """The JSON lines of a decode; records: the bench receiver's records (JSON lines) instead of a symbol file"""
    if not records:
        return run(["decode", path] + (["--unpacked"] if unpacked else []))
    import nxdntool         # the records' form is the bench's, not the mode's
    with tempfile.TemporaryDirectory() as tmp:
        return run(["decode", nxdntool.records_to_syms(path, tmp), "--unpacked"])


def summary(lines):
    """(the last call's fields, the stats line), either None when absent"""
    calls = [x for x in lines if x["ev"] in ("call_start", "call_update")]
    stats = next((x for x in lines if x["ev"] == "stats"), None)
    return (calls[-1] if calls else None), stats


def show(lines, as_json):
    for x in lines:
        if as_json:
            print(json.dumps(x))
        elif x["ev"] in ("call_start", "call_update"):
            print("%-11s %s -> %s  DG-ID %02d  radio ID %s%s" % (x["ev"], x["src"] or "?", x["dst"] or "?", x["dgid"],
                                                               x["src_id"] or "?", "  (late)" if x["late"] else ""))
        elif x["ev"] == "call_end":
            print("call_end")
        elif x["ev"] == "stats":
            c = x["call"]
            print("frames %d, FICH failed %d, DCH failed %d, voice frames %d, FEC bits %d" % (
                c["frames"], c["fich_fail"], c["dch_fail"], x["voice_out"], c["fec_bits"]))


def selftest():
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "t.dibits")
        run(["gen", path, "N0CALL", 12, 40])
        call, stats = summary(decode(path))
    ok = call is not None and call["src"] == "N0CALL" and call["dgid"] == 12 and call["src_id"] == "K7Q2Z" and \
        stats["voice_out"] == 40 and stats["call"]["fich_fail"] == 0
    print("ysftool selftest: %s" % ("ok" if ok else "FAILED %r %r" % (call, stats)))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description="YSF DN on the bench through the firmware's own code")
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("decode")
    d.add_argument("path")
    d.add_argument("--unpacked", action="store_true")
    d.add_argument("--records", action="store_true")
    d.add_argument("--json", action="store_true")
    g = sub.add_parser("gen")
    g.add_argument("path")
    g.add_argument("--call", default="N0CALL")
    g.add_argument("--dgid", type=int, default=0)
    g.add_argument("--frames", type=int, default=40)
    sub.add_parser("selftest")
    a = ap.parse_args()
    if a.cmd == "selftest":
        return selftest()
    if a.cmd == "gen":
        show(run(["gen", a.path, a.call, a.dgid, a.frames]), True)
        return 0
    lines = decode(a.path, a.unpacked, a.records)
    show(lines, a.json)
    return 0 if summary(lines)[0] is not None else 1


if __name__ == "__main__":
    sys.exit(main())
