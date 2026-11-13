#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Generate firmware satellite defaults from a shared daily TLE cache."""
import argparse
import datetime as dt
import json
import os
import sys
import zlib

import satellite_tle as tle
from satellite_builtin_fallback import DISPLAY_NAMES, FALLBACK_RECORDS


CATALOG_IDS = (25544, 27607, 43017)
CACHE_TTL = dt.timedelta(days=1)
MAX_CACHE_AGE_DAYS = 36500
UTC = dt.timezone.utc


def _plain_records(records, now):
    plain = [{key: item[key] for key in ("catalog_id", "name", "line1", "line2")} for item in records]
    validated = tle.validate_records(plain, CATALOG_IDS, now=now, max_age_days=MAX_CACHE_AGE_DAYS)
    return [{
        "catalog_id": item["catalog_id"],
        "name": DISPLAY_NAMES[item["catalog_id"]],
        "line1": item["line1"],
        "line2": item["line2"],
    } for item in validated]


def _read_cache(path, now):
    try:
        bundle = tle.load_bundle(path)
        records = _plain_records(bundle["records"], now)
        fetched_at = dt.datetime.fromisoformat(bundle["fetched_at"].replace("Z", "+00:00"))
        return records, fetched_at
    except (OSError, tle.SatelliteTleError, KeyError, TypeError, ValueError):
        return None, None


def resolve_records(cache_path, offline=False, now=None, fetch_records=None, warn=None):
    now = dt.datetime.now(UTC) if now is None else now
    fetch_records = tle.fetch_records if fetch_records is None else fetch_records
    warn = (lambda _message: None) if warn is None else warn
    cached, fetched_at = _read_cache(cache_path, now)
    cache_fresh = fetched_at is not None and dt.timedelta(0) <= now - fetched_at < CACHE_TTL
    if cache_fresh or offline:
        return cached if cached is not None else _plain_records(FALLBACK_RECORDS, now), \
            "cache" if cached is not None else "fallback"

    try:
        records = []
        for catalog_id in CATALOG_IDS:
            records.extend(fetch_records([catalog_id], now=now))
        records = _plain_records(records, now)
        tle.save_bundle(cache_path, tle.make_bundle(tle.DEFAULT_PROVIDER_URL, records, fetched_at=now))
        return records, "refreshed"
    except (OSError, tle.SatelliteTleError) as exc:
        warn(f"satellite TLE refresh failed ({exc}); using " + ("cached" if cached else "checked-in") + " data")
        return cached if cached is not None else _plain_records(FALLBACK_RECORDS, now), \
            "cache-fallback" if cached is not None else "checked-in-fallback"


def _c_string(value):
    value.encode("ascii")
    return json.dumps(value, ensure_ascii=True)


def render_header(records, build_id=0):
    payload = json.dumps(records, sort_keys=True, separators=(",", ":")).encode("ascii")
    build_hash = zlib.crc32(payload, build_id & 0xFFFFFFFF) & 0xFFFFFFFF
    lines = ["/* Generated from the daily satellite TLE cache. Do not edit. */",
             f"#define SATELLITE_BUILTIN_BUILD_HASH 0x{build_hash:08X}u",
             "static const satellite_record_t built_in_records[] = {"]
    for record in records:
        name = record["name"]
        lines.extend([
            "\t{",
            f"\t\t.catalog_id = {record['catalog_id']}u,",
            f"\t\t.name_length = sizeof({_c_string(name)}) - 1u,",
            f"\t\t.name = {_c_string(name)},",
            f"\t\t.line1 = {_c_string(record['line1'])},",
            f"\t\t.line2 = {_c_string(record['line2'])},",
            "\t},",
        ])
    lines.extend(["};", ""])
    return "\n".join(lines)


def write_if_changed(path, content):
    try:
        with open(path, "r", encoding="ascii") as current:
            if current.read() == content:
                return False
    except OSError:
        pass
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    temporary = path + ".tmp"
    with open(temporary, "w", encoding="ascii", newline="\n") as output:
        output.write(content)
    os.replace(temporary, path)
    return True


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True)
    parser.add_argument("--cache", default="build/satellite-tle-cache.json")
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--build-id", type=lambda value: int(value.rstrip("uU"), 0), default=0)
    args = parser.parse_args(argv)
    try:
        records, source = resolve_records(args.cache, offline=args.offline,
                                          warn=lambda message: print(message, file=sys.stderr))
        write_if_changed(args.output, render_header(records, args.build_id))
        print(f"satellite built-ins: {source}")
        return 0
    except (OSError, tle.SatelliteTleError, UnicodeError) as exc:
        print(f"gen_satellite_builtins: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())