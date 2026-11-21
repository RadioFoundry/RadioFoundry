#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Gather, validate, bundle, and install selected satellite TLEs (docs/spec/satellite.md 6)."""
import argparse
import datetime as dt
import json
import math
import os
import re
import struct
import sys
import tempfile
import urllib.parse
import urllib.request
import zlib

from rfoundry_debug import DebugLink, DebugLinkError


DEFAULT_PROVIDER_URL = "https://celestrak.org/NORAD/elements/gp.php"
PROVIDER_URLS = {
    "amsat": "https://www.amsat.org/tle/daily-bulletin.txt",
    "ariss": "https://live.ariss.org/iss.txt",
}
DEFAULT_MAX_AGE_DAYS = 30
SCHEMA_VERSION = 3
SUPPORTED_SCHEMA_VERSIONS = (1, 2, 3)
PROFILE_VALUES_PROTOCOL_VERSION = 5
PROFILE_CHUNK_PROTOCOL_VERSION = 6
SATELLITE_PROTOCOL_VERSION = 7
REPLACE_PROTOCOL_MIN_VERSION = 4
PROFILE_PROTOCOL_MIN_VERSION = 3
LEGACY_SATELLITE_PROTOCOL_VERSION = 2
SATELLITE_COMMAND = 0x26
OP_INFO, OP_LIST, OP_BEGIN, OP_DATA, OP_COMMIT, OP_ABORT, OP_SYNC_CHANNELS = range(7)
OP_PROFILE_GET, OP_PROFILE_SET = 7, 8
OP_REPLACE_BEGIN = 9
OP_PROFILE_STAGE, OP_PROFILE_VALUES_GET, OP_PROFILE_STAGE_CHUNK = 10, 11, 12
DEFAULT_PROFILE_IDS = (25544, 27607, 43017)
SUPPORTED_PROFILE_IDS = frozenset((25544, 27607, 40908, 40931, 43017, 43678, 61781,
                                   63213, 63214, 63215, 63217, 63218, 63219, 63237, 63238, 63239, 67291))
MAX_PROFILES = 10
MAX_NAME_BYTES = 48
MAX_BUNDLE_BYTES = 1_000_000
MAX_TLE_RESPONSE_BYTES = 1_000_000
DATA_CHUNK_MAX = 59
UTC = dt.timezone.utc


class SatelliteTleError(Exception):
    """Invalid orbital input or an unsuccessful satellite operation."""


def _utc_now():
    return dt.datetime.now(UTC)


def parse_catalog_ids(values):
    ids = []
    for value in values:
        for item in value.split(","):
            try:
                catalog_id = int(item, 10)
            except ValueError as exc:
                raise SatelliteTleError(f"invalid catalog ID {item!r}") from exc
            if not 1 <= catalog_id <= 99999:
                raise SatelliteTleError(f"catalog ID must be in 1..99999: {catalog_id}")
            if catalog_id in ids:
                raise SatelliteTleError(f"duplicate catalog ID: {catalog_id}")
            ids.append(catalog_id)
    if not ids:
        raise SatelliteTleError("select at least one catalog ID")
    return sorted(ids)


def _https_url(url):
    if not isinstance(url, str):
        raise SatelliteTleError("provider URL must be text")
    try:
        parsed = urllib.parse.urlsplit(url)
    except ValueError as exc:
        raise SatelliteTleError(f"invalid provider URL: {exc}") from exc
    if parsed.scheme.lower() != "https" or not parsed.hostname or parsed.username or parsed.password or parsed.fragment:
        raise SatelliteTleError("provider URL must be an HTTPS URL without credentials or a fragment")
    return parsed


class _HttpsRedirectHandler(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, file, code, message, headers, new_url):
        try:
            scheme = urllib.parse.urlsplit(new_url).scheme.lower()
        except ValueError as exc:
            raise SatelliteTleError("provider redirected to an invalid URL") from exc
        if scheme != "https":
            raise SatelliteTleError("provider redirect must remain HTTPS")
        return super().redirect_request(request, file, code, message, headers, new_url)


def _open_provider(request, timeout, opener=None):
    if opener is not None:
        return opener(request, timeout=timeout)
    return urllib.request.build_opener(_HttpsRedirectHandler()).open(request, timeout=timeout)


def _checksum(line):
    if len(line) != 69 or not line[68].isdigit():
        return False
    checksum = sum(int(ch) for ch in line[:68] if ch.isdigit())
    checksum += line[:68].count("-")
    return checksum % 10 == int(line[68])


def _tle_epoch(line1):
    field = line1[18:32]
    if not re.fullmatch(r"\d{2}\d{3}\.\d{8}", field):
        raise SatelliteTleError("invalid TLE epoch field")
    year_short = int(field[:2])
    year = 1900 + year_short if year_short >= 57 else 2000 + year_short
    day = int(field[2:5])
    fraction = float("0." + field[6:])
    days_in_year = 366 if _leap_year(year) else 365
    if day < 1 or day > days_in_year:
        raise SatelliteTleError("TLE epoch day is outside its calendar year")
    return dt.datetime(year, 1, 1, tzinfo=UTC) + dt.timedelta(days=day - 1 + fraction)


def _leap_year(year):
    return year % 4 == 0 and (year % 100 != 0 or year % 400 == 0)


def _finite_field(value, label):
    try:
        number = float(value)
    except ValueError as exc:
        raise SatelliteTleError(f"invalid {label} field") from exc
    if not math.isfinite(number):
        raise SatelliteTleError(f"non-finite {label} field")
    return number


def validate_record(record, now=None, max_age_days=DEFAULT_MAX_AGE_DAYS):
    if not isinstance(record, dict) or set(record) != {"catalog_id", "name", "line1", "line2"}:
        raise SatelliteTleError("each TLE record must contain catalog_id, name, line1, and line2 only")
    catalog_id = record["catalog_id"]
    if isinstance(catalog_id, bool) or not isinstance(catalog_id, int) or not 1 <= catalog_id <= 99999:
        raise SatelliteTleError("catalog_id must be an integer in 1..99999")
    name = record["name"]
    if not isinstance(name, str) or not name or any(ord(ch) < 32 or ord(ch) == 127 for ch in name):
        raise SatelliteTleError(f"catalog {catalog_id}: name must be nonempty printable text")
    try:
        name_bytes = name.encode("utf-8")
    except UnicodeEncodeError as exc:
        raise SatelliteTleError(f"catalog {catalog_id}: name is not valid UTF-8") from exc
    if len(name_bytes) > MAX_NAME_BYTES:
        raise SatelliteTleError(f"catalog {catalog_id}: UTF-8 name exceeds {MAX_NAME_BYTES} bytes")

    line1, line2 = record["line1"], record["line2"]
    if not isinstance(line1, str) or not isinstance(line2, str):
        raise SatelliteTleError(f"catalog {catalog_id}: TLE lines must be text")
    try:
        line1.encode("ascii")
        line2.encode("ascii")
    except UnicodeEncodeError as exc:
        raise SatelliteTleError(f"catalog {catalog_id}: TLE lines must be ASCII") from exc
    if len(line1) != 69 or len(line2) != 69 or line1[0:2] != "1 " or line2[0:2] != "2 ":
        raise SatelliteTleError(f"catalog {catalog_id}: expected two 69-character TLE lines numbered 1 and 2")
    if not _checksum(line1) or not _checksum(line2):
        raise SatelliteTleError(f"catalog {catalog_id}: TLE checksum mismatch")
    if not line1[2:7].isdigit() or not line2[2:7].isdigit() or \
            int(line1[2:7]) != catalog_id or int(line2[2:7]) != catalog_id:
        raise SatelliteTleError(f"catalog {catalog_id}: TLE catalog IDs do not match the record")
    if line1[7] not in "UCS" or line1[8] != " " or line1[17] != " " or line1[32] != " ":
        raise SatelliteTleError(f"catalog {catalog_id}: malformed line 1 fixed-format fields")
    if not re.fullmatch(r"[ 0-9A-Za-z]{8}", line1[9:17]) or \
            not re.fullmatch(r"[ +-]\.\d{8}", line1[33:43]) or \
            not re.fullmatch(r"[ +-]\d{5}[+-]\d", line1[44:52]) or \
            not re.fullmatch(r"[ +-]\d{5}[+-]\d", line1[53:61]) or \
            line1[43] != " " or line1[52] != " " or line1[61] != " " or \
            not line1[62].isdigit() or line1[63] != " " or not line1[64:68].strip().isdigit():
        raise SatelliteTleError(f"catalog {catalog_id}: malformed line 1 fixed-format fields")
    if line2[7] != " " or line2[16] != " " or line2[25] != " " or line2[33] != " " or \
            line2[42] != " " or line2[51] != " " or not re.fullmatch(r" *\d{1,5}", line2[63:68]):
        raise SatelliteTleError(f"catalog {catalog_id}: malformed line 2 fixed-format fields")

    inclination = _finite_field(line2[8:16], "inclination")
    raan = _finite_field(line2[17:25], "right ascension")
    eccentricity_text = line2[26:33]
    if not eccentricity_text.isdigit():
        raise SatelliteTleError(f"catalog {catalog_id}: invalid eccentricity field")
    eccentricity = int(eccentricity_text) / 10_000_000.0
    argument = _finite_field(line2[34:42], "argument of perigee")
    anomaly = _finite_field(line2[43:51], "mean anomaly")
    mean_motion = _finite_field(line2[52:63], "mean motion")
    angle_pattern = r" *\d{1,3}\.\d{4}"
    if any(not re.fullmatch(angle_pattern, line2[start:end]) for start, end in
           ((8, 16), (17, 25), (34, 42), (43, 51))) or \
            not re.fullmatch(r"(?:\d{2}| \d)\.\d{8}", line2[52:63]):
        raise SatelliteTleError(f"catalog {catalog_id}: malformed line 2 numeric fields")
    if not (0 <= inclination <= 180 and 0 <= raan < 360 and 0 <= eccentricity < 1 and
            0 <= argument < 360 and 0 <= anomaly < 360 and mean_motion > 0):
        raise SatelliteTleError(f"catalog {catalog_id}: orbital field is outside its valid range")

    epoch = _tle_epoch(line1)
    now = _utc_now() if now is None else now
    if now.tzinfo is None:
        raise SatelliteTleError("validation time must include a timezone")
    age_days = (now.astimezone(UTC) - epoch).total_seconds() / 86400.0
    if age_days < -1:
        raise SatelliteTleError(f"catalog {catalog_id}: TLE epoch is more than one day in the future")
    if not math.isfinite(max_age_days) or max_age_days <= 0:
        raise SatelliteTleError("maximum TLE age must be positive")
    return {"catalog_id": catalog_id, "name": name, "line1": line1, "line2": line2,
            "epoch": epoch, "age_days": age_days, "stale": age_days > max_age_days}


def validate_records(records, expected_ids=None, now=None, max_age_days=DEFAULT_MAX_AGE_DAYS):
    if not isinstance(records, list) or not records:
        raise SatelliteTleError("bundle must contain at least one TLE record")
    validated = []
    seen = set()
    for record in records:
        item = validate_record(record, now, max_age_days)
        if item["catalog_id"] in seen:
            raise SatelliteTleError(f"duplicate catalog ID: {item['catalog_id']}")
        seen.add(item["catalog_id"])
        validated.append(item)
    if expected_ids is not None and seen != set(expected_ids):
        missing = sorted(set(expected_ids) - seen)
        extra = sorted(seen - set(expected_ids))
        raise SatelliteTleError(f"provider ID set mismatch (missing={missing}, unexpected={extra})")
    return sorted(validated, key=lambda item: item["catalog_id"])


def _unique_json_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise SatelliteTleError(f"duplicate JSON member: {key}")
        result[key] = value
    return result


def load_bundle(path):
    try:
        with open(path, "rb") as source:
            raw = source.read(MAX_BUNDLE_BYTES + 1)
        if len(raw) > MAX_BUNDLE_BYTES:
            raise SatelliteTleError("bundle exceeds the 1 MB safety limit")
        bundle = json.loads(raw.decode("utf-8"), object_pairs_hook=_unique_json_object)
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise SatelliteTleError(f"cannot read bundle {path}: {exc}") from exc
    if not isinstance(bundle, dict) or set(bundle) not in (
            {"schema_version", "provider", "fetched_at", "records"},
            {"schema_version", "provider", "fetched_at", "records", "profile_ids"},
            {"schema_version", "provider", "fetched_at", "records", "profile_ids", "profiles"}):
        raise SatelliteTleError("bundle must contain schema_version, provider, fetched_at, records, and matching profile data")
    if type(bundle["schema_version"]) is not int or bundle["schema_version"] not in SUPPORTED_SCHEMA_VERSIONS:
        raise SatelliteTleError(f"unsupported bundle schema version: {bundle['schema_version']!r}")
    if bundle["schema_version"] == 1 and "profile_ids" in bundle or \
            bundle["schema_version"] == 2 and ("profile_ids" not in bundle or "profiles" in bundle) or \
            bundle["schema_version"] == 3 and not {"profile_ids", "profiles"} <= set(bundle):
        raise SatelliteTleError("bundle schema version and profile data do not match")
    if bundle["schema_version"] == 2:
        bundle["profile_ids"] = validate_profile_ids(bundle["profile_ids"])
    if bundle["schema_version"] == 3:
        bundle["profiles"] = validate_frequency_profiles(bundle["profiles"])
        ids = [profile["catalog_id"] for profile in bundle["profiles"]]
        bundle["profile_ids"] = validate_catalog_profile_ids(bundle["profile_ids"])
        if bundle["profile_ids"] != ids:
            raise SatelliteTleError("schema-v3 profile_ids must exactly match frequency profiles")
    if not isinstance(bundle["provider"], str):
        raise SatelliteTleError("bundle provider must be text")
    _https_url(bundle["provider"])
    try:
        fetched_at = dt.datetime.fromisoformat(bundle["fetched_at"].replace("Z", "+00:00"))
    except (AttributeError, TypeError, ValueError) as exc:
        raise SatelliteTleError("bundle fetched_at must be an ISO-8601 timestamp") from exc
    if fetched_at.tzinfo is None:
        raise SatelliteTleError("bundle fetched_at must include a UTC offset")
    if fetched_at.utcoffset() != dt.timedelta(0):
        raise SatelliteTleError("bundle fetched_at must be UTC")
    if not isinstance(bundle["records"], list):
        raise SatelliteTleError("bundle records must be an array")
    return bundle


def validate_profile_ids(profile_ids):
    if not isinstance(profile_ids, list) or not profile_ids or len(profile_ids) > MAX_PROFILES:
        raise SatelliteTleError(f"profile_ids must contain 1..{MAX_PROFILES} catalog IDs")
    if any(type(catalog_id) is not int or catalog_id not in SUPPORTED_PROFILE_IDS for catalog_id in profile_ids):
        raise SatelliteTleError("profile_ids contains an unsupported frequency profile ID")
    if len(set(profile_ids)) != len(profile_ids):
        raise SatelliteTleError("profile_ids contains duplicate IDs")
    return sorted(profile_ids)


def validate_catalog_profile_ids(profile_ids):
    if not isinstance(profile_ids, list) or not profile_ids or len(profile_ids) > MAX_PROFILES:
        raise SatelliteTleError(f"profile_ids must contain 1..{MAX_PROFILES} catalog IDs")
    if any(type(catalog_id) is not int or not 1 <= catalog_id <= 99999 for catalog_id in profile_ids):
        raise SatelliteTleError("profile_ids contains an invalid catalog ID")
    if len(set(profile_ids)) != len(profile_ids):
        raise SatelliteTleError("profile_ids contains duplicate IDs")
    return sorted(profile_ids)


def validate_frequency_profiles(profiles):
    required = {"catalog_id", "rx_hz", "tx_hz", "tx_tone_tenths"}
    if not isinstance(profiles, list) or not profiles or len(profiles) > MAX_PROFILES:
        raise SatelliteTleError(f"profiles must contain 1..{MAX_PROFILES} frequency records")
    validated = []
    for profile in profiles:
        if not isinstance(profile, dict) or set(profile) != required:
            raise SatelliteTleError("each frequency profile must contain catalog_id, rx_hz, tx_hz, and tx_tone_tenths")
        if type(profile["catalog_id"]) is not int or not 1 <= profile["catalog_id"] <= 99999 or \
                type(profile["rx_hz"]) is not int or not 0 < profile["rx_hz"] < 1_000_000_000 or \
                type(profile["tx_hz"]) is not int or not 0 < profile["tx_hz"] < 1_000_000_000 or \
                type(profile["tx_tone_tenths"]) is not int or \
                not (profile["tx_tone_tenths"] == 0 or 600 <= profile["tx_tone_tenths"] <= 2600):
            raise SatelliteTleError("frequency profile contains an out-of-range value")
        validated.append(dict(profile))
    ids = [profile["catalog_id"] for profile in validated]
    if len(set(ids)) != len(ids):
        raise SatelliteTleError("frequency profiles contain duplicate catalog IDs")
    return sorted(validated, key=lambda profile: profile["catalog_id"])


def parse_profile_ids(values):
    profile_ids = []
    for value in values:
        for item in value.split(","):
            try:
                profile_ids.append(int(item, 10))
            except ValueError as exc:
                raise SatelliteTleError(f"invalid frequency profile ID {item!r}") from exc
    return validate_profile_ids(profile_ids)


def make_bundle(provider, records, fetched_at=None, profile_ids=None, profiles=None):
    fetched_at = _utc_now() if fetched_at is None else fetched_at
    if fetched_at.tzinfo is None or fetched_at.utcoffset() != dt.timedelta(0):
        raise SatelliteTleError("bundle timestamp must be timezone-aware UTC")
    _https_url(provider)
    plain_records = [{key: item[key] for key in ("catalog_id", "name", "line1", "line2")} for item in records]
    schema_version = 3 if profiles is not None else 2 if profile_ids is not None else 1
    bundle = {"schema_version": schema_version, "provider": provider,
          "fetched_at": fetched_at.astimezone(UTC).isoformat(timespec="seconds").replace("+00:00", "Z"),
          "records": plain_records}
    if profiles is not None:
        bundle["profiles"] = validate_frequency_profiles(profiles)
        frequency_ids = [profile["catalog_id"] for profile in bundle["profiles"]]
        if profile_ids is not None and validate_catalog_profile_ids(profile_ids) != frequency_ids:
            raise SatelliteTleError("profile_ids must exactly match frequency profiles")
        if {record["catalog_id"] for record in plain_records} != set(frequency_ids):
            raise SatelliteTleError("bundle TLE records must exactly match frequency profiles")
        bundle["profile_ids"] = frequency_ids
    elif profile_ids is not None:
        bundle["profile_ids"] = validate_profile_ids(profile_ids)
    return bundle


def save_bundle(path, bundle):
    directory = os.path.dirname(os.path.abspath(path))
    os.makedirs(directory, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".satellite-tle-", suffix=".json", dir=directory)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as target:
            json.dump(bundle, target, indent=2, ensure_ascii=False)
            target.write("\n")
            target.flush()
            os.fsync(target.fileno())
        os.replace(temporary, path)
    except Exception:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def default_state_path():
    if os.name == "nt" and os.environ.get("LOCALAPPDATA"):
        root = os.path.join(os.environ["LOCALAPPDATA"], "RadioFoundry")
    else:
        root = os.environ.get("XDG_CONFIG_HOME", os.path.join(os.path.expanduser("~"), ".config"))
        root = os.path.join(root, "radiofoundry")
    return os.path.join(root, "satellite-inventory.json")


def _radio_key(link):
    try:
        uid = link.chip().get("uid")
    except DebugLinkError:
        uid = None
    if isinstance(uid, (tuple, list)) and len(uid) == 3 and all(isinstance(word, int) for word in uid):
        return "uid:" + "".join(f"{word:08X}" for word in uid)
    return "port:" + str(link.port)


def _load_inventory_cache(path):
    try:
        with open(path, "r", encoding="utf-8") as source:
            data = json.load(source, object_pairs_hook=_unique_json_object)
    except FileNotFoundError:
        return {"schema_version": 1, "radios": {}}
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise SatelliteTleError(f"cannot read local satellite inventory cache {path}: {exc}") from exc
    if not isinstance(data, dict) or type(data.get("schema_version")) is not int or \
            data["schema_version"] != 1 or not isinstance(data.get("radios"), dict):
        raise SatelliteTleError("local satellite inventory cache has an unsupported format")
    for radio_key, radio in data["radios"].items():
        if not isinstance(radio_key, str) or not isinstance(radio, dict) or not isinstance(radio.get("records"), list):
            raise SatelliteTleError("local satellite inventory cache has a malformed radio entry")
        for record in radio["records"]:
            if not isinstance(record, dict) or isinstance(record.get("catalog_id"), bool) or \
                    not isinstance(record.get("catalog_id"), int) or \
                    not 1 <= record["catalog_id"] <= 99999 or isinstance(record.get("epoch"), bool) or \
                    not isinstance(record.get("epoch"), int) or not isinstance(record.get("name"), str):
                raise SatelliteTleError("local satellite inventory cache has a malformed record")
    return data


def remember_inventory(path, key, entries, records=()):
    cache = _load_inventory_cache(path)
    old_entries = cache["radios"].get(key, {}).get("records", [])
    names = {row.get("catalog_id"): row.get("name", "") for row in old_entries if isinstance(row, dict)}
    epochs = {row.get("catalog_id"): row.get("epoch") for row in old_entries if isinstance(row, dict)}
    for record in records:
        catalog_id = record["catalog_id"]
        names[catalog_id] = record["name"]
        epochs[catalog_id] = int(record["epoch"].timestamp())
    saved = []
    for catalog_id, epoch in entries:
        name = names.get(catalog_id, "") if epochs.get(catalog_id) == epoch else ""
        saved.append({"catalog_id": catalog_id, "epoch": epoch, "name": name})
    cache["radios"][key] = {"read_at": _utc_now().isoformat(timespec="seconds").replace("+00:00", "Z"),
                             "records": saved}
    save_bundle(path, cache)
    return saved


def radio_inventory(link):
    info = _request(link, OP_INFO)
    if len(info) != 5:
        raise SatelliteTleError("radio returned malformed satellite capability data")
    version, capacity, installed_count = struct.unpack("<BHH", info)
    if version not in (LEGACY_SATELLITE_PROTOCOL_VERSION, PROFILE_PROTOCOL_MIN_VERSION,
                       REPLACE_PROTOCOL_MIN_VERSION, PROFILE_VALUES_PROTOCOL_VERSION,
                       PROFILE_CHUNK_PROTOCOL_VERSION, SATELLITE_PROTOCOL_VERSION) or \
            capacity == 0 or installed_count > capacity:
        raise SatelliteTleError("radio satellite capability is unsupported or invalid")
    entries = list_installed(link, capacity, installed_count)
    if len(entries) != installed_count or len({row[0] for row in entries}) != installed_count:
        raise SatelliteTleError("radio satellite inventory does not match its capability report")
    return capacity, entries


def _parse_provider_text(text, strict=False):
    lines = [line.rstrip("\r") for line in text.lstrip("\ufeff").splitlines()]
    records = []
    i = 0
    while i < len(lines):
        line = lines[i]
        if not line.strip():
            i += 1
            continue
        if line.startswith("2 "):
            raise SatelliteTleError("provider response contains an unpaired TLE line 2")
        if not line.startswith("1 "):
            if strict:
                if i + 2 >= len(lines) or not lines[i + 1].startswith("1 ") or not lines[i + 2].startswith("2 "):
                    raise SatelliteTleError("provider response is not a complete name, line 1, line 2 sequence")
                if not lines[i + 1][2:7].isdigit():
                    raise SatelliteTleError("provider response contains an invalid catalog ID")
                name = line[2:].strip() if line.startswith("0 ") else line.strip()
                records.append({"catalog_id": int(lines[i + 1][2:7]), "name": name,
                                "line1": lines[i + 1], "line2": lines[i + 2]})
                i += 3
                continue
            i += 1
            continue
        if strict:
            raise SatelliteTleError("provider response contains a TLE pair without its satellite name")
        if not line[2:7].isdigit() or i + 1 >= len(lines) or not lines[i + 1].startswith("2 "):
            raise SatelliteTleError("provider response contains an incomplete TLE pair")
        name_index = i - 1
        while name_index >= 0 and not lines[name_index].strip():
            name_index -= 1
        if name_index < 0:
            raise SatelliteTleError("provider TLE entry is missing its satellite name")
        name_line = lines[name_index].strip()
        name = name_line[2:].strip() if name_line.startswith("0 ") else name_line
        records.append({"catalog_id": int(line[2:7]), "name": name,
                        "line1": line, "line2": lines[i + 1]})
        i += 2
    if not records:
        raise SatelliteTleError("provider response contains no TLE records")
    return records


def fetch_catalog(provider_url, now=None, max_age_days=DEFAULT_MAX_AGE_DAYS, opener=None):
    parsed = _https_url(provider_url)
    request = urllib.request.Request(provider_url, headers={"User-Agent": "RadioFoundry satellite TLE utility"})
    try:
        with _open_provider(request, 20, opener) as response:
            if urllib.parse.urlsplit(response.geturl()).scheme.lower() != "https":
                raise SatelliteTleError("provider redirected away from HTTPS")
            body = response.read(MAX_TLE_RESPONSE_BYTES + 1)
    except SatelliteTleError:
        raise
    except Exception as exc:
        raise SatelliteTleError(f"TLE provider request failed: {exc}") from exc
    if len(body) > MAX_TLE_RESPONSE_BYTES:
        raise SatelliteTleError("provider response exceeds the 1 MB safety limit")
    try:
        text = body.decode("ascii")
    except UnicodeDecodeError as exc:
        raise SatelliteTleError("provider response is not ASCII TLE text") from exc
    records = _parse_provider_text(text)
    return validate_records(records, now=now, max_age_days=max_age_days)


def select_catalog(records, values):
    selected_ids = set(parse_catalog_ids(values))
    by_id = {record["catalog_id"]: record for record in records}
    missing = selected_ids - set(by_id)
    if missing:
        raise SatelliteTleError("catalog does not contain selected ID(s): " + ", ".join(map(str, sorted(missing))))
    return [by_id[catalog_id] for catalog_id in sorted(selected_ids)]


def fetch_records(catalog_ids, provider_url=DEFAULT_PROVIDER_URL, now=None,
                  max_age_days=DEFAULT_MAX_AGE_DAYS, opener=None):
    ids = parse_catalog_ids([str(i) for i in catalog_ids])
    parsed = _https_url(provider_url)
    query = urllib.parse.parse_qsl(parsed.query, keep_blank_values=True)
    query = [(key, value) for key, value in query if key.upper() not in {"CATNR", "FORMAT"}]
    query.extend([("CATNR", ",".join(str(i) for i in ids)), ("FORMAT", "TLE")])
    url = urllib.parse.urlunsplit((parsed.scheme, parsed.netloc, parsed.path, urllib.parse.urlencode(query), ""))
    request = urllib.request.Request(url, headers={"User-Agent": "RadioFoundry satellite TLE utility"})
    try:
        with _open_provider(request, 20, opener) as response:
            if urllib.parse.urlsplit(response.geturl()).scheme.lower() != "https":
                raise SatelliteTleError("provider redirected away from HTTPS")
            body = response.read(MAX_TLE_RESPONSE_BYTES + 1)
    except SatelliteTleError:
        raise
    except Exception as exc:
        raise SatelliteTleError(f"TLE provider request failed: {exc}") from exc
    if len(body) > MAX_TLE_RESPONSE_BYTES:
        raise SatelliteTleError("provider response exceeds the 1 MB safety limit")
    try:
        text = body.decode("ascii")
    except UnicodeDecodeError as exc:
        raise SatelliteTleError("provider response is not ASCII TLE text") from exc
    records = _parse_provider_text(text)
    validated = validate_records(records, ids, now, max_age_days)
    return validated


def _request(link, operation, payload=b""):
    _, reply = link.request(SATELLITE_COMMAND, bytes([operation]) + payload)
    return reply


def radio_profile_ids(link, protocol_version=None):
    if protocol_version is None:
        info = _request(link, OP_INFO)
        if len(info) != 5:
            raise SatelliteTleError("radio returned malformed satellite capability data")
        protocol_version = struct.unpack("<BHH", info)[0]
    if protocol_version < PROFILE_PROTOCOL_MIN_VERSION:
        raise SatelliteTleError("radio profile updates require satellite protocol version 3 or newer")
    reply = _request(link, OP_PROFILE_GET)
    if not reply or len(reply) != 1 + reply[0] * 4 or reply[0] > MAX_PROFILES:
        raise SatelliteTleError("radio returned a malformed frequency profile list")
    profile_ids = list(struct.unpack_from(f"<{reply[0]}I", reply, 1)) if reply[0] else []
    return validate_catalog_profile_ids(profile_ids)


def set_radio_profile_ids(link, profile_ids, protocol_version=None):
    profile_ids = validate_profile_ids(profile_ids)
    if protocol_version is None:
        info = _request(link, OP_INFO)
        if len(info) != 5:
            raise SatelliteTleError("radio returned malformed satellite capability data")
        protocol_version = struct.unpack("<BHH", info)[0]
    if protocol_version < PROFILE_PROTOCOL_MIN_VERSION:
        raise SatelliteTleError("radio profile updates require satellite protocol version 3 or newer")
    payload = bytes([len(profile_ids)]) + struct.pack(f"<{len(profile_ids)}I", *profile_ids)
    if _request(link, OP_PROFILE_SET, payload):
        raise SatelliteTleError("radio returned unexpected data after profile update")
    actual = radio_profile_ids(link, protocol_version)
    if actual != profile_ids:
        raise SatelliteTleError("radio frequency profile list did not match the requested selection")
    return actual


def stage_radio_frequency_profiles(link, transaction, profiles, protocol_version=SATELLITE_PROTOCOL_VERSION):
    profiles = validate_frequency_profiles(profiles)
    if protocol_version >= PROFILE_CHUNK_PROTOCOL_VERSION:
        for start in range(0, len(profiles), 4):
            chunk = profiles[start:start + 4]
            payload = bytearray(struct.pack("<HBBB", transaction, len(profiles), start, len(chunk)))
            for profile in chunk:
                payload.extend(struct.pack("<IIIH", profile["catalog_id"], profile["rx_hz"], profile["tx_hz"],
                                           profile["tx_tone_tenths"]))
            if _request(link, OP_PROFILE_STAGE_CHUNK, payload):
                raise SatelliteTleError("radio returned unexpected data after staging frequency profiles")
    else:
        if len(profiles) > 4:
            raise SatelliteTleError("more than four frequency profiles require satellite protocol version 6")
        payload = bytearray(struct.pack("<HB", transaction, len(profiles)))
        for profile in profiles:
            payload.extend(struct.pack("<IIIH", profile["catalog_id"], profile["rx_hz"], profile["tx_hz"],
                                       profile["tx_tone_tenths"]))
        if _request(link, OP_PROFILE_STAGE, payload):
            raise SatelliteTleError("radio returned unexpected data after staging frequency profiles")


def radio_frequency_profiles(link):
    reply = _request(link, OP_PROFILE_VALUES_GET)
    if not reply or reply[0] > MAX_PROFILES or len(reply) != 1 + reply[0] * 14:
        raise SatelliteTleError("radio returned a malformed frequency profile table")
    profiles = []
    for index in range(reply[0]):
        catalog_id, rx_hz, tx_hz, tone = struct.unpack_from("<IIIH", reply, 1 + index * 14)
        profiles.append({"catalog_id": catalog_id, "rx_hz": rx_hz, "tx_hz": tx_hz,
                         "tx_tone_tenths": tone})
    return validate_frequency_profiles(profiles)


def list_installed(link, capacity, expected_count):
    entries = []
    cursor = 0
    while cursor < expected_count:
        reply = _request(link, OP_LIST, struct.pack("<H", cursor))
        if not reply:
            raise SatelliteTleError("radio returned an empty satellite list page")
        count = reply[0]
        if len(reply) != 1 + count * 8 or count == 0 or cursor + count > expected_count:
            raise SatelliteTleError("radio returned a malformed or incomplete satellite list page")
        for offset in range(count):
            catalog_id, epoch = struct.unpack_from("<II", reply, 1 + offset * 8)
            entries.append((catalog_id, epoch))
        cursor += count
    return entries


def _serialize_records(records):
    stream = bytearray()
    for item in sorted(records, key=lambda row: row["catalog_id"]):
        name = item["name"].encode("utf-8")
        stream.extend(struct.pack("<IB", item["catalog_id"], len(name)))
        stream.extend(name)
        stream.extend(item["line1"].encode("ascii"))
        stream.extend(item["line2"].encode("ascii"))
    if not stream or len(stream) > 0xFFFF:
        raise SatelliteTleError("serialized satellite batch must be between 1 and 65535 bytes")
    return bytes(stream)


def install_records(link, records, profile_ids=None, profiles=None):
    allowed = {"catalog_id", "name", "line1", "line2", "epoch", "age_days", "stale"}
    if any(not isinstance(item, dict) or set(item) - allowed or
           not {"catalog_id", "name", "line1", "line2"} <= set(item) for item in records):
        raise SatelliteTleError("install records may contain orbital fields only")
    plain_records = [{key: item[key] for key in ("catalog_id", "name", "line1", "line2")} for item in records]
    validated = validate_records(plain_records)
    if len(validated) > MAX_PROFILES:
        raise SatelliteTleError(f"at most {MAX_PROFILES} TLE records can be installed")
    selected_ids = None if profile_ids is None else validate_catalog_profile_ids(profile_ids)
    full_profiles = None if profiles is None else validate_frequency_profiles(profiles)
    if full_profiles is not None:
        full_ids = [profile["catalog_id"] for profile in full_profiles]
        if selected_ids is not None and selected_ids != full_ids:
            raise SatelliteTleError("frequency profiles must exactly match profile_ids")
        selected_ids = full_ids
    try:
        info = _request(link, OP_INFO)
    except DebugLinkError as exc:
        if exc.status in (1, 4):
            raise SatelliteTleError("radio does not advertise satellite-data support; no raw flash fallback is used") from exc
        raise
    if len(info) != 5:
        raise SatelliteTleError("radio returned malformed satellite capability data")
    version, capacity, installed_count = struct.unpack("<BHH", info)
    if version not in (LEGACY_SATELLITE_PROTOCOL_VERSION, PROFILE_PROTOCOL_MIN_VERSION,
                       REPLACE_PROTOCOL_MIN_VERSION, PROFILE_VALUES_PROTOCOL_VERSION,
                       PROFILE_CHUNK_PROTOCOL_VERSION, SATELLITE_PROTOCOL_VERSION) or \
            capacity == 0 or installed_count > capacity:
        raise SatelliteTleError("radio satellite capability is unsupported or invalid")
    if version < REPLACE_PROTOCOL_MIN_VERSION:
        raise SatelliteTleError("replacing the selected TLE set requires satellite protocol version 4")
    if full_profiles is not None and version < PROFILE_VALUES_PROTOCOL_VERSION:
        raise SatelliteTleError("frequency-valued bundles require satellite protocol version 5")
    if full_profiles is not None and len(full_profiles) > 4 and version < PROFILE_CHUNK_PROTOCOL_VERSION:
        raise SatelliteTleError("more than four frequency profiles require satellite protocol version 6")
    if selected_ids is not None and {item["catalog_id"] for item in validated} != set(selected_ids):
        raise SatelliteTleError("replacement bundle TLE records must exactly match its selected profile IDs")
    before = list_installed(link, capacity, installed_count)
    if len(before) != installed_count or len({row[0] for row in before}) != len(before):
        raise SatelliteTleError("radio satellite inventory does not match its capability report")

    record_ids = {item["catalog_id"] for item in validated}
    expected_count = len(record_ids)
    if expected_count > capacity:
        raise SatelliteTleError(f"radio capacity {capacity} cannot hold {expected_count} selected records")
    stream = _serialize_records(validated)
    checksum = zlib.crc32(stream) & 0xFFFFFFFF
    begin = _request(link, OP_REPLACE_BEGIN, struct.pack("<HHI", len(validated), len(stream), checksum))
    if len(begin) != 2:
        raise SatelliteTleError("radio returned malformed satellite transaction ID")
    transaction = struct.unpack("<H", begin)[0]
    committed = False
    try:
        for offset in range(0, len(stream), DATA_CHUNK_MAX):
            chunk = stream[offset:offset + DATA_CHUNK_MAX]
            next_offset = _request(link, OP_DATA, struct.pack("<HH", transaction, offset) + chunk)
            if len(next_offset) != 2 or struct.unpack("<H", next_offset)[0] != offset + len(chunk):
                raise SatelliteTleError("radio satellite transfer offset did not advance as expected")
        if full_profiles is not None:
            stage_radio_frequency_profiles(link, transaction, full_profiles, version)
        reply = _request(link, OP_COMMIT, struct.pack("<HI", transaction, checksum))
        if reply:
            raise SatelliteTleError("radio returned unexpected data after satellite commit")
        committed = True
    finally:
        if not committed:
            try:
                _request(link, OP_ABORT, struct.pack("<H", transaction))
            except Exception:
                pass

    if selected_ids is not None and full_profiles is None:
        try:
            set_radio_profile_ids(link, selected_ids, version)
        except (DebugLinkError, SatelliteTleError) as exc:
            raise SatelliteTleError("TLE inventory committed, but frequency profile selection was not verified: " +
                                    str(exc)) from exc

    after_capacity, after = radio_inventory(link)
    expected_inventory = {item["catalog_id"]: int(item["epoch"].timestamp()) for item in validated}
    if after_capacity != capacity or dict(after) != expected_inventory:
        raise SatelliteTleError("radio satellite inventory changed unexpectedly after commit")
    result = {"capacity": capacity, "count": len(after), "installed": sorted(record_ids), "inventory": after}
    if selected_ids is not None:
        result["profile_ids"] = selected_ids
    if full_profiles is not None:
        actual_profiles = radio_frequency_profiles(link)
        if actual_profiles != full_profiles:
            raise SatelliteTleError("radio frequency profile readback did not match the requested values")
        result["frequency_profiles"] = actual_profiles
    return result


def _stale_entries(records):
    return [item for item in records if item["stale"]]


def _accept_stale(records, accepted):
    stale = _stale_entries(records)
    if not stale or accepted:
        return
    print("Stale TLE elements:")
    for item in stale:
        print(f"  {item['catalog_id']} {item['name']}: {item['age_days']:.1f} days old")
    if not sys.stdin.isatty():
        raise SatelliteTleError("stale elements require --accept-stale in non-interactive use")
    if input("Use these stale elements? [y/N] ").strip().lower() not in {"y", "yes"}:
        raise SatelliteTleError("cancelled because the selected TLE elements are stale")


def _preview(records):
    for item in records:
        age = f", {item['age_days']:.1f} days old" if item["stale"] else ""
        epoch = item["epoch"].isoformat(timespec="seconds").replace("+00:00", "Z")
        print(f"{item['catalog_id']}  {item['name']}  epoch {epoch}{age}")


def _confirm_install(yes):
    if yes:
        return
    if not sys.stdin.isatty():
        raise SatelliteTleError("install requires an interactive confirmation or --yes")
    if input("Install exactly these satellite records? [y/N] ").strip().lower() not in {"y", "yes"}:
        raise SatelliteTleError("installation cancelled")


def _choose_catalog(records, supplied_ids):
    if supplied_ids:
        return select_catalog(records, supplied_ids)
    _preview(records)
    if not sys.stdin.isatty():
        raise SatelliteTleError("catalog sync requires explicit --id values when stdin is not interactive")
    entered = input("Catalog IDs to sync (comma-separated; blank cancels): ").strip()
    if not entered:
        raise SatelliteTleError("catalog selection cancelled")
    return select_catalog(records, [entered])


def _add_age_options(parser):
    parser.add_argument("--max-age-days", type=float, default=DEFAULT_MAX_AGE_DAYS)
    parser.add_argument("--accept-stale", action="store_true", help="explicitly allow TLEs older than the age limit")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="radio debug-link port (default: discovered RadioFoundry radio)")
    parser.add_argument("--timeout", type=float, default=1.0, help="debug-link response timeout in seconds")
    parser.add_argument("--state-file", default=default_state_path(), help="local per-radio remembered inventory path")
    commands = parser.add_subparsers(dest="command", required=True)

    fetch = commands.add_parser("fetch", help="fetch an explicit satellite ID set and write an offline bundle")
    fetch.add_argument("--id", dest="ids", action="append", required=True, help="catalog ID; repeat or comma-separate")
    fetch.add_argument("--provider-url", default=DEFAULT_PROVIDER_URL)
    fetch.add_argument("--output", required=True, help="bundle JSON path")
    fetch.add_argument("--profile-id", dest="profile_ids", action="append",
                       help="built-in frequency profile ID to select in the bundle; repeat or comma-separate")
    _add_age_options(fetch)

    validate = commands.add_parser("validate", help="validate an offline bundle without network access")
    validate.add_argument("bundle")
    _add_age_options(validate)

    install = commands.add_parser("install", help="install a validated bundle to a satellite-capable radio")
    install.add_argument("bundle")
    install.add_argument("--yes", action="store_true", help="confirm this explicit install without a prompt")
    _add_age_options(install)

    profiles = commands.add_parser("profiles", help="read or update built-in satellite frequency profiles")
    profiles.add_argument("--id", dest="profile_ids", action="append",
                          help="profile ID to select; repeat or comma-separate (up to 10)")
    profiles.add_argument("--yes", action="store_true", help="confirm profile changes without a prompt")

    catalog = commands.add_parser("catalog", help="browse the current AMSAT or ARISS catalog")
    catalog.add_argument("--source", choices=sorted(PROVIDER_URLS), default="amsat")
    catalog.add_argument("--provider-url", help="override the selected source's HTTPS URL")
    _add_age_options(catalog)

    sync = commands.add_parser("sync", help="choose satellite IDs from a provider catalog and sync only those records")
    sync.add_argument("--source", choices=sorted(PROVIDER_URLS), default="amsat")
    sync.add_argument("--provider-url", help="override the selected source's HTTPS URL")
    sync.add_argument("--id", dest="ids", action="append", default=[], help="catalog ID(s), repeated or comma-separated")
    sync.add_argument("--yes", action="store_true", help="confirm install without a prompt")
    _add_age_options(sync)

    commands.add_parser("read", help="read the radio's installed satellite list and remember it locally")

    args = parser.parse_args(argv)
    try:
        if args.command == "fetch":
            ids = parse_catalog_ids(args.ids)
            records = fetch_records(ids, args.provider_url, max_age_days=args.max_age_days)
            _preview(records)
            _accept_stale(records, args.accept_stale)
            profile_ids = parse_profile_ids(args.profile_ids) if args.profile_ids else None
            bundle = make_bundle(args.provider_url, records, profile_ids=profile_ids)
            save_bundle(args.output, bundle)
            print(f"saved {len(records)} satellite records to {args.output}")
            return 0

        if args.command == "catalog":
            provider_url = args.provider_url or PROVIDER_URLS[args.source]
            records = fetch_catalog(provider_url, max_age_days=args.max_age_days)
            _preview(records)
            print(f"{args.source} catalog: {len(records)} satellites; use sync with selected catalog IDs")
            return 0

        if args.command == "sync":
            provider_url = args.provider_url or PROVIDER_URLS[args.source]
            catalog_records = fetch_catalog(provider_url, max_age_days=args.max_age_days)
            records = _choose_catalog(catalog_records, args.ids)
            _preview(records)
            _accept_stale(records, args.accept_stale)
            _confirm_install(args.yes)
            with DebugLink(args.port, args.timeout) as link:
                result = install_records(link, records)
                remember_inventory(args.state_file, _radio_key(link), result["inventory"], records)
            print(f"synced {len(result['installed'])} selected records; radio now has {result['count']}/"
                  f"{result['capacity']} records, read-back verified")
            return 0

        if args.command == "read":
            with DebugLink(args.port, args.timeout) as link:
                key = _radio_key(link)
                capacity, entries = radio_inventory(link)
                saved = remember_inventory(args.state_file, key, entries)
            for item in saved:
                epoch = dt.datetime.fromtimestamp(item["epoch"], UTC).isoformat(timespec="seconds").replace("+00:00", "Z")
                print(f"{item['catalog_id']}  {item['name'] or '(name not cached)'}  epoch {epoch}")
            print(f"radio inventory: {len(saved)}/{capacity} satellites; remembered in {args.state_file}")
            return 0

        if args.command == "profiles":
            with DebugLink(args.port, args.timeout) as link:
                info = _request(link, OP_INFO)
                if len(info) != 5:
                    raise SatelliteTleError("radio returned malformed satellite capability data")
                version = struct.unpack("<BHH", info)[0]
                if args.profile_ids:
                    selected = parse_profile_ids(args.profile_ids)
                    _confirm_install(args.yes)
                    actual = set_radio_profile_ids(link, selected, version)
                else:
                    actual = radio_profile_ids(link, version)
            names = {25544: "ISS", 27607: "SO-50", 40908: "LILACSAT-2", 40931: "IO-86",
                     43017: "AO-91", 43678: "PO-101", 61781: "AO-123", 63213: "TEVEL2-4",
                     63214: "TEVEL2-5", 63215: "TEVEL2-6", 63217: "TEVEL2-1",
                     63218: "TEVEL2-3", 63219: "TEVEL2-2", 63237: "TEVEL2-9",
                     63238: "TEVEL2-7", 63239: "TEVEL2-8", 67291: "RS95S"}
            for catalog_id in actual:
                print(f"{names[catalog_id]} ({catalog_id})")
            print(f"frequency profiles: {len(actual)}/{MAX_PROFILES}; read-back verified")
            return 0

        bundle = load_bundle(args.bundle)
        records = validate_records(bundle["records"], now=_utc_now(), max_age_days=args.max_age_days)
        _preview(records)
        _accept_stale(records, args.accept_stale)
        if args.command == "validate":
            print(f"valid bundle: {len(records)} satellite records; no network used")
            return 0
        _confirm_install(args.yes)
        with DebugLink(args.port, args.timeout) as link:
            result = install_records(link, records, bundle.get("profile_ids"), bundle.get("profiles"))
        print(f"installed {len(result['installed'])} selected records; radio now has {result['count']}/"
              f"{result['capacity']} records, read-back verified")
        if "profile_ids" in result:
            print(f"replaced the radio TLE list with {len(records)} selected satellites and verified "
                  f"{len(result['profile_ids'])} selected frequency profiles")
        return 0
    except (SatelliteTleError, DebugLinkError, OSError) as exc:
        print(f"satellite_tle: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Fetch, validate and bundle satellite two-line elements (docs/spec/satellite.md 6)."""
import argparse
from datetime import datetime, timedelta, timezone
import json
import re
import sys
from urllib.error import URLError
from urllib.parse import urlparse
from urllib.request import Request, urlopen


LEGACY_SCHEMA_VERSION = 1
DEFAULT_PROVIDER = DEFAULT_PROVIDER_URL
DEFAULT_MAX_AGE_DAYS = 30
CATALOG_ID = re.compile(r"^[0-9]{1,5}$")


class TleError(ValueError):
    pass


def _number(text, label):
    try:
        value = float(text)
    except ValueError as exc:
        raise TleError("invalid %s" % label) from exc
    if not (value == value and abs(value) != float("inf")):
        raise TleError("invalid %s" % label)
    return value


def _legacy_checksum(line, number):
    if len(line) != 69 or not line[-1].isdigit():
        raise TleError("TLE line %d must be exactly 69 columns with a checksum" % number)
    value = sum(int(ch) for ch in line[:68] if ch.isdigit()) + line[:68].count("-")
    if value % 10 != int(line[-1]):
        raise TleError("TLE line %d checksum mismatch" % number)


def _legacy_epoch(line1):
    year_short = int(line1[18:20])
    year = 1900 + year_short if year_short >= 57 else 2000 + year_short
    day = _number(line1[20:32], "epoch day")
    limit = 366 if year % 4 == 0 and (year % 100 != 0 or year % 400 == 0) else 365
    if day < 1 or day >= limit + 1:
        raise TleError("epoch day is outside its year")
    return datetime(year, 1, 1, tzinfo=timezone.utc) + timedelta(days=day - 1)


def validate_pair(name, line1, line2):
    name = name.strip()
    line1, line2 = line1.rstrip("\r\n"), line2.rstrip("\r\n")
    if not name or len(line1) != 69 or len(line2) != 69:
        raise TleError("name and two complete 69-column TLE lines are required")
    if line1[0] != "1" or line2[0] != "2":
        raise TleError("TLE lines must begin with 1 and 2")
    _legacy_checksum(line1, 1)
    _legacy_checksum(line2, 2)
    catalog1, catalog2 = line1[2:7].strip(), line2[2:7].strip()
    if not CATALOG_ID.fullmatch(catalog1) or catalog1 != catalog2:
        raise TleError("TLE catalog IDs are invalid or do not match")
    if line1[7] not in "UCS" or not line1[18:32].strip():
        raise TleError("invalid TLE classification or epoch")
    epoch = _legacy_epoch(line1)
    inclination = _number(line2[8:16], "inclination")
    raan = _number(line2[17:25], "right ascension")
    eccentricity_text = line2[26:33]
    if not eccentricity_text.isdigit():
        raise TleError("invalid eccentricity")
    eccentricity = int(eccentricity_text) / 10000000.0
    arg_perigee = _number(line2[34:42], "argument of perigee")
    mean_anomaly = _number(line2[43:51], "mean anomaly")
    mean_motion = _number(line2[52:63], "mean motion")
    if not (0 <= inclination <= 180 and 0 <= raan < 360 and 0 <= eccentricity < 1 and
            0 <= arg_perigee < 360 and 0 <= mean_anomaly < 360 and 0 < mean_motion < 20):
        raise TleError("TLE orbital fields are outside supported ranges")
    return {"catalog_id": int(catalog1), "name": name, "line1": line1, "line2": line2,
            "epoch": epoch.isoformat().replace("+00:00", "Z")}


def parse_tle_text(text):
    """Parse named or unnamed 3-line TLE records; reject partial and duplicate records."""
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    records, index = [], 0
    while index < len(lines):
        name = lines[index]
        if name.startswith("1 "):
            name = ""
        else:
            index += 1
        if not name or index + 1 >= len(lines) or not lines[index].startswith("1 ") or not lines[index + 1].startswith("2 "):
            raise TleError("incomplete TLE record near line %d" % (index + 1))
        records.append(validate_pair(name, lines[index], lines[index + 1]))
        index += 2
    ids = [record["catalog_id"] for record in records]
    if len(set(ids)) != len(ids):
        raise TleError("duplicate catalog ID in provider response")
    return records


def _check_age(records, now, max_age_days, accept_stale):
    stale = []
    for record in records:
        epoch = datetime.fromisoformat(record["epoch"].replace("Z", "+00:00"))
        age = (now - epoch).total_seconds() / 86400.0
        if age < -1 or age > max_age_days:
            stale.append((record["catalog_id"], age))
    if stale and not accept_stale:
        ids = ", ".join("%d (%.1f days)" % (catalog_id, age) for catalog_id, age in stale)
        raise TleError("stale or future TLE epoch(s): %s; pass --accept-stale to explicitly accept" % ids)
    return stale


def _legacy_make_bundle(records, provider, fetched_at):
    return {"schema_version": LEGACY_SCHEMA_VERSION, "provider": provider, "fetched_at": fetched_at,
            "satellites": records}


def _legacy_load_bundle(path):
    try:
        with open(path, "r", encoding="utf-8") as source:
            bundle = json.load(source)
    except (OSError, json.JSONDecodeError) as exc:
        raise TleError("could not read bundle: %s" % exc) from exc
    if not isinstance(bundle, dict) or bundle.get("schema_version") != LEGACY_SCHEMA_VERSION or not isinstance(bundle.get("satellites"), list):
        raise TleError("unsupported or malformed bundle schema")
    records = []
    for item in bundle["satellites"]:
        if not isinstance(item, dict):
            raise TleError("malformed satellite record")
        record = validate_pair(item.get("name", ""), item.get("line1", ""), item.get("line2", ""))
        if item.get("catalog_id") != record["catalog_id"] or item.get("epoch") != record["epoch"]:
            raise TleError("bundle metadata does not match its TLE record")
        records.append(record)
    ids = [record["catalog_id"] for record in records]
    if len(set(ids)) != len(ids):
        raise TleError("duplicate catalog ID in bundle")
    return _legacy_make_bundle(records, str(bundle.get("provider", "")), str(bundle.get("fetched_at", "")))


def _fetch(url, timeout):
    parsed = urlparse(url)
    if parsed.scheme != "https" or not parsed.netloc:
        raise TleError("provider URL must use HTTPS")
    request = Request(url, headers={"User-Agent": "RadioFoundry satellite TLE tool"})
    try:
        with urlopen(request, timeout=timeout) as response:
            return response.read(1024 * 1024 + 1).decode("ascii")
    except (OSError, URLError, UnicodeDecodeError) as exc:
        raise TleError("provider fetch failed: %s" % exc) from exc


def _ids(values):
    result = set()
    for value in values:
        for token in value.split(","):
            if not CATALOG_ID.fullmatch(token.strip()):
                raise TleError("invalid catalog ID: %s" % token)
            result.add(int(token))
    if not result:
        raise TleError("select at least one catalog ID with --id")
    return result


def _legacy_main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    fetch = sub.add_parser("fetch", help="fetch selected TLEs from an HTTPS provider")
    fetch.add_argument("--url", default=DEFAULT_PROVIDER)
    fetch.add_argument("--id", action="append", default=[], help="catalog ID(s), comma-separated; never installs a whole provider group")
    fetch.add_argument("--output", required=True)
    fetch.add_argument("--max-age-days", type=float, default=DEFAULT_MAX_AGE_DAYS)
    fetch.add_argument("--accept-stale", action="store_true")
    fetch.add_argument("--timeout", type=float, default=15)
    validate = sub.add_parser("validate", help="validate an offline JSON TLE bundle")
    validate.add_argument("bundle")
    validate.add_argument("--max-age-days", type=float, default=DEFAULT_MAX_AGE_DAYS)
    validate.add_argument("--accept-stale", action="store_true")
    args = parser.parse_args(argv)
    try:
        now = datetime.now(timezone.utc)
        if args.command == "fetch":
            selected = _ids(args.id)
            text = _fetch(args.url, args.timeout)
            records = parse_tle_text(text)
            found = {record["catalog_id"] for record in records}
            missing = selected - found
            if missing:
                raise TleError("provider did not return selected catalog ID(s): %s" % ", ".join(map(str, sorted(missing))))
            records = [record for record in records if record["catalog_id"] in selected]
            _check_age(records, now, args.max_age_days, args.accept_stale)
            bundle = _legacy_make_bundle(records, args.url, now.isoformat().replace("+00:00", "Z"))
            with open(args.output, "w", encoding="utf-8", newline="\n") as output:
                json.dump(bundle, output, indent=2)
                output.write("\n")
        else:
            bundle = _legacy_load_bundle(args.bundle)
            _check_age(bundle["satellites"], now, args.max_age_days, args.accept_stale)
            records = bundle["satellites"]
        for record in records:
            epoch = record["epoch"]
            print("%5d  %-24s epoch %s" % (record["catalog_id"], record["name"][:24], epoch))
        print("Validated %d satellite TLE(s)." % len(records))
        return 0
    except (TleError, OSError) as exc:
        print("satellite_tle: %s" % exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())