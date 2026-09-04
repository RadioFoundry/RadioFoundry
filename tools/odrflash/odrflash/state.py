# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""What odrflash remembers between runs: the last debug-link probe of each radio (so the codec answer survives the
user switching the radio into its bootloader, when the debug link is gone) and, when the user asks, a derived key.
"""
import json
import os
import sys
import time

PROBE_MAX_AGE_S = 30 * 60


def config_dir():
    if sys.platform == "win32":
        base = os.environ.get("APPDATA") or os.path.expanduser("~\\AppData\\Roaming")
        return os.path.join(base, "odrflash")
    if sys.platform == "darwin":
        return os.path.expanduser("~/Library/Application Support/odrflash")
    return os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "odrflash")


def _path(root=None):
    return os.path.join(root or config_dir(), "state.json")


def load(root=None):
    try:
        with open(_path(root), "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save(data, root=None):
    path = _path(root)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path + ".tmp", "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
    os.replace(path + ".tmp", path)


def remember_probe(probe, root=None, now=None):
    data = load(root)
    rec = dict(probe, time=now if now is not None else time.time())
    data["last_probe"] = rec
    save(data, root)
    return rec


def recent_probe(root=None, now=None, max_age=PROBE_MAX_AGE_S):
    """The last probe if it is younger than max_age seconds, else None."""
    rec = load(root).get("last_probe")
    if not rec:
        return None
    age = (now if now is not None else time.time()) - rec.get("time", 0)
    if age < 0 or age > max_age:
        return None
    return dict(rec, age_s=int(age))


def forget_probe(root=None):
    data = load(root)
    if data.pop("last_probe", None) is not None:
        save(data, root)


def key_path(board_id, root=None):
    return os.path.join(root or config_dir(), "keys", "%s.key" % board_id)
