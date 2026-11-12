#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""
dmr_bench: the DMR TX pass checks the HIL tests share (check_tx on a burst analysis, check_modem_calls on a listen job's
records) and their limits, with a board's overrides (board.toml [hil.limits]).
"""

OFF_SLOT_MIN_DB = 30.0      # dB: the carrier a radio leaves between its bursts, under the bursts' level


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


# A TX burst's key-up and key-down ramps, each: the regions either side of the 27.5 ms of symbols in the power mask of
# ETSI TS 102 361-1 clause 10.2.3.1.1 (figure 10.3), which keeps a burst out of the other slot. A board can be held to
# another limit (board.toml [hil.limits] dmr_tx_ramp_ms, with its reason)
RAMP_MAX_MS = 1.5


def limit(limits, name, default):
    """(value, reason) of a pass limit: the board's override from limits ([hil.limits]), else (default, None)."""
    o = (limits or {}).get(name)
    return (o["value"], o["reason"]) if o else (default, None)


def check_ramps(per_burst, c, limits=None):
    """The slowest key-up and key-down ramp of the bursts' edges against RAMP_MAX_MS or the board's limit."""
    ru = max(e["ramp_up_ms"] for e in per_burst)
    rd = max(e["ramp_down_ms"] for e in per_burst)
    rmax, why = limit(limits, "dmr_tx_ramp_ms", RAMP_MAX_MS)
    c.add(f"ramps < {rmax} ms" + (f" (board limit: {why})" if why else ""), ru < rmax and rd < rmax,
          f"up {ru}, down {rd}")


def check_tx(sdr, expect, radio_id, cc, c, limits=None):
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
    # A lone burst whose family differs from both neighbors (`isolated`: one misread in a weak signal, not a second
    # transmitter) is reported but does not fail the check.
    fams = sorted({x["family"] for x in b if x["family"] != "emb" and not x.get("isolated")})
    lone = [x for x in b if x["family"] != "emb" and x.get("isolated")]
    c.add("sync family MS only (DMO)", fams == ["ms"],
          ",".join(fams) + (f" ({len(lone)} isolated misread ignored: "
                            + ", ".join(f"{x['family']}@{x['t_ms']:.0f}" for x in lone[:4]) + ")" if lone else ""))
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
    emb_call = [e for e in emb if e.get("type", "grp_voice") in ("grp_voice", "priv_voice")]   # not alias / GPS
    if emb_call and lcs:
        c.add("embedded LC = header LC", all((e.get("src"), e.get("dst")) == (lcs[0].get("src"), lcs[0].get("dst"))
                                            for e in emb_call), "")
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
        check_ramps(per_burst, c, limits)
    else:
        long = [e["on_ms"] for e in edges if e["on_ms"] >= 40]
        c.add("per-burst keying (carrier off in the other slot)", False,
              f"continuous carrier segments {long[:3]} ms" if long else "no keyed segment")
    s = sdr["summary"]
    c.add("no overlapping bursts", s["overlaps"] == 0, f"{s['overlaps']}")
    c.add("no power steps inside bursts", s["power_steps"] == 0, f"{s['power_steps']}")
    c.add("no unexplained carrier", not s["unexplained_carrier"], f"{s['unexplained_carrier'][:3]}")
    res = s.get("off_slot_residual_db")
    if res is not None:
        # A receiver close enough sees the carrier a radio leaves between its bursts; at the floor it's only a bound
        measured = s.get("off_slot_residual_measured")
        c.add(f"off-slot residual <= -{OFF_SLOT_MIN_DB:g} dB", res <= -OFF_SLOT_MIN_DB or not measured,
              f"{res:+.1f} dB" + ("" if measured else " (at the noise floor)"))


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
