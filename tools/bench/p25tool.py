#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""
p25tool: P25 Phase 1 conventional voice on the bench, through the firmware's own code (core/p25, core/air, core/fec,
built for the host from tools/bench/p25: `make -C tools/bench/p25`, done here when the program is missing or older
than its sources).

    p25tool.py gen OUT.dibits --expect EXP.json [--nac 293] [--tg N | --dst N] [--src N] [--pairs N]
               [--voice HEX22 | --random-voice SEED | --voice-file FILE] [--ss unknown|talkaround|busy|idle] [--lead N] [--tail N]
               [--ber P --seed S] [--emergency] [--unpacked]
    p25tool.py decode IN.dibits [--expect EXP.json] [--unpacked | --air-jsonl] [--soft FILE] [--nac 293]
    p25tool.py disc IN.raw --rate HZ [--full HZ | --counts-per-khz N] [--invert] [--bits 8 --shift S] [--skip S]
               [--meta IN.json] [--expect EXP.json]
    p25tool.py cc OUT.dibits --expect EXP.json [--nac 293] [--wacn A4C21] [--sysid 2F3] [--rfss N] [--site N]
               [--units N] [--ch 1004] [--base HZ] [--spacing HZ] [--tx-offset HZ]
               [--grant TG:CH[:SRC[:SVC]] [--grant-at FIRST[:COUNT[:EVERY]]]] [--reg UNIT[:RV] [--reg-at FIRST[:COUNT[:EVERY]]]]
               [--aff UNIT:TG[:GAV] [--aff-at ...]] [--deny UNIT[:REASON[:SVC]] [--deny-at ...]]
               [--queue UNIT[:REASON[:SVC]] [--queue-at ...]]
    p25tool.py inbound OUT.dibits --expect EXP.json [--nac 293] [--src N] [--tg N] [--wacn A4C21] [--sysid 2F3]
               [--svc N] [--gap SYMS] [--reps N]
    p25tool.py mod IN.dibits OUT.raw [--rate HZ] [--full HZ] [--dev-scale PCT] [--offset HZ] [--noise HZ] [--ppm P]
               [--invert] [--carrier-gap S] [--carrier-lead S] [--seed S]
    p25tool.py selftest

gen writes a transmission as the firmware's transmit path builds it (p25_call_tx): the profile's idle symbols as a
lead, a header, N pairs of LDU1 and LDU2, a terminator, a tail. Voice frames are one made-up 88-bit pattern (--voice),
random (--random-voice), or the 11-byte full-rate frames of a file in order (--voice-file, as
tests/data/voice/test_transmission.imbe holds them; its last one repeats). The dibit file is the air interface's
packing (four a byte, the first in bits 7-6; 01 +3, 00 +1, 10 -1, 11 -3), which the bench's fsk4 transmitter takes as
it is. --ber flips bits after the expect file was written. The expect file (JSON) lists every unit with its symbols, position and
voice frames, for decode and disc to compare against.

decode reads symbols (a dibit file, one symbol a byte with --unpacked, or `rfoundry_debug.py air rx` records with
--air-jsonl) through the framer and the call layer and prints every unit: its data unit ID, NAC, NID errors, link
control, encryption sync, low speed data, each voice frame's corrected bits, the status symbols, then the call's
start, updates and end. With --expect: channel symbol and bit errors per unit (before FEC), voice frames wrong after
FEC, units missed.

disc reads discriminator samples (int16 LE, or int8 with --bits 8 as `rfoundry_debug.py air tap --bits 8` writes
them; a WAV's rate and samples are read from its header), 8 to 48 kHz, through the soft modem first. --full is the
deviation at full scale in Hz (the bench's disc stream gives it as `full`), --counts-per-khz the tap's scale as the
debug link reports it; without either the soft modem follows the level itself. --invert for a discriminator of the
other sense. It adds the soft modem's syncs (time, outer deviation, carrier offset) and, with --expect, the soft
values' error against the ideal levels (an EVM in percent of the outer level).

cc writes a trunked system's control channel: units of one to three trunking signaling blocks back to back (network
and RFSS status, the channel identifier with --tx-offset as the inbound offset, an adjacent site, a secondary control
channel). Answers to one unit go ahead of the broadcasts in the units --X-at names (FIRST[:COUNT[:EVERY]], unit
numbers from 0; COUNT 0 or left out: to the end; EVERY: each EVERYth of them, which leaves a file that answers all
through room for its broadcasts; the expect file gives each unit's symbol position): --grant a group voice grant
(CH in hex: identifier in the top four bits; default from unit 10), --reg a registration response (RV 0 accept, 1
fail, 2 deny, 3 refused; from unit 5), --aff an affiliation response (from unit 8), --deny and --queue a deny or
queued response to a group voice request (REASON 0x10 and 0x2F unless given; from unit 12). Play the file in a loop
for a site that keeps answering. decode and disc with --trunk take trunking units too and print each block's message;
with a cc expect file they compare blocks.

inbound writes the bursts a radio sends on the control channel's inbound frequency: a registration request, an
affiliation request and a group voice request, one block each, --gap idle symbols around them. decode and disc with
--inbound read blocks by the inbound opcodes: run them on a capture of the inbound frequency to see what the radio
sent. The radio's voice on the granted channel is a conventional transmission: decode and disc read it as they are.

mod models a C4FM transmitter and a discriminator (for tests): samples a disc capture would hold.

Output: one line per unit and event, a summary; --json prints the program's JSON lines as they are.
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SRC = os.path.join(HERE, "p25")
BIN = os.path.join(ROOT, "build", "bench", "p25tool")
LEVEL = {1: 3, 0: 1, 2: -1, 3: -3}
SS_NAMES = ("talkaround", "busy", "unknown", "idle")
RV = ("accepted", "failed", "denied", "refused")


def build():
    """The program, built when it is missing or older than any source it is made of."""
    deps = [os.path.join(SRC, "p25tool.c"), os.path.join(SRC, "Makefile")]
    for d in ("core/p25", "core/air", "core/fec"):
        p = os.path.join(ROOT, d)
        deps += [os.path.join(p, f) for f in os.listdir(p) if f.endswith((".c", ".h"))]
    if os.path.exists(BIN) and os.path.getmtime(BIN) >= max(os.path.getmtime(f) for f in deps):
        return BIN
    r = subprocess.run(["make", "-s", "-C", SRC], capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(BIN):
        sys.exit(f"p25tool: the build failed:\n{r.stdout}{r.stderr}")
    if r.stderr.strip():
        print(r.stderr, file=sys.stderr)
    return BIN


def run(args):
    """The program's JSON lines."""
    r = subprocess.run([build()] + [str(a) for a in args], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"p25tool: {' '.join(str(a) for a in args[:1])} failed: {r.stderr.strip()}")
    return [json.loads(x) for x in r.stdout.splitlines() if x.strip()]


def syms_of(hexstr, n):
    b = bytes.fromhex(hexstr)
    return [(b[i >> 2] >> (6 - 2 * (i & 3))) & 3 for i in range(n)]


# ── Printing ──

def lc_text(lc):
    if lc is None:
        return "-"
    if lc.get("lcf") == 0:
        return f"group TG {lc['tgid']} from {lc['src']}{' EMERGENCY' if lc.get('emergency') else ''}"
    if lc.get("lcf") == 3:
        return f"unit {lc['dst']} from {lc['src']}"
    return f"LCF 0x{lc.get('lcf', 0):02X}"


def unit_text(u, disc):
    t = f"{u['t_us'] / 1000:9.3f} ms" if disc else f"sym {u.get('at', 0):7d}"
    s = f"{t}  {u['name']:<5} NAC {u['nac']:03X} NID errs {u['nid_errs']:>2}"
    if u.get("no_sync"):
        s += " (no sync)"
    ss = u.get("ss", [0, 0, 0, 0])
    s += "  ss " + "/".join(f"{SS_NAMES[i]} {n}" for i, n in enumerate(ss) if n)
    if u["name"] == "HDU":
        h = u.get("hdr")
        s += f"  hdr RS {u['hdr_rs']}" + (f" ALG {h['algid']:02X} KID {h['kid']} TG {h['tgid']} MFID {h['mfid']}"
                                          if h else " FAILED")
    elif u["name"] in ("LDU1", "TDULC"):
        s += f"  LC RS {u['lc_rs']} " + (lc_text(u.get("lc")) if u["lc_rs"] >= 0 else "FAILED")
    elif u["name"] == "LDU2":
        e = u.get("es")
        s += f"  ES RS {u['es_rs']}" + (f" ALG {e['algid']:02X} KID {e['kid']} MI {e['mi']}" if e else " FAILED")
    if "voice" in u:
        s += f"  LSD {u['lsd']} ({u['lsd_errs']})  voice errs " + ",".join(str(v["errs"]) for v in u["voice"])
    for b in u.get("blocks", []):
        s += "\n" + " " * 16 + (f"block CRC FAILED" if b["errs"] < 0 else f"block ({b['errs']}) {msg_text(b['msg'])}")
    return s


def msg_text(m):
    """A trunking block's message, or a packet's header."""
    if "mbt" in m:
        return (f"multi-block trunking header: opcode 0x{m['opcode']:02X}, {m['blocks']} blocks, SAP {m['sap']}"
                if m["mbt"] else "packet header (not trunking)")
    k, s = m["kind"], f"0x{m['opcode']:02X}"
    if k == "grant":
        s += (f" unit to unit: ch {m['ch']:04X} to {m['dst']} from {m['src']}" if m["unit"] else
              f" group grant: ch {m['ch']:04X} TG {m['tg']}" + (f", ch {m['ch2']:04X} TG {m['tg2']}" if m["ch2"] else "")
              + (f" from {m['src']}" if m["src"] else ""))
        if "svc" in m:
            s += f" svc {m['svc']:02X}"
    elif k == "iden":
        s += (f" identifier {m['id']}: base {m['base_hz'] / 1e6:.5f} MHz, spacing {m['spacing_hz'] / 1e3:g} kHz"
              + (f", TDMA {m['slots']} slots" if m["tdma"] else ""))
    elif k == "net":
        s += f" network status: WACN {m['wacn']:05X} system {m['sysid']:03X} ch {m['ch']:04X}"
    elif k in ("rfss", "adj"):
        s += (f" {'RFSS' if k == 'rfss' else 'adjacent site'} status: system {m['sysid']:03X} RFSS {m['rfss']} "
              f"site {m['site']} ch {m['ch']:04X}")
    elif k == "reg_rsp":
        s += f" registration response: unit {m['id']} {RV[m['rv']]}, system {m['sysid']:03X}, address {m['addr']}"
    elif k == "aff_rsp":
        s += f" affiliation response: unit {m['dst']} TG {m['tg']} {RV[m['gav']]}{' (global)' if m['global'] else ''}"
    elif k in ("deny", "queued"):
        s += f" {'deny' if k == 'deny' else 'queued'} response: unit {m['dst']} to 0x{m['svc']:02X}, reason 0x{m['reason']:02X}"
    elif k == "u_reg_req":
        s += f" registration request: unit {m['src']} WACN {m['wacn']:05X} system {m['sysid']:03X}"
    elif k == "grp_aff_req":
        s += f" affiliation request: unit {m['src']} TG {m['tg']} system {m['sysid']:03X}"
    elif k == "grp_v_req":
        s += f" group voice request: unit {m['src']} TG {m['tg']} svc {m['svc']:02X}"
    elif k == "sccb":
        s += f" secondary control channels: " + ", ".join(f"{c:04X}" for c in m["ch"] if c)
    else:
        s += f" {k}" + (f" (MFID {m['mfid']:02X})" if k == "mfr" else "")
    return s


def compare_cc(lines, e):
    """Blocks of a cc expect file found in the output (by content), and blocks whose CRC failed."""
    want = [b["raw"] for u in e["units"] for b in u["blocks"]]
    got = [b["msg"]["raw"] for d in lines if d["ev"] == "unit" for b in d.get("blocks", []) if b["errs"] >= 0]
    failed = sum(1 for d in lines if d["ev"] == "unit" for b in d.get("blocks", []) if b["errs"] < 0)
    have = set(got)
    return {"blocks_expected": len(want), "blocks_found": sum(1 for w in want if w in have), "crc_failed": failed}


def show(lines, disc, as_json):
    for d in lines:
        if as_json:
            print(json.dumps(d))
            continue
        ev = d["ev"]
        if ev == "unit":
            print("  " + unit_text(d, disc))
        elif ev in ("call_start", "call_update"):
            alg = f" ALG {d['algid']:02X} KID {d['kid']}" if d["have_alg"] else ""
            late = " (late entry)" if d["late"] else ""
            print(f"  {'call start' if ev == 'call_start' else 'call update'}{late}: NAC {d['nac']:03X} "
                  f"{lc_text(d['lc']) if d['have_lc'] else 'TG ' + str(d['lc'].get('tgid', '?')) + ' (header)'}{alg}")
        elif ev == "call_end":
            print("  call end")
        elif ev == "sync":
            hz = (f" outer {d['outer_hz']:.0f} Hz ({100 * d['level_rel']:.1f} % of 1800), offset {d['offset_hz']:+.0f} Hz"
                  if "outer_hz" in d else " (scale unknown: no level)")
            print(f"  {d['t_ms']:9.3f} ms  soft modem sync:{hz}")
        elif ev == "stats":
            f, c = d["framer"], d["call"]
            print(f"framer: {f['syncs']} syncs, {f['misses']} missed, {f['locks']} locks, {f['rejects']} rejects, "
                  f"{f['frames']} frames; calls {c['calls']}, units {c['units']}, voice frames {c['voice']} "
                  f"({c['voice_bad']} bad, {c['voice_bits']} bits corrected), LC fails {c['lc_fail']}, "
                  f"ES fails {c['es_fail']}, voice handed on {d['voice_out']}")
        elif ev == "softmodem":
            print(f"soft modem: {d['syncs']} syncs, {d['chunks']} chunks, {d['free_run_chunks']} free running, "
                  f"{d['gap_chunks']} after a gap")
        elif ev == "input":
            if "samples" in d:
                print(f"input: {d['samples']} samples at {d['rate']} Hz ({d['seconds']:.2f} s), "
                      f"{d['counts_per_khz']:.0f} counts/kHz{' (level followed)' if not d['counts_per_khz'] else ''}"
                      f"{', inverted' if d['invert'] else ''}")
            else:
                print(f"input: {d['syms']} symbols")
        elif ev in ("gen", "mod"):
            print(json.dumps(d))


# ── Comparing with an expect file ──

def compare(lines, exp):
    """Received units against the expected ones, matched in order by data unit ID (the closest of the next few
    of that kind by symbol errors). Channel symbol and bit errors are before FEC; voice frames after it."""
    units = [d for d in lines if d["ev"] == "unit"]
    want = exp["units"]
    r = {"units_expected": len(want), "units_got": len(units), "units_matched": 0, "units_unmatched": 0,
         "syms": 0, "sym_errs": 0, "bits": 0, "bit_errs": 0, "voice_frames": 0, "voice_frames_wrong": 0,
         "voice_bit_errs": 0, "voice_frames_wrong_fec": 0, "voice_frames_flagged": 0, "lc_wrong": 0, "nac_wrong": 0, "soft_n": 0, "soft_se": 0.0,
         "matched": []}
    last = -1
    for u in units:
        got = syms_of(u["syms"], u["nsyms"])
        best = None
        for j in [j for j in range(last + 1, len(want)) if want[j]["duid"] == u["duid"]][:3]:
            ref = syms_of(want[j]["syms"], want[j]["nsyms"])
            errs = sum(1 for a, b in zip(got, ref) if a != b)
            if best is None or errs < best[1]:
                best = (j, errs, ref)
        if best is None:
            r["units_unmatched"] += 1
            continue
        j, errs, ref = best
        last = j
        r["units_matched"] += 1
        r["matched"].append(j)
        n = min(len(got), len(ref))
        r["syms"] += n
        r["sym_errs"] += errs
        r["bits"] += 2 * n
        r["bit_errs"] += sum(bin(a ^ b).count("1") for a, b in zip(got, ref))
        if u["nac"] != exp["nac"]:
            r["nac_wrong"] += 1
        if u["name"] in ("LDU1", "TDULC") and u.get("lc") != exp["lc"]:
            r["lc_wrong"] += 1
        if "soft" in u:
            soft = struct.unpack(f"{u['nsyms']}b", bytes.fromhex(u["soft"]))
            for s, d in zip(soft, ref):
                r["soft_se"] += (s - 32 * LEVEL[d]) ** 2
            r["soft_n"] += n
        for v, w in zip(u.get("voice", []), want[j].get("voice", [])):
            r["voice_frames"] += 1
            x = int.from_bytes(bytes.fromhex(v["imbe"]), "big") ^ int.from_bytes(bytes.fromhex(w), "big")
            diff, bare = bin(x).count("1"), bin(x & 0x7F).count("1")
            r["voice_bit_errs"] += diff
            r["voice_frames_wrong"] += 1 if diff else 0
            # The last 7 bits (u7) ride without FEC: a channel error there always reaches the frame
            r["voice_frames_wrong_fec"] += 1 if diff > bare else 0
            r["voice_frames_flagged"] += 1 if v["errs"] >= 9 else 0
    r["units_missed"] = len(want) - len(set(r["matched"]))
    r["missed"] = [f"{want[j]['name']}@{want[j]['at']}" for j in range(len(want)) if j not in r["matched"]]
    r["ser"] = r["sym_errs"] / r["syms"] if r["syms"] else None
    r["ber"] = r["bit_errs"] / r["bits"] if r["bits"] else None
    r["evm_pct"] = 100 * (r["soft_se"] / r["soft_n"]) ** 0.5 / 96 if r["soft_n"] else None
    del r["matched"], r["soft_se"]
    return r


def compare_text(r):
    s = (f"vs expect: units {r['units_matched']} of {r['units_expected']} matched ({r['units_missed']} missed"
         f"{': ' + ', '.join(r['missed']) if r['missed'] else ''}; {r['units_unmatched']} unmatched); "
         f"channel symbols {r['sym_errs']} wrong of {r['syms']}")
    if r["ser"] is not None:
        s += f" (SER {100 * r['ser']:.3f} %, BER {100 * r['ber']:.3f} %)"
    s += (f"; voice frames {r['voice_frames_wrong']} wrong of {r['voice_frames']} ({r['voice_bit_errs']} bits; "
          f"{r['voice_frames_wrong_fec']} with errors past the FEC, the rest only in the 7 bits it leaves bare; "
          f"{r['voice_frames_flagged']} flagged bad); NAC wrong {r['nac_wrong']}, LC wrong {r['lc_wrong']}")
    if r["evm_pct"] is not None:
        s += f"; soft EVM {r['evm_pct']:.1f} % of the outer level"
    return s


# ── Inputs ──

def air_jsonl_to_files(path, tmp):
    """`rfoundry_debug.py air rx` records to one symbol a byte and their soft values (a chunk without them: 0)."""
    syms, soft, gaps = bytearray(), bytearray(), 0
    with open(path) as f:
        for line in f:
            if not line.strip():
                continue
            c = json.loads(line)
            n = c["nsyms"]
            syms += bytes(syms_of(c["hard"], n))
            s = c.get("soft") or [0] * n
            soft += struct.pack(f"{n}b", *s)
            gaps += 1 if "GAP" in (c.get("flags") or []) else 0
    sp, fp = os.path.join(tmp, "air.syms"), os.path.join(tmp, "air.soft")
    with open(sp, "wb") as f:
        f.write(syms)
    with open(fp, "wb") as f:
        f.write(soft)
    if gaps > 1:
        print(f"note: {gaps} chunks say symbols were lost before them; the symbols are joined", file=sys.stderr)
    return sp, fp


def wav_to_raw(path, tmp):
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2:
            sys.exit("p25tool: 16-bit WAVs only")
        ch, rate = w.getnchannels(), w.getframerate()
        data = w.readframes(w.getnframes())
    if ch > 1:
        data = b"".join(data[i:i + 2] for i in range(0, len(data), 2 * ch))
    out = os.path.join(tmp, "disc.raw")
    with open(out, "wb") as f:
        f.write(data)
    return out, rate


# ── Commands ──

def pass_opts(a, names):
    out = []
    for n in names:
        v = getattr(a, n.replace("-", "_"), None)
        if v is True:
            out.append(f"--{n}")
        elif v not in (None, False):
            out += [f"--{n}", v]
    return out


def cmd_gen(a):
    lines = run(["gen", a.out, a.expect] + pass_opts(a, ["nac", "tg", "dst", "src", "pairs", "voice", "random-voice", "voice-file",
                                                         "ss", "lead", "tail", "ber", "seed", "emergency",
                                                         "unpacked"]))
    d = lines[-1]
    with open(a.expect) as f:
        e = json.load(f)
    print(f"{a.out}: {d['syms']} symbols ({d['syms'] / 4800:.3f} s at 4800/s), {len(e['units'])} units, "
          f"{d['frames']} voice frames, {d['bit_errors_applied']} bit errors applied; expect: {a.expect}")
    return 0


def report(lines, a, disc):
    show(lines, disc, a.json)
    if a.expect:
        with open(a.expect) as f:
            e = json.load(f)
        if e.get("format") == "p25-cc-expect/1":
            r = compare_cc(lines, e)
            print(json.dumps({"ev": "compare", **r}) if a.json else
                  f"blocks found {r['blocks_found']} of {r['blocks_expected']}, CRC failed {r['crc_failed']}")
            return 0 if r["blocks_found"] == r["blocks_expected"] else 1
        r = compare(lines, e)
        print(json.dumps({"ev": "compare", **r}) if a.json else compare_text(r))
        return 0 if r["units_matched"] else 1
    return 0 if any(d["ev"] == "unit" for d in lines) else 1


def cmd_decode(a):
    with tempfile.TemporaryDirectory() as tmp:
        args = ["decode", a.input]
        if a.air_jsonl:
            sp, fp = air_jsonl_to_files(a.input, tmp)
            args = ["decode", sp, "--unpacked", "--soft", fp]
        else:
            args += pass_opts(a, ["unpacked", "soft"])
        lines = run(args + pass_opts(a, ["nac", "trunk", "inbound"]))
    return report(lines, a, False)


def cmd_disc(a):
    rate, full, inp = a.rate, a.full, a.input
    if a.meta:
        with open(a.meta) as f:
            m = json.load(f)
        rate = rate or m.get("rate")
        full = full or m.get("full")
        a.invert = a.invert or bool(m.get("invert"))
    with tempfile.TemporaryDirectory() as tmp:
        if inp.lower().endswith(".wav"):
            inp, wrate = wav_to_raw(inp, tmp)
            rate = rate or wrate
        if not rate:
            sys.exit("p25tool disc: --rate (or --meta, or a WAV)")
        args = ["disc", inp, "--rate", int(round(float(rate)))]
        if full:
            args += ["--full", full]
        lines = run(args + pass_opts(a, ["counts-per-khz", "invert", "bits", "shift", "skip", "nac", "trunk", "inbound"]))
    return report(lines, a, True)


def cmd_cc(a):
    d = run(["cc", a.out, a.expect] + pass_opts(a, ["nac", "wacn", "sysid", "rfss", "site", "units", "ch", "base",
                                                    "spacing", "tx-offset", "grant", "grant-at", "reg", "reg-at", "aff",
                                                    "aff-at", "deny", "deny-at", "queue", "queue-at", "unpacked"]))[-1]
    print(f"{a.out}: {d['syms']} symbols ({d['syms'] / 4800:.3f} s at 4800/s), {d['units']} units; expect: {a.expect}")
    return 0


def cmd_inbound(a):
    d = run(["inbound", a.out, a.expect] + pass_opts(a, ["nac", "src", "tg", "wacn", "sysid", "svc", "gap", "reps",
                                                         "unpacked"]))[-1]
    print(f"{a.out}: {d['syms']} symbols ({d['syms'] / 4800:.3f} s at 4800/s), {d['units']} bursts; expect: {a.expect}")
    return 0


def cmd_mod(a):
    for d in run(["mod", a.input, a.out] + pass_opts(a, ["rate", "full", "dev-scale", "offset", "noise", "ppm",
                                                         "invert", "carrier-gap", "carrier-lead", "seed", "lpf", "unpacked"])):
        print(json.dumps(d))
    return 0


def cmd_selftest(a):
    """Generate, decode; generate, modulate (the C4FM model), demodulate and decode; compare each with its expect file."""
    fails = []
    with tempfile.TemporaryDirectory() as tmp:
        def p(n):
            return os.path.join(tmp, n)

        def check(what, ok, r):
            print(f"{'ok  ' if ok else 'FAIL'} {what}: {compare_text(r)}")
            if not ok:
                fails.append(what)

        def gen(name, *opts):
            run(["gen", p(name + ".dibits"), p(name + ".json")] + list(opts))
            with open(p(name + ".json")) as f:
                return json.load(f)

        e = gen("clean", "--pairs", 5, "--random-voice", 1, "--tg", 4001, "--src", 7654321, "--lead", 48, "--tail", 24)
        n_units = len(e["units"])
        r = compare(run(["decode", p("clean.dibits")]), e)
        check("decode clean", r["units_matched"] == n_units and r["sym_errs"] == 0 and r["voice_frames_wrong"] == 0
              and r["lc_wrong"] == 0, r)
        e2 = gen("unit", "--pairs", 2, "--dst", 42, "--src", 99, "--ss", "talkaround", "--nac", "F12", "--unpacked")
        lines = run(["decode", p("unit.dibits"), "--unpacked", "--nac", "F12"])
        r = compare(lines, e2)
        talk = all(u["ss"][0] == sum(u["ss"]) for u in lines if u["ev"] == "unit")
        check("decode unit to unit, talkaround, NAC F12, one symbol a byte", r["units_matched"] == len(e2["units"])
              and r["lc_wrong"] == 0 and talk, r)
        r = compare(run(["decode", p("unit.dibits"), "--unpacked", "--nac", "293"]), e2)
        check("another NAC is refused", r["units_got"] == 0, r)
        for ber in (0.002, 0.01, 0.03):
            e3 = gen(f"ber{ber}", "--pairs", 5, "--random-voice", 2, "--ber", ber, "--seed", 5)
            lines = run(["decode", p(f"ber{ber}.dibits")])
            r = compare(lines, e3)
            st = [d for d in lines if d["ev"] == "stats"][0]["call"]
            print(f"     BER {ber}: {e3['bit_errors_applied']} bits flipped in {2 * e3['total_syms']}; "
                  f"{st['voice_bits']} voice bits corrected by FEC")
            # Every unit still framed; voice past the FEC only as often as two errors in one Hamming word allow
            check(f"decode BER {100 * ber:g} %", r["units_matched"] == n_units and
                  r["voice_frames_wrong_fec"] <= {0.002: 1, 0.01: 5, 0.03: 30}[ber], r)
        cases = [
            ("disc 8 kHz, scale known", ["--rate", 8000, "--full", 5000], ["--rate", 8000, "--full", 5000], 0),
            ("disc 48 kHz, +300 Hz, 90 % deviation, noise 150 Hz", ["--rate", 48000, "--full", 6000, "--offset", 300,
             "--dev-scale", 90, "--noise", 150], ["--rate", 48000, "--full", 6000], 0),
            ("disc 8 kHz inverted, 105 % deviation", ["--rate", 8000, "--full", 5000, "--invert", "--dev-scale", 105],
             ["--rate", 8000, "--full", 5000, "--invert"], 0),
            ("disc 16 kHz, scale unknown, -500 Hz, clock 40 ppm", ["--rate", 16000, "--full", 5000, "--offset", -500,
             "--ppm", 40], ["--rate", 16000], 1),
            ("disc 8 kHz, noise 350 Hz", ["--rate", 8000, "--full", 5000, "--noise", 350],
             ["--rate", 8000, "--full", 5000], 1),
        ]
        for what, mopts, dopts, slack in cases:
            run(["mod", p("clean.dibits"), p("x.raw")] + mopts)
            lines = run(["disc", p("x.raw")] + dopts)
            r = compare(lines, e)
            syncs = [d for d in lines if d["ev"] == "sync"]
            lv = [d for d in syncs if "outer_hz" in d]
            note = ""
            if lv:
                outer = sum(d["outer_hz"] for d in lv) / len(lv)
                off = sum(d["offset_hz"] for d in lv) / len(lv)
                note = f"; soft modem: {len(syncs)} syncs, outer {outer:.0f} Hz, offset {off:+.0f} Hz"
            else:
                note = f"; soft modem: {len(syncs)} syncs (scale unknown: no level in Hz)"
            print(f"     {what}{note}")
            check(what, r["units_matched"] >= n_units - slack and r["voice_frames_wrong_fec"] <= 5 * slack, r)
        # A control channel: every block back, as symbols and through the C4FM model; none without --trunk
        run(["cc", p("cc.dibits"), p("cc.json"), "--units", 60, "--grant", "100:100a:1001", "--grant-at", 20])
        with open(p("cc.json")) as f:
            ec = json.load(f)
        for what, lines in (("cc decode", run(["decode", p("cc.dibits"), "--trunk"])),
                            ("cc disc 8 kHz, noise 300 Hz", (run(["mod", p("cc.dibits"), p("cc.raw"), "--rate", 8000,
                                                                   "--full", 5000, "--noise", 300]),
                                                              run(["disc", p("cc.raw"), "--rate", 8000, "--full", 5000,
                                                                   "--trunk"]))[1])):
            r = compare_cc(lines, ec)
            ok = r["blocks_found"] == r["blocks_expected"] and r["crc_failed"] == 0
            grants = sum(1 for d in lines if d["ev"] == "unit" for b in d.get("blocks", [])
                         if b["msg"].get("kind") == "grant" and b["msg"].get("tg") == 100)
            print(f"{'ok  ' if ok and grants == 40 else 'FAIL'} {what}: blocks {r['blocks_found']} of "
                  f"{r['blocks_expected']}, CRC failed {r['crc_failed']}, grants {grants}")
            if not ok or grants != 40:
                fails.append(what)
        r = compare_cc(run(["decode", p("cc.dibits")]), ec)
        print(f"{'ok  ' if r['blocks_found'] == 0 else 'FAIL'} cc without --trunk: no blocks taken")
        if r["blocks_found"]:
            fails.append("cc without --trunk")
        # A site answering unit 1234567: registration, affiliation to TG 201, a grant, a deny to another unit
        run(["cc", p("site.dibits"), p("site.json"), "--units", 40, "--tx-offset", -45000000, "--reg", 1234567,
             "--reg-at", "3:4", "--aff", "1234567:201", "--aff-at", "8:4", "--grant", "201:100a:1234567",
             "--grant-at", "14:6", "--deny", "7654321", "--deny-at", "22:2", "--queue", "7654321:0x2f", "--queue-at",
             "26:2"])
        with open(p("site.json")) as f:
            es = json.load(f)
        lines = run(["decode", p("site.dibits"), "--trunk"])
        r = compare_cc(lines, es)
        msgs = [b["msg"] for d in lines if d["ev"] == "unit" for b in d.get("blocks", [])]
        got = {k: sum(1 for m in msgs if m.get("kind") == k) for k in ("reg_rsp", "aff_rsp", "grant", "deny", "queued")}
        ok = (r["blocks_found"] == r["blocks_expected"] and got == {"reg_rsp": 4, "aff_rsp": 4, "grant": 6, "deny": 2,
                                                                    "queued": 2}
              and all(m["id"] == 1234567 and m["rv"] == 0 for m in msgs if m.get("kind") == "reg_rsp")
              and all(m["tg"] == 201 and m["dst"] == 1234567 for m in msgs if m.get("kind") == "aff_rsp")
              and all(m["tx_off"] == -180 for m in msgs if m.get("kind") == "iden"))
        print(f"{'ok  ' if ok else 'FAIL'} cc with answers: blocks {r['blocks_found']} of {r['blocks_expected']}, {got}")
        if not ok:
            fails.append("cc with answers")
        # A site that answers all through its file, one answer a unit: the broadcasts keep their turn
        run(["cc", p("loop.dibits"), p("loop.json"), "--units", 27, "--reg", 1234567, "--reg-at", "0:0:3", "--aff",
             "1234567:201", "--aff-at", "1:0:3", "--grant", "201:100a:1234567", "--grant-at", "2:0:3"])
        msgs = [b["msg"] for d in run(["decode", p("loop.dibits"), "--trunk"]) if d["ev"] == "unit"
                for b in d.get("blocks", [])]
        got = {k: sum(1 for m in msgs if m.get("kind") == k) for k in ("reg_rsp", "aff_rsp", "grant", "net", "iden")}
        ok = got["reg_rsp"] == got["aff_rsp"] == got["grant"] == 9 and got["net"] >= 3 and got["iden"] >= 3
        print(f"{'ok  ' if ok else 'FAIL'} cc answering every third unit: {got}")
        if not ok:
            fails.append("cc answering every third unit")
        # A radio's inbound bursts, as symbols and through the C4FM model
        run(["inbound", p("in.dibits"), p("in.json"), "--src", 1234567, "--tg", 201, "--reps", 3])
        with open(p("in.json")) as f:
            ei = json.load(f)
        for what, lines in (("inbound decode", run(["decode", p("in.dibits"), "--inbound"])),
                            ("inbound disc 8 kHz, noise 300 Hz", (run(["mod", p("in.dibits"), p("in.raw"), "--rate",
                                                                        8000, "--full", 5000, "--noise", 300]),
                                                                    run(["disc", p("in.raw"), "--rate", 8000, "--full",
                                                                         5000, "--inbound"]))[1])):
            r = compare_cc(lines, ei)
            kinds = [b["msg"].get("kind") for d in lines if d["ev"] == "unit" for b in d.get("blocks", [])]
            ok = r["blocks_found"] == r["blocks_expected"] == 9 and kinds == ["u_reg_req", "grp_aff_req", "grp_v_req"] * 3
            print(f"{'ok  ' if ok else 'FAIL'} {what}: blocks {r['blocks_found']} of {r['blocks_expected']}")
            if not ok:
                fails.append(what)
    print(f"selftest: {'all passed' if not fails else str(len(fails)) + ' failed: ' + ', '.join(fails)}")
    return 1 if fails else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    g = sub.add_parser("gen", help="a transmission as dibits, and its expect file")
    g.add_argument("out")
    g.add_argument("--expect", required=True)
    g.add_argument("--nac", default=None, help="hex (293)")
    g.add_argument("--tg", type=int, default=None, help="talk group of a group call (1)")
    g.add_argument("--dst", type=int, default=None, help="a unit to unit call to this unit")
    g.add_argument("--src", type=int, default=None, help="source unit (1)")
    g.add_argument("--emergency", action="store_true")
    g.add_argument("--pairs", type=int, default=None, help="LDU1 + LDU2 pairs (3)")
    g.add_argument("--voice", default=None, help="every voice frame: 11 bytes in hex")
    g.add_argument("--random-voice", type=int, default=None, metavar="SEED", help="random voice frames")
    g.add_argument("--voice-file", default=None, help="voice frames from a file of 11-byte frames (the last repeats)")
    g.add_argument("--ss", choices=("unknown", "talkaround", "busy", "idle"), default=None,
                   help="the status symbols (unknown: a subscriber, direct or to a repeater)")
    g.add_argument("--lead", type=int, default=None, help="idle symbols before (0)")
    g.add_argument("--tail", type=int, default=None, help="idle symbols after (0)")
    g.add_argument("--ber", type=float, default=None, help="bit error rate applied to the symbols")
    g.add_argument("--seed", type=int, default=None, help="for --ber")
    g.add_argument("--unpacked", action="store_true", help="one symbol a byte")
    g.set_defaults(fn=cmd_gen)

    def rx_common(s):
        s.add_argument("--expect", default=None, help="a gen expect file to compare with")
        s.add_argument("--nac", default=None, help="hex: take only this NAC (F7E: any, the default)")
        s.add_argument("--json", action="store_true", help="the JSON lines as they are")
        s.add_argument("--trunk", action="store_true", help="take trunking signaling and packets too, and print them")
        s.add_argument("--inbound", action="store_true", help="as --trunk, the blocks read as a radio's inbound ones")

    d = sub.add_parser("decode", help="symbols through the framer and the call layer")
    d.add_argument("input")
    d.add_argument("--unpacked", action="store_true", help="one symbol a byte")
    d.add_argument("--air-jsonl", action="store_true", help="rfoundry_debug.py air rx records")
    d.add_argument("--soft", default=None, help="a soft value (int8) per symbol")
    rx_common(d)
    d.set_defaults(fn=cmd_decode)

    s = sub.add_parser("disc", help="discriminator samples through the soft modem, then as decode")
    s.add_argument("input", help="int16 LE (or int8 with --bits 8), or a WAV")
    s.add_argument("--rate", type=float, default=None, help="Hz, 8000 to 48000")
    s.add_argument("--full", type=float, default=None, help="Hz of deviation at full scale")
    s.add_argument("--counts-per-khz", type=float, default=None, help="the tap's scale (16-bit counts)")
    s.add_argument("--invert", action="store_true", help="the discriminator's sense is the other way")
    s.add_argument("--bits", type=int, choices=(8, 16), default=None)
    s.add_argument("--shift", type=int, default=None, help="--bits 8: the shift the tap applied (8)")
    s.add_argument("--skip", type=float, default=None, help="seconds to skip at the start")
    s.add_argument("--meta", default=None, help="a JSON sidecar with rate, full, invert (fsk4_bench.py disc writes it)")
    rx_common(s)
    s.set_defaults(fn=cmd_disc)

    c = sub.add_parser("cc", help="a trunked system's control channel as dibits, and its expect file")
    c.add_argument("out")
    c.add_argument("--expect", required=True)
    c.add_argument("--nac", default=None, help="hex (293)")
    c.add_argument("--wacn", default=None, help="hex (A4C21)")
    c.add_argument("--sysid", default=None, help="hex (2F3)")
    c.add_argument("--rfss", type=int, default=None)
    c.add_argument("--site", type=int, default=None)
    c.add_argument("--units", type=int, default=None, help="trunking signaling units (40)")
    c.add_argument("--ch", default=None, help="hex: the control channel's own number (1004)")
    c.add_argument("--base", type=int, default=None, help="its identifier's base, Hz (851006250)")
    c.add_argument("--spacing", type=int, default=None, help="its identifier's spacing, Hz (6250)")
    c.add_argument("--grant", default=None, metavar="TG:CH[:SRC[:SVC]]", help="a group voice grant (CH in hex; SVC a number, 0x40 encrypted)")
    c.add_argument("--grant-at", default=None, metavar="FIRST[:COUNT[:EVERY]]", help="the units with the grant (10)")
    c.add_argument("--tx-offset", type=int, default=None, help="the identifier's inbound offset, Hz, 250 kHz steps (0)")
    c.add_argument("--reg", default=None, metavar="UNIT[:RV]", help="a registration response")
    c.add_argument("--reg-at", default=None, metavar="FIRST[:COUNT[:EVERY]]", help="(5)")
    c.add_argument("--aff", default=None, metavar="UNIT:TG[:GAV]", help="an affiliation response")
    c.add_argument("--aff-at", default=None, metavar="FIRST[:COUNT[:EVERY]]", help="(8)")
    c.add_argument("--deny", default=None, metavar="UNIT[:REASON[:SVC]]", help="a deny response")
    c.add_argument("--deny-at", default=None, metavar="FIRST[:COUNT[:EVERY]]", help="(12)")
    c.add_argument("--queue", default=None, metavar="UNIT[:REASON[:SVC]]", help="a queued response")
    c.add_argument("--queue-at", default=None, metavar="FIRST[:COUNT[:EVERY]]", help="(12)")
    c.add_argument("--unpacked", action="store_true", help="one symbol a byte")
    c.set_defaults(fn=cmd_cc)

    i = sub.add_parser("inbound", help="a radio's inbound bursts as dibits, and their expect file")
    i.add_argument("out")
    i.add_argument("--expect", required=True)
    i.add_argument("--nac", default=None, help="hex (293)")
    i.add_argument("--src", type=int, default=None, help="the unit (1)")
    i.add_argument("--tg", type=int, default=None, help="the talk group (1)")
    i.add_argument("--wacn", default=None, help="hex (A4C21)")
    i.add_argument("--sysid", default=None, help="hex (2F3)")
    i.add_argument("--svc", type=int, default=None, help="the voice request's service options (0)")
    i.add_argument("--gap", type=int, default=None, help="idle symbols between bursts (480)")
    i.add_argument("--reps", type=int, default=None, help="the three bursts this many times (1)")
    i.add_argument("--unpacked", action="store_true", help="one symbol a byte")
    i.set_defaults(fn=cmd_inbound)

    m = sub.add_parser("mod", help="a C4FM model: dibits to discriminator samples (int16 LE)")
    m.add_argument("input")
    m.add_argument("out")
    m.add_argument("--rate", type=int, default=None, help="Hz, dividing 240000 (48000)")
    m.add_argument("--full", type=float, default=None, help="Hz at full scale (5000)")
    m.add_argument("--dev-scale", type=float, default=None, help="percent of 1800 Hz (100)")
    m.add_argument("--offset", type=float, default=None, help="carrier offset, Hz")
    m.add_argument("--noise", type=float, default=None, help="Gaussian noise on the discriminator, Hz RMS")
    m.add_argument("--ppm", type=float, default=None, help="symbol clock error")
    m.add_argument("--invert", action="store_true")
    m.add_argument("--carrier-gap", type=float, default=None, help="s of noise (no carrier) before and after (0.2)")
    m.add_argument("--carrier-lead", type=float, default=None, help="s of unmodulated carrier before the first "
                   "symbol (0)")
    m.add_argument("--lpf", type=float, default=None, help="the discriminator's low-pass, Hz")
    m.add_argument("--seed", type=int, default=None)
    m.add_argument("--unpacked", action="store_true")
    m.set_defaults(fn=cmd_mod)

    t = sub.add_parser("selftest", help="gen, decode, mod, disc against each other")
    t.set_defaults(fn=cmd_selftest)

    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
