#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""
dmr_bench: run a named DMR bench test case end to end from the Mac against the radio under test, with the SDR
bench's MMDVM_HS modem board and RTL-SDR as the other station and the instruments. New tool; the test cases and their
pass criteria are described in docs/testing/dmr-bench.md.

    dmr_bench.py list                          the test cases, in bring-up order
    dmr_bench.py show CASE                     what CASE does, what the user does, the pass criteria
    dmr_bench.py sync [--dry-run]              copy the bench-side tools to $ODR_SDR_BENCH_HOST:~/odr-bench/
    dmr_bench.py run CASE [--dry-run] [--yes] [--radio-id ID] [--tg TG] [--freq MHz] [--cc N] [--gain dB]
                          [--modem-power N] [--rpt-rx MHz --rpt-tx MHz] [--ambe FILE] [--port DEV] [--out DIR]
    dmr_bench.py check CASE SDRMON.json [--modem LISTEN.jsonl] [--radio-id ID] [--tg TG] [--cc N]
                                               re-run the automatic checks on saved results

--dry-run prints every command, in order, exactly as it would run (local and over ssh), including the rfgw service
stop and start, and runs nothing: give that to the owner for approval before the first real run of a case.

Configuration (environment): ODR_SDR_BENCH_HOST (ssh host or alias, default sdr-bench), ODR_SDR_BENCH_SERVICE
(default radiodesk-rfgw@$(hostname), expanded on the bench), ODR_SDR_BENCH_DIR (default odr-bench, under the bench
user's home), ODR_SDR_BENCH_PY (rfgw's venv python).

Service handling. The bench modem and the RTL-SDR are normally held by the rfgw service. A case that needs either
takes a *lease*: one detached process on the SDR bench stops the service, waits while ~/odr-bench/hold exists (at most
--lease-max seconds, default 300), then starts the service again, whatever happens to this script or the ssh link
(a signal to the lease process also ends the wait and restarts the service). This script removes the hold file in a
finally block and then checks that the service is active again. Cases that only need rfgw's own decode (T0) don't
stop anything.

The script never deletes captures; they collect in ~/odr-bench/runs/ on the SDR bench (about 50 MB per 16 s capture)
and the analysis JSON comes back to --out (default ./bench-results/). Clean up by hand when asked to.

Needs: ssh access to the SDR bench (passwordless sudo for systemctl there); the bench-side tools from `sync`; python3
on the Mac (no third-party packages for this script). The radio is operated by the user from its keys; the script
prompts for each action and for what the radio's diagnostics page shows.
"""
import argparse
import datetime
import json
import os
import shlex
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH_HOST = os.environ.get("ODR_SDR_BENCH_HOST", "sdr-bench")    # an ssh host or alias for the SDR bench
# The rfgw instance is named after the bench's host name: the default is expanded by the shell on the bench
SERVICE = os.environ.get("ODR_SDR_BENCH_SERVICE", "radiodesk-rfgw@$(hostname)")
DIR = os.environ.get("ODR_SDR_BENCH_DIR", "odr-bench")          # relative to the SDR bench user's home
PY = os.environ.get("ODR_SDR_BENCH_PY", "/opt/radiodesk-rfgw/.venv/bin/python")
BENCH_TOOLS = ["bench_tx.py", "bench_repeater.py", "sdrmon.py", "sdr_watch.py"]

# ── test cases ───────────────────────────────────────────────────────────────
# Step kinds: ("capture", seconds[, freq_key]) raw SDR capture in the background (READY + 2 s waited);
# ("listen", seconds) bench_tx listen in the background; ("modem", args) bench_tx in the foreground; ("repeater", seconds)
# bench_repeater in the background; ("prompt", text) the user acts on the radio, Enter when done; ("ask", [questions])
# what the radio shows; ("wait", s); ("watch", seconds) sdr_watch (no lease); ("local", argv) a Mac command;
# ("gains", [dB...], seconds) captures at several gains.
# Placeholders: {tg} {radio_id} {cc} {cc_wrong} {freq} {ambe} {port} {rpt_rx} {rpt_tx} {modem_power}

R_ASK_VOICE = ["Diagnostics: slot ticks still counting? (y/n)", "Voice bursts counted (number, or ?)",
               "CRC errors (number)", "Last CC shown", "Last sync family shown (MS/BS/TS1/TS2)",
               "Call shown on the main screen with the right caller and TG? (y/n)",
               "Call ended within about 0.5 s of the bench modem's end? (y/n)"]

CASES = [
    {"id": "C0", "phase": "setup", "title": "SDR level check: ADC clipping at several gains",
     "why": "The RTL-SDR clips with the radio on the bench (gain 15 was 28 % at 1 W). Pick a gain with < 0.1 % "
            "clipping before any TX analysis.",
     "user": "Set the radio to low power on the test channel. Hold PTT (FM or DMR) for the whole sweep (~30 s).",
     "steps": [("prompt", "Radio on low power, test channel. Press PTT and keep holding it, then press Enter."),
               ("gains", [0, 5, 10, 15], 4),
               ("prompt", "Release PTT. Enter.")],
     "pass": "Choose the highest gain with clip_pct < 0.1 % and burst level <= -6 dBFS; use it as --gain."},
    {"id": "T0", "phase": "radio transmits", "title": "Radio TX smoke test through rfgw's own SDR decode (no stop)",
     "why": "Zero-intrusion first look: does the radio put decodable DMR on air at all? rfgw keeps running.",
     "user": "Key PTT on a DMR channel at {freq} MHz, cc {cc}, for about 3 s, after the prompt.",
     "steps": [("watch", 15), ("prompt", "Within 10 s: press PTT for ~3 s on the DMR test channel, release.")],
     "pass": "sdr_watch shows ms-data headers, voice, a terminator, cc {cc}, src = the radio's ID, dst = the TG."},
    # (a) the radio receives
    {"id": "R1", "phase": "radio receives", "title": "Group voice from the bench modem (3 superframes, 8 headers)",
     "why": "First DMR RX: headers, voice A-F, embedded LC, terminator, all MS sync, our cc.",
     "user": "Radio on the DMR test channel, TG {tg} in its RX group list, diagnostics page open.",
     "steps": [("prompt", "Diagnostics page open on the radio, counters noted. Enter to send."),
               ("modem", "voice --dst {tg} --superframes 3"), ("ask", R_ASK_VOICE)],
     "pass": "Voice bursts 18 (+-1) more than before, CRC errors unchanged, cc {cc}, sync MS, call shown "
             "(9990301 -> TG {tg}) and ended by the terminator; speaker: AMBE silence (quiet)."},
    {"id": "R2", "phase": "radio receives", "title": "Private call to the radio's ID",
     "steps": [("modem", "voice --private --dst {radio_id} --superframes 3"), ("ask", R_ASK_VOICE)],
     "user": "Radio on the test channel.", "why": "FLCO 3, destination filter = own ID.",
     "pass": "Shown as a private call from 9990301; a private call to another ID (rerun with --radio-id 1) is not."},
    {"id": "R3", "phase": "radio receives", "title": "Late entry (no header, first 8 bursts left out)",
     "steps": [("modem", "voice --dst {tg} --late 8 --superframes 5"), ("ask", R_ASK_VOICE)],
     "user": "Radio on the test channel.", "why": "Call acquired from the embedded LC (dmr-modem.md 2.4).",
     "pass": "Call shown with 9990301 -> TG {tg} (not a stale LC) within ~1 s of the first burst; ended by the "
             "terminator; no voice burst counted as a call before a complete embedded LC."},
    {"id": "R4", "phase": "radio receives", "title": "Three calls back to back, 60 ms apart, sources alternating",
     "steps": [("modem", "voice --dst {tg} --superframes 2 --calls 3 --gap-ms 60"), ("ask", R_ASK_VOICE + [
         "Three separate calls seen, callers 9990301, 9990302, 9990301? (y/n)"])],
     "user": "Radio on the test channel.", "why": "Call end / start state machine, new LC per call.",
     "pass": "Three calls with the right callers, none merged or lost."},
    {"id": "R5", "phase": "radio receives", "title": "Call with no terminator (receiver time-out)",
     "steps": [("modem", "voice --dst {tg} --superframes 2 --terminators 0"), ("ask", R_ASK_VOICE)],
     "user": "Radio on the test channel.", "why": "End of call by ~390 ms of no burst (13 slot ticks).",
     "pass": "Call ends 0.3-0.8 s after the last burst; slot ticks keep counting; radio back to idle."},
    {"id": "R6", "phase": "radio receives", "title": "Wrong color code (bursts with cc+1)",
     "steps": [("modem", "voice --dst {tg} --superframes 2 --burst-cc {cc_wrong}"),
               ("ask", ["Call shown or audio heard? (y/n)", "Last CC shown", "Bursts / CRC errors counters"])],
     "user": "Radio on the test channel (cc {cc}).", "why": "CC filter.",
     "pass": "No call, no audio. Counters may show the bursts as other-CC (last CC {cc_wrong}) but never as voice."},
    {"id": "R7", "phase": "radio receives", "title": "CSBKs: 4 preambles then a call alert (CSBKO 0x1F) to the radio",
     "steps": [("modem", "--listen-after 3 csbk --dst {radio_id} --count 4 --opcode 1F"),
               ("ask", ["CSBK counter increased by 5? (number)", "Call alert shown? (y/n, later firmware)"])],
     "user": "Radio on the test channel.", "why": "Data-sync burst path, CSBK CRC, data service hook.",
     "pass": "5 CSBKs counted, CRC errors unchanged. With the data service: alert shown and the radio's ack CSBK "
             "in the bench modem's listen output (CSBKO 0x20)."},
    {"id": "R8", "phase": "radio receives", "title": "Unconfirmed data to the radio (2 preambles, header, 4 blocks)",
     "steps": [("modem", "data --dst {radio_id} --octets 40 --preambles 2"),
               ("ask", ["Data header counted (number)", "Rate 1/2 blocks counted (number)", "CRC errors"])],
     "user": "Radio on the test channel.", "why": "Data header + rate 1/2 blocks.",
     "pass": "2 CSBK + 1 header + 4 blocks counted, CRC errors unchanged."},
    {"id": "R9", "phase": "radio receives", "title": "Confirmed data to the radio, listen for its response",
     "steps": [("modem", "--listen-after 4 data --dst {radio_id} --octets 40 --confirmed"),
               ("ask", ["Data received (counters / message)? (y/n)"])],
     "user": "Radio on the test channel (needs the data service in the firmware).",
     "why": "Confirmed data and the radio's response packet (ACK) on air.",
     "pass": "The bench modem hears a response data header (DPF response) from {radio_id} within 1 s of the last block."},
    {"id": "R10", "phase": "radio receives", "title": "BS-synced voice in DMO timing (no CACH)",
     "steps": [("modem", "voice --dst {tg} --superframes 2 --family bs"),
               ("ask", ["Last sync family shown", "Voice bursts counted", "Call shown? (y/n)"])],
     "user": "Radio on a channel in repeater mode or 'any' framing, test frequency.",
     "why": "The receiver must detect all four sync families and say which (dmr-modem.md 1.3). Not a repeater "
            "outbound (no CACH, not continuous): see P1 for that.",
     "pass": "Sync family BS reported for the headers / voice A."},
    {"id": "R11", "phase": "radio receives", "title": "TDMA direct mode sync (TS1, then TS2)",
     "steps": [("modem", "voice --dst {tg} --superframes 2 --family ts1"), ("ask", ["Last sync family shown"]),
               ("modem", "voice --dst {tg} --superframes 2 --family ts2"), ("ask", ["Last sync family shown"])],
     "user": "Radio on the test channel.", "why": "DCDM / SFR framings.",
     "pass": "TS1 then TS2 reported."},
    {"id": "R12", "phase": "radio receives", "title": "Long call (50 superframes, 18 s)",
     "steps": [("modem", "voice --dst {tg} --superframes 50"), ("ask", R_ASK_VOICE + ["Any drop / restart? (y/n)"])],
     "user": "Radio on the test channel.", "why": "Slot clock stability, no SYNC_LOST over a long call.",
     "pass": "300 (+-3) voice bursts, one call, no restart, CRC errors unchanged."},
    {"id": "R13", "phase": "radio receives", "title": "Emergency, privacy flag, PI bit",
     "steps": [("modem", "voice --dst {tg} --superframes 2 --emergency"), ("ask", ["Emergency shown? (y/n)"]),
               ("modem", "voice --dst {tg} --superframes 2 --service-options 40 --pi"),
               ("ask", ["Privacy call muted / marked (no key configured)? (y/n)"])],
     "user": "Radio on the test channel, no privacy key.", "why": "Service options and EMB PI (dmr-modem.md 3).",
     "pass": "Emergency indicated; the PI call is not played (or played as configured)."},
    {"id": "R14", "phase": "radio receives", "title": "Talker alias in the embedded LC",
     "steps": [("modem", "voice --dst {tg} --superframes 8 --talker-alias 'ODR bench modem'"),
               ("ask", ["Alias shown ('ODR bench modem')? (y/n)"])],
     "user": "Radio on the test channel.", "why": "FLCO 4-7 assembly (dmr-modem.md 4).",
     "pass": "Alias shown within ~2 s; the call's LC stays right between alias superframes."},
    # (b) the radio transmits
    {"id": "T1", "phase": "radio transmits", "title": "Radio group call: SDR capture + bench modem listen",
     "why": "The full TX timing check: sync family, cc, headers, superframes, embedded LC, terminator, slot spacing, "
            "frequency offset, ramps, burst length.",
     "user": "Radio on the DMR test channel, TG {tg}, low power.",
     "steps": [("capture", 14), ("listen", 12),
               ("prompt", "Press PTT, say 'one two three' (about 3 s), release. Enter when released."),
               ("wait", 3)],
     "checks": "tx", "expect": {"flco": 0, "dst": "{tg}"},
     "pass": "All bursts MS family, cc {cc}; >= 1 (3 expected) voice LC headers with src {radio_id} dst {tg}; "
             "voice A-F in order; embedded LC = the header's; >= 1 terminator after the last voice; same-slot "
             "spacing 60.00 +- 0.05 ms (sd < 0.1); |frequency offset| < 500 Hz; burst on-time 27-31.5 ms; key-up "
             "/ key-down ramps < 1.5 ms; no power steps, no overlaps, no unexplained carrier; capture clip < 1 %. "
             "bench modem: one call, terminator, AMBE FEC corrections 0 on the bench."},
    {"id": "T2", "phase": "radio transmits", "title": "Radio PTT tap (< 0.3 s)",
     "steps": [("capture", 10), ("listen", 8), ("prompt", "Tap PTT as briefly as you can. Enter."), ("wait", 2)],
     "checks": "tx", "expect": {"flco": 0, "dst": "{tg}"}, "user": "As T1.",
     "why": "PTT release at a superframe boundary; a short over still has headers and a terminator.",
     "pass": "As T1; the over ends after a voice F (whole superframes) with a terminator."},
    {"id": "T3", "phase": "radio transmits", "title": "Radio long over (20 s)",
     "steps": [("capture", 28), ("listen", 26), ("prompt", "Hold PTT ~20 s (talk or stay quiet). Enter."),
               ("wait", 2)],
     "checks": "tx", "expect": {"flco": 0, "dst": "{tg}"}, "user": "As T1.",
     "why": "Slot clock drift and stability (no missed slot over 330 bursts).",
     "pass": "As T1; spacing sd < 0.1 ms over the whole over; no gaps (missing bursts) in the voice."},
    {"id": "T4", "phase": "radio transmits", "title": "Radio private call to 9990301",
     "steps": [("capture", 12), ("listen", 10),
               ("prompt", "Make a private call to 9990301 (~3 s). Enter."), ("wait", 2)],
     "checks": "tx", "expect": {"flco": 3, "dst": 9990301}, "user": "A private contact 9990301 on the radio.",
     "why": "FLCO 3 in the header, terminator and embedded LC.", "pass": "As T1 with FLCO 3, dst 9990301."},
    {"id": "T5", "phase": "radio transmits", "title": "Reply in the bench modem's call hang",
     "steps": [("capture", 16),
               ("prompt", "After Enter the bench modem sends a 1.5 s call to TG {tg}: as soon as it ends on the radio, press "
                          "PTT ~2 s (a reply). Press Enter now."),
               ("modem", "--listen-after 8 voice --dst {tg} --superframes 2"), ("wait", 2)],
     "checks": "tx", "expect": {"flco": 0}, "user": "Radio on the test channel with TG {tg}.",
     "why": "Reply timing in DMO (aligned to the call heard), capture timeout / hang.",
     "pass": "As T1 for the reply; the reply's dst is TG {tg} (talkback) and starts in step with the bench modem's slot "
             "(phase within +-1 ms)."},
    # (c) voice quality
    {"id": "Q1", "phase": "voice quality", "title": "Radio speech to AMBE files (off air, bench modem)",
     "steps": [("listen", 14), ("prompt", "Press PTT and read a sentence for ~8 s, release. Enter."), ("wait", 2)],
     "checks": "voice", "user": "Radio on the test channel.",
     "why": "Encoder + I2S + AMBE FEC on the TX side, judged off air without any vocoder in this project.",
     "pass": "One call; AMBE FEC corrections 0; non-silence frames during speech; decode the .amb with your own "
             "decoder (see docs/testing/dmr-bench.md) and listen: intelligible, no clicks or dropouts."},
    {"id": "Q2", "phase": "voice quality", "title": "Recorded speech from the bench modem to the radio's speaker",
     "steps": [("modem", "voice --dst {tg} --ambe {ambe}"),
               ("ask", ["Speech intelligible, level OK, no clicks? (y/n)", "CRC errors counter"])],
     "user": "Radio on the test channel, volume mid. Needs --ambe FILE (a 9-byte-frame .ambe recording).",
     "why": "Decoder + I2S + speaker path with real speech.", "pass": "Clean, intelligible speech."},
    # (d) repeater mode
    {"id": "P1", "phase": "repeater", "title": "Repeater: BS activation and a call through the bench modem repeater",
     "steps": [("capture", 40, "rpt"), ("repeater", 38),
               ("prompt", "Radio on its repeater channel (TX {rpt_rx}, RX {rpt_tx}). Press PTT ~3 s, release. "
                          "Then again within 3 s (hang). Enter."), ("wait", 4)],
     "checks": "repeater", "user": "A repeater channel on the radio, slot 1 or 2, cc {cc}.",
     "why": "BS_Dwn_Act CSBK, wait for the outbound, align to CACH TC, TX on the channel's slot; second over in "
            "the hang without a new activation.",
     "pass": "The repeater log shows a wakeup from {radio_id} then key up; the radio's call repeated on its slot; "
             "the second over needs no new wakeup. SDR (input): the radio's bursts in the channel's slot, phase "
             "locked to the outbound. Radio: shows its own call on the outbound / hears the hang."},
    # hotspot / SFR (planned firmware: USB + MMDVM)
    {"id": "H1", "phase": "hotspot (planned)", "title": "MMDVM hotspot cases (rf_* and net_*)",
     "steps": [("local", ["python3", "-I", os.path.join(HERE, "hotspot_cases.py"), "--port", "{port}", "--freq",
                          "{freq}", "--cc", "{cc}", "--cases", "all"])],
     "user": "Radio connected to the Mac over USB, hotspot mode. RadioDesk server venv python (RADIODESK).",
     "why": "Automated coverage of the modem driver and call stack: every burst heard / sent is visible.",
     "pass": "All cases PASS (see hotspot_cases.py --list)."},
    {"id": "S1", "phase": "SFR (planned)", "title": "SFR repeat of one bench modem call (raw SDR capture)",
     "steps": [("capture", 14), ("local", ["python3", "-I", os.path.join(HERE, "mmdvm_hotspot.py"), "--port",
                                           "{port}", "--sfr", "--seconds", "11", "--freq", "{freq}", "--cc", "{cc}"],
                                 "bg"),
               ("wait", 2), ("modem", "voice --superframes 1"), ("wait", 10)],
     "checks": "sfr", "user": "Radio over USB with SFR in the firmware.",
     "why": "The single frequency repeater: inbound MS on one slot, repeat on the other.",
     "pass": "inbound(MS) -> repeat gap 60 ms (+-1), repeat family as configured, no overlaps."},
]
BY_ID = {c["id"]: c for c in CASES}


# ── helpers ──────────────────────────────────────────────────────────────────

class Bench:
    def __init__(self, args):
        self.a = args
        self.dry = getattr(args, "dry_run", False)
        self.log = []
        self.bg = []
        self.leased = False
        self.out = None

    def fmt(self, s):
        a = self.a
        cc = a.cc
        return str(s).format(tg=a.tg, radio_id=a.radio_id, cc=cc, cc_wrong=(cc % 15) + 1, freq=a.freq,
                             ambe=f"~/{DIR}/runs/speech.ambe", port=a.port, rpt_rx=a.rpt_rx, rpt_tx=a.rpt_tx,
                             modem_power=a.modem_power)

    def note(self, line):
        self.log.append(line)
        print(line, flush=True)

    def local(self, argv, bg=False, capture=False, timeout=600):
        self.note("$ " + " ".join(shlex.quote(x) for x in argv) + (" &" if bg else ""))
        if self.dry:
            return ""
        if bg:
            p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            self.bg.append(p)
            return p
        r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        if r.returncode != 0:
            print(r.stdout[-2000:], r.stderr[-2000:], file=sys.stderr)
        return r.stdout if capture else r.returncode == 0

    def ssh(self, cmd, bg=False, capture=False, timeout=600):
        return self.local(["ssh", BENCH_HOST, cmd], bg=bg, capture=capture, timeout=timeout)

    # The lease: a detached process on the SDR bench owns the service stop and its restart
    def lease(self):
        hold = f"~/{DIR}/hold"
        guard = (f"trap 'rm -f {hold}' TERM HUP INT; sudo systemctl stop {SERVICE}; t=0; "
                 f"while [ -e {hold} ] && [ $t -lt {self.a.lease_max} ]; do sleep 1; t=$((t+1)); done; "
                 f"rm -f {hold}; sudo systemctl start {SERVICE}; rc=$?; "
                 f"echo \"$(date -Is) restarted rc=$rc\" >> ~/{DIR}/lease.log")
        # Only the guard goes to the background: "a && b && guard &" would background the whole list in a subshell
        # that keeps ssh's stdout open, so ssh would wait the whole lease out
        self.ssh(f"mkdir -p ~/{DIR}/runs && touch {hold} && "
                 f"{{ setsid nohup bash -c {shlex.quote(guard)} > /dev/null 2>&1 < /dev/null & }}")
        self.leased = True
        self.ssh(f"for i in $(seq 1 40); do systemctl is-active --quiet {SERVICE} || exit 0; sleep 0.25; done; "
                 f"echo 'service still active' >&2; exit 1")

    def release(self):
        if not self.leased:
            return
        self.ssh(f"rm -f ~/{DIR}/hold; for i in $(seq 1 80); do systemctl is-active --quiet {SERVICE} && "
                 f"{{ echo {SERVICE} active; exit 0; }}; sleep 0.25; done; "
                 f"echo \"{SERVICE} NOT active: run sudo systemctl start {SERVICE}\" >&2; exit 1")
        self.leased = False

    def user(self, text):
        self.note(f">>> USER: {text}")
        if self.dry:
            return ""
        if self.a.yes:
            time.sleep(3)
            return ""
        return input("    (Enter) ")


def needs_lease(case):
    return any(s[0] in ("capture", "listen", "modem", "repeater", "gains") for s in case["steps"]) \
        or case["id"].startswith("H")


# ── automatic checks ─────────────────────────────────────────────────────────

class Checks:
    def __init__(self):
        self.items = []

    def add(self, what, ok, detail=""):
        self.items.append({"check": what, "ok": None if ok is None else bool(ok), "detail": detail})

    @property
    def verdict(self):
        if any(i["ok"] is False for i in self.items):
            return "FAIL"
        if not self.items or all(i["ok"] is None for i in self.items):
            return "MANUAL"
        return "PASS"


def check_tx(sdr, expect, radio_id, cc, c):
    meta = sdr.get("meta", {})
    clip = meta.get("clip_pct_analyzed") or meta.get("clip_pct") or 0
    c.add("capture not saturated (clip < 1 %)", clip < 1.0, f"{clip} %")
    b = sdr.get("bursts", [])
    edges_all = sdr.get("edges", [])
    # Only the radio's transmission: from its first voice LC header (by source ID) on, on that header's slot. Without
    # --radio-id everything in the capture counts (keep the bench modem off the air then).
    mine = [x for x in b if x["kind"] == "VLC_HDR" and x.get("lc") and radio_id and x["lc"].get("src") == radio_id]
    if mine:
        t0, slot = mine[0]["t_ms"], mine[0]["slot"]
        b = [x for x in b if x["t_ms"] >= t0 - 1 and x["slot"] == slot]
        edges_all = [e for e in edges_all if e["up_ms"] >= t0 - 40]
    c.add("bursts found" + (" (from the radio's first header)" if mine else ""), len(b) > 0, f"{len(b)}")
    if not b:
        return
    fams = sorted({x["family"] for x in b if x["family"] != "emb"})
    c.add("sync family MS only (DMO)", fams == ["ms"], ",".join(fams))
    ccs = sorted({x["cc"] for x in b if x["cc"] is not None})
    c.add(f"color code {cc} only", ccs == [cc], f"{ccs}")
    hdr = [x for x in b if x["kind"] == "VLC_HDR"]
    term = [x for x in b if x["kind"] == "TERM_LC"]
    voice = [x for x in b if x["kind"].startswith("VOICE")]
    c.add(">= 1 voice LC header", len(hdr) >= 1, f"{len(hdr)} (3 expected)")
    c.add("3 voice LC headers (dmr-modem.md 2.5)", None if len(hdr) >= 1 and len(hdr) != 3 else len(hdr) == 3,
          f"{len(hdr)}")
    lcs = [x["lc"] for x in hdr + term if x.get("lc")]
    if lcs:
        lc = lcs[0]
        if radio_id:
            c.add("LC source = radio ID", all(x.get("src") == radio_id for x in lcs), f"{lc.get('src')}")
        if expect.get("dst") not in (None, "", "None"):
            c.add("LC destination", all(x.get("dst") == int(expect["dst"]) for x in lcs), f"{lc.get('dst')}")
        if "flco" in expect:
            c.add("FLCO", all(x.get("flco") == expect["flco"] for x in lcs), f"{lc.get('flco')}")
    else:
        c.add("header / terminator LC decoded", False)
    emb = [x["lc"] for x in voice if x.get("lc") and "emb_flco" in x["lc"]]
    c.add("embedded LC decoded (B-E)", len(emb) > 0, f"{len(emb)} superframes")
    if emb and lcs:
        c.add("embedded LC = header LC", all((e.get("src"), e.get("dst")) == (lcs[0].get("src"), lcs[0].get("dst"))
                                            for e in emb), "")
    if voice:
        c.add("terminator after the last voice", any(t["t_ms"] > voice[-1]["t_ms"] for t in term), f"{len(term)}")
        letters = [x["kind"][6:7] for x in voice]
        c.add("voice A-F in order (whole superframes)", "".join(letters).replace("ABCDEF", "") == "",
              "".join(letters)[-24:])
    sp = sdr["summary"].get("same_slot_spacing_ms") or {}
    c.add("same-slot spacing 60.00 +- 0.05 ms", sp and abs(sp["mean"] - 60.0) <= 0.05 and sp["sd"] < 0.1, f"{sp}")
    fo = sorted(x["foff_hz"] for x in b)
    med = fo[len(fo) // 2]
    c.add("|frequency offset| < 500 Hz", abs(med) < 500, f"median {med} Hz")
    edges = [e for e in edges_all if "on_ms" in e]
    per_burst = [e for e in edges if 5 <= e["on_ms"] < 40]       # under 5 ms: noise blips
    if per_burst:
        on = [e["on_ms"] for e in per_burst]
        c.add("burst on-time 27-31.5 ms", all(27 <= x <= 31.5 for x in on), f"{min(on):.2f}-{max(on):.2f}")
        ru = max(e["ramp_up_ms"] for e in per_burst)
        rd = max(e["ramp_down_ms"] for e in per_burst)
        c.add("ramps < 1.5 ms", ru < 1.5 and rd < 1.5, f"up {ru}, down {rd}")
    else:
        long = [e["on_ms"] for e in edges if e["on_ms"] >= 40]
        c.add("per-burst keying (carrier off in the other slot)", False,
              f"continuous carrier segments {long[:3]} ms" if long else "no keyed segment")
    s = sdr["summary"]
    c.add("no overlapping bursts", s["overlaps"] == 0, f"{s['overlaps']}")
    c.add("no power steps inside bursts", s["power_steps"] == 0, f"{s['power_steps']}")
    c.add("no unexplained carrier", not s["unexplained_carrier"], f"{s['unexplained_carrier'][:3]}")


def check_modem_calls(recs, radio_id, c, voice_quality=False):
    calls = [r for r in recs if "call" in r]
    c.add("bench modem heard one call", len(calls) == 1, f"{len(calls)}")
    for k in calls[:1]:
        c.add("bench modem: ended by terminator", k.get("ended") == "terminator", f"{k.get('ended')}")
        c.add("bench modem: started by header (not late entry)", k.get("start") == "header", k.get("start"))
        if radio_id:
            c.add("bench modem: source = radio ID", k.get("src") == radio_id, f"{k.get('src')}")
        c.add("bench modem: AMBE FEC corrections 0 (bench)", k.get("ambe_fec_bits_corrected", 1) == 0,
              f"{k.get('ambe_fec_bits_corrected')}")
        c.add("bench modem: voice bursts in whole superframes", k.get("voice", 0) % 6 == 0, f"{k.get('voice')}")
        if voice_quality:
            c.add("bench modem: speech present (non-silence frames)",
                  k.get("ambe_frames", 0) - k.get("ambe_silence_frames", 0) > 50,
                  f"{k.get('ambe_frames')} frames, {k.get('ambe_silence_frames')} silence")
            c.add("listen to the .amb with an AMBE+2 decoder", None, f"{k.get('files')}")


def check_repeater(recs, radio_id, c):
    wake = [r for r in recs if "wakeup" in r]
    keys = [r for r in recs if "key" in r]
    rx = [r["rx"] for r in recs if "rx" in r]
    c.add("BS_Dwn_Act wakeup heard", len(wake) > 0,
          f"{len(wake)} from {sorted({w['wakeup'] for w in wake}, key=str)}")
    if radio_id and wake:
        c.add("wakeup source = radio ID", any(w["wakeup"] == radio_id for w in wake), "")
    c.add("repeater keyed up", any(k["key"] for k in keys), f"{len(keys)} key events")
    hdr = [r for r in rx if r.get("dt") == 1]
    c.add("radio's voice header heard through the repeater input", len(hdr) > 0, f"{len(hdr)}")
    c.add("radio voice heard", sum(1 for r in rx if "voice" in r) > 0, "")


def check_sfr(sdr, c):
    gaps = sdr["summary"].get("repeat_gap_ms") or []
    c.add("repeats seen (TS/BS after MS)", len(gaps) > 0, f"{len(gaps)}")
    if gaps:
        med = gaps[len(gaps) // 2]
        c.add("repeat gap 60 +- 1 ms", abs(med - 60) <= 1, f"median {med}")
    c.add("no overlapping bursts", sdr["summary"]["overlaps"] == 0, "")


def run_checks(case, a, sdr=None, modem=None, rpt=None):
    c = Checks()
    kind = case.get("checks")
    exp = {k: (BENCH_FMT(a, v) if isinstance(v, str) else v) for k, v in case.get("expect", {}).items()}
    if kind in ("tx", "sfr") and sdr is None:
        c.add("SDR analysis available", False)
    if kind == "tx" and sdr is not None:
        check_tx(sdr, exp, a.radio_id, a.cc, c)
    if kind == "tx" and modem is not None and case["id"] != "T5":
        check_modem_calls(modem, a.radio_id, c)
    if kind == "voice" and modem is not None:
        check_modem_calls(modem, a.radio_id, c, voice_quality=True)
    if kind == "repeater" and rpt is not None:
        check_repeater(rpt, a.radio_id, c)
    if kind == "sfr" and sdr is not None:
        check_sfr(sdr, c)
    return c


def BENCH_FMT(a, s):
    return Bench(a).fmt(s)


def jsonl(text):
    out = []
    for line in (text or "").splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                out.append(json.loads(line))
            except ValueError:
                pass
    return out


# ── commands ─────────────────────────────────────────────────────────────────

def cmd_list(a):
    for c in CASES:
        print(f"{c['id']:4s} {c['phase']:18s} {c['title']}")


def cmd_show(a):
    c = BY_ID[a.case]
    b = Bench(a)
    print(f"{c['id']} ({c['phase']}): {c['title']}\n\nWhy: {b.fmt(c.get('why', ''))}\nUser: {b.fmt(c.get('user', ''))}"
          f"\nPass: {b.fmt(c['pass'])}\n\nSteps:")
    for s in c["steps"]:
        print("  ", s[0], b.fmt(s[1]) if len(s) > 1 and not isinstance(s[1], list) else (s[1] if len(s) > 1 else ""))
    print(f"\nService lease (rfgw stopped and restored): {'yes' if needs_lease(c) else 'no'}")


def cmd_sync(a):
    b = Bench(a)
    b.ssh(f"mkdir -p ~/{DIR}/runs")
    b.local(["scp"] + [os.path.join(HERE, t) for t in BENCH_TOOLS] + [f"{BENCH_HOST}:{DIR}/"])


def cmd_run(a):
    case = BY_ID[a.case]
    b = Bench(a)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    run = f"{stamp}_{case['id']}"
    rdir = f"~/{DIR}/runs/{run}"
    out = os.path.join(a.out, run)
    if not b.dry:
        os.makedirs(out, exist_ok=True)
    modem = f"cd ~/{DIR} && {PY} bench_tx.py --freq {a.freq} --cc {a.cc} --power {a.modem_power}"
    results = {"case": case["id"], "title": case["title"], "run": run, "args": vars(a), "radio": [], "steps": []}
    captures, listen_p, rpt_p, sdr, modem_recs, rpt_recs = [], None, None, None, [], []

    def interrupted(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    b.note(f"# {case['id']}: {case['title']}  ->  {out}")
    b.note(f"# {b.fmt(case.get('user', ''))}")
    try:
        if a.ambe:
            b.ssh(f"mkdir -p ~/{DIR}/runs")
            b.local(["scp", a.ambe, f"{BENCH_HOST}:{DIR}/runs/speech.ambe"])
        if needs_lease(case):
            b.lease()
        b.ssh(f"mkdir -p {rdir}")
        for step in case["steps"]:
            kind = step[0]
            if kind == "capture":
                secs = step[1]
                freq, off = a.freq, a.offset_khz
                if len(step) > 2 and step[2] == "rpt":
                    freq = a.rpt_rx
                    # one capture holding both repeater frequencies, its center off both channels
                    center = (a.rpt_rx + a.rpt_tx) / 2 + 0.1
                    off = (a.rpt_rx - center) * 1000
                name = f"{rdir}/cap{len(captures)}.cs8"
                b.ssh(f"cd ~/{DIR} && (setsid nohup {PY} sdrmon.py capture --service '' --seconds {secs} --freq {freq} "
                      f"--offset-khz {off:.1f} --gain {a.gain} --out {name} > {name}.log 2>&1 &) ; "
                      f"for i in $(seq 1 60); do grep -q READY {name}.log && break; sleep 0.25; done; sleep 2; "
                      f"grep READY {name}.log")
                captures.append((name, freq, off, secs))
            elif kind == "gains":
                for g in step[1]:
                    name = f"{rdir}/gain{g}.cs8"
                    b.ssh(f"cd ~/{DIR} && {PY} sdrmon.py capture --service '' --seconds {step[2]} --freq {a.freq} "
                          f"--offset-khz {a.offset_khz} --gain {g} --out {name} 2>/dev/null | tail -2")
                    if not b.dry:
                        meta = json.loads(b.ssh(f"cat {name}.json", capture=True) or "{}")
                        results["steps"].append({"gain": g, "clip_pct": meta.get("clip_pct")})
                        b.note(f"    gain {g}: ADC clipping {meta.get('clip_pct')} %")
            elif kind == "listen":
                listen_p = b.ssh(f"{modem} --ambe-out {rdir}/heard listen --json --seconds {step[1]}", bg=True)
                if not b.dry:
                    time.sleep(3)       # port open + tune + 1 s settle
            elif kind == "repeater":
                rpt_p = b.ssh(f"cd ~/{DIR} && {PY} bench_repeater.py --rx {a.rpt_rx} --tx {a.rpt_tx} --cc {a.cc} "
                              f"--power {a.modem_power} --seconds {step[1]} --json", bg=True)
                if not b.dry:
                    time.sleep(3)
            elif kind == "modem":
                text = b.ssh(f"{modem} {b.fmt(step[1])}", capture=True, timeout=300)
                modem_recs += jsonl(text)
                results["steps"].append({"modem": b.fmt(step[1]), "ok": text is not None})
            elif kind == "watch":
                listen_p = b.ssh(f"cd ~/{DIR} && export $(sudo cat /etc/radiodesk-rfgw/env | xargs) && "
                                 f"{PY} -W ignore sdr_watch.py --receiver {a.receiver} --seconds {step[1]} --json",
                                 bg=True)
            elif kind == "prompt":
                b.user(b.fmt(step[1]))
            elif kind == "ask":
                for q in step[1]:
                    ans = b.user(b.fmt(q))
                    results["radio"].append({"q": b.fmt(q), "a": ans})
            elif kind == "wait":
                b.note(f"# wait {step[1]} s")
                if not b.dry:
                    time.sleep(step[1])
            elif kind == "local":
                argv = [b.fmt(x) for x in step[1]]
                if len(step) > 2 and step[2] == "bg":
                    b.local(argv, bg=True)
                else:
                    ok = b.local(argv)
                    results["steps"].append({"local": argv, "ok": ok})
        # collect background processes
        for p in b.bg:
            try:
                txt, _ = p.communicate(timeout=120)
            except subprocess.TimeoutExpired:
                p.kill()
                txt = ""
            if p is listen_p:
                modem_recs += jsonl(txt)
                with open(os.path.join(out, "listen.jsonl"), "w") as f:
                    f.write(txt)
            elif p is rpt_p:
                rpt_recs = jsonl(txt)
                with open(os.path.join(out, "repeater.jsonl"), "w") as f:
                    f.write(txt)
        if captures:
            b.ssh(f"for i in $(seq 1 120); do pgrep -f 'sdrmon[.]py capture' > /dev/null || exit 0; sleep 0.5; done")
    except KeyboardInterrupt:
        b.note("# interrupted")
    finally:
        for p in b.bg:
            if p.poll() is None:
                p.terminate()
        b.release()
    # analysis runs with rfgw back up (about 150 MB for a 15 s capture on the 824 MB SDR bench)
    for i, (name, freq, off, secs) in enumerate(captures):
        freqs = [freq] + ([a.rpt_tx] if case.get("checks") == "repeater" else [])
        for f_ in freqs:
            tag = f"{os.path.basename(name)}_{f_}"
            txt = b.ssh(f"cd ~/{DIR} && {PY} sdrmon.py analyze {name} --freq {f_} --cc {a.cc} "
                        f"--json {name}.{f_}.json > {name}.{f_}.txt 2>&1; cat {name}.{f_}.json", capture=True)
            if not b.dry and txt:
                with open(os.path.join(out, tag + ".json"), "w") as fh:
                    fh.write(txt)
                b.local(["scp", f"{BENCH_HOST}:{name[2:]}.{f_}.txt", os.path.join(out, tag + ".txt")])
                if sdr is None:
                    sdr = json.loads(txt)
    if b.dry:
        b.note("# dry run: nothing was executed")
        return 0
    if modem_recs and case["id"] == "T0":
        fams = {}
        for r in modem_recs:
            fams[(r.get("family"), r.get("type"), r.get("cc"), r.get("src"), r.get("dst"))] = \
                fams.get((r.get("family"), r.get("type"), r.get("cc"), r.get("src"), r.get("dst")), 0) + 1
        for k, v in sorted(fams.items(), key=str):
            b.note(f"    {v:4d}  family {k[0]} type {k[1]} cc {k[2]} src {k[3]} dst {k[4]}")
    checks = run_checks(case, a, sdr=sdr, modem=modem_recs or None, rpt=rpt_recs or None)
    results["checks"] = checks.items
    results["verdict"] = checks.verdict if case.get("checks") else "MANUAL"
    results["commands"] = b.log
    with open(os.path.join(out, "report.json"), "w") as f:
        json.dump(results, f, indent=1, default=str)
    with open(os.path.join(out, "report.md"), "w") as f:
        f.write(report_md(case, results, b))
    print(open(os.path.join(out, "report.md")).read())
    return 0 if results["verdict"] != "FAIL" else 1


def report_md(case, r, b):
    lines = [f"# {case['id']}: {case['title']}", "", f"- run: `{r['run']}`", f"- verdict: **{r['verdict']}**",
             f"- pass criteria: {b.fmt(case['pass'])}", "", "## Automatic checks", "", "| check | result | detail |",
             "|---|---|---|"]
    for i in r.get("checks", []):
        res = {True: "ok", False: "FAIL", None: "manual"}[i["ok"]]
        lines.append(f"| {i['check']} | {res} | {i['detail']} |")
    if r["radio"]:
        lines += ["", "## Radio (user observation)", "", "| question | answer |", "|---|---|"]
        lines += [f"| {x['q']} | {x['a']} |" for x in r["radio"]]
    lines += ["", "## Commands", "", "```"] + r.get("commands", []) + ["```", ""]
    return "\n".join(lines)


def cmd_check(a):
    case = BY_ID[a.case]
    sdr = json.load(open(a.sdr)) if a.sdr else None
    modem = jsonl(open(a.modem).read()) if a.modem else None
    c = run_checks(case, a, sdr=sdr, modem=modem, rpt=modem if case.get("checks") == "repeater" else None)
    for i in c.items:
        print(f"{ {True: 'ok  ', False: 'FAIL', None: 'man '}[i['ok']]} {i['check']}  {i['detail']}")
    print(c.verdict)
    return 0 if c.verdict != "FAIL" else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")

    def common(p):
        p.add_argument("--radio-id", type=int, default=None, help="the radio's own DMR ID (for LC checks)")
        p.add_argument("--tg", type=int, default=9, help="talkgroup on the radio's test channel")
        p.add_argument("--freq", type=float, default=461.6875)
        p.add_argument("--cc", type=int, default=2)
        p.add_argument("--gain", type=float, default=5.0, help="RTL-SDR gain for captures (C0 picks it)")
        p.add_argument("--offset-khz", type=float, default=-200.0, help="channel minus SDR center (DC spike)")
        p.add_argument("--modem-power", type=int, default=40, help="bench modem RF level 0-255 (bench: low)")
        p.add_argument("--rpt-rx", type=float, default=None, help="repeater input MHz (P cases)")
        p.add_argument("--rpt-tx", type=float, default=None, help="repeater output MHz (P cases)")
        p.add_argument("--ambe", default=None, help="Q2: a 9-byte-frame .ambe speech recording on the Mac")
        p.add_argument("--port", default=None, help="H/S cases: the radio's MMDVM serial port")
        p.add_argument("--receiver", default="SDR15", help="rfgw SDR receiver on --freq (T0)")

    s = sub.add_parser("show")
    s.add_argument("case", choices=list(BY_ID))
    common(s)
    s = sub.add_parser("sync")
    s.add_argument("--dry-run", action="store_true")
    s = sub.add_parser("run")
    s.add_argument("case", choices=list(BY_ID))
    s.add_argument("--dry-run", action="store_true")
    s.add_argument("--yes", action="store_true", help="don't wait for Enter at prompts (3 s pauses)")
    s.add_argument("--out", default="bench-results")
    s.add_argument("--lease-max", type=int, default=300, help="s: the rfgw service comes back after this at most")
    common(s)
    s = sub.add_parser("check")
    s.add_argument("case", choices=list(BY_ID))
    s.add_argument("sdr", nargs="?", default=None)
    s.add_argument("--modem", default=None)
    common(s)
    a = ap.parse_args()
    if a.cmd == "run" and BY_ID[a.case].get("checks") == "repeater" and not (a.rpt_rx and a.rpt_tx):
        ap.error("P cases need --rpt-rx and --rpt-tx (a licensed split pair, within ~1 MHz so one SDR capture "
                 "holds both)")
    if a.cmd == "run" and a.case == "Q2" and not a.ambe:
        ap.error("Q2 needs --ambe FILE")
    if a.cmd == "run" and a.case in ("H1", "S1") and not a.port:
        ap.error(f"{a.case} needs --port (the radio's USB MMDVM serial port): planned firmware capability")
    return {"list": cmd_list, "show": cmd_show, "sync": cmd_sync, "run": cmd_run, "check": cmd_check}[a.cmd](a) or 0


if __name__ == "__main__":
    sys.exit(main())
