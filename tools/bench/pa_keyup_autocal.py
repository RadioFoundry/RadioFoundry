#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""
pa_keyup_autocal: tune a radio's DMR PA key-up constants (docs/spec/pa-keyup.md 5) on a conducted path, unattended.

The radio transmits into a dummy load; a tap through fixed pads feeds an AD936x SDR's receive port (libiio, raw I/Q).
Each try sets the constants (RAM, debug link op 37), selects a power level, keys DMR for --key-s and measures the
bursts' median -3 dB point against the first symbol, the overshoot and the first symbol's level (burstramp.py).

Steps: preflight (SDR gain and LO, input level, the radio's test TX limit), baseline with the board's constants, the
loop with the boost off (dead zone, knee, low-pass), then either
  - a loop with a knee: knee at the lowest level, slope at the top level, late for the levels between, or
  - a loop with no knee (every level up the same time after the PA's point): the boost off, the PA's point only,
and a verify of every level, twice. It prints the constants and the debug link command, and always restores the
board's constants and the lowest power level at the end.

The radio and the SDR may be on other hosts (--radio-ssh, --sdr-ssh): the debug link runs where the radio is plugged in,
the capture and its analysis where the SDR is. This file and burstramp.py are copied to --sdr-dir there.

    pa_keyup_autocal.py --radio-port PORT --sdr-uri usb:1.2.5 --freq 446.05 --pad-db 57 --max-w 5 --boost-us 2550

`analyze` is the capture host's half (cs16 capture -> one JSON line).
"""
import argparse
import json
import math
import os
import shlex
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TARGET = -0.25          # ms: the -3 dB point before the first symbol (pa-keyup.md 5)
TOL = 0.06
EARLY_MAX = -0.35       # verify: no level more than about 0.3 ms early
OVER_MAX = 3.0          # dB
SYM1_MIN = -3.0         # dB
OFFSET_HZ = 200e3       # the LO this far under the carrier, clear of the SDR's DC


# ---- the capture host's half -------------------------------------------------------------------------------------

def cut(src, fs, off_hz, peak_full=2048.0):
	"""A raw cs16 capture cut to the channel at about 288 kS/s by FFT, in dBFS units: flat to +-60 kHz, zero past
	+-130 kHz. Returns (complex64 samples, rate)."""
	import numpy as np
	r = np.fromfile(src, dtype=np.int16)
	x = r[0::2].astype(np.float32) + 1j * r[1::2].astype(np.float32)
	n = len(x)
	X = np.fft.fft(x)
	k = int(round(n * 288e3 / fs)) & ~1
	f = np.fft.fftfreq(k, d=1.0 / (fs * k / n))
	c = int(round(off_hz * n / fs))
	Y = X[(c + np.round(f * n / fs).astype(np.int64)) % n]
	a = np.abs(f)
	taper = np.where(a <= 60e3, 1.0, np.where(a >= 130e3, 0.0, 0.5 + 0.5 * np.cos(np.pi * (a - 60e3) / 70e3)))
	y = (np.fft.ifft(Y * taper) * (k / n) / peak_full).astype(np.complex64)
	return y, fs * k / n


def analyze(argv):
	ap = argparse.ArgumentParser(prog="pa_keyup_autocal.py analyze")
	ap.add_argument("file"); ap.add_argument("--fs", type=float, required=True)
	ap.add_argument("--off-hz", type=float, default=OFFSET_HZ)
	a = ap.parse_args(argv)
	sys.path.insert(0, HERE)
	import burstramp
	y, rate = cut(a.file, a.fs, a.off_hz)
	tmp = a.file + ".cf32"
	y.view("float32").tofile(tmp)
	json.dump({"fmt": "cf32", "fs": rate, "rate": rate, "freq": 0, "center": 0, "bw": 120e3}, open(tmp + ".json", "w"))
	try:
		s = burstramp.analyze(tmp)["summary"]
	finally:
		for p in (tmp, tmp + ".json"):
			os.remove(p)
	bk = s.get("by_kind", {})
	b = bk.get("VOICE_B_F") or next(iter(bk.values()), {})
	med = lambda k: (b.get(k) or {}).get("median")
	print(json.dumps({"n": (b.get("settled_db") or {}).get("n", 0), "rise3": med("rise_ms.-3"),
	                  "over": med("overshoot_db"),
	                  "sym1": med("sym1_db"), "settled": med("settled_db"), "empty_slot": b.get("empty_slot_db_median"),
	                  "kinds": {k: (v.get("settled_db") or {}).get("n", 0) for k, v in bk.items()}}))


# ---- the control host's half -------------------------------------------------------------------------------------

class Stop(Exception):
	pass


class Cal:
	def __init__(self, a):
		self.a = a
		self.pw = {}            # level -> its measured output (label)
		self.lo = int(round(a.freq * 1e6 - OFFSET_HZ))
		self.log = open(a.log, "a") if a.log else None
		self.rxc = a.rx - 1     # ad9361-phy input channel
		v = 2 * self.rxc        # cf-ad9361-lpc I/Q pair
		self.iq = f"voltage{v} voltage{v + 1}"

	def sh(self, host, cmd, timeout=180):
		args = ["ssh", host, cmd] if host else ["sh", "-c", cmd]
		p = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
		return p.returncode, p.stdout.strip(), p.stderr.strip()

	def dbg(self, *cmds):
		a = self.a
		tools = a.radio_tools or (os.path.join(HERE, "..") if not a.radio_ssh else "tools")
		base = f"{a.radio_python} {shlex.quote(os.path.join(tools, 'rfoundry_debug.py'))} --port {shlex.quote(a.radio_port)}"
		rc, out, err = self.sh(a.radio_ssh, " && ".join(f"{base} {c}" for c in cmds))
		if rc:
			raise Stop(f"debug link: {' / '.join(cmds)}: {err or out}")
		return out

	def view(self):
		return dict(l.split("=", 1) for l in self.dbg("view").splitlines() if "=" in l)

	def attr(self, *specs):
		u = shlex.quote(self.a.sdr_uri)
		return self.sh(self.a.sdr_ssh, "; ".join(f"timeout 10 iio_attr -u {u} {s}" for s in specs))

	def sdr_setup(self):
		a, c = self.a, self.rxc
		self.attr(f"-i -c ad9361-phy voltage0 sampling_frequency {a.fs} >/dev/null",
		          "-i -c ad9361-phy voltage0 rf_bandwidth 1000000 >/dev/null",
		          f"-c ad9361-phy altvoltage0 frequency {self.lo} >/dev/null",
		          "-c ad9361-phy altvoltage0 powerdown 0 >/dev/null",
		          f"-i -c ad9361-phy voltage{c} gain_control_mode manual >/dev/null",
		          f"-i -c ad9361-phy voltage{c} hardwaregain {a.gain:g} >/dev/null")
		rc, out, err = self.sh(a.sdr_ssh, f"mkdir -p {shlex.quote(a.sdr_dir)} && command -v iio_readdev >/dev/null && "
		                       "python3 -c 'import numpy'")
		if rc:
			raise Stop(f"the SDR host needs iio_readdev, python3 and numpy: {err or out}")
		files = [os.path.join(HERE, "pa_keyup_autocal.py"), os.path.join(HERE, "burstramp.py")]
		if a.sdr_ssh:
			if subprocess.run(["scp", "-q", *files, f"{a.sdr_ssh}:{a.sdr_dir}/"]).returncode:
				raise Stop(f"can't copy the analysis to {a.sdr_ssh}:{a.sdr_dir}")
		else:
			for f in files:
				subprocess.run(["cp", f, a.sdr_dir + "/"], check=True)

	def sdr_check(self):
		"""Gain, manual mode and the LO, read back from the SDR before every key."""
		c = self.rxc
		rc, out, _ = self.attr(f"-i -c ad9361-phy voltage{c} hardwaregain", f"-i -c ad9361-phy voltage{c} gain_control_mode",
		                       "-c ad9361-phy altvoltage0 powerdown", "-c ad9361-phy altvoltage0 frequency")
		g = out.splitlines()
		try:
			ok = (abs(float(g[0].split()[0]) - self.a.gain) < 0.01 and g[1] == "manual" and g[2] == "0"
			      and abs(int(g[3]) - self.lo) < 100)
		except (IndexError, ValueError):
			ok = False
		if not ok:
			raise Stop(f"SDR not at gain {self.a.gain:g} manual with its LO up at {self.lo}: {out!r}")

	def lease_check(self):
		if self.a.lease_cmd:
			cmd = self.a.lease_cmd.replace("{mhz}", f"{self.a.freq:g}")
			if subprocess.run(cmd, shell=True, capture_output=True).returncode:
				raise Stop(f"--lease-cmd says {self.a.freq:g} MHz may not be keyed now")

	def note(self, **k):
		if self.log:
			k["t"] = time.time()
			self.log.write(json.dumps(k) + "\n")
			self.log.flush()

	def watts(self, dbfs):
		w = 10 ** ((dbfs + self.a.dbfs_to_dbm + self.a.pad_db) / 10) / 1000
		return f"{w:.1f} W" if w >= 1 else f"{w * 1000:.0f} mW"

	def lab(self, level):
		return self.pw.get(level, f"level {level}")

	def try_(self, level, c, tag):
		"""One key at a level with constants c (9 values; None = the board's): its metrics."""
		a = self.a
		self.sdr_check()
		self.lease_check()
		ku = "radio pa-keyup " + (" ".join(str(int(x)) for x in (c if c[8] else c[:8])) if c else "default")
		self.dbg(ku, f"radio power {level}")
		n = int((2.0 + a.key_s + 0.7) * a.fs)
		d = shlex.quote(a.sdr_dir)
		cap = (f"cd {d} && timeout 60 iio_readdev -u {shlex.quote(a.sdr_uri)} -b 1048576 -s {n} cf-ad9361-lpc {self.iq} "
		       "> ac.cs16")
		p = subprocess.Popen(["ssh", a.sdr_ssh, cap] if a.sdr_ssh else ["sh", "-c", cap], stderr=subprocess.DEVNULL)
		time.sleep(2.0)                 # the capture is running before the key
		try:
			self.dbg(f"ptt --seconds {a.key_s:g}")
		finally:
			p.wait(timeout=90)
		rc, out, err = self.sh(a.sdr_ssh, f"cd {d} && nice python3 pa_keyup_autocal.py analyze ac.cs16 --fs {a.fs}; "
		                       "rm -f ac.cs16")
		try:
			m = json.loads(out.splitlines()[-1])
		except (IndexError, ValueError):
			raise Stop(f"analysis failed: {out} {err}")
		m.update(level=level, c=c, tag=tag)
		self.note(**m)
		if m.get("settled") is not None:
			self.pw.setdefault(level, self.watts(m["settled"]))
			if m["settled"] + a.dbfs_to_dbm > a.max_in_dbm:
				raise Stop(f"{self.lab(level)}: {m['settled'] + a.dbfs_to_dbm:.1f} dBm into the SDR, over {a.max_in_dbm:g}")
		f = lambda v, n=2: "-" if v is None else f"{v:.{n}f}"
		cs = "board" if c is None else "/".join(str(int(x)) for x in c)
		print(f"  {tag:9s} {self.lab(level):>7s} {cs:34s} n={m['n']:2d} -3dB {f(m['rise3']):>6s} ms  "
		      f"over {f(m['over'], 1):>4s}  sym1 {f(m['sym1'], 1):>5s}", flush=True)
		if not m["n"] or m["rise3"] is None:
			v = self.view()
			raise Stop(f"no bursts at {self.lab(level)} ({m.get('kinds')}); radio tx_hz {v.get('tx_hz')}, txlim refused "
			           f"{v.get('txlim.refused')}, tx_deny {v.get('tx_deny')}")
		return m

	@staticmethod
	def bisect(fn, lo, hi, want, tol, rising, n=8):
		"""x in [lo, hi] with fn(x)["rise3"] near want; it falls as x rises unless rising. (x, metrics) of the best."""
		best = None
		for _ in range(n):
			x = round((lo + hi) / 2)
			m = fn(x)
			v = m["rise3"]
			if best is None or abs(v - want) < abs(best[1]["rise3"] - want):
				best = (x, m)
			if abs(v - want) <= tol or hi - lo <= 1:
				break
			if (v > want) != rising:
				lo = x
			else:
				hi = x
		return best

	def verify(self, levels, c, base):
		print("verify")
		res = {l: [self.try_(l, list(c), "verify") for _ in range(2)] for l in levels}
		ok = True
		for l, ms in res.items():
			r3 = [m["rise3"] for m in ms]
			ov = max(m["over"] or 0 for m in ms)
			s1 = min(m["sym1"] or 0 for m in ms)
			bad = [w for w, b in (("late", max(r3) > 0), ("early", min(r3) < EARLY_MAX), ("overshoot", ov > OVER_MAX),
			                      ("sym1", s1 < SYM1_MIN), ("unstable", max(r3) - min(r3) > 0.1)) if b]
			ok &= not bad
			print(f"  {self.lab(l):>7s} -3dB {r3[0]:+.2f}/{r3[1]:+.2f} ms  over {ov:.1f}  sym1 {s1:.1f}  "
			      f"(board {base[l]['rise3']:+.2f} ms, {base[l]['over'] or 0:.1f} dB)  {'OK' if not bad else ' '.join(bad)}")
		names = ("dead", "knee", "slope_us", "boost", "hold", "boost_us", "pa_us", "drive_us", "late")
		print(("PASS" if ok else "CHECK") + ": " + " ".join(f"{k} {v}" for k, v in zip(names, c)))
		print("  rfoundry_debug.py radio pa-keyup " + " ".join(map(str, c if c[8] else c[:8])))
		self.note(result=c, ok=ok)
		return ok

	def pa_only(self, levels, dl, pa0, base):
		"""No knee: the boost off, the PA's point moved until the slowest level's -3 dB is at TARGET + 0.05."""
		slow = max(dl, key=dl.get)
		want = TARGET + 0.05
		c = [0, 0, 0, 100, 100, 0, pa0, 0, 0]
		guess = round((dl[slow] - want) * 1000)
		print(f"  PA point on {self.lab(slow)} (the slowest), first guess {guess} us")

		def at(p):
			c[6] = p
			return self.try_(slow, list(c), "pa_us")
		c[6], _ = self.bisect(at, max(100, guess - 300), guess + 300, want, 0.04, rising=False, n=6)
		return self.verify(levels, c, base)

	def run(self):
		a = self.a
		print(f"PA key-up auto-cal at {a.freq:g} MHz, pads {a.pad_db:g} dB, SDR RX{a.rx} gain {a.gain:g} dB")
		top_in = 10 * math.log10(a.max_w * 1000) - a.pad_db
		if top_in > a.max_in_dbm:
			raise Stop(f"{a.max_w:g} W through {a.pad_db:g} dB is {top_in:.1f} dBm into the SDR, over {a.max_in_dbm:g}: add "
			           f"{math.ceil(top_in - a.max_in_dbm)} dB of pads, or leave the top levels out (--levels, --max-w)")
		self.lease_check()
		self.sdr_setup()
		self.sdr_check()
		self.dbg(f"radio tx-limit {a.freq:g}", "radio vfo on", f"radio freq {a.freq:g}")
		v = self.view()
		drives = [int(x) for x in v["tx_drive"].split("/")]
		board = [int(x) for x in v["pa_keyup"].split("/")] + [0] * 9
		board = board[:9]
		levels = list(range(len(drives))) if a.levels == "all" else [int(x) for x in a.levels.split(",")]
		low, top = levels[0], levels[-1]
		print(f"drives {[drives[l] for l in levels]}, board constants {board}")
		if a.arm:
			self.dbg(a.arm)
		self.dbg("radio mode dmr", f"radio cc {a.cc}", f"radio ts {a.ts}")

		print("baseline, the board's constants")
		base = {l: self.try_(l, None, "baseline") for l in levels}

		if a.start:
			c = ([int(x) for x in a.start.split(",")] + [0] * 9)[:9]
			if not c[5]:
				return self.verify(levels, c, base)         # no boost: nothing to tune, check it
		else:
			print("the loop with the boost off: from the PA's point to -3 dB")
			pa0 = board[6] or a.boost_us
			dl = {l: pa0 / 1000 + self.try_(l, [0, 0, 0, 100, 100, 0, pa0, 0, 0], "char")["rise3"] for l in levels}
			dmin = min(drives[l] for l in levels)
			fit = None
			for dead in range(0, dmin, max(1, dmin // 200)):
				x = [1 / (drives[l] - dead) for l in levels]
				y = [dl[l] for l in levels]
				mx, my = sum(x) / len(x), sum(y) / len(y)
				sxx = sum((xi - mx) ** 2 for xi in x)
				if not sxx:
					continue
				k = sum((xi - mx) * (yi - my) for xi, yi in zip(x, y)) / sxx
				tau = my - k * mx
				err = sum((k * xi + tau - yi) ** 2 for xi, yi in zip(x, y))
				if k > 0 and tau >= 0 and (fit is None or err < fit[0]):
					fit = (err, dead, k, tau)
			spread = max(dl.values()) - min(dl.values())
			if fit is None or fit[2] / (dmin - fit[1]) < 0.15 or spread < 0.2:
				print(f"  no knee: {min(dl.values()):.2f}-{max(dl.values()):.2f} ms from the PA's point at every level")
				return self.pa_only(levels, dl, pa0, base)
			_, dead, knee, tau = fit
			print(f"  dead {dead}, knee {knee:.0f} counts x ms, low-pass {tau * 1000:.0f} us")
			hold = 80
			while hold < 100 and hold * drives[low] / 100 <= dead:
				hold += 5
			c = [dead, round(knee), round(tau * 1000), 120, hold, a.boost_us, a.pa_us or a.boost_us, a.boost_us // 3, 0]
		late_ok = True
		for rnd in range(a.rounds):
			print(f"round {rnd + 1}: knee on {self.lab(low)}")

			def at_knee(k):
				c[1] = k
				return self.try_(low, list(c), "knee")
			c[1], _ = self.bisect(at_knee, max(1, c[1] // 4), c[1] * 3, TARGET, TOL, rising=False)
			if top != low:
				print(f"round {rnd + 1}: slope on {self.lab(top)}")

				def at_slope(s):
					c[2] = s
					return self.try_(top, list(c), "slope")
				c[2], m = self.bisect(at_slope, 0, max(4 * c[2], 2000), TARGET, TOL, rising=False)
				if (m["over"] or 0) > OVER_MAX:
					print(f"  {self.lab(top)} overshoots {m['over']:.1f} dB: hold down 10 %")
					c[4] = max(c[4] - 10, 50)
			mids = [l for l in levels if l not in (low, top)]
			if mids and late_ok:
				print(f"round {rnd + 1}: late for {', '.join(self.lab(l) for l in mids)}")
				rt = self.try_(top, list(c), "late-ref")["rise3"]

				def spread_at(lt):
					c[8] = lt
					ms = [self.try_(l, list(c), "late")["rise3"] for l in mids]
					return {"rise3": sum(ms) / len(ms) - rt}
				try:
					c[8], _ = self.bisect(spread_at, 0, 100, 0.0, TOL, rising=True, n=6)
				except Stop as e:
					if "debug link" not in str(e):
						raise
					print("  this firmware has no late: kept 0")
					c[8], late_ok = 0, False
		return self.verify(levels, c, base)

	def restore(self):
		try:
			self.dbg("radio pa-keyup default", "radio power 0")
			if self.a.disarm:
				self.dbg(self.a.disarm)
			print("board constants restored, lowest power level" + (", disarmed" if self.a.disarm else ""))
		except Exception as e:
			print(f"RESTORE FAILED: {e}: check the radio by hand")


def main(argv):
	if argv[:1] == ["analyze"]:
		return analyze(argv[1:]) or 0
	ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	g = ap.add_argument_group("radio")
	g.add_argument("--radio-port", required=True, help="the debug link port, on the radio's host")
	g.add_argument("--radio-ssh", default="", help="the radio's host (ssh), if not this one")
	g.add_argument("--radio-tools", default="", help="the dir with rfoundry_debug.py there (default: this tree's tools/)")
	g.add_argument("--radio-python", default="python3")
	g.add_argument("--arm", default="", help="a debug link command before keying (a radio with a TX arm option)")
	g.add_argument("--disarm", default="", help="and after")
	g.add_argument("--cc", type=int, default=2); g.add_argument("--ts", type=int, default=1)
	g = ap.add_argument_group("SDR")
	g.add_argument("--sdr-uri", required=True, help="libiio context of the AD936x SDR (usb:..., ip:...)")
	g.add_argument("--sdr-ssh", default="", help="the host the SDR is on (ssh), if not this one")
	g.add_argument("--sdr-dir", default="/tmp/pa_keyup_autocal", help="work dir there")
	g.add_argument("--rx", type=int, choices=(1, 2), default=2, help="the receive port the pads feed")
	g.add_argument("--fs", type=int, default=1000000)
	g.add_argument("--gain", type=float, default=0.0, help="RX gain, dB, manual (checked before every key)")
	g = ap.add_argument_group("level")
	g.add_argument("--pad-db", type=float, required=True, help="the pads between the radio and the SDR")
	g.add_argument("--max-w", type=float, required=True, help="the highest level's output, W (the preflight)")
	g.add_argument("--max-in-dbm", type=float, default=-13.0, help="never more than this into the SDR")
	g.add_argument("--dbfs-to-dbm", type=float, default=3.6,
	               help="dBm at the SDR's input = the settled level in dBFS + this, at --gain; measure it against a "
	                    "wattmeter (the default is one AD9363 at gain 0, UHF)")
	g = ap.add_argument_group("run")
	g.add_argument("--freq", type=float, required=True, help="MHz; a frequency the radio may key here, into the load")
	g.add_argument("--lease-cmd", default="", 
	               help="shell command that must succeed before every key ({mhz}: the frequency)")
	g.add_argument("--levels", default="all", help="power level numbers to tune on, low to high (default all)")
	g.add_argument("--boost-us", type=int, required=True, help="the earliest point the board allows (its max_ahead)")
	g.add_argument("--pa-us", type=int, default=None, help="the PA's point with a boost (default --boost-us)")
	g.add_argument("--key-s", type=float, default=1.2, help="DMR key per try (about 20 bursts)")
	g.add_argument("--rounds", type=int, default=1)
	g.add_argument("--start", default=None, 
	               help="skip the loop fit: DEAD,KNEE,SLOPE,BOOST,HOLD,BOOST_US,PA_US,DRIVE_US,LATE "
	                    "(BOOST_US 0: only verify them)")
	g.add_argument("--log", default="", help="JSON lines, one per try")
	a = ap.parse_args(argv)
	if not a.radio_ssh and not a.radio_tools:
		a.radio_python = sys.executable
	cal = Cal(a)
	try:
		ok = cal.run()
	except (Stop, subprocess.TimeoutExpired) as e:
		print(f"STOPPED: {e}")
		ok = None
	finally:
		cal.restore()
	return 0 if ok else 1


if __name__ == "__main__":
	sys.exit(main(sys.argv[1:]))
