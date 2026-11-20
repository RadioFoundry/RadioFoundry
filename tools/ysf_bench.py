#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Yaesu System Fusion on the bench (docs/testing/ysf-bench.md): drive the radio over its debug link, capture what it
sends with an SDR and decode it with the independent reference decoder (tools/ysf_ref.py), make signals to replay into
it, and keep a record of each step. Every run writes a JSON record in --out, so a session leaves the numbers.

    tools/ysf_bench.py view                              the radio's ysf.* keys
    tools/ysf_bench.py watch [--seconds 120]             receive: a line per transmission heard (callsigns, mode,
                                                         DG-ID, frames, sync bit errors, whitening order, offsets)
    tools/ysf_bench.py over --mhz F --allow-rf [--seconds 4] [--sdr-mhz F] [--capture CMD]
                                                         one over of ours, keyed over the debug link and captured;
                                                         decoded by the reference: callsigns, levels in Hz, polarity
    tools/ysf_bench.py analyze CAPTURE --offset HZ       a capture again, offline
    tools/ysf_bench.py synth OUT [--rate R] [--src CALL] [--dgid N] [--level DB]
                                                         a DN transmission as I/Q for an SDR transmitter (replay
                                                         into the radio through attenuation), or --audio as a WAV
    tools/ysf_bench.py session [--out DIR]               the on-air checklist with a visiting Yaesu radio: prompts
                                                         each step, snapshots the radio's keys, records the answers

Nothing keys without --allow-rf (AGENTS.md rule 8): `over` refuses, the others never key. The SDR command defaults to
rtl_sdr; --capture takes a template with {hz} {rate} {n} {out}. The SDR sits --sdr-mhz (default 25 kHz below the
channel, off its DC spike); the decoder is told the offset.
"""
import argparse
import json
import os
import shlex
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))

RATE = 1536000
DEFAULT_CAPTURE = "rtl_sdr -f {hz} -s {rate} -n {n} {out}"
OUTER_HZ = 2700.0           # a wide +3 symbol (docs/spec/ysf-modem.md 2.1)


def out_dir(args):
    d = args.out or os.path.join(os.environ.get("TMPDIR", "/tmp"), "ysf-bench")
    os.makedirs(d, exist_ok=True)
    return d


def record(args, name, data):
    path = os.path.join(out_dir(args), "%s-%s.json" % (name, time.strftime("%Y%m%d-%H%M%S")))
    with open(path, "w") as f:
        json.dump(data, f, indent=1, default=str)
    print("record: %s" % path)
    return path


def link(args):
    import rfoundry_debug
    return rfoundry_debug.DebugLink(args.port) if args.port else rfoundry_debug.DebugLink()


def ysf_keys(dl):
    """The ysf.* keys of the ysf view (docs/spec/ysf-modem.md 5; docs/spec/debuglink.md 3, view 40)."""
    v = dl.view(["ysf"])
    return {k[4:]: v[k] for k in v if k.startswith("ysf.")}


def num(d, k):
    try:
        return int(d.get(k, 0))
    except ValueError:
        return 0


# --- view, watch ---

def cmd_view(args):
    k = ysf_keys(link(args))
    if k.get("built") != "1":
        print("this firmware has no Fusion (ysf.built=0): build with CONFIG_YSF")
        return 1
    for key in sorted(k):
        print("ysf.%s=%s" % (key, k[key]))
    return 0


def over_line(a, b):
    """One heard transmission from two snapshots of the keys (before and after)."""
    bits = num(b, "sync_bits") - num(a, "sync_bits")
    errs = num(b, "sync_errors") - num(a, "sync_errors")
    return ("%s > %s  %s DG-ID %s%s  frames %d played %d  FICH bad %d  DCH good %d bad %d  whiten-all %d  "
            "sync BER %s  %s  %s ppm  %s Hz" % (
                b.get("src") or "?", b.get("dest") or "?", b.get("mode"), b.get("dgid"),
                " (muted)" if b.get("muted") == "1" else "",
                num(b, "rx_frames") - num(a, "rx_frames"), num(b, "rx_played") - num(a, "rx_played"),
                num(b, "fich_bad") - num(a, "fich_bad"), num(b, "csd_good") - num(a, "csd_good"),
                num(b, "csd_bad") - num(a, "csd_bad"), num(b, "order_all") - num(a, "order_all"),
                "%.1e" % (errs / bits) if bits else "-", "inverted" if b.get("rx_invert") == "1" else "upright",
                b.get("ppm"), b.get("offset_hz")))


def cmd_watch(args):
    dl = link(args)
    k = ysf_keys(dl)
    if k.get("on") != "1":
        print("not on a Fusion channel (ysf.on=0, fault %s): select one first" % k.get("fault"))
        return 1
    log, start, before, prev, in_over = [], time.time(), k, k, False
    print("watching %d s; key the Fusion radio" % args.seconds)
    while time.time() - start < args.seconds:
        time.sleep(0.25)
        now = ysf_keys(dl)
        rx = now.get("rx") == "1"
        if rx and not in_over:
            in_over, before = True, prev
        if in_over and not rx:
            in_over = False
            line = over_line(before, now)
            print(line)
            log.append({"before": before, "after": now, "line": line})
        prev = now
    record(args, "watch", {"overs": log, "end": ysf_keys(dl)})
    return 0 if log else 1


# --- capture and our over ---

def capture(template, hz, seconds, out):
    n = int(RATE * seconds)
    cmd = template.format(hz=int(hz), rate=RATE, n=n, out=shlex.quote(out))
    return subprocess.Popen(cmd, shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def summarize(recs, expect_src=None):
    """Checks on the decoded frames of our over; returns (lines, ok)."""
    import statistics
    lines, ok = [], True
    good = [r for r in recs if r["fich"] is not None]
    lines.append("frames %d, FICH good %d" % (len(recs), len(good)))
    if not good:
        return lines + ["FAIL: no frame decoded (polarity, deviation scale, the route: ysf-bench.md 3)"], False
    heads = [r for r in good if r["fich"]["fi"] == 0]
    terms = [r for r in good if r["fich"]["fi"] == 2]
    lines.append("headers %d, terminators %d" % (len(heads), len(terms)))
    src = None
    for r in heads + terms:
        if r.get("csd1"):
            src = r["csd1"][10:].decode("ascii", "replace").strip()
            lines.append("CSD1: to %r from %r (%s order)" % (r["csd1"][:10].decode("ascii", "replace").strip(), src,
                                                             r["csd1_order"]))
            break
    if expect_src is not None and src != expect_src:
        ok = False
        lines.append("FAIL: source %r, the radio says it sends %r" % (src, expect_src))
    outer = statistics.median(r["outer_hz"] for r in good)
    inner = statistics.median(r["inner_hz"] for r in good)
    lines.append("levels: inner %.0f Hz, outer %.0f Hz, ratio %.2f (3.00); outer against wide 2700 Hz: %+.0f %%" % (
        inner, outer, outer / inner, 100 * (outer / OUTER_HZ - 1)))
    lines.append("tx_1khz to set: the current value x %.3f (docs/testing/ysf-bench.md 3)" % (OUTER_HZ / outer))
    lines.append("polarity: %s" % ("inverted: set tx_invert" if good[0]["inverted"] else "upright"))
    lines.append("sync bit errors over the frames: %d of %d" % (sum(r["sync_errors"] for r in good), 40 * len(good)))
    if not heads:
        ok = False
        lines.append("FAIL: no header decoded (the first may be lost to key-up; two are sent)")
    if not terms:
        ok = False
        lines.append("FAIL: no terminator decoded")
    if abs(outer / inner - 3.0) > 0.3:
        ok = False
        lines.append("FAIL: the level ratio is off 3: the route clips or isn't flat")
    return lines, ok


def cmd_over(args):
    if not args.allow_rf:
        print("over transmits: add --allow-rf (a dummy load, the lowest power, a frequency you may use)")
        return 2
    import ysf_ref
    dl = link(args)
    k = ysf_keys(dl)
    if k.get("on") != "1" or k.get("no_call") == "1" or k.get("fault") != "ok":
        print("the radio isn't ready: ysf.on=%s fault=%s no_call=%s" % (k.get("on"), k.get("fault"), k.get("no_call")))
        return 1
    sdr_mhz = args.sdr_mhz if args.sdr_mhz else args.mhz - 0.025
    offset = (args.mhz - sdr_mhz) * 1e6
    cap = os.path.join(out_dir(args), "over-%s.cs8" % time.strftime("%Y%m%d-%H%M%S"))
    p = capture(args.capture, sdr_mhz * 1e6, args.seconds + 2.0, cap)
    time.sleep(0.5)
    dl.ptt_for(args.seconds)
    p.wait()
    after = ysf_keys(dl)
    recs = ysf_ref.decode_records(cap, offset=offset)
    lines, ok = summarize(recs, k.get("tx_src"))
    for line in lines:
        print(line)
    record(args, "over", {"capture": cap, "offset_hz": offset, "before": k, "after": after, "summary": lines,
                          "ok": ok, "frames": [{x: r[x] for x in r if x not in ("voice",)} for r in recs]})
    return 0 if ok else 1


def cmd_analyze(args):
    import ysf_ref
    recs = ysf_ref.decode_records(args.capture, offset=args.offset, fmt=args.format, rate=args.rate)
    for r in recs:
        for line in ysf_ref.describe(r):
            print(line)
    lines, ok = summarize(recs, args.expect_src)
    for line in lines:
        print(line)
    return 0 if ok else 1


def cmd_synth(args):
    import ysf_ref
    a = argparse.Namespace(out=args.out_file, src=args.src, dest=args.dest, frames=args.frames, headers=2,
                           dgid=args.dgid, order=args.order, rate=args.rate, dev=900.0, offset=args.offset,
                           amp=10 ** (args.level / 20.0), audio=args.audio, format=args.format)
    ysf_ref.synth(a)
    print("replay it into the radio through enough attenuation for a weak signal (ysf-bench.md 2.3), then watch")
    return 0


# --- the on-air session with a visiting Yaesu radio ---

STEPS = [
    ("setup", "Both radios on the test frequency, lowest power, a metre apart. The Yaesu radio in DN, DG-ID 0, "
     "its callsign set; ours on a Fusion channel (DG-ID 0 both ways). Note the Yaesu model and firmware.", False),
    ("rx_dn", "The Yaesu radio sends 5 s of speech (DN). Did ours show its callsign and play clear audio?", True),
    ("rx_late", "Start the Yaesu over, then switch ours to the channel 2 s into it. Callsign within a second? Audio?",
     True),
    ("rx_dgid", "Set the Yaesu radio to DG-ID 20 and send. Does ours show DG-ID 20? Set ours to hear 20, send again: "
     "plays? Set ours to hear 30: shown but muted?", True),
    ("rx_vw", "The Yaesu radio in VW (voice full rate), 5 s of speech. Ours shows VW and plays it clear, no gaps "
     "(ysf.rx_concealed hardly grows)?", True),
    ("tx_dn", "Ours sends 5 s of speech (PTT). Does the Yaesu show our callsign and play clear audio? Any squelch "
     "or DG-ID it needed?", True),
    ("tx_vw", "Ours on a channel set to Voice sent VW (the Codeplug Editor): 5 s of speech. Does the Yaesu show VW, "
     "our callsign and play clear audio? (ysf.tx_vw=1, ysf.tx_underruns 0)", True),
    ("tx_dgid", "Ours set to send DG-ID 20, the Yaesu to DG-ID 20: heard? The Yaesu to 30: silent?", True),
    ("tx_short", "Three 1 s overs from ours in a row: each heard whole on the Yaesu, its first word not clipped?", True),
    ("tx_weak", "Walk the Yaesu radio away (or add attenuation) until ours breaks up on it, then theirs on ours: "
     "note where each goes, roughly", True),
    ("rpt", "Optional, through a Fusion repeater or hotspot in range: ours heard on the output, its callsign shown "
     "(the down / uplink fields)?", True),
]


def cmd_session(args):
    dl = None
    try:
        dl = link(args)
    except Exception as e:  # the session still records answers without the radio's keys
        print("no debug link (%s): answers only" % e)
    log = {"started": time.strftime("%Y-%m-%d %H:%M:%S"), "steps": []}
    for name, text, keys in STEPS:
        print("\n== %s: %s" % (name, text))
        before = ysf_keys(dl) if dl and keys else None
        ans = input("  result (y / n / s skip), then Enter: ").strip().lower()[:1]
        note = input("  notes (what you saw, settings changed): ").strip()
        after = ysf_keys(dl) if dl and keys else None
        step = {"step": name, "result": ans, "note": note, "before": before, "after": after}
        if before and after:
            step["line"] = over_line(before, after)
            print("  " + step["line"])
        log["steps"].append(step)
        record(args, "session", log)    # after every step: a session cut short keeps what it did
    print("\ndone: send the session record and the capture files to the developer")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", help="the debug link's serial port (default: rfoundry_debug's)")
    ap.add_argument("--out", help="directory for records and captures (default $TMPDIR/ysf-bench)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("view")
    w = sub.add_parser("watch")
    w.add_argument("--seconds", type=float, default=120)
    o = sub.add_parser("over")
    o.add_argument("--mhz", type=float, required=True, help="the channel's frequency")
    o.add_argument("--sdr-mhz", type=float, help="the SDR's center (default 25 kHz below)")
    o.add_argument("--seconds", type=float, default=4.0)
    o.add_argument("--capture", default=DEFAULT_CAPTURE, help="the SDR command template")
    o.add_argument("--allow-rf", action="store_true")
    a = sub.add_parser("analyze")
    a.add_argument("capture")
    a.add_argument("--offset", type=float, default=0.0)
    a.add_argument("--format", default="cs8")
    a.add_argument("--rate", type=float)
    a.add_argument("--expect-src")
    s = sub.add_parser("synth")
    s.add_argument("out_file")
    s.add_argument("--rate", type=int, default=2000000, help="the transmitting SDR's rate (HackRF 2 Msps)")
    s.add_argument("--src", default="N0CALL")
    s.add_argument("--dest", default="ALL")
    s.add_argument("--frames", type=int, default=50)
    s.add_argument("--dgid", type=int, default=0)
    s.add_argument("--order", choices=("data", "all"), default="data")
    s.add_argument("--offset", type=float, default=0.0)
    s.add_argument("--level", type=float, default=-3.0, help="I/Q level, dB of full scale")
    s.add_argument("--audio", action="store_true")
    s.add_argument("--format", choices=("cs8", "cs16"), default="cs8", help="cs8 for a HackRF, cs16 for a PlutoSDR")
    sub.add_parser("session")
    args = ap.parse_args()
    return {"view": cmd_view, "watch": cmd_watch, "over": cmd_over, "analyze": cmd_analyze, "synth": cmd_synth,
            "session": cmd_session}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
