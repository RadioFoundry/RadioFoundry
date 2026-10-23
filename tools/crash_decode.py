#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Read a crash report (docs/spec/crash-log.md) and, with the build's ELF, say where it happened.

    tools/crash_decode.py REPORT [--elf build/dm1701/rfoundry_dm1701.elf]
    tools/crash_decode.py --hex 4f4301010103...  --elf rfoundry_dm32.elf
    tools/rfoundry_debug.py crash | tools/crash_decode.py - --elf rfoundry_dm1701.elf
    tools/crash_decode.py --self-test

REPORT is the packed report as received (the UDP payload, section 4) in a binary file, the same as hex text, or the
debug link's crash view (`rfoundry_debug.py crash`: crash.* lines). '-' reads standard input. With --elf, pc, lr and the
return addresses go through addr2line: arm-none-eabi-addr2line for a Cortex-M4 record, csky-elf-addr2line for a
CK803S one, or --addr2line / $RFOUNDRY_ADDR2LINE. The record's build must match the ELF's commit.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys

ARCHS = {1: "Cortex-M4", 2: "CK803S"}
ADDR2LINE = {1: "arm-none-eabi-addr2line", 2: "csky-elf-addr2line"}
CODE = {1: [(0x08000000, 0x08100000)], 2: [(0x03000000, 0x04000000)]}  # flash (CK803S: XIP; RAM code isn't known here)
KINDS = {1: "Fault", 2: "Stack overflow", 3: "Out of memory", 4: "Assert", 5: "Hang"}
FLAGS = [(0x01, "in an interrupt handler"), (0x02, "build had local changes"), (0x04, "stack copied"),
         (0x08, "seen"), (0x10, "sent")]
CK803_VECTORS = {1: "misaligned access", 2: "access error", 3: "divide by zero", 4: "illegal instruction",
                 7: "breakpoint", 8: "unrecoverable error"}
# CFSR (the Cortex-M4's configurable fault status): MemManage, BusFault and UsageFault bits
CFSR = [(0, "IACCVIOL instruction access violation"), (1, "DACCVIOL data access violation"),
        (3, "MUNSTKERR MemManage on unstacking"), (4, "MSTKERR MemManage on stacking"),
        (5, "MLSPERR MemManage in FP lazy state"), (7, "MMARVALID MMFAR holds the address"),
        (8, "IBUSERR instruction bus error"), (9, "PRECISERR precise data bus error"),
        (10, "IMPRECISERR imprecise data bus error"), (11, "UNSTKERR BusFault on unstacking"),
        (12, "STKERR BusFault on stacking"), (13, "LSPERR BusFault in FP lazy state"),
        (15, "BFARVALID BFAR holds the address"), (16, "UNDEFINSTR undefined instruction"),
        (17, "INVSTATE invalid state (Thumb bit)"), (18, "INVPC invalid EXC_RETURN"), (19, "NOCP no coprocessor"),
        (24, "UNALIGNED unaligned access"), (25, "DIVBYZERO divide by zero")]
HFSR = [(1, "VECTTBL vector table read"), (30, "FORCED escalated to HardFault"), (31, "DEBUGEVT debug event")]

PACK_FIXED = 76


class ReportError(Exception):
    pass


def unpack(data):
    """The packed report (crash-log.md 4) as a dict."""
    if len(data) < PACK_FIXED or data[0:2] != b"OC":
        raise ReportError("not a crash report: it starts with 'OC' and has at least 76 bytes")
    version, arch, kind, flags, count, n = data[2:8]
    if version != 1:
        raise ReportError(f"record version {version}: this tool reads version 1")
    if len(data) < PACK_FIXED + 4 * n:
        raise ReportError(f"cut short: {n} return addresses need {PACK_FIXED + 4 * n} bytes, got {len(data)}")
    build, uptime, cause, pc, lr, sp, psr = struct.unpack_from("<7I", data, 8)
    status = list(struct.unpack_from("<4I", data, 36))
    task = data[52:60].split(b"\0", 1)[0].decode("ascii", "replace")
    regs = list(struct.unpack_from("<4I", data, 60))
    rets = list(struct.unpack_from(f"<{n}I", data, PACK_FIXED))
    return dict(arch=arch, kind=kind, flags=flags, count=count, build=build, uptime_ms=uptime, cause=cause, pc=pc,
                lr=lr, sp=sp, psr=psr, status=status, task=task, r=regs, returns=rets)


def from_view(text):
    """The debug link's crash view (crash.* lines) as the same dict; the return addresses from crash.stack."""
    v = {}
    for line in text.splitlines():
        k, sep, val = line.strip().partition("=")
        if sep and k.startswith("crash."):
            v[k[6:]] = val
    if v.get("valid") != "1":
        raise ReportError("the view holds no crash record (crash.valid is not 1)")

    def num(k):
        return int(v.get(k, "0"), 0)

    arch = num("arch")
    stack = [int(w, 16) for w in v.get("stack", "").split(",") if w]
    rets = [w for w in stack if is_code(arch, w)][:12]
    tasks = []
    for k in range(num("tasks")):
        name, _, where = v.get(f"t{k}", ",0").rpartition(",")
        tasks.append((name.rstrip("\0 "), int(where, 16)))
    return dict(arch=arch, kind=num("kind"), flags=num("flags"), count=num("count"), build=int(v.get("build", "0"), 16),
                uptime_ms=num("uptime_ms"), cause=num("cause"), pc=num("pc"), lr=num("lr"), sp=num("sp"),
                psr=num("psr"), status=[int(w, 0) for w in v.get("status", "0,0,0,0").split(",")],
                task=v.get("task", ""), r=[num(f"r{k}") for k in range(14)], returns=rets, tasks=tasks,
                tasks_more=num("tasks_more"))


def is_code(arch, addr):
    return any(lo <= addr < hi for lo, hi in CODE.get(arch, []))


def read_report(raw):
    """Binary, hex text or the view text, whichever it is."""
    if raw[:2] == b"OC":
        return unpack(raw)
    text = raw.decode("ascii", "replace")
    if "crash." in text:
        return from_view(text)
    digits = "".join(text.split()).replace("0x", "")
    try:
        return unpack(bytes.fromhex(digits))
    except ValueError:
        raise ReportError("neither a packed report, its hex, nor the crash view") from None


def bits(word, table):
    return [name for bit, name in table if word >> bit & 1]


def lookup(addr2line, elf, addrs):
    """addr2line for each address: 'function at file:line'."""
    if not addrs:
        return []
    out = subprocess.run([addr2line, "-e", elf, "-f", "-C", "-i"] + [f"0x{a:x}" for a in addrs],
                         capture_output=True, text=True, check=True).stdout.splitlines()
    # -i can give more than two lines per address (inlined frames): resolve one address at a time when it does
    if len(out) != 2 * len(addrs):
        return [lookup(addr2line, elf, [a])[0] for a in addrs] if len(addrs) > 1 else [" / ".join(out)]
    return [f"{out[2 * k]} at {out[2 * k + 1]}" for k in range(len(addrs))]


def describe(rep, elf=None, addr2line=None):
    arch = rep["arch"]
    lines = [f"{KINDS.get(rep['kind'], 'Crash')} on {ARCHS.get(arch, f'arch {arch}')}"]
    cause = rep["cause"]
    if rep["kind"] == 1:
        why = CK803_VECTORS.get(cause, "") if arch == 2 else ("HardFault" if cause == 3 else "")
        lines.append(f"  exception {cause}" + (f" ({why})" if why else ""))
    elif rep["kind"] == 4:
        lines.append(f"  assertion at line {cause} (configASSERT: see the caller below)")
    elif rep["kind"] == 5:
        lines.append(f"  no watchdog feed for {cause} ms: pc is where the tick found the task that kept the idle task out")
    task = rep["task"] or ("(an interrupt handler)" if rep["flags"] & 1 else "(none)")
    lines.append(f"  task {task}, after {rep['uptime_ms'] / 1000:.1f} s, {rep['count']} in a row")
    build = f"{rep['build']:08x}" if rep["build"] else "unknown"
    lines.append(f"  build {build}; flags: " + (", ".join(n for b, n in FLAGS if rep["flags"] & b) or "none"))
    lines.append(f"  pc {rep['pc']:08x}  lr {rep['lr']:08x}  sp {rep['sp']:08x}  psr {rep['psr']:08x}")
    lines.append("  r0-r3 " + " ".join(f"{w:08x}" for w in rep["r"][:4]))
    st = rep["status"]
    if arch == 1:
        # MMFAR and BFAR mean something only with MMARVALID / BFARVALID set; otherwise they hold old contents
        mm = f"{st[2]:08x}" if st[0] >> 7 & 1 else "(not valid)"
        bf = f"{st[3]:08x}" if st[0] >> 15 & 1 else "(not valid)"
        lines.append(f"  CFSR {st[0]:08x}  HFSR {st[1]:08x}  MMFAR {mm}  BFAR {bf}")
        for name in bits(st[0], CFSR) + bits(st[1], HFSR):
            lines.append(f"    {name}")
    elif arch == 2:
        lines.append(f"  PSR in the handler {st[0]:08x}")

    # Thumb addresses have bit 0 set; a return address points after the call, so addr2line gets the byte before it
    where = [("pc", rep["pc"], rep["pc"] & ~1)]
    if is_code(arch, rep["lr"]):
        where.append(("lr", rep["lr"], (rep["lr"] & ~1) - 1))
    where += [(f"#{k}", a, (a & ~1) - 1) for k, a in enumerate(rep["returns"])]
    # The other tasks (record version 2, the view only): bit 0 set, waiting in the kernel, the rest the return address
    # of the code that waits; clear, ready, the rest its pc; 0 unknown
    others = [(name, w) for name, w in rep.get("tasks", [])]
    tool = None
    if elf:
        tool = addr2line or os.environ.get("RFOUNDRY_ADDR2LINE") or ADDR2LINE.get(arch, "addr2line")
        if shutil.which(tool) is None:
            raise ReportError(f"{tool} not found: pass --addr2line")
        names = lookup(tool, elf, [q for _, _, q in where])
        lines.append("  where:")
        for (label, a, _), name in zip(where, names):
            lines.append(f"    {label:>3} {a:08x}  {name}")
    else:
        lines.append("  where (pass --elf to name them): " + " ".join(f"{label} {a:08x}" for label, a, _ in where))
    if others:
        known = [(n, w) for n, w in others if w]
        names = dict(zip(known, lookup(tool, elf, [(w & ~1) - 1 if w & 1 else w for _, w in known]))) if tool else {}
        lines.append("  other tasks (waiting: the call that waits; ready: where it was):")
        for n, w in others:
            state = "unknown" if not w else "waiting" if w & 1 else "ready"
            at = f"  {w & ~1:08x}" if w else ""
            lines.append(f"    {n or '?':<4} {state:<7}{at}" + (f"  {names[(n, w)]}" if (n, w) in names else ""))
        if rep.get("tasks_more"):
            lines.append(f"    and {rep['tasks_more']} more")
    return "\n".join(lines)


def self_test():
    # The record crashTest.c's fault() makes, with three return addresses, packed by hand
    rets = [0x0801234B, 0x08055501, 0x080FFFFF]
    pkt = b"OC" + bytes([1, 1, 1, 0x04, 2, len(rets)])
    pkt += struct.pack("<7I", 0x34C825A6, 123456, 3, 0x0802C4A9, 0x0803082D, 0x20004F00, 0x21000000)
    pkt += struct.pack("<4I", 0x8200, 0x40000000, 0, 0xDEADBEEF) + b"radio\0\0\0"
    pkt += struct.pack("<4I", 0x1000, 0x1001, 0x1002, 0x1003) + struct.pack("<3I", *rets)
    assert len(pkt) == PACK_FIXED + 12
    for raw in (pkt, pkt.hex().encode(), (" ".join(f"{b:02x}" for b in pkt) + "\n").encode()):
        r = read_report(raw)
        assert r["kind"] == 1 and r["count"] == 2 and r["task"] == "radio" and r["returns"] == rets, r
        assert r["pc"] == 0x0802C4A9 and r["status"][3] == 0xDEADBEEF and r["r"][3] == 0x1003, r
    text = describe(read_report(pkt))
    for want in ("Fault on Cortex-M4", "exception 3 (HardFault)", "task radio", "build 34c825a6",
                 "PRECISERR", "BFARVALID", "FORCED", "BFAR deadbeef", "MMFAR (not valid)", "pc 0802c4a9", "lr 0803082d", "#0 0801234b"):
        assert want in text, (want, text)

    view = "\n".join(["crash.built=1", "crash.valid=1", "crash.arch=2", "crash.kind=4", "crash.cause=712",
                      "crash.flags=0x5", "crash.count=1", "crash.pc=0x03012a40", "crash.lr=0x03012a10",
                      "crash.sp=0x1fff8000", "crash.psr=0x80000000", "crash.status=0x1,0x0,0x0,0x0",
                      "crash.task=", "crash.uptime_ms=5000", "crash.build=00000000"] +
                     [f"crash.r{k}=0x{k:08x}" for k in range(14)] +
                     ["crash.stack=" + ",".join(["1fff9000", "03001235", "00000000", "03fffffe"] + ["0"] * 12)])
    r = read_report(view.encode())
    assert r["arch"] == 2 and r["kind"] == 4 and r["cause"] == 712 and r["returns"] == [0x03001235, 0x03fffffe], r
    text = describe(r)
    assert "assertion at line 712" in text and "(an interrupt handler)" in text and "build unknown" in text, text
    hang = view.replace("crash.kind=4", "crash.kind=5").replace("crash.cause=712", "crash.cause=3000")
    hang += "\ncrash.tasks=3\ncrash.tasks_more=1\ncrash.t0=radi,03012a41\ncrash.t1=Tmr ,03001000\ncrash.t2=ui,00000000"
    r = read_report(hang.encode())
    assert r["tasks"] == [("radi", 0x03012a41), ("Tmr", 0x03001000), ("ui", 0)] and r["tasks_more"] == 1, r
    text = describe(r)
    for want in ("radi waiting  03012a40", "Tmr  ready    03001000", "ui   unknown", "and 1 more"):
        assert want in text, (want, text)
    text = describe(read_report(hang.replace("crash.flags=0x5", "crash.flags=0x4").replace("crash.task=", "crash.task=host").encode()))
    assert "Hang on" in text and "no watchdog feed for 3000 ms" in text and "task host" in text, text

    for bad in (b"OC\x01", b"XX" + bytes(80), b"crash.built=1\ncrash.valid=0\n", b"not hex at all",
                pkt[:-1], b"OC\x02" + bytes(80)):
        try:
            read_report(bad)
        except ReportError:
            continue
        raise AssertionError(f"accepted {bad[:16]!r}")
    print("crash_decode self-test: ok")


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("report", nargs="?", help="a file with the report (binary, hex or the crash view), or -")
    p.add_argument("--hex", help="the packed report as hex on the command line")
    p.add_argument("--elf", help="the build's ELF (build/<board>/rfoundry_<board>.elf), to name the addresses")
    p.add_argument("--addr2line", help="the addr2line to use (default by the record's architecture)")
    p.add_argument("--self-test", action="store_true", help="check the decoder and exit")
    a = p.parse_args()
    if a.self_test:
        self_test()
        return 0
    if a.hex:
        raw = a.hex.encode()
    elif a.report == "-":
        raw = sys.stdin.buffer.read()
    elif a.report:
        with open(a.report, "rb") as f:
            raw = f.read()
    else:
        p.error("give a report file, - or --hex")
    try:
        print(describe(read_report(raw), a.elf, a.addr2line))
    except ReportError as e:
        print(f"crash_decode: {e}", file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as e:
        print(f"crash_decode: addr2line failed: {e.stderr.strip()}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
