#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""
nxdntool: NXDN conventional voice on the bench, through the firmware's own code (core/nxdn, core/air, core/fec,
built for the host from tools/bench/nxdn: `make -C tools/bench/nxdn`, done here when the program is missing or older
than its sources).

    nxdntool.py gen OUT.dibits --expect EXP.json [--9600] [--ran 1] [--group N | --unit N] [--src N]
               [--voice-frames 48] [--voice HEX18 | --random-voice SEED | --voice-file FILE] [--emergency]
               [--lead N] [--tail N]
               [--ber P --seed S] [--unpacked]
    nxdntool.py decode IN.dibits [--9600] [--expect EXP.json] [--ran N] [--unpacked | --records | --air-jsonl]
    nxdntool.py disc IN.raw --rate HZ [--9600] [--full HZ | --counts-per-khz N] [--invert] [--expect EXP.json] [--ran N]
    nxdntool.py mod IN.dibits OUT.raw [--9600] [--rate HZ] [--full HZ] [--dev-scale PCT] [--offset HZ] [--noise HZ]
               [--invert] [--carrier-gap S] [--seed S]
    nxdntool.py selftest

gen writes a transmission as the firmware's transmit path builds it (nxdn_call_tx): idle symbols as a lead, the
preamble, the header frame, superframes of voice, the release frame, a tail. Voice frames are made-up 72-bit patterns
(--voice, or random with --random-voice), or the 72-bit frames of a file as they came off the air (--voice-file, 9
bytes each; its last one repeats): the VCH is opaque to this tool either way. The dibit file is the air
interface's packing (four a byte, the first in bits 7-6; 01 +3, 00 +1, 10 -1, 11 -3). --ber flips bits after the
expect file was written. The expect file (JSON) lists every frame with its LICH, symbols, position and VCH.

decode reads symbols through the framer and the call layer and prints every frame (LICH, SACCH, FACCH1 messages, the
four VCH as on air) and the call's start, updates and end. The input is what p25tool.py decode reads: a dibit file
(as `fsk4_bench.py rx --out` writes it), one symbol a byte with --unpacked, the bench receiver's records
(`fsk4_bench.py rx --records`, their sliced symbols in order) with --records, or `rfoundry_debug.py air rx` records with
--air-jsonl. --ran: the receiver's RAN (0, the default, takes all). With --expect: frames missed, channel bit errors
per frame (before FEC), VCH handed on that differ from the ones sent. The last line sums it up: RAN, source,
destination, call type, frames, voice frames, CRC failures. Exit code 0 when a call was heard (and, with --expect, its
fields match), else 1.

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


def summary(lines):
    """One line for an off-air check: the last call's RAN, source, destination and type, frames, voice frames handed
    on, CRC failures (SACCH and FACCH1) and LICH losses"""
    calls = [x for x in lines if x["ev"] in ("call_start", "call_update")]
    vc = next((x["vcall"] for x in reversed(calls) if x["have_vcall"]), None)
    st = next((x for x in lines if x["ev"] == "stats"), None)
    c = st["call"] if st else {}
    return {"calls": sum(1 for x in lines if x["ev"] == "call_start"),
            "ran": calls[-1]["ran"] if calls else None,
            "src": vc["src"] if vc else None, "dst": vc["dst"] if vc else None,
            "type": vcall_text(vc).split(" ")[0] if vc else None,
            "emergency": bool(vc["cc_opt"] & 0x80) if vc else None,
            "frames": c.get("frames", 0), "voice": c.get("voice", 0), "voice_held": c.get("voice_held", 0),
            "crc_fail": c.get("sacch_fail", 0) + c.get("facch_fail", 0), "lich_fail": c.get("lich_fail", 0),
            "fec_bits": c.get("fec_bits", 0), "other_ran": st["ran_rejects"] if st else 0}


def summary_text(m):
    if not m["calls"]:
        return "no call: frames %d, other RAN %d" % (m["frames"], m["other_ran"])
    return ("RAN %s, src %s, dst %s, %s%s; frames %d, voice frames %d (held %d), CRC failures %d, LICH lost %d"
            % (m["ran"], m["src"], m["dst"], m["type"], " emergency" if m["emergency"] else "", m["frames"],
               m["voice"], m["voice_held"], m["crc_fail"], m["lich_fail"]))


def records_to_syms(path, tmp):
    """Records (JSON lines) to one symbol a byte: `rfoundry_debug.py air rx` chunks ("hard", "nsyms") or the bench
    receiver's records (a "rec" with its sliced symbols as hex in "dibits", "syms" or "symbols", and "n"; or the
    bench API's {"dibits": {"n", "hex"}}). Packed four a byte, the first in bits 7-6"""
    out, gaps = bytearray(), 0
    with open(path) as f:
        for line in f:
            if not line.strip():
                continue
            d = json.loads(line)
            if isinstance(d.get("hard"), str):
                h, n = d["hard"], d["nsyms"]
                gaps += 1 if "GAP" in (d.get("flags") or []) else 0
            else:
                r = d.get("rec") or {}
                key = next((k for k in ("dibits", "syms", "symbols") if isinstance(r.get(k), (str, dict))), None)
                if key is None:
                    continue
                if isinstance(r[key], dict):    # the bench API's form: {"dibits": {"t", "k", "n", "hex"}}
                    h, r = r[key].get("hex"), r[key]
                    if not isinstance(h, str):
                        continue
                else:
                    h = r[key]
                n = r.get("n") or r.get("nsyms") or 4 * (len(h) // 2)
            b = bytes.fromhex(h)
            out += bytes((b[i >> 2] >> (6 - 2 * (i & 3))) & 3 for i in range(n))
    sp = os.path.join(tmp, "rec.syms")
    with open(sp, "wb") as f:
        f.write(out)
    if gaps > 1:
        print("note: %d chunks say symbols were lost before them; the symbols are joined" % gaps, file=sys.stderr)
    return sp


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
            "--voice-file",
            "--emergency", "--lead", "--tail", "--ber", "--seed", "--unpacked"]
MOD_OPTS = ["--9600", "--rate", "--full", "--dev-scale", "--offset", "--noise", "--invert", "--carrier-gap", "--seed",
            "--unpacked"]


def report(lines, a):
    show(lines, a.json)
    ok = True
    if a.expect:
        with open(a.expect) as f:
            r = compare(lines, json.load(f))
        print(json.dumps({"ev": "compare", **r}) if a.json else compare_text(r))
        ok = r["vcall_ok"]
    m = summary(lines)
    print(json.dumps({"ev": "summary", **m}) if a.json else summary_text(m))
    return 0 if ok and m["calls"] else 1


def cmd_gen(a):
    show(run(["gen", a.out, a.expect] + pass_opts(a, GEN_OPTS)), a.json)


def cmd_decode(a):
    if a.records or a.air_jsonl:
        with tempfile.TemporaryDirectory() as tmp:
            sp = records_to_syms(a.input, tmp)
            return report(run(["decode", sp, "--unpacked"] + pass_opts(a, ["--9600", "--ran"])), a)
    return report(run(["decode", a.input] + pass_opts(a, ["--9600", "--ran", "--unpacked"])), a)


def cmd_disc(a):
    return report(run(["disc", a.input] + pass_opts(a, ["--9600", "--ran", "--rate", "--full", "--counts-per-khz", "--invert"])), a)


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
        # The same call as the bench receiver's records, cut into chunks that end inside a byte
        with open(p("clean4800.dibits"), "rb") as f:
            packed = f.read()
        syms = [(packed[i >> 2] >> (6 - 2 * (i & 3))) & 3 for i in range(4 * len(packed))]
        with open(p("rec.jsonl"), "w") as f:
            for i in range(0, len(syms), 999):
                part = syms[i:i + 999]
                b = bytearray((len(part) + 3) // 4)
                for j, d in enumerate(part):
                    b[j >> 2] |= d << (6 - 2 * (j & 3))
                f.write(json.dumps({"rec": {"dibits": b.hex(), "n": len(part)}}) + "\n")
            f.write(json.dumps({"rec": {"sync": 1}}) + "\n")
        with tempfile.TemporaryDirectory() as t2:
            r = cmp("clean4800", run(["decode", records_to_syms(p("rec.jsonl"), t2), "--unpacked", "--ran", "7"]))
        check("4800 from bench records", r["voice_wrong"] == 0 and r["voice_heard"] == r["voice_sent"] and r["vcall_ok"],
              r)
        m = summary(run(["decode", p("clean4800.dibits")]))
        check("summary", m["calls"] == 1 and m["ran"] == 7 and m["src"] == 1001 and m["dst"] == 77 and
              m["type"] == "group" and m["crc_fail"] == 0 and m["voice"] > 0, summary_text(m))
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
    s.add_argument("--voice-file")
    s.add_argument("--ber", type=float)
    s.add_argument("--emergency", action="store_true")
    s.add_argument("--unpacked", action="store_true")
    s.set_defaults(fn=cmd_gen)

    s = sub.add_parser("decode")
    s.add_argument("input")
    s.add_argument("--expect")
    s.add_argument("--ran", type=int)
    g = s.add_mutually_exclusive_group()
    g.add_argument("--unpacked", action="store_true", help="one symbol a byte")
    g.add_argument("--records", action="store_true", help="fsk4_bench.py rx --records")
    g.add_argument("--air-jsonl", action="store_true", help="rfoundry_debug.py air rx records")
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
