#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""
fsk4_bench: a bench board's raw 4-level FSK transmitter and receiver, and its FM discriminator capture, over the bench
line protocol (bench_line.py, TCP 7373; the signal modes of the bench API). For P25 Phase 1 with p25tool.py, and any
other 4-level mode framed by the client.

    fsk4_bench.py tx   --bench-api HOST[:PORT] --freq MHz --dibits FILE --allow-rf [--shape c4fm|rrc|rc|nrz]
                       [--baud 4800] [--dev HZ] [--dev-scale PCT] [--offset HZ] [--clip HZ] [--repeat N] [--gap S]
                       [--lead S] [--tail S] [--level 0-255 | --power-dbm DBM] [--field KEY=VALUE ...] [--dry-run]
    fsk4_bench.py rx   --bench-api HOST[:PORT] --freq MHz --seconds S --out FILE.dibits [--records FILE.jsonl]
                       [--sync HEX --sync-len N] [--baud 4800] [--nominal-dev HZ] [--max-symbols N] [--dry-run]
    fsk4_bench.py disc --bench-api HOST[:PORT] --freq MHz --seconds S --out FILE.raw [--bw HZ] [--decode
                       [--expect EXP.json]] [--dry-run]

Dibits are packed four a byte, the first in bits 7-6: 01 +3, 00 +1, 10 -1, 11 -3 (p25tool.py gen writes them). tx sends
them in the request as hex when they are 8192 symbols or fewer (the board then knows their peak deviation), else after
the request line. A C4FM stream sent after the line peaks past what some front ends take: give --clip then (the board's
limit; its refusal names it). Nothing transmits without --allow-rf; the frequency's lease is the caller's.

rx asks for the board's 4FSK receiver: by default P25's frame sync (5575F5FF77FF, 24 symbols), 4800 baud, 1800 Hz.
Its records (syncs with their offset, level, baud error, EVM, eye, the time from carrier to the first symbol) go to
--records, the sliced dibits to --out.

disc captures the FM receiver's raw discriminator (int16 LE at the channel's rate, about 48 kS/s) to --out, gaps filled
with zeros, and writes FILE.json beside it (rate, full: Hz at int16 full scale, t0, freq) for p25tool.py disc --meta.
--decode runs that decode at the end.

--bench-api defaults to $RFOUNDRY_BENCH_API, --level to $RFOUNDRY_BENCH_API_LEVEL. --dry-run prints the request line and
stops. Exit codes as bench_line.py's: 0, 1, 3 nothing heard, 69 unreachable or unsupported, 75 busy or timed out, 77
refused.
"""
import argparse
import json
import os
import subprocess
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench_line as L  # noqa: E402

P25_SYNC = "5575F5FF77FF"
HEX_MAX_SYMS = 8192          # symbols the board takes in the request itself (it then knows their peak)


def read_dibits(path, symbols=None):
    with open(path, "rb") as f:
        data = f.read()
    n = symbols or 4 * len(data)
    if n > 4 * len(data):
        sys.exit(f"fsk4_bench: {path} holds {4 * len(data)} symbols, not {n}")
    return data[:(n + 3) // 4], n


def field_value(v):
    for t in (int, float):
        try:
            return t(v)
        except ValueError:
            pass
    if v in ("true", "false"):
        return v == "true"
    return v


def tx_request(a, data, nsyms):
    """The tx request and the bytes that follow its line (none when the symbols go as hex)."""
    req = {"op": "tx", "mode": "fsk4", "freq": a.freq, "symbols": nsyms, "label": a.label or "fsk4_bench"}
    after = b""
    if nsyms <= HEX_MAX_SYMS and not a.stream:
        req["hex"] = data.hex()
    else:
        after = data
    for k, v in (("baud", a.baud), ("shape", a.shape), ("dev_hz", a.dev), ("dev_scale_pct", a.dev_scale),
                 ("offset_hz", a.offset), ("clip_hz", a.clip), ("repeat", a.repeat), ("gap_s", a.gap),
                 ("lead_s", a.lead), ("tail_s", a.tail), ("power_dbm", a.power_dbm), ("session", a.session),
                 ("wait_max", a.wait_max), ("max_s", a.max_s)):
        if v is not None:
            req[k] = v
    if a.power_dbm is None and a.level is not None:
        req["power"] = a.level
    for kv in a.field or []:
        k, _, v = kv.partition("=")
        req[k] = field_value(v)
    return req, after


def rx_request(a):
    req = {"op": "rx", "mode": "fsk4", "freq": a.freq, "seconds": a.seconds, "label": a.label or "fsk4_bench",
           "sync": a.sync, "sync_len": a.sync_len, "baud": a.baud, "nominal_dev_hz": a.nominal_dev}
    for k, v in (("max_symbols", a.max_symbols), ("bw", a.bw), ("session", a.session), ("wait_max", a.wait_max)):
        if v is not None:
            req[k] = v
    for kv in a.field or []:
        k, _, v = kv.partition("=")
        req[k] = field_value(v)
    return req


def disc_request(a):
    req = {"op": "rx", "mode": "fm", "freq": a.freq, "seconds": a.seconds, "streams": "disc",
           "label": a.label or "fsk4_bench"}
    for k, v in (("bw", a.bw), ("session", a.session), ("wait_max", a.wait_max)):
        if v is not None:
            req[k] = v
    return req


def stream(where, req, on_line, after=b""):
    """Send req (and the bytes after its line once the board's header is in); waits and notes to stderr, every other
    line to on_line, which may return an exit code to end with. An error reply ends it with bench_line's code."""
    c = None
    try:
        c = L.Conn(where)
        c.send(req)
        for d, data in c.lines():
            if "error" in d:
                print(f"rfoundry-bench: {d['error']}", file=sys.stderr, flush=True)
                return L.exit_code(L.BenchLineError(d["error"], d.get("code")))
            if d.get("state") == "queued":
                print(L.describe_wait(d), file=sys.stderr, flush=True)
                continue
            if d.get("state") == "granted":
                print(f"rfoundry-bench: got the {d.get('kind')} after {d.get('waited_s', 0):.0f} s", file=sys.stderr)
                continue
            if "note" in d and len(d) == 1:
                print(f"rfoundry-bench: {d['note']}", file=sys.stderr, flush=True)
                continue
            if after and ("tx" in d):
                # TCP's flow control paces it: all at once, on its own thread
                threading.Thread(target=lambda: c.s.sendall(after), daemon=True).start()
                after = b""
            rc = on_line(d, data)
            if rc is not None:
                return rc
        return 1
    except L.BenchLineError as e:
        print(f"rfoundry-bench: {e}", file=sys.stderr)
        return L.exit_code(e)
    except (OSError, ValueError) as e:
        print(f"rfoundry-bench: the bench board stopped answering: {e}", file=sys.stderr)
        return 69
    finally:
        if c:
            c.close()


def dry(req, after=b""):
    print(json.dumps(req))
    if after:
        print(f"(then {len(after)} bytes of dibits after the line)")
    return 0


def cmd_tx(a):
    data, n = read_dibits(a.dibits, a.symbols)
    req, after = tx_request(a, data, n)
    if a.dry_run:
        return dry(req, after)
    if not a.allow_rf:
        sys.exit("fsk4_bench tx: this keys the bench board's transmitter: --allow-rf (and the frequency leased)")

    def on(d, _):
        print(json.dumps(d), flush=True)
        if "done" in d:
            return 1 if d["done"].get("cut") or d["done"].get("symbols_short") else 0
        return None
    return stream(L.address(a.bench_api), req, on, after)


def cmd_rx(a):
    req = rx_request(a)
    if a.dry_run:
        return dry(req)
    rec = open(a.records, "w") if a.records else None
    syms = bytearray()
    nsyms = [0]

    def add_hex(h, n=None):
        b = bytes.fromhex(h)
        k = n if n is not None else 4 * len(b)
        # Packed four a byte: appended symbol by symbol, so a record that ends inside a byte joins up
        for i in range(k):
            d = (b[i >> 2] >> (6 - 2 * (i & 3))) & 3
            j = nsyms[0]
            if j % 4 == 0:
                syms.append(0)
            syms[j >> 2] |= d << (6 - 2 * (j & 3))
            nsyms[0] += 1

    def on(d, data):
        if rec:
            rec.write(json.dumps(d) + "\n")
        r = d.get("rec") or {}
        # The sliced symbols: as hex in a record, or as a payload after a line that says how many
        for key in ("dibits", "syms", "symbols"):
            if isinstance(r.get(key), str):
                add_hex(r[key], r.get("n") or r.get("nsyms"))
            elif isinstance(r.get(key), dict) and isinstance(r[key].get("hex"), str):
                add_hex(r[key]["hex"], r[key].get("n"))     # the bench API's form: {"dibits": {"t", "k", "n", "hex"}}
            elif isinstance(d.get(key), dict) and data is not None:
                add_hex(data.hex(), d[key].get("n") or d[key].get("nsyms"))
        if "rec" in d and not a.quiet:
            print(json.dumps(d), flush=True)
        elif "rec" not in d:
            print(json.dumps(d), flush=True)
        if "done" in d:
            return 0 if nsyms[0] or d["done"].get("samples") else 3
        return None
    try:
        rc = stream(L.address(a.bench_api), req, on)
    finally:
        if rec:
            rec.close()
        with open(a.out, "wb") as f:
            f.write(syms)
    print(f"{nsyms[0]} symbols -> {a.out}", file=sys.stderr)
    return rc


def cmd_disc(a):
    req = disc_request(a)
    if a.dry_run:
        return dry(req)
    meta = {"freq": a.freq, "rate": None, "full": None, "t0": None, "gap_samples": 0, "samples": 0}
    out = open(a.out, "wb")
    rate = [None]

    def on(d, data):
        if "rx" in d:
            h = d["rx"]
            rate[0] = h.get("rate")
            meta.update({"t0": h.get("t0"), "sc0": h.get("sc0"), "bw": h.get("bw"), "squelch_dbfs": h.get("squelch_dbfs")})
            for s in h.get("streams", []):
                if s.get("name") == "disc":
                    meta["rate"] = s.get("rate", rate[0])
                    meta["full"] = s.get("full")
                    meta["unit"] = s.get("unit")
            print(json.dumps(d), flush=True)
            return None
        if "pcm" in d and data is not None:
            if d["pcm"].get("stream") == "disc":
                out.write(data)
                meta["samples"] += len(data) // 2
            return None
        if "gap" in d and "samples" in d["gap"] and rate[0] and meta["rate"]:
            n = int(round(d["gap"]["samples"] * meta["rate"] / rate[0]))
            out.write(b"\0\0" * n)
            meta["gap_samples"] += n
        if "rec" not in d:
            print(json.dumps(d), flush=True)
        if "done" in d:
            return 0 if meta["samples"] else 3
        return None
    try:
        rc = stream(L.address(a.bench_api), req, on)
    finally:
        out.close()
        with open(os.path.splitext(a.out)[0] + ".json", "w") as f:
            json.dump(meta, f, indent=1)
    print(f"{meta['samples']} samples at {meta['rate']} Hz (full scale {meta['full']} Hz), {meta['gap_samples']} "
          f"filled -> {a.out}", file=sys.stderr)
    if rc == 0 and a.decode:
        cmd = [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "p25tool.py"), "disc", a.out,
               "--meta", os.path.splitext(a.out)[0] + ".json"]
        if a.expect:
            cmd += ["--expect", a.expect]
        return subprocess.call(cmd)
    return rc


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(s):
        s.add_argument("--bench-api", default=os.environ.get("RFOUNDRY_BENCH_API"), metavar="HOST[:PORT]",
                       help="the bench board ($RFOUNDRY_BENCH_API)")
        s.add_argument("--freq", type=float, required=True, help="MHz")
        s.add_argument("--session", default=None, help="a bench session's ID (bench_line.py session)")
        s.add_argument("--wait-max", type=float, default=None, help="s to wait for the board (exit 75 after)")
        s.add_argument("--label", default=None)
        s.add_argument("--dry-run", action="store_true", help="print the request and stop")

    t = sub.add_parser("tx", help="dibits on the carrier")
    common(t)
    t.add_argument("--dibits", required=True, help="packed four a byte, the first in bits 7-6")
    t.add_argument("--symbols", type=int, default=None, help="how many of the file's symbols (all)")
    t.add_argument("--stream", action="store_true", help="send the symbols after the line even when few")
    t.add_argument("--allow-rf", action="store_true", help="this keys the transmitter")
    t.add_argument("--shape", choices=("c4fm", "rrc", "rc", "nrz"), default=None, help="pulse (c4fm)")
    t.add_argument("--baud", type=float, default=None, help="symbols a second (4800)")
    t.add_argument("--dev", type=float, default=None, help="outer deviation, Hz (1800; 1944 with rrc)")
    t.add_argument("--dev-scale", type=float, default=None, help="percent of --dev (100)")
    t.add_argument("--offset", type=float, default=None, help="carrier offset, Hz")
    t.add_argument("--clip", type=float, default=None, help="deviation limit, Hz")
    t.add_argument("--repeat", type=int, default=None, help="plays (1)")
    t.add_argument("--gap", type=float, default=None, help="s unkeyed between plays (0: one keying)")
    t.add_argument("--lead", type=float, default=None, help="bare carrier before, s")
    t.add_argument("--tail", type=float, default=None, help="bare carrier after, s")
    t.add_argument("--max-s", type=float, default=None, help="cut the job after this many seconds")
    t.add_argument("--level", type=int, default=(int(os.environ["RFOUNDRY_BENCH_API_LEVEL"])
                                                 if os.environ.get("RFOUNDRY_BENCH_API_LEVEL") else None),
                   metavar="0-255", help="the board's transmit level ($RFOUNDRY_BENCH_API_LEVEL)")
    t.add_argument("--power-dbm", type=float, default=None, help="dBm at the antenna (over --level)")
    t.add_argument("--field", action="append", metavar="KEY=VALUE", help="another field of the request (repeatable)")
    t.set_defaults(fn=cmd_tx)

    r = sub.add_parser("rx", help="the board's 4FSK receiver: syncs, levels, sliced dibits")
    common(r)
    r.add_argument("--seconds", type=float, required=True)
    r.add_argument("--out", required=True, help="the sliced dibits, packed")
    r.add_argument("--records", default=None, help="every reply line, JSON")
    r.add_argument("--sync", default=P25_SYNC, help="hex, the pattern's dibits packed (P25's frame sync)")
    r.add_argument("--sync-len", type=int, default=24, help="symbols of --sync")
    r.add_argument("--baud", type=float, default=4800)
    r.add_argument("--nominal-dev", type=float, default=1800, help="outer deviation, Hz, for the ratios")
    r.add_argument("--max-symbols", type=int, default=None)
    r.add_argument("--bw", type=float, default=None, help="channel filter, Hz")
    r.add_argument("--field", action="append", metavar="KEY=VALUE")
    r.add_argument("--quiet", action="store_true", help="no records on stdout")
    r.set_defaults(fn=cmd_rx)

    d = sub.add_parser("disc", help="the FM discriminator, raw")
    common(d)
    d.add_argument("--seconds", type=float, required=True)
    d.add_argument("--out", required=True, help="int16 LE; FILE.json beside it")
    d.add_argument("--bw", type=float, default=None, help="channel filter, Hz (the board's 16000)")
    d.add_argument("--decode", action="store_true", help="p25tool.py disc on it afterwards")
    d.add_argument("--expect", default=None, help="with --decode: a p25tool.py gen expect file")
    d.set_defaults(fn=cmd_disc)

    a = ap.parse_args(argv)
    if not a.bench_api and not a.dry_run:
        ap.error("--bench-api HOST[:PORT] (or $RFOUNDRY_BENCH_API)")
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
