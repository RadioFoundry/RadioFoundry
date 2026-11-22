#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Runs the host test suites for tests/host/Makefile, each with its output in a log of its own.

    suite.py run NAME CMD [CMD ...]   run each shell command in turn, output to out/run/NAME.log; print the log on a
                                      failure (HOST_VERBOSE=1: always)
    suite.py summary NAME [NAME ...]  one line per suite in the order given, then the count; fails if one didn't pass
    suite.py times [N]                the N (15) slowest suites to build and to run, from the last build and run

Suites run in parallel under make -j, so nothing is printed while one runs: a suite that passes says so in the summary,
in the Makefile's order, which keeps the output the same from run to run.
"""
import os
import subprocess
import sys
import time

RUN_DIR = os.path.join("out", "run")
TIMES_DIR = os.path.join("out", "times")
SHOW_LINES = 200	# of a failed suite's log; the whole log stays in out/run


def last_line(path):
	try:
		with open(path, errors="replace") as f:
			lines = [l.rstrip() for l in f if l.strip() and not l.startswith("$ ")]
	except OSError:
		return ""
	return lines[-1][:100] if lines else ""


def cmd_run(name, cmds):
	os.makedirs(RUN_DIR, exist_ok=True)
	os.makedirs(os.path.join(TIMES_DIR, "run"), exist_ok=True)
	log_path = os.path.join(RUN_DIR, name + ".log")
	result_path = os.path.join(RUN_DIR, name + ".result")
	if os.path.exists(result_path):
		os.remove(result_path)
	start = time.time()
	status = 0
	with open(log_path, "w") as log:
		for cmd in cmds:
			log.write("$ %s\n" % " ".join(cmd.split()))
			log.flush()
			status = subprocess.call(cmd, shell=True, stdout=log, stderr=subprocess.STDOUT)
			if status != 0:
				break
	elapsed = time.time() - start
	with open(result_path, "w") as f:
		f.write("%s %.2f %d\n" % ("pass" if status == 0 else "FAIL", elapsed, status))
	with open(os.path.join(TIMES_DIR, "run", name), "w") as f:
		f.write("%.2f\n" % elapsed)
	if status != 0 or os.environ.get("HOST_VERBOSE") == "1":
		with open(log_path, errors="replace") as f:
			lines = f.readlines()
		head = "FAILED (exit %d)" % status if status != 0 else "passed"
		out = ["==== %s %s, %.1f s: %s\n" % (name, head, elapsed, log_path)]
		if len(lines) > SHOW_LINES:
			out.append("[... %d lines; the last %d:]\n" % (len(lines), SHOW_LINES))
		out += lines[-SHOW_LINES:]
		out.append("==== end of %s\n" % name)
		sys.stdout.write("".join(out))
		sys.stdout.flush()
	return 0 if status == 0 else 1


def cmd_summary(names):
	passed, failed, missing, total = 0, [], [], 0.0
	rows = []
	for name in dict.fromkeys(names):	# once each (a suite two groups list), in the order given
		try:
			with open(os.path.join(RUN_DIR, name + ".result")) as f:
				state, secs, _ = f.read().split()
		except (OSError, ValueError):
			missing.append(name)
			rows.append("  ?     %-28s (not run)" % name)
			continue
		total += float(secs)
		if state == "pass":
			passed += 1
		else:
			failed.append(name)
		rows.append("  %-4s %6.1f s  %-28s %s" % (state, float(secs), name,
			last_line(os.path.join(RUN_DIR, name + ".log"))))
	print("\n".join(rows))
	print("# host tests: %d suites passed, %d failed, %d not run (%.0f s of test time; logs in tests/host/%s)"
		% (passed, len(failed), len(missing), total, RUN_DIR))
	if failed:
		print("# failed: " + " ".join(failed))
	return 1 if failed or missing else 0


def read_times(kind):
	d = os.path.join(TIMES_DIR, kind)
	out = []
	for name in sorted(os.listdir(d)) if os.path.isdir(d) else []:
		try:
			with open(os.path.join(d, name)) as f:
				out.append((float(f.read()), name))
		except (OSError, ValueError):
			pass
	return sorted(out, reverse=True)


def cmd_times(n):
	for kind, what in (("build", "to build (compile and link, from the last build)"),
			("run", "to run (from the last run)")):
		rows = read_times(kind)
		print("# the %d slowest of %d suites %s, total %.0f s" % (min(n, len(rows)), len(rows), what,
			sum(t for t, _ in rows)))
		for t, name in rows[:n]:
			print("  %7.1f s  %s" % (t, name))
	return 0


def main():
	if len(sys.argv) >= 3 and sys.argv[1] == "run":
		return cmd_run(sys.argv[2], sys.argv[3:])
	if len(sys.argv) >= 2 and sys.argv[1] == "summary":
		return cmd_summary(sys.argv[2:])
	if len(sys.argv) >= 2 and sys.argv[1] == "times":
		return cmd_times(int(sys.argv[2]) if len(sys.argv) > 2 else 15)
	sys.stderr.write(__doc__)
	return 2


if __name__ == "__main__":
	sys.exit(main())
