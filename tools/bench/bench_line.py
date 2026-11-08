#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""
bench_line: a client of the bench line protocol, TCP port 7373. A bench board serves it, and it has the same ops as
the shared bench daemon. One request per connection: a JSON line goes out, and JSON lines come back. A line with
"bytes": N is followed by N bytes of payload. Closing the connection cancels the request: a capture stops, a
transmission unkeys, a session ends.

    bench_line.py HOST[:PORT] info | status            the board's view, as JSON
    bench_line.py HOST[:PORT] run bench_tx ARGS...      a bench_tx.py command line, run as a job on the board

Library use: request() for one-line answers, Request for a stream read on its own thread, job_from_args() for a
bench_tx / bench_repeater command line as the job the board runs (it builds and sends the bursts itself).

Errors are {"error": text, "code": ...}. The codes: timeout, busy, deadlock (a wait that ran out), refused (a
frequency the board doesn't list), unsupported (an op or option the board doesn't do), no_signal. This client adds
unavailable (no connection) and lost (the stream broke mid-request: the board restarted or hung).
"""
import json
import os
import shlex
import socket
import sys
import threading
import time

PORT = 7373

# bench_tx / bench_repeater options that don't take a value
FLAGS = {"private", "pi", "emergency", "confirmed", "tms_no_ack", "etsi_no_response", "group", "json", "verbose"}
# options sent as text: free text, and the hex fields (the board reads a string as hex, a number as itself)
TEXT = {"tms", "etsi", "ip_text", "talker_alias", "gps", "udp_payload", "opcode", "fid", "octet3", "service_options",
        "slots", "family", "ambe_out", "voice"}


class BenchLineError(Exception):
    """An {"error"} reply: code is the board's (timeout, busy, deadlock, refused, unsupported, no_signal, ...)."""

    def __init__(self, text, code=None):
        super().__init__(text)
        self.code = code


def exit_code(e):
    """The daemon client's exit code for a BenchLineError: 75 busy or out of time, 77 refused, 3 no signal, 69 else."""
    return {"timeout": 75, "busy": 75, "deadlock": 75, "refused": 77, "no_signal": 3}.get(e.code, 69)


def address(spec):
    """(host, port) of "HOST", "HOST:PORT" or "http://HOST[/...]" (the board's web address: the line port on it)."""
    s = spec.strip()
    if "://" in s:
        s = s.split("://", 1)[1].split("/", 1)[0]
        s = s.rsplit(":", 1)[0] if s.count(":") == 1 else s      # a web port isn't the line port
        return s, PORT
    host, _, port = s.rpartition(":") if s.count(":") == 1 else (s, "", "")
    return (host or s), int(port) if port else PORT


class Conn:
    """One request's connection."""

    def __init__(self, where, timeout=10.0):
        try:
            self.s = socket.create_connection(where, timeout=timeout)
        except OSError as e:
            raise BenchLineError(f"the bench board at {where[0]}:{where[1]} doesn't answer ({e})", "unavailable")
        self.s.settimeout(None)
        # A board that restarts or drops off the network mid-request ends the read, which would wait for ever
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        for opt, v in (("TCP_KEEPIDLE", 10), ("TCP_KEEPINTVL", 5), ("TCP_KEEPCNT", 3)):
            if hasattr(socket, opt):
                try:
                    self.s.setsockopt(socket.IPPROTO_TCP, getattr(socket, opt), v)
                except OSError:
                    pass
        self.f = self.s.makefile("rb")

    def send(self, req):
        self.s.sendall((json.dumps(req) + "\n").encode())

    def lines(self):
        """(reply, payload) pairs: payload is the bytes a reply with "bytes" announces, else None."""
        for raw in self.f:
            if not raw.strip():
                continue
            d = json.loads(raw)
            data = None
            if "bytes" in d:
                data = self.f.read(d["bytes"])
                if len(data) < d["bytes"]:
                    return
            yield d, data

    def end(self):
        """From another thread: the reads see the end of the stream."""
        try:
            self.s.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass

    def close(self):
        for x in (self.f, self.s):
            try:
                x.close()
            except OSError:
                pass


def request(where, req, timeout=30.0):
    """The first reply to req (info, status, claim ...). An {"error"} reply raises BenchLineError."""
    c = Conn(where)
    c.s.settimeout(timeout)
    try:
        c.send(req)
        for d, _ in c.lines():
            if "error" in d:
                raise BenchLineError(d["error"], d.get("code"))
            if d.get("state") in ("queued", "granted") or "note" in d:
                continue
            return d
        raise BenchLineError("the bench board closed the connection with no answer", "internal")
    except socket.timeout:
        raise BenchLineError(f"no answer to {req.get('op')} in {timeout:g} s", "timeout")
    except (OSError, ValueError) as e:     # a board that restarts mid-request, or a garbled line
        raise BenchLineError(f"the bench board stopped answering {req.get('op')}: {e}", "lost")
    finally:
        c.close()


def describe_wait(d):
    who = ", ".join(f"{b.get('label') or b.get('session')} ({b.get('kind', '')}, ~{b.get('remaining_s', 0):.0f} s left)"
                    for b in d.get("blockers", []))
    return (f"rfoundry-bench: waiting for the {d.get('kind')}: about {d.get('eta_s', 0):.0f} s (at most "
            f"{d.get('worst_s', 0):.0f} s), held by {who or 'requests ahead'}")


class Request:
    """A streaming request on its own thread. Every reply line is kept in .replies (with its arrival on
    time.monotonic() in .stamps). on_line(reply, payload) sees each line as it comes. Waits and notes go to log.
    .error is the BenchLineError the request ended with, if any; .first is set at the first reply that isn't a wait
    or a note (a ready, started, armed, or an error)."""

    def __init__(self, where, req, on_line=None, log=None):
        self.req, self.on_line = req, on_line
        self.log = log or (lambda s: print(s, file=sys.stderr, flush=True))
        self.replies, self.stamps = [], []
        self.error = None
        self.canceled = False
        self.first = threading.Event()
        self.done = threading.Event()
        self.conn = Conn(where)
        try:
            self.conn.send(req)
        except OSError as e:
            self.conn.close()
            raise BenchLineError(f"the bench board stopped answering {req.get('op')}: {e}", "lost")
        self.thread = threading.Thread(target=self._pump, daemon=True)
        self.thread.start()

    def _pump(self):
        try:
            for d, data in self.conn.lines():
                if "error" in d:
                    self.error = BenchLineError(d["error"], d.get("code"))
                    break
                if d.get("state") == "queued":
                    self.log(describe_wait(d))
                    continue
                if d.get("state") == "granted":
                    self.log(f"rfoundry-bench: got the {d.get('kind')} after {d.get('waited_s', 0):.0f} s")
                    continue
                if "note" in d and "started" not in d:     # a tx job's started line carries a note of its own
                    self.log(f"rfoundry-bench: {d['note']}")
                    continue
                self.stamps.append(time.monotonic())
                self.replies.append(d)
                self.first.set()
                if self.on_line is not None:
                    self.on_line(d, data)
        except (OSError, ValueError) as e:
            if not self.done.is_set():
                self.error = BenchLineError(f"the bench board's stream broke: {e}", "lost")
        finally:
            self.first.set()
            self.done.set()
            self.conn.close()

    def wait(self, timeout=None):
        """True when the request ended within timeout."""
        return self.done.wait(timeout)

    def cancel(self):
        """Close the connection: the board ends the request (a transmission unkeys)."""
        self.canceled = True
        self.done.set()
        self.conn.end()
        self.thread.join(5)


def job_from_args(tool, args):
    """The job a bench_tx.py / bench_repeater.py command line (a string or a list) describes: the tool's options by
    their long names with _ for -, the subcommand as "cmd". An --ambe file goes along as its bytes (ambe_hex)."""
    argv = shlex.split(args) if isinstance(args, str) else list(args)
    job = {"tool": tool}
    i = 0
    while i < len(argv):
        a = argv[i]
        if not a.startswith("--"):
            if "cmd" in job:
                raise ValueError(f"{tool}: a second command {a!r} in {shlex.join(argv)}")
            job["cmd"] = a
            i += 1
            continue
        key = a[2:].replace("-", "_")
        if key in FLAGS:
            job[key] = True
            i += 1
            continue
        if i + 1 >= len(argv):
            raise ValueError(f"{tool}: {a} needs a value")
        v = argv[i + 1]
        i += 2
        if key == "ambe":
            with open(os.path.expanduser(v), "rb") as f:
                job["ambe_hex"] = f.read().hex()
            continue
        if key not in TEXT:
            try:
                v = int(v)
            except ValueError:
                try:
                    v = float(v)
                except ValueError:
                    pass
        job[key] = v
    return job


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("board", help="HOST[:PORT]")
    ap.add_argument("op", choices=("info", "status", "run"))
    ap.add_argument("rest", nargs=argparse.REMAINDER)
    a = ap.parse_args(argv)
    where = address(a.board)
    try:
        if a.op != "run":
            print(json.dumps(request(where, {"op": a.op}), indent=1))
            return 0
        if not a.rest:
            ap.error("run: bench_tx ARGS... or bench_repeater ARGS...")
        job = job_from_args(a.rest[0], a.rest[1:])
        rc = [1]

        def on(d, data):
            if "out" in d:
                print(d["out"], flush=True)
            elif "err" in d:
                print(d["err"], file=sys.stderr, flush=True)
            elif "exit" in d:
                rc[0] = d["exit"]
        r = Request(where, {"op": "run", "job": job, "label": "bench_line"}, on)
        r.wait()
        if r.error:
            raise r.error
        return rc[0]
    except BenchLineError as e:
        print(f"rfoundry-bench: {e}", file=sys.stderr)
        return exit_code(e)


if __name__ == "__main__":
    sys.exit(main())
