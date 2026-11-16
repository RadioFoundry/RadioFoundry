#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""
nxdntool: NXDN conventional voice on the bench, through the firmware's own code (core/nxdn, core/air, core/fec,
built for the host from tools/bench/nxdn: `make -C tools/bench/nxdn`, done here when the program is missing or older
than its sources).

    nxdntool.py gen OUT.dibits --expect EXP.json [--9600] [--ran 1] [--group N | --unit N] [--src N]
               [--voice-frames 48] [--voice HEX18 | --random-voice SEED] [--emergency] [--lead N] [--tail N]
               [--ber P --seed S] [--unpacked]
    nxdntool.py decode IN.dibits [--9600] [--expect EXP.json] [--ran N] [--unpacked]
    nxdntool.py disc IN.raw --rate HZ [--9600] [--full HZ | --counts-per-khz N] [--invert] [--expect EXP.json] [--ran N]
    nxdntool.py mod IN.dibits OUT.raw [--9600] [--rate HZ] [--full HZ] [--dev-scale PCT] [--offset HZ] [--noise HZ]
               [--invert] [--carrier-gap S] [--seed S]
    nxdntool.py selftest

gen writes a transmission as the firmware's transmit path builds it (nxdn_call_tx): idle symbols as a lead, the
preamble, the header frame, superframes of voice, the release frame, a tail. Voice frames are made-up 72-bit patterns
(--voice, or random with --random-voice), never speech: the VCH is opaque to this tool. The dibit file is the air
interface's packing (four a byte, the first in bits 7-6; 01 +3, 00 +1, 10 -1, 11 -3). --ber flips bits after the
expect file was written. The expect file (JSON) lists every frame with its LICH, symbols, position and VCH.

decode reads symbols through the framer and the call layer and prints every frame (LICH, SACCH, FACCH1 messages, the
four VCH as on air) and the call's start, updates and end. --ran: the receiver's RAN (0, the default, takes all).
With --expect: frames missed, channel bit errors per frame (before FEC), VCH handed on that differ from the ones sent.

disc reads discriminator samples (int16 LE) through the soft modem first: 8 kHz up to ten samples a symbol (24 kHz at
4800 bit/s, 48 kHz at 9600). --full is the deviation at full scale in Hz, --counts-per-khz the tap's scale.

mod models the transmitter (TS 1-A 3.4's pulse) and a discriminator (for tests).

Output: one line per frame and event, a summary; --json prints the program's JSON lines as they are.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SRC = os.path.join(HERE, "nxdn")
BIN = os.path.join(ROOT, "build", "bench", "nxdntool")


def build():
    srcs = [os.path.join(SRC, "nxdntool.c"), os.path.join(SRC, "Makefile")]
    for d in ("core/nxdn", "core/air", "core/fec"):
        p = os.path.join(ROOT, d)
        srcs += [os.path.join(p, f) for f in os.listdir(p) if f.endswith((".c", ".h"))]
    if not os.path.exists(BIN) or os.path.getmtime(BIN) < max(os.path.getmtime(s) for s in srcs):
        r = subprocess.run(["make", "-s", "-C", SRC], capture_output=True, text=True)
        if r.returncode:
            sys.exit("nxdntool: build failed\n" + r.stdout + r.stderr)
    return BIN


def run(args):
    r = subprocess.run([build()] + [str(a) for a in args], capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stderr.strip() or "nxdntool: failed")
    return [json.loads(line) for line in r.stdout.splitlines() if line.startswith("{")]


def bits_differ(a, b):
    return sum(bin(x ^ y).count("1") for x, y in zip(bytes.fromhex(a), bytes.fromhex(b)))


def vcall_text(v):
    kind = {0: "broadcast", 1: "group", 4: "unit", 6: "interconnect", 7: "speed dial"}.get(v["call_type"], v["call_type"])
    cipher = {0: "", 1: " scrambled", 2: " DES", 3: " AES"}.get(v["cipher"], " cipher %d" % v["cipher"])
    emerg = " emergency" if v["cc_opt"] & 0x80 else ""
    return "%s %d -> %d%s%s" % (kind, v["src"], v["dst"], cipher, emerg)


def show(lines, as_json):
    if as_json:
        for x in lines:
            print(json.dumps(x))
        return
    for x in lines:
        ev = x["ev"]
        if ev == "frame":
            lich = x["lich"]
            at = (" at %d" % x["at"]) if "at" in x else ""
            if lich < 0:
                print("frame %d%s: LICH lost" % (x["i"], at))
                continue
            usc, opt = (lich >> 3) & 3, (lich >> 1) & 3
            facch = ", ".join("FACCH1 %d: %s" % (f["half"], ("type %02x (%d fixed)" % (f["type"], f["errs"]))
                                                if f["errs"] >= 0 else "CRC failed") for f in x.get("facch", []))
            sacch = ("SACCH %s (%d fixed)" % (x["sacch"], x["sacch_errs"])) if x["sacch_errs"] >= 0 else "SACCH CRC failed"
            print("frame %d%s: LICH %02x (usc %d, steal %d), %s%s" % (x["i"], at, lich, usc, opt, sacch,
                                                                     (", " + facch) if facch else ""))
        elif ev in ("call_start", "call_update"):
            print("%s: RAN %d%s%s%s" % (ev.replace("_", " "), x["ran"], " late" if x["late"] else "",
                                         (", " + vcall_text(x["vcall"])) if x["have_vcall"] else "",
                                         ", IV seen" if x["iv"] else ""))
        elif ev == "call_end":
            print("call end")
        elif ev == "stats":
            c = x["call"]
            print("frames %d (LICH lost %d), SACCH fails %d, FACCH1 fails %d, %d bits corrected, VCH handed on %d, held %d,"
                  " other RAN %d" % (c["frames"], c["lich_fail"], c["sacch_fail"], c["facch_fail"], c["fec_bits"],
                                     c["voice"], c["voice_held"], x["ran_rejects"]))
        elif ev == "gen":
            print("wrote %d symbols, %d frames, %d voice frames, %d bit errors" % (x["syms"], x["frames"],
                                                                              x["voice_given"], x["bit_errors_applied"]))
        elif ev == "mod":
            print("wrote %d symbols as samples at %d Hz, deviation %.0f Hz" % (x["syms"], x["rate"], x["dev_hz"]))


def compare(lines, exp):
    """What came out against what went in: frames, their channel bits (decode only), the voice in order"""
    frames = [x for x in lines if x["ev"] == "frame"]
    by_at = {f["at"]: f for f in frames if "at" in f}
    r = {"frames": len(exp["frames"]), "found": 0, "chan_bits": 0, "chan_errs": 0}
    if by_at:
        for e in exp["frames"]:
            f = by_at.get(e["at"])
            if f:
                r["found"] += 1
                r["chan_bits"] += 2 * 192
                r["chan_errs"] += bits_differ(f["syms"], e["syms"])
    else:
        r["found"] = min(len(frames), len(exp["frames"]))
    sent = [v for e in exp["frames"] for v in e["voice"]]
    heard = [x["vch"] for x in lines if x["ev"] == "voice"]
    # A late entry hears the end: line up on the first one heard
    start = next((i for i, v in enumerate(sent) if heard and v == heard[0]), 0)
    r["voice_sent"], r["voice_heard"] = len(sent), len(heard)
    r["voice_wrong"] = sum(1 for i, v in enumerate(heard) if start + i >= len(sent) or sent[start + i] != v)
    # (raw channel bits: the VCH's own FEC is the voice path's)
    r["voice_bits_wrong"] = sum(bits_differ(v, sent[start + i]) for i, v in enumerate(heard) if start + i < len(sent))
    r["voice_first"] = start
    starts = [x for x in lines if x["ev"] in ("call_start", "call_update") and x["have_vcall"]]
    r["vcall_ok"] = bool(starts) and starts[-1]["vcall"] == exp["vcall"]
    r["ended"] = any(x["ev"] == "call_end" for x in lines)
    return r


def compare_text(r):
    s = "frames %d of %d" % (r["found"], r["frames"])
    if r["chan_bits"]:
        s += ", channel bit errors %d (%.2f %%)" % (r["chan_errs"], 100.0 * r["chan_errs"] / r["chan_bits"])
    s += ", voice %d of %d heard (from %d), %d wrong (%d bits), call fields %s, %s" % (
        r["voice_heard"], r["voice_sent"], r["voice_first"], r["voice_wrong"], r["voice_bits_wrong"], "match" if r["vcall_ok"] else "DIFFER",
        "ended" if r["ended"] else "NOT ENDED")
    return s


def pass_opts(a, names):
    out = []
    for n in names:
        v = getattr(a, n.lstrip("-").replace("-", "_").replace("9600", "r9600"), None)
        if v is True:
            out.append(n)
        elif v not in (None, False):
            out += [n, v]
    return out


GEN_OPTS = ["--9600", "--ran", "--group", "--unit", "--src", "--voice-frames", "--voice", "--random-voice",
            "--emergency", "--lead", "--tail", "--ber", "--seed", "--unpacked"]
MOD_OPTS = ["--9600", "--rate", "--full", "--dev-scale", "--offset", "--noise", "--invert", "--carrier-gap", "--seed",
            "--unpacked"]


def report(lines, a):
    show(lines, a.json)
    if a.expect:
        with open(a.expect) as f:
            r = compare(lines, json.load(f))
        print(json.dumps({"ev": "compare", **r}) if a.json else compare_text(r))
        return r
    return None


def cmd_gen(a):
    show(run(["gen", a.out, a.expect] + pass_opts(a, GEN_OPTS)), a.json)


def cmd_decode(a):
    report(run(["decode", a.input] + pass_opts(a, ["--9600", "--ran", "--unpacked"])), a)


def cmd_disc(a):
    report(run(["disc", a.input] + pass_opts(a, ["--9600", "--ran", "--rate", "--full", "--counts-per-khz", "--invert"])), a)


def cmd_mod(a):
    show(run(["mod", a.input, a.out] + pass_opts(a, MOD_OPTS)), a.json)


def cmd_selftest(a):
    fails = []
    with tempfile.TemporaryDirectory() as tmp:
        def p(n):
            return os.path.join(tmp, n)

        def check(what, ok, r):
            print("%s %s: %s" % ("ok  " if ok else "FAIL", what, compare_text(r) if isinstance(r, dict) else r))
            if not ok:
                fails.append(what)

        def gen(name, *opts):
            run(["gen", p(name + ".dibits"), p(name + ".json")] + list(opts))
            with open(p(name + ".json")) as f:
                return json.load(f)

        def cmp(name, lines):
            with open(p(name + ".json")) as f:
                return compare(lines, json.load(f))

        # (the noise is per sample: the same figure is more noise per symbol at 2400 symbols/s)
        for rate, extra, srate, noise in ((4800, [], 24000, 25), (9600, ["--9600"], 48000, 50)):
            e = gen("clean%d" % rate, "--random-voice", "5", "--ran", "7", "--src", "1001", "--group", "77", "--lead",
                    "200", "--tail", "200", *extra)
            r = cmp("clean%d" % rate, run(["decode", p("clean%d.dibits" % rate), "--ran", "7"] + extra))
            check("%d clean" % rate, r["found"] == r["frames"] and r["chan_errs"] == 0 and r["voice_wrong"] == 0 and
                  r["voice_heard"] == r["voice_sent"] and r["vcall_ok"] and r["ended"], r)
            lines = run(["decode", p("clean%d.dibits" % rate), "--ran", "8"] + extra)
            st = [x for x in lines if x["ev"] == "stats"][0]
            check("%d another RAN" % rate, not any(x["ev"] == "call_start" for x in lines) and st["ran_rejects"] > 0,
                  "%d frames dropped" % st["ran_rejects"])
            gen("ber%d" % rate, "--random-voice", "5", "--ran", "7", "--lead", "200", "--tail", "200", "--ber", "0.005",
                "--seed", "3", *extra)
            lines = run(["decode", p("ber%d.dibits" % rate)] + extra)
            r = cmp("ber%d" % rate, lines)
            st = [x for x in lines if x["ev"] == "stats"][0]
            check("%d bit errors 0.5 %%" % rate, r["vcall_ok"] and r["voice_heard"] >= r["voice_sent"] - 4 and
                  st["call"]["fec_bits"] > 0, r)
            run(["mod", p("clean%d.dibits" % rate), p("clean%d.raw" % rate), "--rate", srate, "--noise", noise] + extra)
            r = cmp("clean%d" % rate, run(["disc", p("clean%d.raw" % rate), "--rate", srate, "--full", "5000"] + extra))
            check("%d through the soft modem at %d Hz, noise %d Hz" % (rate, srate, noise), r["voice_wrong"] == 0 and
                  r["voice_heard"] == r["voice_sent"] and r["vcall_ok"] and r["ended"], r)
        e = gen("unit", "--unit", "2002", "--src", "1001", "--emergency", "--voice-frames", "8")
        r = cmp("unit", run(["decode", p("unit.dibits")]))
        check("unit call, emergency", r["vcall_ok"] and e["vcall"]["call_type"] == 4 and e["vcall"]["cc_opt"] & 0x80, r)
    print("selftest: %s" % ("all passed" if not fails else "%d FAILED" % len(fails)))
    return 1 if fails else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="NXDN conventional voice through the firmware's code (see the module text)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(s):
        s.add_argument("--9600", dest="r9600", action="store_true", help="9600 bit/s (4800 symbols/s)")
        s.add_argument("--json", action="store_true")

    s = sub.add_parser("gen")
    s.add_argument("out")
    s.add_argument("--expect", required=True)
    common(s)
    for n in ("--ran", "--group", "--unit", "--src", "--voice-frames", "--random-voice", "--lead", "--tail", "--seed"):
        s.add_argument(n, type=int)
    s.add_argument("--voice")
    s.add_argument("--ber", type=float)
    s.add_argument("--emergency", action="store_true")
    s.add_argument("--unpacked", action="store_true")
    s.set_defaults(fn=cmd_gen)

    s = sub.add_parser("decode")
    s.add_argument("input")
    s.add_argument("--expect")
    s.add_argument("--ran", type=int)
    s.add_argument("--unpacked", action="store_true")
    common(s)
    s.set_defaults(fn=cmd_decode)

    s = sub.add_parser("disc")
    s.add_argument("input")
    s.add_argument("--expect")
    s.add_argument("--ran", type=int)
    s.add_argument("--rate", type=int, required=True)
    s.add_argument("--full", type=float)
    s.add_argument("--counts-per-khz", type=float)
    s.add_argument("--invert", action="store_true")
    common(s)
    s.set_defaults(fn=cmd_disc)

    s = sub.add_parser("mod")
    s.add_argument("input")
    s.add_argument("out")
    for n in ("--rate", "--seed"):
        s.add_argument(n, type=int)
    for n in ("--full", "--dev-scale", "--offset", "--noise", "--carrier-gap"):
        s.add_argument(n, type=float)
    s.add_argument("--invert", action="store_true")
    s.add_argument("--unpacked", action="store_true")
    common(s)
    s.set_defaults(fn=cmd_mod)

    s = sub.add_parser("selftest")
    s.set_defaults(fn=cmd_selftest)
    a = ap.parse_args(argv)
    return a.fn(a) or 0


if __name__ == "__main__":
    sys.exit(main())
