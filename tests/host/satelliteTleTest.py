#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Host tests for the explicit PC satellite TLE workflow."""
import datetime as dt
import json
import os
import random
import struct
import sys
import tempfile
import urllib.parse
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
import satellite_tle as tle  # noqa: E402
from rfoundry_debug import DebugLinkError  # noqa: E402


failures = 0
checks = 0


def check(condition, message):
    global checks, failures
    checks += 1
    if not condition:
        failures += 1
        print(f"FAIL {message}")


def _checksum(line):
    value = sum(int(ch) for ch in line if ch.isdigit()) + line.count("-")
    return line + str(value % 10)


def make_record(catalog_id, name=None, day=None):
    now = dt.datetime.now(dt.timezone.utc)
    day = now.timetuple().tm_yday if day is None else day
    epoch = f"{now.year % 100:02d}{day:03d}.00000000"
    line1 = f"1 {catalog_id:05d}U 98067A   {epoch}  .00000000  00000-0  00000-0 0 0999"
    line2 = f"2 {catalog_id:05d}  51.6400 120.0000 0005000  10.0000  20.0000 15.5000000012345"
    if len(line1) != 68 or len(line2) != 68:
        raise AssertionError((len(line1), len(line2), line1, line2))
    return {"catalog_id": catalog_id, "name": name or f"SAT-{catalog_id}",
            "line1": _checksum(line1), "line2": _checksum(line2)}


class FakeResponse:
    def __init__(self, body, url):
        self.body = body
        self.url = url

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return False

    def geturl(self):
        return self.url

    def read(self, size):
        return self.body[:size]


class FakeRadio:
    def __init__(self, installed=None, capacity=8, fail_data=False, protocol_version=tle.SATELLITE_PROTOCOL_VERSION):
        self.capacity = capacity
        self.installed = dict(installed or {})
        self.protocol_version = protocol_version
        self.profiles = list(tle.DEFAULT_PROFILE_IDS)
        self.frequency_profiles = []
        self.staged_profiles = None
        self.fail_data = fail_data
        self.calls = []
        self.stream = bytearray()
        self.transaction = 73
        self.count = 0
        self.expected_crc = 0
        self.inject_after_commit = False
        self.replace_inventory = False

    def request(self, command, payload):
        self.calls.append((command, bytes(payload)))
        if command != tle.SATELLITE_COMMAND:
            raise AssertionError(f"unexpected command {command:#x}")
        operation = payload[0]
        args = payload[1:]
        if operation == tle.OP_INFO:
            return 0, struct.pack("<BHH", self.protocol_version, self.capacity, len(self.installed))
        if operation == tle.OP_LIST:
            (start,) = struct.unpack("<H", args)
            rows = sorted(self.installed.items())[start:start + 2]
            return 0, bytes([len(rows)]) + b"".join(struct.pack("<II", *row) for row in rows)
        if operation in (tle.OP_BEGIN, tle.OP_REPLACE_BEGIN):
            self.count, size, self.expected_crc = struct.unpack("<HHI", args)
            self.stream = bytearray()
            self.replace_inventory = operation == tle.OP_REPLACE_BEGIN
            self.staged_profiles = None
            if size > 65535:
                raise AssertionError("oversized fake transaction")
            return 0, struct.pack("<H", self.transaction)
        if operation == tle.OP_DATA:
            transaction, offset = struct.unpack_from("<HH", args)
            if self.fail_data:
                raise DebugLinkError("fixture transfer failure")
            if transaction != self.transaction or offset != len(self.stream):
                raise AssertionError("invalid transaction or offset")
            self.stream.extend(args[4:])
            return 0, struct.pack("<H", len(self.stream))
        if operation == tle.OP_COMMIT:
            transaction, expected_crc = struct.unpack("<HI", args)
            if transaction != self.transaction or expected_crc != zlib.crc32(self.stream) & 0xFFFFFFFF or \
                    expected_crc != self.expected_crc:
                raise AssertionError("commit CRC mismatch")
            offset = 0
            if self.replace_inventory:
                self.installed.clear()
            for _ in range(self.count):
                catalog_id, name_len = struct.unpack_from("<IB", self.stream, offset)
                offset += 5
                offset += name_len
                line1 = bytes(self.stream[offset:offset + 69]).decode("ascii")
                offset += 69
                line2 = bytes(self.stream[offset:offset + 69]).decode("ascii")
                offset += 69
                record = {"catalog_id": catalog_id, "name": "fixture", "line1": line1, "line2": line2}
                item = tle.validate_record(record)
                self.installed[catalog_id] = int(item["epoch"].timestamp())
            if offset != len(self.stream):
                raise AssertionError("trailing record bytes")
            if self.staged_profiles is not None:
                if any(profile is None for profile in self.staged_profiles):
                    raise AssertionError("incomplete staged profile set")
                self.frequency_profiles = self.staged_profiles
                self.profiles = [profile["catalog_id"] for profile in self.frequency_profiles]
            if self.inject_after_commit:
                self.installed[40000] = 1_794_398_402
            return 0, b""
        if operation == tle.OP_ABORT:
            return 0, b""
        if operation == tle.OP_SYNC_CHANNELS:
            return 0, b""
        if operation == tle.OP_PROFILE_GET:
            if self.protocol_version < tle.PROFILE_PROTOCOL_MIN_VERSION:
                raise DebugLinkError("profile operations unsupported", 1)
            return 0, bytes([len(self.profiles)]) + struct.pack(f"<{len(self.profiles)}I", *self.profiles)
        if operation == tle.OP_PROFILE_SET:
            if self.protocol_version < tle.PROFILE_PROTOCOL_MIN_VERSION:
                raise DebugLinkError("profile operations unsupported", 1)
            count = args[0]
            self.profiles = list(struct.unpack_from(f"<{count}I", args, 1))
            return 0, b""
        if operation == tle.OP_PROFILE_STAGE:
            transaction, count = struct.unpack_from("<HB", args)
            if transaction != self.transaction or len(args) != 3 + count * 14:
                raise AssertionError("invalid staged profile payload")
            self.staged_profiles = []
            for index in range(count):
                catalog_id, rx_hz, tx_hz, tone = struct.unpack_from("<IIIH", args, 3 + index * 14)
                self.staged_profiles.append({"catalog_id": catalog_id, "rx_hz": rx_hz, "tx_hz": tx_hz,
                                             "tx_tone_tenths": tone})
            return 0, b""
        if operation == tle.OP_PROFILE_STAGE_CHUNK:
            transaction, total, start, count = struct.unpack_from("<HBBB", args)
            if transaction != self.transaction or total != self.count or count == 0 or count > 4 or \
                    len(args) != 5 + count * 14:
                raise AssertionError("invalid chunked profile payload")
            if self.staged_profiles is None:
                if start != 0:
                    raise AssertionError("profile chunks must start at zero")
                self.staged_profiles = [None] * total
            if len(self.staged_profiles) != total or start != next(
                    (index for index, profile in enumerate(self.staged_profiles) if profile is None), total):
                raise AssertionError("profile chunks must be consecutive")
            for index in range(count):
                offset = 5 + index * 14
                catalog_id, rx_hz, tx_hz, tone = struct.unpack_from("<IIIH", args, offset)
                self.staged_profiles[start + index] = {"catalog_id": catalog_id, "rx_hz": rx_hz,
                                                       "tx_hz": tx_hz, "tx_tone_tenths": tone}
            return 0, b""
        if operation == tle.OP_PROFILE_VALUES_GET:
            profiles = self.frequency_profiles
            return 0, bytes([len(profiles)]) + b"".join(
                struct.pack("<IIIH", profile["catalog_id"], profile["rx_hz"], profile["tx_hz"],
                            profile["tx_tone_tenths"]) for profile in profiles)
        raise AssertionError(f"unexpected operation {operation}")


class UnsupportedRadio:
    def __init__(self):
        self.commands = []

    def request(self, command, payload):
        self.commands.append((command, payload))
        raise DebugLinkError("unknown command", 1)


def test_tle_validation():
    record = make_record(25544)
    result = tle.validate_record(record)
    check(result["catalog_id"] == 25544 and not result["stale"], "valid TLE and epoch")
    bad = dict(record, line2=record["line2"][:-1] + str((int(record["line2"][-1]) + 1) % 10))
    try:
        tle.validate_record(bad)
        check(False, "bad checksum rejected")
    except tle.SatelliteTleError:
        check(True, "bad checksum rejected")
    mismatch = dict(record, catalog_id=25545)
    try:
        tle.validate_record(mismatch)
        check(False, "mismatched catalog ID rejected")
    except tle.SatelliteTleError:
        check(True, "mismatched catalog ID rejected")
    duplicate = [record, dict(record)]
    try:
        tle.validate_records(duplicate)
        check(False, "duplicate record rejected")
    except tle.SatelliteTleError:
        check(True, "duplicate record rejected")
    malformed = dict(record, line1=record["line1"][:20] + "999" + record["line1"][23:])
    try:
        tle.validate_record(malformed)
        check(False, "malformed epoch rejected")
    except tle.SatelliteTleError:
        check(True, "malformed epoch rejected")
    stale_record = make_record(12345, day=1)
    stale = tle.validate_record(stale_record)
    check(stale["stale"], "old TLE is reported stale")
    try:
        tle.validate_record(record, max_age_days=0)
        check(False, "invalid age limit rejected")
    except tle.SatelliteTleError:
        check(True, "invalid age limit rejected")


def test_provider_selection():
    records = [make_record(25544), make_record(20580)]
    body = "\n".join(part for record in records for part in
                     (record["name"], record["line1"], record["line2"])) + "\n"
    seen = {}

    def opener(request, timeout):
        seen["url"] = request.full_url
        seen["timeout"] = timeout
        return FakeResponse(body.encode("ascii"), request.full_url)

    got = tle.fetch_records([20580, 25544], now=dt.datetime.now(dt.timezone.utc), opener=opener)
    query = urllib.parse.parse_qs(urllib.parse.urlsplit(seen["url"]).query)
    check([item["catalog_id"] for item in got] == [20580, 25544], "batch returns every explicit ID in order")
    check(query.get("CATNR") == ["20580,25544"] and query.get("FORMAT") == ["TLE"],
          "provider query contains only explicit IDs")
    try:
        tle._parse_provider_text("malformed provider text\n" + body, strict=True)
        check(False, "malformed provider response is not silently skipped")
    except tle.SatelliteTleError:
        check(True, "malformed provider response is not silently skipped")
    try:
        tle.fetch_records([25544], opener=opener)
        check(False, "provider extra ID rejected")
    except tle.SatelliteTleError:
        check(True, "provider extra ID rejected")
    try:
        tle.fetch_records([25544], "http://example.test/tle", opener=opener)
        check(False, "non-HTTPS provider rejected")
    except tle.SatelliteTleError:
        check(True, "non-HTTPS provider rejected")
    try:
        tle._https_url("https://[invalid")
        check(False, "malformed HTTPS URL rejected")
    except tle.SatelliteTleError:
        check(True, "malformed HTTPS URL rejected")
    try:
        tle._HttpsRedirectHandler().redirect_request(None, None, 302, "Found", {}, "http://example.test/tle")
        check(False, "HTTPS downgrade redirect rejected")
    except tle.SatelliteTleError:
        check(True, "HTTPS downgrade redirect rejected")
    try:
        tle.parse_catalog_ids(["25544,25544"])
        check(False, "duplicate explicit selection rejected")
    except tle.SatelliteTleError:
        check(True, "duplicate explicit selection rejected")


def test_provider_parser_fuzz():
    rng = random.Random(0x5A71)
    record = make_record(25544)
    valid_text = "\n".join((record["name"], record["line1"], record["line2"])) + "\n"
    alphabet = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz.-+\n"
    for iteration in range(256):
        candidate = valid_text
        for _ in range(1 + iteration % 3):
            if candidate:
                index = rng.randrange(len(candidate))
                candidate = candidate[:index] + rng.choice(alphabet) + candidate[index + 1:]
        try:
            tle._parse_provider_text(candidate, strict=True)
        except tle.SatelliteTleError:
            continue
        except Exception as exc:
            check(False, f"provider parser fuzz raised {type(exc).__name__}")
            return
    check(True, "provider parser mutation loop completed without unexpected exceptions")


def test_bundle_round_trip():
    records = tle.validate_records([make_record(25544), make_record(20580)])
    bundle = tle.make_bundle(tle.DEFAULT_PROVIDER_URL, records)
    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "selected.json")
        tle.save_bundle(path, bundle)
        loaded = tle.load_bundle(path)
        validated = tle.validate_records(loaded["records"])
        check(loaded["schema_version"] == 1 and len(validated) == 2, "offline bundle round trip")
        check(not any("frequency" in key or "mode" in key for key in loaded),
              "bundle excludes radio frequency and mode configuration")
        invalid_version = dict(bundle, schema_version=True)
        with open(path, "w", encoding="utf-8") as target:
            json.dump(invalid_version, target)
        try:
            tle.load_bundle(path)
            check(False, "boolean bundle schema version rejected")
        except tle.SatelliteTleError:
            check(True, "boolean bundle schema version rejected")
        profile_bundle = tle.make_bundle(tle.DEFAULT_PROVIDER_URL, validated, profile_ids=[43017, 25544])
        tle.save_bundle(path, profile_bundle)
        loaded_profiles = tle.load_bundle(path)
        check(loaded_profiles["schema_version"] == 2 and loaded_profiles["profile_ids"] == [25544, 43017],
              "schema-v2 bundle round trips sorted profile IDs")
        custom_profiles = [
            {"catalog_id": 20580, "rx_hz": 145900000, "tx_hz": 435100000, "tx_tone_tenths": 885},
            {"catalog_id": 25544, "rx_hz": 437800000, "tx_hz": 145990000, "tx_tone_tenths": 670},
        ]
        custom_bundle = tle.make_bundle(tle.DEFAULT_PROVIDER_URL, validated, profiles=custom_profiles)
        tle.save_bundle(path, custom_bundle)
        loaded_custom = tle.load_bundle(path)
        check(loaded_custom["schema_version"] == 3 and loaded_custom["profiles"] == custom_profiles,
              "schema-v3 bundle round trips arbitrary frequency profiles")
        for invalid_profiles in ([25544, 25544], [99999], list(sorted(tle.SUPPORTED_PROFILE_IDS)[:11]), []):
            try:
                tle.validate_profile_ids(invalid_profiles)
                check(False, f"invalid profile selection rejected: {invalid_profiles}")
            except tle.SatelliteTleError:
                check(True, f"invalid profile selection rejected: {invalid_profiles}")
        with open(path, "w", encoding="utf-8") as target:
            target.write('{"schema_version":1,"schema_version":1}')
        try:
            tle.load_bundle(path)
            check(False, "duplicate JSON member rejected")
        except tle.SatelliteTleError:
            check(True, "duplicate JSON member rejected")
        cache_path = os.path.join(directory, "inventory.json")
        with open(cache_path, "w", encoding="utf-8") as target:
            json.dump({"schema_version": 1, "radios": {"port:test": None}}, target)
        try:
            tle.remember_inventory(cache_path, "port:test", [], [])
            check(False, "malformed local inventory entry rejected")
        except tle.SatelliteTleError:
            check(True, "malformed local inventory entry rejected")
        with open(cache_path, "w", encoding="utf-8") as target:
            json.dump({"schema_version": True, "radios": {}}, target)
        try:
            tle.remember_inventory(cache_path, "port:test", [], [])
            check(False, "boolean inventory schema version rejected")
        except tle.SatelliteTleError:
            check(True, "boolean inventory schema version rejected")


def test_protocol_versions():
    ids = sorted(tle.SUPPORTED_PROFILE_IDS)[:10]
    records = tle.validate_records([make_record(catalog_id) for catalog_id in ids])
    profiles = [{"catalog_id": catalog_id, "rx_hz": 145900000, "tx_hz": 435100000,
                 "tx_tone_tenths": 885} for catalog_id in ids]
    for version in (6, 7):
        radio = FakeRadio(capacity=10, protocol_version=version)
        result = tle.install_records(radio, records, ids, profiles)
        capacity, entries = tle.radio_inventory(radio)
        chunks = [payload for _, payload in radio.calls if payload[0] == tle.OP_PROFILE_STAGE_CHUNK]
        check(result["frequency_profiles"] == profiles and capacity == 10 and len(entries) == 10 and len(chunks) == 3,
              f"protocol-v{version} installs and reads ten profiles using chunks")
    radio = FakeRadio(protocol_version=8)
    try:
        tle.radio_inventory(radio)
        check(False, "unknown protocol is rejected")
    except tle.SatelliteTleError:
        check(True, "unknown protocol is rejected")


def test_install_transaction():
    selected = tle.validate_records([make_record(25544), make_record(20580)])
    old_epoch = int(selected[0]["epoch"].timestamp()) - 1000
    radio = FakeRadio({99999: old_epoch, 25544: old_epoch})
    result = tle.install_records(radio, selected)
    expected_inventory = [(20580, int(selected[1]["epoch"].timestamp())),
                          (25544, int(selected[0]["epoch"].timestamp()))]
    check(result == {"capacity": 8, "count": 2, "installed": [20580, 25544],
                     "inventory": expected_inventory}, "install reports verified IDs and inventory")
    check(99999 not in radio.installed and radio.installed[25544] != old_epoch,
          "install removes unselected satellites and updates existing selected epochs")
    check(any(payload[0] == tle.OP_REPLACE_BEGIN for _, payload in radio.calls),
          "orbital-only install uses an inventory replacement")
    too_many = FakeRadio(capacity=20)
    try:
        tle.install_records(too_many, [make_record(50000 + i) for i in range(11)])
        check(False, "more than ten selected TLEs are rejected")
    except tle.SatelliteTleError:
        check(not too_many.calls, "more than ten selected TLEs are rejected before contacting the radio")
    check(all(payload[0] != tle.OP_SYNC_CHANNELS for _, payload in radio.calls),
          "TLE install leaves codeplug zones and channels unchanged")
    data_calls = [payload for command, payload in radio.calls if command == tle.SATELLITE_COMMAND and
                  payload[0] == tle.OP_DATA]
    check(len(data_calls) > 1 and all(len(payload) <= 64 for payload in data_calls), "install uses bounded chunks")
    profiles_radio = FakeRadio({99999: old_epoch})
    selected_profiles = [25544, 40908, 61781, 63213]
    selected_records = tle.validate_records([make_record(catalog_id) for catalog_id in selected_profiles])
    profile_result = tle.install_records(profiles_radio, selected_records, selected_profiles)
    check(profile_result["profile_ids"] == selected_profiles and profiles_radio.profiles == selected_profiles,
          "install updates frequency profiles and verifies read-back")
    check(profile_result["inventory"] == [(catalog_id, int(record["epoch"].timestamp()))
                                           for catalog_id, record in zip(selected_profiles, selected_records)],
          "profile sync removes previously installed satellites that were unselected")
    check(all(payload[0] != tle.OP_BEGIN for _, payload in profiles_radio.calls) and
          any(payload[0] == tle.OP_REPLACE_BEGIN for _, payload in profiles_radio.calls),
          "profile sync uses explicit replace-inventory transaction")
    check(all(payload[0] != tle.OP_SYNC_CHANNELS for _, payload in profiles_radio.calls),
          "profile sync does not create codeplug zones or channels")
    check(tle.radio_profile_ids(profiles_radio) == selected_profiles, "profile list reads back through protocol")
    manual = [make_record(99001)]
    manual_profiles = [{"catalog_id": 99001, "rx_hz": 145900000, "tx_hz": 435100000, "tx_tone_tenths": 885}]
    manual_radio = FakeRadio(protocol_version=tle.PROFILE_VALUES_PROTOCOL_VERSION)
    manual_result = tle.install_records(manual_radio, manual, [99001], manual_profiles)
    check(manual_result["frequency_profiles"] == manual_profiles and manual_radio.frequency_profiles == manual_profiles,
            "schema-v3 install commits and verifies custom frequencies")
    check(any(payload[0] == tle.OP_PROFILE_STAGE for _, payload in manual_radio.calls),
            "schema-v3 install stages frequency profiles before commit")
    check(all(payload[0] != tle.OP_SYNC_CHANNELS for _, payload in manual_radio.calls),
          "schema-v3 install leaves the codeplug unchanged")
    large_ids = sorted(tle.SUPPORTED_PROFILE_IDS)[:10]
    large_records = tle.validate_records([make_record(catalog_id) for catalog_id in large_ids])
    large_profiles = [{"catalog_id": catalog_id, "rx_hz": 145900000, "tx_hz": 435100000,
                       "tx_tone_tenths": 885} for catalog_id in large_ids]
    large_radio = FakeRadio(capacity=10)
    large_result = tle.install_records(large_radio, large_records, large_ids, large_profiles)
    large_chunks = [payload for _, payload in large_radio.calls if payload[0] == tle.OP_PROFILE_STAGE_CHUNK]
    check(large_result["frequency_profiles"] == large_profiles and len(large_chunks) == 3 and
          all(len(payload) <= 64 for payload in large_chunks),
          "ten-profile install stages three bounded protocol-v6 chunks")
    old_radio = FakeRadio(protocol_version=tle.PROFILE_VALUES_PROTOCOL_VERSION)
    try:
        tle.install_records(old_radio, large_records, large_ids, large_profiles)
        check(False, "ten-profile replacement rejects protocol-v5 radio before writing")
    except tle.SatelliteTleError as exc:
        check("protocol version 6" in str(exc) and
              all(payload[0] != tle.OP_REPLACE_BEGIN for _, payload in old_radio.calls),
              "protocol-v5 radio gets a clear ten-profile limit before transaction")
    legacy_radio = FakeRadio(protocol_version=tle.LEGACY_SATELLITE_PROTOCOL_VERSION)
    try:
        tle.install_records(legacy_radio, selected)
        check(False, "orbital replacement rejects protocol-v2 before writing")
    except tle.SatelliteTleError as exc:
        check("requires satellite protocol version 4" in str(exc) and
              all(payload[0] not in (tle.OP_BEGIN, tle.OP_REPLACE_BEGIN) for _, payload in legacy_radio.calls),
              "orbital replacement rejects older radio before starting a transaction")
    try:
        tle.install_records(legacy_radio, selected, selected_profiles)
        check(False, "profile bundle is rejected before modification on protocol-v2 radio")
    except tle.SatelliteTleError as exc:
        check("requires satellite protocol version 4" in str(exc), "profile update rejects older radio")
    unexpected = FakeRadio()
    unexpected.inject_after_commit = True
    try:
        tle.install_records(unexpected, selected)
        check(False, "unexpected post-commit inventory entry rejected")
    except tle.SatelliteTleError as exc:
        check("changed unexpectedly" in str(exc), "unexpected post-commit inventory entry rejected")
    try:
        tle.install_records(UnsupportedRadio(), selected)
        check(False, "unsupported radio refused")
    except tle.SatelliteTleError as exc:
        check("no raw flash fallback" in str(exc), "unsupported radio refused without raw fallback")
    mixed_config = FakeRadio()
    try:
        tle.install_records(mixed_config, [dict(selected[0], rx_hz=145900000)])
        check(False, "frequency settings rejected from orbital install")
    except tle.SatelliteTleError:
        check(not mixed_config.calls, "frequency settings rejected before radio transaction")
    failed = FakeRadio({99999: old_epoch, 25544: old_epoch}, fail_data=True)
    try:
        tle.install_records(failed, selected)
        check(False, "failed transfer rejected")
    except DebugLinkError:
        check(any(payload[0] == tle.OP_ABORT for _, payload in failed.calls) and
              failed.installed == {99999: old_epoch, 25544: old_epoch},
              "failed replacement is aborted and leaves the inventory intact")


def main():
    test_tle_validation()
    test_provider_selection()
    test_provider_parser_fuzz()
    test_bundle_round_trip()
    test_install_transaction()
    test_protocol_versions()
    print(f"# satelliteTleTest {'FAILED' if failures else 'PASSED'} ({checks} checks, {failures} failures)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())