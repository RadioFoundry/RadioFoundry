#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Host tests for the satellite TLE catalog and remembered radio inventory. No network or radio."""
import datetime as dt
import json
import os
import struct
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, TOOLS)

import satellite_tle  # noqa: E402


def tle_line(text):
    body = text.ljust(68)[:68]
    checksum = (sum(int(ch) for ch in body if ch.isdigit()) + body.count("-")) % 10
    return body + str(checksum)


class SatelliteTleTest(unittest.TestCase):
    def setUp(self):
        self.now = dt.datetime(2026, 11, 11, 18, tzinfo=dt.timezone.utc)
        self.line1 = tle_line("1 25544U 98067A   26315.50000000  .00001264  00000-0  29621-4 0  999")
        self.line2 = tle_line("2 25544  51.6442 211.1803 0001677  80.2346  29.8792 15.4936234825637")
        self.iss = {"catalog_id": 25544, "name": "ISS (ZARYA)", "line1": self.line1, "line2": self.line2}

    def test_parses_amateur_bulletin_and_skips_prose(self):
        text = "AMSAT daily bulletin header\n\nISS\n%s\n%s\n/EX\n" % (self.line1, self.line2)
        records = satellite_tle._parse_provider_text(text)
        self.assertEqual([(row["catalog_id"], row["name"]) for row in records], [(25544, "ISS")])
        checked = satellite_tle.validate_records(records, now=self.now)
        self.assertEqual(checked[0]["epoch"], dt.datetime(2026, 11, 11, 12, tzinfo=dt.timezone.utc))

    def test_rejects_bad_checksum_mismatched_id_and_partial_pair(self):
        bad = dict(self.iss, line1=self.line1[:-1] + str((int(self.line1[-1]) + 1) % 10))
        with self.assertRaisesRegex(satellite_tle.SatelliteTleError, "checksum"):
            satellite_tle.validate_record(bad, now=self.now)
        other_line = tle_line("2 25545  51.6442 211.1803 0001677  80.2346  29.8792 15.4936234825637")
        with self.assertRaisesRegex(satellite_tle.SatelliteTleError, "catalog IDs"):
            satellite_tle.validate_record(dict(self.iss, line2=other_line), now=self.now)
        with self.assertRaisesRegex(satellite_tle.SatelliteTleError, "incomplete"):
            satellite_tle._parse_provider_text("ISS\n%s\n" % self.line1)

    def test_accepts_space_padded_four_digit_revolution_number(self):
        line2 = tle_line(self.line2[:63] + " 7540")
        checked = satellite_tle.validate_record(dict(self.iss, line2=line2), now=self.now)
        self.assertEqual(checked["catalog_id"], 25544)

    def test_catalog_selection_never_selects_the_whole_provider(self):
        second = dict(self.iss, catalog_id=25545, name="Satellite 2",
                      line1=tle_line("1 25545U 98067B   26315.50000000  .00001264  00000-0  29621-4 0  999"),
                      line2=tle_line("2 25545  51.6442 211.1803 0001677  80.2346  29.8792 15.4936234825637"))
        catalog = satellite_tle.validate_records([self.iss, second], now=self.now)
        self.assertEqual([row["catalog_id"] for row in satellite_tle.select_catalog(catalog, ["25545"])], [25545])
        with self.assertRaisesRegex(satellite_tle.SatelliteTleError, "does not contain"):
            satellite_tle.select_catalog(catalog, ["25546"])

    def test_offline_bundle_round_trip(self):
        records = satellite_tle.validate_records([self.iss], now=self.now)
        bundle = satellite_tle.make_bundle("https://live.ariss.org/iss.txt", records, self.now)
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "iss.json")
            satellite_tle.save_bundle(path, bundle)
            loaded = satellite_tle.load_bundle(path)
        self.assertEqual(loaded, bundle)
        self.assertEqual(satellite_tle.validate_records(loaded["records"], now=self.now)[0]["catalog_id"], 25544)

    def test_fetch_catalog_is_https_and_fixture_driven(self):
        class Response:
            def __enter__(self):
                return self

            def __exit__(self, *_args):
                pass

            def geturl(self):
                return "https://www.amsat.org/tle/daily-bulletin.txt"

            def read(self, _limit):
                return ("AMSAT\nISS\n%s\n%s\n" % (self_line1, self_line2)).encode("ascii")

        self_line1, self_line2 = self.line1, self.line2
        opener = mock.Mock(return_value=Response())
        rows = satellite_tle.fetch_catalog("https://www.amsat.org/tle/daily-bulletin.txt", self.now, opener=opener)
        self.assertEqual([row["catalog_id"] for row in rows], [25544])
        with self.assertRaisesRegex(satellite_tle.SatelliteTleError, "HTTPS"):
            satellite_tle.fetch_catalog("http://example.test/tle", self.now, opener=opener)

    def test_radio_inventory_cache_remembers_names_only_for_matching_epochs(self):
        record = satellite_tle.validate_record(self.iss, now=self.now)
        epoch = int(record["epoch"].timestamp())
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "state", "satellites.json")
            saved = satellite_tle.remember_inventory(path, "uid:1234", [(25544, epoch)], [record])
            self.assertEqual(saved[0]["name"], "ISS (ZARYA)")
            read_back = satellite_tle.remember_inventory(path, "uid:1234", [(25544, epoch)])
            self.assertEqual(read_back, saved)
            changed = satellite_tle.remember_inventory(path, "uid:1234", [(25544, epoch + 1)])
            self.assertEqual(changed[0]["name"], "")
            with open(path, "r", encoding="utf-8") as source:
                self.assertEqual(json.load(source)["radios"]["uid:1234"]["records"][0]["epoch"], epoch + 1)

    def test_radio_readback_pages_the_installed_inventory(self):
        class FixtureLink:
            rows = [(25544, 1_794_398_400), (20580, 1_794_398_401)]

            def request(self, command, payload):
                self.assert_satellite_command(command)
                operation = payload[0]
                if operation == satellite_tle.OP_INFO:
                    return 0, struct.pack("<BHH", satellite_tle.SATELLITE_PROTOCOL_VERSION, 8, len(self.rows))
                if operation == satellite_tle.OP_LIST:
                    (start,) = struct.unpack("<H", payload[1:])
                    page = self.rows[start:start + 1]
                    return 0, bytes([len(page)]) + b"".join(struct.pack("<II", *row) for row in page)
                raise AssertionError("unexpected satellite operation")

            @staticmethod
            def assert_satellite_command(command):
                if command != satellite_tle.SATELLITE_COMMAND:
                    raise AssertionError("unexpected debug-link command")

        capacity, entries = satellite_tle.radio_inventory(FixtureLink())
        self.assertEqual(capacity, 8)
        self.assertEqual(entries, FixtureLink.rows)

    def test_install_rejects_unexpected_record_after_commit(self):
        record = satellite_tle.validate_record(self.iss, now=self.now)
        old = (20580, 1_794_398_401)
        unexpected = (40000, 1_794_398_402)
        calls = []

        def request(_link, operation, payload=b""):
            calls.append(operation)
            if operation == satellite_tle.OP_INFO:
                count = 1 if calls.count(satellite_tle.OP_INFO) == 1 else 3
                return struct.pack("<BHH", satellite_tle.SATELLITE_PROTOCOL_VERSION, 8, count)
            if operation == satellite_tle.OP_LIST:
                (start,) = struct.unpack("<H", payload)
                rows = [old] if calls.count(satellite_tle.OP_INFO) == 1 else [old, (25544, int(record["epoch"].timestamp())), unexpected]
                page = rows[start:start + 2]
                return bytes([len(page)]) + b"".join(struct.pack("<II", *row) for row in page)
            if operation == satellite_tle.OP_REPLACE_BEGIN:
                return b"\x01\x00"
            if operation == satellite_tle.OP_DATA:
                return struct.pack("<H", struct.unpack_from("<H", payload, 2)[0] + len(payload) - 4)
            if operation == satellite_tle.OP_COMMIT:
                return b""
            if operation == satellite_tle.OP_SYNC_CHANNELS:
                return b""
            raise AssertionError(f"unexpected operation {operation}")

        with mock.patch.object(satellite_tle, "_utc_now", return_value=self.now), \
                mock.patch.object(satellite_tle, "_request", side_effect=request):
            with self.assertRaisesRegex(satellite_tle.SatelliteTleError, "changed unexpectedly"):
                satellite_tle.install_records(object(), [record])
        commit = calls.index(satellite_tle.OP_COMMIT)
        readback = calls.index(satellite_tle.OP_INFO, commit + 1)
        self.assertLess(commit, readback)
        self.assertNotIn(satellite_tle.OP_SYNC_CHANNELS, calls)


if __name__ == "__main__":
    unittest.main()