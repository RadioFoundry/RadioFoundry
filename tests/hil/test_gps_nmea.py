# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""tools/rfoundry_gps.py's NMEA from the radio's fixes (docs/spec/tcp-services.md 5.1): no radio needed."""
import datetime
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import rfoundry_gps  # noqa: E402

NOON = datetime.datetime(2026, 11, 21, 12, 40, 0, tzinfo=datetime.timezone.utc)
FIX = {"gps": 1, "lat": 33.749, "lon": -84.388, "alt_m": 320, "kmh": 4, "course": 271, "sats": 9, "hdop": 0.9,
       "utc": 123519}


def test_checksum():
    # NMEA 0183's own example sentence
    assert rfoundry_gps.nmea("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,").endswith("*47\r\n")


def test_gga_and_rmc():
    gga, rmc = rfoundry_gps.fix_to_nmea(FIX, NOON).splitlines()
    assert gga.startswith("$GPGGA,123519.00,3344.94000,N,08423.28000,W,1,09,0.9,320.0,M,,M,,*")
    assert rmc.startswith("$GPRMC,123519.00,A,3344.94000,N,08423.28000,W,2.2,271.0,211126,,,A*")


def test_unknown_fields_left_empty():
    fix = {"gps": 1, "lat": -0.0000001, "lon": 180.0, "sats": 4, "utc": 5}
    gga, rmc = rfoundry_gps.fix_to_nmea(fix, NOON).splitlines()
    assert gga.startswith("$GPGGA,000005.00,0000.00001,S,18000.00000,E,1,04,,,,,M,,*")
    assert rmc.startswith("$GPRMC,000005.00,A,0000.00001,S,18000.00000,E,,,211126,,,A*")


def test_minutes_round_up_to_the_next_degree():
    gga = rfoundry_gps.fix_to_nmea(dict(FIX, lat=33.99999999), NOON).splitlines()[0]
    assert ",3400.00000,N," in gga


def test_a_fix_before_midnight_keeps_its_date():
    just_after = datetime.datetime(2026, 11, 22, 0, 0, 1, tzinfo=datetime.timezone.utc)
    rmc = rfoundry_gps.fix_to_nmea(dict(FIX, utc=235959), just_after).splitlines()[1]
    assert ",211126," in rmc
