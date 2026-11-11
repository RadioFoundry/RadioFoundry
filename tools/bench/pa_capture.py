#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""
pa_capture: a radio's output, carrier by carrier, on a conducted path (docs/spec/pa-keyup.md 7).

The radio transmits into a dummy load; a tap through fixed pads feeds an AD936x SDR's receive port (libiio, raw I/Q),
as for pa_keyup_autocal.py. One capture runs while the radio keys, then every carrier in it (an FM sweep point, a DMR
burst, a whole FM transmission) becomes one row:

  t_ms      its start: the first point within 20 dB of its settled level (above the floor)
  W         its settled level (the median of its second half, the last tenth left out), through --pad-db
  r10 r3 r1 ms from t_ms to -10, -3 and -1 dB of the settled level
  over      dB over the settled level before it settles
  f3_20     ms from its last -3 dB point to its last -20 dB point (the fall)
  len       ms from t_ms to that last -3 dB point
  hz        its mean frequency against --freq

and a power log at 10 us steps (float16 dBFS) is kept beside the table, for plots and anything the table doesn't say.

  sweep     FM, the PA drive swept on one key-up (rfoundry_debug.py radio pa-sweep): each row also gets its drive
  key       any debug link commands while capturing (DMR: --mode dmr --key "ptt --seconds 1.2")
  plot      the rows' envelopes on one time base, aligned at t_ms, from a saved capture (needs matplotlib)

    pa_capture.py sweep --radio-port PORT --sdr-uri usb:1.2.5 --freq 445 --pad-db 57 --max-w 17 \\
        --grid 16,16,90,15,10 --out sw1
    pa_capture.py plot sw1

The capture host's half (`analyze`: cs16 -> rows, power log) runs where the SDR is; this file and pa_keyup_autocal.py
are copied there.
"""
import argparse
import csv
import json
import math
import os
import shlex
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pa_keyup_autocal as autocal  # noqa: E402

LOG_STEP_S = 10e-6
COLS = ("i", "drive", "t_ms", "dbfs", "dbm", "W", "r10", "r3", "r1", "over", "f3_20", "len", "hz")


# ---- the capture host's half -------------------------------------------------------------------------------------

def segments(e, dt_ms, floor, min_db=15.0, smooth=None):
	"""Carriers in the power log e (dBFS, one point per dt_ms): rows as dicts (no drive)."""
	import numpy as np
	lin = 10 ** (e / 10)
	k = max(1, int(round(0.1 / dt_ms)))
	s = 10 * np.log10(np.convolve(lin, np.ones(k) / k, "same") + 1e-15)
	on = (s > floor + min_db).astype(np.int8)
	d = np.diff(np.r_[0, on, 0])
	starts, ends = np.flatnonzero(d == 1), np.flatnonzero(d == -1)
	pad = int(round(3.0 / dt_ms))            # slow edges reach past the threshold
	w = max(1, int(round(0.05 / dt_ms)))     # the overshoot on a 50 us average
	sm = 10 * np.log10(np.convolve(lin, np.ones(w) / w, "same") + 1e-15)
	rows = []
	for a, b in zip(starts, ends):
		n = b - a
		if n * dt_ms < 0.3:
			continue
		settled = float(np.median(e[a + n // 2: b - n // 10])) if n >= 10 else float(np.median(e[a:b]))
		if settled < floor + min_db:            # a noise peak over the threshold, not a carrier
			continue
		lo, hi = max(0, a - pad), min(len(e), b + pad)
		if rows and lo < rows[-1]["_hi"]:
			lo = rows[-1]["_hi"]
		v = e[lo:hi]
		l20 = max(settled - 20, floor + 6)

		def first(level, frm=0):
			j = np.flatnonzero(v[frm:] >= level)
			return frm + int(j[0]) if len(j) else None

		t0 = first(l20)
		if t0 is None:
			continue
		r = {}
		for name, dbd in (("r10", 10), ("r3", 3), ("r1", 1)):
			j = first(settled - dbd, t0)
			r[name] = None if j is None else round((j - t0) * dt_ms, 3)
		mid = a - lo + n // 2
		over = float(sm[lo + t0: lo + max(mid, t0 + 1)].max() - settled)
		above3 = np.flatnonzero(v >= settled - 3)
		above20 = np.flatnonzero(v >= l20)
		off3 = int(above3[-1]) if len(above3) else t0
		off20 = int(above20[-1]) if len(above20) else off3
		rows.append(dict(t_ms=round((lo + t0) * dt_ms, 3), dbfs=round(settled, 2), **r, over=round(over, 2),
		                 f3_20=round((off20 - off3) * dt_ms, 3), len=round((off3 - t0) * dt_ms, 3),
		                 _hi=hi, _set=(a + n // 2, b - n // 10)))
	return rows


def channel(src, fs, off_hz, step_s=LOG_STEP_S, block=1 << 18, peak_full=2048.0):
	"""A raw cs16 capture mixed down by off_hz, low-passed (+-60 kHz) and averaged to one complex point per step_s, in
	blocks (a small capture host can't hold the whole capture's FFT). Returns (complex64 points, power of each in
	dBFS, the step in ms)."""
	import numpy as np
	B = max(1, int(round(fs * step_s)))
	nt = 101
	k = np.arange(nt) - (nt - 1) / 2
	h = (np.sinc(2 * 60e3 / fs * k) * np.hamming(nt) * 2 * 60e3 / fs).astype(np.float32)
	raw = np.memmap(src, dtype=np.int16, mode="r")
	n = len(raw) // 2
	block -= block % B
	pts, pw, tail, done = [], [], np.zeros(nt - 1, np.complex64), 0
	while done < n:
		r = np.asarray(raw[2 * done: 2 * min(n, done + block)], dtype=np.float32)
		x = (r[0::2] + 1j * r[1::2]).astype(np.complex64) / peak_full
		x *= np.exp(-2j * np.pi * off_hz / fs * (done + np.arange(len(x)))).astype(np.complex64)
		z = np.r_[tail, x]
		y = np.convolve(z, h, "valid")         # aligned to x, delayed by the filter's half length
		tail = z[-(nt - 1):]
		m = len(y) // B
		y = y[: m * B].reshape(m, B)
		pts.append(y.mean(1))
		pw.append((np.abs(y) ** 2).mean(1))
		done += len(x)
	return np.concatenate(pts), 10 * np.log10(np.concatenate(pw) + 1e-15), B / fs * 1e3


def analyze(argv):
	ap = argparse.ArgumentParser(prog="pa_capture.py analyze")
	ap.add_argument("file")
	ap.add_argument("--fs", type=float, required=True)
	ap.add_argument("--off-hz", type=float, default=autocal.OFFSET_HZ)
	ap.add_argument("--log", required=True, help="the power log: float16 dBFS, LOG_STEP_S apart")
	ap.add_argument("--min-db", type=float, default=12.0, help="a carrier: this far over the floor")
	a = ap.parse_args(argv)
	import numpy as np
	z, e, dt_ms = channel(a.file, a.fs, a.off_hz)
	e.astype(np.float16).tofile(a.log)
	floor = float(np.percentile(e, 5))
	rows = segments(e, dt_ms, floor, a.min_db)
	for r in rows:
		i, j = r.pop("_set")
		r.pop("_hi")
		v = z[i:j]
		r["hz"] = (round(float(np.angle(np.sum(v[1:] * np.conj(v[:-1])))) / (2 * math.pi * dt_ms * 1e-3), 1)
		           if len(v) > 1 else None)
	print(json.dumps({"dt_ms": dt_ms, "floor": round(floor, 2), "points": int(len(e)), "rows": rows}))


# ---- the control host's half -------------------------------------------------------------------------------------

def sweep_drives(rows, runs, step, on_ms, per_ms):
	"""Gives each row its drive. The sweeps of one grid come about 0.2 s apart, so rows split into sweeps at gaps longer
	than a point; a sweep's lowest drives may be under the floor, so its rows count from its highest drive (the last
	point going up, the first going down). Rows that don't fit get none."""
	groups, cur = [], []
	# The carrier at the power level before the first sweep is longer than any point
	for r in [r for r in rows if r["len"] <= on_ms + 3]:
		if cur and r["t_ms"] - cur[-1]["t_ms"] > per_ms + 60:
			groups.append(cur)
			cur = []
		cur.append(r)
	if cur:
		groups.append(cur)
	runs = [(d, n) for d, n in runs if n]
	if len(groups) > len(runs):
		return False
	for g, (d0, n) in zip(groups, runs[len(runs) - len(groups):]):
		if len(g) > n:
			return False
		idx = range(n - len(g), n) if step > 0 else range(len(g))
		for r, i in zip(g, idx):
			r["drive"] = d0 + i * step
	return True


class Capture(autocal.Cal):
	FILES = ("pa_capture.py", "pa_keyup_autocal.py", "burstramp.py")

	def dbm(self, dbfs):
		return dbfs + self.a.dbfs_to_dbm + self.a.pad_db

	def capture(self, seconds, *cmds):
		"""The capture around cmds (debug link, on the radio's host): returns (the debug link's output, the analysis)."""
		a = self.a
		self.sdr_check()
		self.lease_check()
		n = int((1.0 + seconds + 0.7) * a.fs)
		d = shlex.quote(a.sdr_dir)
		cap = (f"cd {d} && timeout {int(seconds) + 30} iio_readdev -u {shlex.quote(a.sdr_uri)} -b 1048576 -s {n} "
		       f"cf-ad9361-lpc {self.iq} > cap.cs16")
		p = subprocess.Popen(["ssh", a.sdr_ssh, cap] if a.sdr_ssh else ["sh", "-c", cap], stderr=subprocess.DEVNULL)
		time.sleep(1.0)                 # the capture runs before the key
		try:
			out = self.dbg(*cmds)
		finally:
			p.wait(timeout=seconds + 60)
		rc, res, err = self.sh(a.sdr_ssh, f"cd {d} && nice python3 pa_capture.py analyze cap.cs16 --fs {a.fs} "
		                       f"--log cap.f16; rm -f cap.cs16")
		try:
			m = json.loads(res.splitlines()[-1])
		except (IndexError, ValueError):
			raise autocal.Stop(f"analysis failed: {res} {err}")
		src = f"{a.sdr_ssh}:{a.sdr_dir}/cap.f16" if a.sdr_ssh else os.path.join(a.sdr_dir, "cap.f16")
		subprocess.run(["scp", "-q", src, a.out + ".f16"] if a.sdr_ssh else ["cp", src, a.out + ".f16"], check=True)
		return out, m

	def save(self, m, extra):
		a = self.a
		for i, r in enumerate(m["rows"]):
			r["i"] = i
			r.setdefault("drive", None)
			r["dbm"] = round(self.dbm(r["dbfs"]), 2)
			w = 10 ** (r["dbm"] / 10) / 1000
			r["W"] = round(w, 4)
		with open(a.out + ".csv", "w", newline="") as f:
			wr = csv.DictWriter(f, COLS, extrasaction="ignore")
			wr.writeheader()
			wr.writerows(m["rows"])
		meta = dict(dt_ms=m["dt_ms"], floor_dbfs=m["floor"], points=m["points"], freq_mhz=a.freq, pad_db=a.pad_db,
		            dbfs_to_dbm=a.dbfs_to_dbm, fs=a.fs, **extra)
		json.dump(meta, open(a.out + ".json", "w"), indent=1)
		lab = lambda r: "-" if r is None else f"{r:.2f}"
		print(f"{'i':>3} {'drive':>5} {'t ms':>8} {'out':>8} {'r10':>5} {'r3':>5} {'r1':>5} {'over':>5} {'f3_20':>5} "
		      f"{'len':>6} {'Hz':>6}")
		for r in m["rows"]:
			w = r["W"]
			print(f"{r['i']:3d} {r['drive'] if r['drive'] is not None else '-':>5} {r['t_ms']:8.1f} "
			      f"{(f'{w:.2f} W' if w >= 1 else f'{w * 1000:.0f} mW'):>8} {lab(r['r10']):>5} {lab(r['r3']):>5} "
			      f"{lab(r['r1']):>5} {r['over']:5.1f} {r['f3_20']:5.2f} {r['len']:6.1f} "
			      f"{(r['hz'] if r['hz'] is not None else 0):6.0f}")
		print(f"{len(m['rows'])} carriers, floor {m['floor']:.1f} dBFS; {a.out}.csv, {a.out}.json, power log {a.out}.f16")

	def preflight(self, max_w):
		a = self.a
		top_in = 10 * math.log10(max_w * 1000) - a.pad_db
		if top_in > a.max_in_dbm:
			raise autocal.Stop(f"{max_w:g} W through {a.pad_db:g} dB is {top_in:.1f} dBm into the SDR, over "
			                   f"{a.max_in_dbm:g}: add pads or lower --max-w")
		self.lease_check()
		self.sdr_setup()
		self.sdr_check()
		self.dbg(f"radio tx-limit {a.freq:g}", "radio vfo on", f"radio freq {a.freq:g}", f"radio mode {a.mode}",
		         f"radio power {a.level}")

	def sweep(self):
		a = self.a
		g = [int(x) for x in a.grid.split(",")]
		if len(g) < 5:
			raise autocal.Stop("--grid START,STEP,COUNT,ON_MS,OFF_MS[,CEILING_PCT[,MARK_MS]]")
		start, step, count, on, off = g[:5]
		ceil = g[5] if len(g) > 5 else 100
		# Points over the top level's drive put out more than the top level: about (drive ratio)^2
		self.preflight(a.max_w * (max(ceil, 100) / 100) ** 2)
		per = on + off
		secs = count * per / 1000 + (count * per / 950 + 1) * 0.25 + 0.5
		out, m = self.capture(secs, "radio pa-sweep " + " ".join(str(x) for x in g))
		line = next((l for l in out.splitlines() if l.startswith("points")), "")
		runs = [tuple(int(x) for x in t.split(":")) for t in line.split("sweeps")[-1].split()] if "sweeps" in line else []
		if not sweep_drives(m["rows"], runs, step, on, per):
			print(f"WARNING: {len(m['rows'])} carriers don't match the sweeps {runs}: no drives given")
		self.save(m, dict(kind="sweep", grid=g, sweeps=runs, level=a.level))

	def key(self):
		a = self.a
		self.preflight(a.max_w)
		cmds = a.key if a.key else ["ptt --seconds 1.2"]
		secs = sum(float(c.split("--seconds")[1].split()[0]) for c in cmds if "--seconds" in c) or a.seconds
		_, m = self.capture(secs + 0.5, *cmds)
		self.save(m, dict(kind="key", cmds=cmds, level=a.level, mode=a.mode))

	def restore(self):
		try:
			self.dbg("radio power 0")
			print("lowest power level")
		except Exception as e:
			print(f"RESTORE FAILED: {e}: check the radio by hand")


def plot(argv):
	ap = argparse.ArgumentParser(prog="pa_capture.py plot")
	ap.add_argument("out", help="a capture's --out prefix")
	ap.add_argument("--pre-ms", type=float, default=1.0)
	ap.add_argument("--ms", type=float, default=6.0, help="how much of each carrier from its start")
	ap.add_argument("--fall", action="store_true", help="align at the fall (the last -3 dB point) instead")
	ap.add_argument("--png", default="", help="write here instead of showing")
	ap.add_argument("--snr", type=float, default=25.0, help="only carriers this far over the floor, dB")
	a = ap.parse_args(argv)
	import numpy as np
	import matplotlib
	if a.png:
		matplotlib.use("Agg")
	import matplotlib.pyplot as plt
	meta = json.load(open(a.out + ".json"))
	e = np.fromfile(a.out + ".f16", dtype=np.float16).astype(np.float64)
	rows = [r for r in csv.DictReader(open(a.out + ".csv")) if float(r["dbfs"]) >= meta["floor_dbfs"] + a.snr]
	dt = meta["dt_ms"]
	fig, ax = plt.subplots(figsize=(10, 6))
	cmap = plt.get_cmap("viridis")
	ws = [float(r["W"]) for r in rows]
	norm = matplotlib.colors.LogNorm(min(ws), max(ws)) if rows else None
	for k, r in enumerate(rows):
		t0 = float(r["t_ms"]) + (float(r["len"]) if a.fall else 0.0)
		i0 = int(round((t0 - a.pre_ms) / dt))
		i1 = int(round((t0 + a.ms) / dt))
		if i0 < 0 or i1 > len(e):
			continue
		t = np.arange(i1 - i0) * dt - a.pre_ms
		lab = f"{r['drive']}" if r["drive"] else f"#{r['i']}"
		ax.plot(t, e[i0:i1] - float(r["dbfs"]), lw=1, color=cmap(norm(float(r["W"]))), label=lab)
	ax.set_xlabel(f"ms from each carrier's {'fall (-3 dB)' if a.fall else 'start (-20 dB)'}")
	ax.set_ylabel("dB relative to its settled level")
	ax.set_ylim(-30, 6)
	ax.grid(alpha=0.3)
	if len(rows) <= 12:
		ax.legend(fontsize=7, title="drive")
	elif rows:
		fig.colorbar(matplotlib.cm.ScalarMappable(norm=norm, cmap=cmap), ax=ax, label="settled output, W")
	ax.set_title(f"{os.path.basename(a.out)}: {meta['freq_mhz']:g} MHz, {len(rows)} carriers")
	if a.png:
		fig.savefig(a.png, dpi=120, bbox_inches="tight")
		print(a.png)
	else:
		plt.show()


def main(argv):
	if argv[:1] == ["analyze"]:
		return analyze(argv[1:]) or 0
	if argv[:1] == ["plot"]:
		return plot(argv[1:]) or 0
	ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("what", choices=("sweep", "key"))
	g = ap.add_argument_group("radio")
	g.add_argument("--radio-port", required=True, help="the debug link port, on the radio's host")
	g.add_argument("--radio-ssh", default="", help="the radio's host (ssh), if not this one")
	g.add_argument("--radio-tools", default="", help="the dir with rfoundry_debug.py there (default: this tree's tools/)")
	g.add_argument("--radio-python", default="python3")
	g.add_argument("--mode", choices=("fm", "dmr"), default="fm", help="key: the mode (sweep: always FM)")
	g.add_argument("--level", type=int, default=0, help="the power level (sweep: the carrier before the first point)")
	g = ap.add_argument_group("SDR")
	g.add_argument("--sdr-uri", required=True, help="libiio context of the AD936x SDR (usb:..., ip:...)")
	g.add_argument("--sdr-ssh", default="", help="the host the SDR is on (ssh), if not this one")
	g.add_argument("--sdr-dir", default="/tmp/pa_capture", help="work dir there")
	g.add_argument("--rx", type=int, choices=(1, 2), default=2, help="the receive port the pads feed")
	g.add_argument("--fs", type=int, default=1000000)
	g.add_argument("--gain", type=float, default=0.0, help="RX gain, dB, manual (checked before every key)")
	g = ap.add_argument_group("level")
	g.add_argument("--pad-db", type=float, required=True, help="the pads between the radio and the SDR")
	g.add_argument("--max-w", type=float, required=True, help="the highest level's output, W (the preflight)")
	g.add_argument("--max-in-dbm", type=float, default=-13.0, help="never more than this into the SDR")
	g.add_argument("--dbfs-to-dbm", type=float, default=3.6, help="as pa_keyup_autocal.py's")
	g = ap.add_argument_group("run")
	g.add_argument("--freq", type=float, required=True, help="MHz; a frequency the radio may key here, into the load")
	g.add_argument("--lease-cmd", default="", help="shell command that must succeed before every key ({mhz})")
	g.add_argument("--grid", default="", help="sweep: START,STEP,COUNT,ON_MS,OFF_MS[,CEILING_PCT[,MARK_MS]]")
	g.add_argument("--key", action="append", default=[], help="key: a debug link command (repeat; default "
	               "\"ptt --seconds 1.2\")")
	g.add_argument("--seconds", type=float, default=2.0, help="key: the capture, when no command has --seconds")
	g.add_argument("--out", required=True, help="prefix of the files written here: .csv .json .f16")
	a = ap.parse_args(argv)
	if a.what == "sweep":
		a.mode = "fm"
	if not a.radio_ssh and not a.radio_tools:
		a.radio_python = sys.executable
	a.log = ""
	c = Capture(a)
	try:
		c.sweep() if a.what == "sweep" else c.key()
		ok = True
	except (autocal.Stop, subprocess.TimeoutExpired) as e:
		print(f"STOPPED: {e}")
		ok = False
	finally:
		c.restore()
	return 0 if ok else 1


if __name__ == "__main__":
	sys.exit(main(sys.argv[1:]))
