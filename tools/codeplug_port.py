#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""codeplug_port: the codeplug half of moving a radio to ODR, shared by the migration tools (md380_migrate.py,
dm32_migrate.py) so every radio converts, cleans and writes its codeplug the same way (docs/spec/codeplug-port.md).

    python3 tools/codeplug_port.py --self-test      the pure logic, no radio, no dmrconf

Each tool reads its stock codeplug with its own dmrconf driver into qdmr YAML. From there:

1. clean_group_lists: drop the RX group lists ODR doesn't need (empty, or only the channel's own TX contact: ODR
   always hears the talk target), lists no channel uses, and duplicates. Stock CPS habits leave many; qdmr's
   open-firmware encoder writes a channel's TX contact or its group list, never both, so each one costs a contact.
2. dmrconf encode --radio openuv380, decode again (the radio tool runs dmrconf).
3. restore_contacts: put back each TX contact the encoder dropped, in the encoded image's channel records.
4. cps_write_dfu / check_read_back: write the image through ODR's CPS shim (tools/cps_client.py), with what was there
   saved first, then read it back and compare. dmrconf's own write reads the radio first and fails against ODR.

Pure logic raises PortError; the tools catch it like their own errors.
"""
import os
import re
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)


class PortError(Exception):
    pass


# What ODR's codeplug layout holds (docs/spec/open-firmware-cps.md 7.2-7.3, docs/user/15-programming-qdmr.md).
# dmrconf's OpenUV380 encoder drops what doesn't fit without an error, so these are checked before writing
ODR_LIMITS = {"channels": 1024, "zones": 68, "contacts": 1024, "groupLists": 76}
SECTION_NAMES = {"channels": "channels", "zones": "zones", "contacts": "contacts", "groupLists": "RX group lists"}

# The open-firmware channel record's TX contact: 16-bit little endian, 1-based, 0 = none (open-firmware-cps.md 7.2)
CH_CONTACT = 0x2E


# ---------------------------------------------------------------------------------------------------------------
# qdmr YAML

def yaml_sections(text):
    """Split qdmr's block-style YAML into {top-level key: [item text, ...]} for list sections ('  - ' items at indent
    2) and {key: ''} for the others. Enough for counting and naming: qdmr writes its files in this regular form."""
    out, key, items = {}, None, None
    for line in text.splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        m = re.match(r"^([A-Za-z_][\w]*):(.*)$", line)
        if m:
            key = m.group(1)
            rest = m.group(2).strip()
            items = out.setdefault(key, [])
            if rest.startswith("[") and rest.endswith("]") and rest[1:-1].strip():
                items.extend(x.strip() for x in rest[1:-1].split(","))
            continue
        if key is None:
            continue
        if re.match(r"^  - ", line) or re.match(r"^- ", line):
            items.append(line.split("- ", 1)[1] + "\n")
        elif items:
            items[-1] += line.strip() + "\n"
    return out


def item_field(item, field):
    """The value of `field:` in one YAML item (inline {...} or block form), or None."""
    m = re.search(r"(?:^|[{,\s])" + re.escape(field) + r":\s*(\"[^\"]*\"|'[^']*'|[^,}\n]*)", item)
    if not m:
        return None
    v = m.group(1).strip()
    if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
        v = v[1:-1]
    return v


def item_list(item, field):
    """The flow list `field: [a, b]` of one YAML item, or None."""
    m = re.search(r"(?:^|[{,\s])" + re.escape(field) + r":\s*\[([^\]]*)\]", item)
    return None if m is None else [x.strip() for x in m.group(1).split(",") if x.strip()]


def codeplug_summary(text):
    """Counts and names of what ODR uses, plus the radio's own DMR ID(s), from qdmr YAML."""
    sec = yaml_sections(text)
    s = {"counts": {}, "names": {}, "radio_ids": []}
    for key in ODR_LIMITS:
        items = sec.get(key) or []
        s["counts"][key] = len(items)
        s["names"][key] = [item_field(i, "name") or "?" for i in items]
    for item in sec.get("radioIDs") or []:
        n = item_field(item, "number")
        if n and n.isdigit():
            s["radio_ids"].append(int(n))
    return s


def limit_report(summary, limits=ODR_LIMITS):
    """[(key, count, limit, [dropped names])] for every section that doesn't fit ODR."""
    out = []
    for key, lim in limits.items():
        n = summary["counts"].get(key, 0)
        if n > lim:
            out.append((key, n, lim, summary["names"][key][lim:]))
    return out


def compare_counts(expected, got):
    """[(key, expected, got)] where a read back differs from what was written (after ODR's limits)."""
    return [(k, min(expected.get(k, 0), lim), got.get(k, 0)) for k, lim in ODR_LIMITS.items()
            if min(expected.get(k, 0), lim) != got.get(k, 0)]


def print_summary(sess, title, summary):
    c = summary["counts"]
    sess.say("   %s: %d channels, %d zones, %d contacts, %d RX group lists; radio ID %s"
             % (title, c["channels"], c["zones"], c["contacts"], c["groupLists"],
                ", ".join(map(str, summary["radio_ids"])) or "none"))


def _section_items(lines, key):
    """(header line index, [(first line, end line)]) of the '  - ' items of top-level section key; (None, []) if
    the section is missing."""
    head, items = None, []
    for i, line in enumerate(lines):
        if head is None:
            if re.match(r"^" + re.escape(key) + r":", line):
                head = i
            continue
        if re.match(r"^\S", line):
            break
        if line.startswith("  - "):
            items.append([i, None])
    end = len(lines)
    if head is not None:
        for i in range(head + 1, len(lines)):
            if re.match(r"^\S", lines[i]):
                end = i
                break
    for k, it in enumerate(items):
        it[1] = items[k + 1][0] if k + 1 < len(items) else end
    return head, [tuple(x) for x in items]


def channels(text):
    """[{index, kind, name, contact, groupList}] of the channels, in order (index 0-based)."""
    lines = text.split("\n")
    _, items = _section_items(lines, "channels")
    out = []
    for n, (a, b) in enumerate(items):
        body = "\n".join(lines[a:b])
        kind = re.match(r"^  - (\w+):", lines[a])
        f = {}
        for fld in ("name", "contact", "groupList"):
            m = re.search(r"(?m)^      " + fld + r": (.*)$", body)
            f[fld] = m.group(1).strip().strip("\"'") if m else None
        out.append({"index": n, "kind": kind.group(1) if kind else "?", "name": f["name"], "contact": f["contact"],
                    "groupList": f["groupList"]})
    return out


def contacts(text):
    """[(id, name, number)] of the contacts, in order."""
    out = []
    for item in yaml_sections(text).get("contacts") or []:
        out.append((item_field(item, "id"), item_field(item, "name"), item_field(item, "number")))
    return out


# ---------------------------------------------------------------------------------------------------------------
# step 1: RX group list clean-up

def clean_group_lists(text):
    """(cleaned text, report) for qdmr YAML. A DMR channel loses its group list when the list is empty or holds only
    the channel's own TX contact; identical lists merge into the first; lists no channel uses go. Everything else is
    left as written, so the same file decodes to the same channels, contacts and zones."""
    lines = text.split("\n")
    head, gitems = _section_items(lines, "groupLists")
    members, order = {}, []
    for a, b in gitems:
        item = "\n".join(lines[a:b])
        gid = item_field(item, "id")
        if gid:
            members[gid] = tuple(item_list(item, "contacts") or [])
            order.append(gid)
    canon, first = {}, {}
    for gid in order:
        canon[gid] = first.setdefault(members[gid], gid)
    rep = {"lists_before": len(order), "dropped_own": [], "dropped_empty": [], "kept": [], "merged": 0,
           "lists_after": 0}
    _, citems = _section_items(lines, "channels")
    drop, used = set(), set()
    for a, b in citems:
        if not lines[a].startswith("  - dmr:"):
            continue
        name = contact = gline = gid = None
        for i in range(a, b):
            m = re.match(r"^      (name|contact|groupList): (.*)$", lines[i])
            if not m:
                continue
            v = m.group(2).strip().strip("\"'")
            if m.group(1) == "name":
                name = v
            elif m.group(1) == "contact":
                contact = v
            else:
                gid, gline = v, i
        if gid is None:
            continue
        mem = members.get(gid)
        if mem is not None and not mem:
            drop.add(gline)
            rep["dropped_empty"].append(name)
        elif mem is not None and contact is not None and mem == (contact,):
            drop.add(gline)
            rep["dropped_own"].append(name)
        else:
            c = canon.get(gid, gid)
            if c != gid:
                lines[gline] = "      groupList: " + c
                rep["merged"] += 1
            used.add(c)
            rep["kept"].append(name)
    for a, b in gitems:
        item = "\n".join(lines[a:b])
        if item_field(item, "id") not in used:
            drop.update(range(a, b))
    rep["lists_after"] = len(used)
    out = [l for i, l in enumerate(lines) if i not in drop]
    if head is not None and not used and re.match(r"^groupLists:\s*$", lines[head]):
        out[out.index(lines[head])] = "groupLists: []"
    return "\n".join(out), rep


def format_cleanup(rep):
    return ("RX group lists %d -> %d; list dropped on %d channels (only their own TX contact) and %d (empty); %d keep "
            "a list%s" % (rep["lists_before"], rep["lists_after"], len(rep["dropped_own"]), len(rep["dropped_empty"]),
                          len(rep["kept"]), "" if not rep["merged"] else ", %d moved to an identical list"
                          % rep["merged"]))


# ---------------------------------------------------------------------------------------------------------------
# step 3: TX contacts the encoder dropped

def restore_contacts(src_text, odr_text, img, channel_addr, decode_channel):
    """Put back in img (the encoded image, flat) the TX contact of every channel that has one in src_text and none
    in odr_text (qdmr's open-firmware encoder writes the contact or the group list, not both). Channels pair by
    position and must have the same name; the contact is found in odr_text by name and number; the record in img
    must carry the channel's name. Returns (restored, skipped): [(number, channel name, contact name, contact
    number)] and [(channel name, reason)]."""
    src, odr = channels(src_text), channels(odr_text)
    src_ct = {cid: (name, num) for cid, name, num in contacts(src_text)}
    index = {}
    for i, (cid, name, num) in enumerate(contacts(odr_text)):
        index.setdefault((name, num), i + 1)
    restored, skipped = [], []
    for s, o in zip(src, odr):
        if s["kind"] != "dmr" or not s["contact"] or o["contact"]:
            continue
        if s["name"] != o["name"]:
            skipped.append((s["name"], "channel order differs (%r in the ODR file)" % o["name"]))
            continue
        ct = src_ct.get(s["contact"])
        k = index.get(ct) if ct else None
        if k is None:
            skipped.append((s["name"], "contact %s not in the ODR file" % s["contact"]))
            continue
        number = o["index"] + 1
        rec = decode_channel(img, number)
        if rec is None or rec["name"] != (s["name"] or "")[:16]:
            skipped.append((s["name"], "record %d holds %r" % (number, rec and rec["name"])))
            continue
        _, a = channel_addr(number)
        struct.pack_into("<H", img, a + CH_CONTACT, k)
        restored.append((number, s["name"], ct[0], ct[1]))
    return restored, skipped


# ---------------------------------------------------------------------------------------------------------------
# DfuSe files (dmrconf's binary codeplug format)

def parse_dfuse(data):
    """[(target name, [(address, bytes)])] from a DfuSe file; checks the prefix, the sizes and the suffix CRC."""
    if len(data) < 11 + 16 or not data.startswith(b"DfuSe"):
        raise PortError("not a DfuSe file")
    if data[-8:-5] != b"UFD":
        raise PortError("DfuSe file without the DFU suffix")
    if struct.unpack_from("<I", data, len(data) - 4)[0] != (~zlib.crc32(data[:-4])) & 0xFFFFFFFF:
        raise PortError("DfuSe file: suffix CRC wrong (damaged file)")
    size = struct.unpack_from("<I", data, 6)[0]
    if size != len(data) - 16:
        raise PortError("DfuSe file: image size %d, file has %d" % (size, len(data) - 16))
    off, targets = 11, []
    for _ in range(data[10]):
        if data[off:off + 6] != b"Target":
            raise PortError("DfuSe file: bad target prefix at %d" % off)
        name = data[off + 11:off + 266].split(b"\0")[0].decode("ascii", "replace")
        tsize, count = struct.unpack_from("<II", data, off + 266)
        off += 274
        end, elems = off + tsize, []
        for _ in range(count):
            addr, n = struct.unpack_from("<II", data, off)
            elems.append((addr, data[off + 8:off + 8 + n]))
            off += 8 + n
        if off != end:
            raise PortError("DfuSe file: target %r size mismatch" % name)
        targets.append((name, elems))
    if off != len(data) - 16:
        raise PortError("DfuSe file: %d stray bytes" % (len(data) - 16 - off))
    return targets


def build_dfuse(targets):
    body = b""
    for name, elems in targets:
        payload = b"".join(struct.pack("<II", a, len(d)) + bytes(d) for a, d in elems)
        nm = name.encode("ascii")[:254]
        body += (b"Target" + b"\x01" + struct.pack("<I", 1) +      # alternate setting 1, named, as dmrconf
                 nm + bytes(255 - len(nm)) + struct.pack("<II", len(payload), len(elems)) + payload)
    head = b"DfuSe\x01" + struct.pack("<I", 11 + len(body)) + bytes([len(targets)])
    data = head + body + b"\xff\xff\xff\xff\xff\xff\x1a\x01UFD\x10"
    return data + struct.pack("<I", (~zlib.crc32(data)) & 0xFFFFFFFF)


def dfu_regions(targets):
    return [(a, a + len(d)) for _, elems in targets for a, d in elems if d]


def dfu_image(targets):
    """A flat image (file offset = CPS wire address before the offset, 0xFF elsewhere) and its regions."""
    regions = dfu_regions(targets)
    size = max([0x120000] + [e for _, e in regions])
    img = bytearray(b"\xff" * size)
    for _, elems in targets:
        for a, d in elems:
            img[a:a + len(d)] = d
    return img, regions


def image_to_dfu(template, img):
    """The template's targets and elements with their bytes taken from img (a read back)."""
    return [(name, [(a, bytes(img[a:a + len(d)])) for a, d in elems]) for name, elems in template]


# ---------------------------------------------------------------------------------------------------------------
# step 4: write through ODR's CPS shim, read back

def _cps():
    import cps_client
    return cps_client


def cps_write_dfu(sess, open_cps, dfu_path, label, before_name, offset=0, floor=0, ceiling=None, block=32,
                  progress=None, reboot=True):
    """Back up what the regions hold, write the DfuSe file's elements at +offset, return (targets, image)."""
    with open(dfu_path, "rb") as f:
        targets = parse_dfuse(f.read())
    img, regions = dfu_image(targets)
    total = sum(e - s for s, e in regions)
    sess.say("   %s: %d bytes in %d regions%s" % (label, total, len(regions),
                                                  ", at +0x%06X in the data flash" % offset if offset else ""))
    for s, e in regions:
        if s + offset < floor or (ceiling is not None and e + offset > ceiling):
            raise PortError("region 0x%X..0x%X is outside ODR's open-firmware image" % (s, e))
    if sess.dry:
        sess.say("   [dry run] would save the regions to %s, then write them" % before_name)
        return targets, img
    cps = open_cps()
    try:
        info = cps.info()
        sess.say("   CPS shim: radio type %d, git %s, build %s" % (info["radio_type"], info["git"], info["build"]))
        before = cps.read_image(regions, image=bytearray(b"\xff" * len(img)), block=block, progress=progress)
        with open(sess.path(before_name), "wb") as f:
            f.write(before)
        sess.say("   saved what was there as %s%s" % (before_name, " (file offset = address - 0x%06X)" % offset
                                                       if offset else ""))
        jobs = _cps().plan_write(img, regions, before)
        sess.say("   writing %d sectors ..." % len(jobs))
        cps.write_image(img, regions, reboot=reboot, block=block, progress=progress, base=before)
    except PortError:
        raise
    except Exception as e:
        raise PortError("CPS write failed: %s. Run 'restore --workdir %s' again" % (e, sess.workdir))
    finally:
        cps.close()
    return targets, img


def check_read_back(sess, open_cps, targets, img, expected, odr_driver, block=32, progress=None):
    """Read the written regions back, save them as readback-odr.dfu / .yaml, compare bytes and counts. Raises
    PortError when a section's count differs from what was written."""
    regions = dfu_regions(targets)
    cps = open_cps()
    try:
        back = cps.read_image(regions, image=bytearray(b"\xff" * len(img)), block=block, progress=progress)
    except Exception as e:
        raise PortError("read back failed: %s" % e)
    finally:
        cps.close()
    diffs = _cps().compare(img, back, regions)
    back_dfu = sess.path("readback-odr.dfu")
    with open(back_dfu, "wb") as f:
        f.write(build_dfuse(image_to_dfu(targets, back)))
    back_yaml = sess.path("readback-odr.yaml")
    sess.dmr("decode", "--radio", odr_driver, back_dfu, back_yaml, radio=False)
    with open(back_yaml, encoding="utf-8") as f:
        got = codeplug_summary(f.read())
    print_summary(sess, "read back", got)
    sess.say("   %d bytes differ from what was written (the radio keeps its own VFO records)" % len(diffs))
    diff = compare_counts(expected or {}, got["counts"])
    if diff:
        for k, want, have in diff:
            sess.say("   ! %s: wrote %d, read back %d" % (SECTION_NAMES[k], want, have))
        raise PortError("the codeplug read back differs from what was written; the files are in %s" % sess.workdir)
    sess.say("   read back matches")
    return back


def convert_for_odr(sess, yaml_path, odr_driver, cleanup=True):
    """Clean (unless cleanup is false), encode, decode, put back the dropped TX contacts. Returns (odr_dfu,
    odr_yaml, report) with the work directory files odr-codeplug.dfu / .yaml; the YAML that went into the encoder is
    odr-source.yaml. Offline only: needs the YAML file to exist."""
    with open(yaml_path, encoding="utf-8") as f:
        src_text = f.read()
    rep = {"cleanup": None, "restored": [], "skipped": []}
    if cleanup:
        src_text, rep["cleanup"] = clean_group_lists(src_text)
        sess.say("   " + format_cleanup(rep["cleanup"]))
    src = sess.path("odr-source.yaml")
    with open(src, "w", encoding="utf-8") as f:
        f.write(src_text)
    encoded = sess.path("odr-encoded.dfu")
    sess.dmr("encode", "--radio", odr_driver, src, encoded, radio=False)
    decoded = sess.path("odr-encoded.yaml")
    sess.dmr("decode", "--radio", odr_driver, encoded, decoded, radio=False)
    with open(encoded, "rb") as f:
        targets = parse_dfuse(f.read())
    with open(decoded, encoding="utf-8") as f:
        odr_text = f.read()
    img, _ = dfu_image(targets)
    c = _cps()
    rep["restored"], rep["skipped"] = restore_contacts(src_text, odr_text, img, c.channel_addr, c.decode_channel)
    odr_dfu, odr_yaml = sess.path("odr-codeplug.dfu"), sess.path("odr-codeplug.yaml")
    with open(odr_dfu, "wb") as f:
        f.write(build_dfuse(image_to_dfu(targets, img)))
    sess.dmr("decode", "--radio", odr_driver, odr_dfu, odr_yaml, radio=False)
    with open(odr_yaml, encoding="utf-8") as f:
        final = channels(f.read())
    want = sum(1 for ch in channels(src_text) if ch["kind"] == "dmr" and ch["contact"])
    have = sum(1 for ch in final if ch["kind"] == "dmr" and ch["contact"])
    sess.say("   TX contacts: %d of %d channels keep theirs (%d put back after the encoder)" % (have, want,
                                                                                         len(rep["restored"])))
    for name, why in rep["skipped"]:
        sess.say("   ! %s: TX contact not put back: %s" % (name, why))
    rep["contacts"] = (have, want)
    return odr_dfu, odr_yaml, rep


# ---------------------------------------------------------------------------------------------------------------
# self-test

SAMPLE = """version: 0.15.1
settings:
  defaultID: id1
radioIDs:
  - dmr: {id: id1, name: N0CALL, number: 3100001}
contacts:
  - dmr: {id: cont1, name: Local9, ring: false, type: GroupCall, number: 9}
  - dmr: {id: cont2, name: TAC 310, ring: false, type: GroupCall, number: 310}
  - dmr: {id: cont3, name: "Alice, A", ring: false, type: PrivateCall, number: 3100002}
groupLists:
  - {id: grp1, name: L9, contacts: [cont1]}
  - {id: grp2, name: Empty, contacts: []}
  - id: grp3
    name: Both
    contacts: [cont1, cont2]
  - {id: grp4, name: Both again, contacts: [cont1, cont2]}
  - {id: grp5, name: Unused, contacts: [cont2]}
channels:
  - dmr:
      id: ch1
      name: Local 9
      contact: cont1
      groupList: grp1
  - fm:
      id: ch2
      name: Simplex FM
  - dmr:
      id: ch3
      name: Empty list
      contact: cont2
      groupList: grp2
  - dmr:
      id: ch4
      name: TAC 310 mon
      contact: cont2
      groupList: grp4
  - dmr:
      id: ch5
      name: No contact
      groupList: grp3
zones:
  - id: zone1
    name: Z1
    A: [ch1, ch2, ch3, ch4, ch5]
scanLists: []
"""


def self_test():
    fails = []

    def check(cond, what):
        if not cond:
            fails.append(what)
            print("FAIL: " + what)

    # clean-up
    out, rep = clean_group_lists(SAMPLE)
    chs = {c["name"]: c for c in channels(out)}
    check(chs["Local 9"]["groupList"] is None and chs["Local 9"]["contact"] == "cont1", "own-contact list dropped")
    check(chs["Empty list"]["groupList"] is None, "empty list dropped")
    check(chs["TAC 310 mon"]["groupList"] == "grp3", "duplicate list merged into the first")
    check(chs["No contact"]["groupList"] == "grp3", "a list on a channel without a contact stays")
    ids = [item_field(i, "id") for i in yaml_sections(out)["groupLists"]]
    check(ids == ["grp3"], "only used lists remain: %r" % ids)
    check(rep["lists_before"] == 5 and rep["lists_after"] == 1 and rep["merged"] == 1, "report %r" % rep)
    check(rep["dropped_own"] == ["Local 9"] and rep["dropped_empty"] == ["Empty list"], "report names")
    s0, s1 = codeplug_summary(SAMPLE), codeplug_summary(out)
    check(s1["counts"] == dict(s0["counts"], groupLists=1), "the rest left as written %r" % s1["counts"])
    check(clean_group_lists(out)[0] == out, "clean-up is idempotent")
    none, nrep = clean_group_lists(SAMPLE.replace("groupList: grp4", "groupList: grp5").replace(
        "groupList: grp3", "groupList: grp2"))
    check(nrep["lists_after"] == 0 and "groupLists: []" in none and not yaml_sections(none)["groupLists"],
          "an emptied section stays valid YAML")

    # restore_contacts against a small fake image: two records, the channel layout passed in
    def addr(n):
        return 0, 0x100 + (n - 1) * 0x40

    def dec(img, n):
        a = addr(n)[1]
        if img[a] == 0xFF:
            return None
        return {"name": bytes(img[a:a + 16]).split(b"\xff")[0].decode()}

    src = SAMPLE
    odr = SAMPLE.replace("      contact: cont2\n      groupList: grp4\n", "      groupList: grp4\n")
    img = bytearray(b"\xff" * 0x400)
    for n, name in ((1, "Local 9"), (2, "Simplex FM"), (3, "Empty list"), (4, "TAC 310 mon"), (5, "No contact")):
        a = addr(n)[1]
        img[a:a + 16] = name.encode().ljust(16, b"\xff")
        img[a + CH_CONTACT:a + CH_CONTACT + 2] = b"\0\0"
    r, sk = restore_contacts(src, odr, img, addr, dec)
    check(r == [(4, "TAC 310 mon", "TAC 310", "310")] and sk == [], "contact put back %r %r" % (r, sk))
    check(struct.unpack_from("<H", img, addr(4)[1] + CH_CONTACT)[0] == 2, "1-based index of TAC 310")
    check(struct.unpack_from("<H", img, addr(5)[1] + CH_CONTACT)[0] == 0, "a channel without a contact untouched")
    img2 = bytearray(img)
    struct.pack_into("<H", img2, addr(4)[1] + CH_CONTACT, 0)
    img2[addr(4)[1]:addr(4)[1] + 16] = b"Other".ljust(16, b"\xff")
    r, sk = restore_contacts(src, odr, img2, addr, dec)
    check(r == [] and len(sk) == 1 and "holds" in sk[0][1], "a record with another name is left alone")
    check(struct.unpack_from("<H", img2, addr(4)[1] + CH_CONTACT)[0] == 0, "... and not written")

    # DfuSe round trip
    t = [("EEPROM", [(0x80, b"\x01\x02"), (0x7500, b"\x03")]), ("Flash", [(0x9B000, bytes(range(40)))])]
    d = build_dfuse(t)
    check(parse_dfuse(d) == [(n, [(a, bytes(x)) for a, x in e]) for n, e in t], "DfuSe round trip")
    im, reg = dfu_image(t)
    check(reg == [(0x80, 0x82), (0x7500, 0x7501), (0x9B000, 0x9B028)] and im[0x9B001] == 1, "flat image")
    check(image_to_dfu(t, im) == [(n, [(a, bytes(x)) for a, x in e]) for n, e in t], "image back to DfuSe")
    for bad in (d[:-1] + bytes([d[-1] ^ 1]), b"nope" + d[4:]):
        try:
            parse_dfuse(bad)
            check(False, "a damaged DfuSe file is refused")
        except PortError:
            pass

    # the shared YAML helpers
    s = codeplug_summary(SAMPLE)
    check(s["counts"] == {"channels": 5, "zones": 1, "contacts": 3, "groupLists": 5}, "counts %r" % s["counts"])
    check(s["names"]["contacts"][2] == "Alice, A" and s["radio_ids"] == [3100001], "names and radio ID")
    check(compare_counts({"channels": 5}, {"channels": 4, "zones": 0, "contacts": 0, "groupLists": 0})
          == [("channels", 5, 4)], "a lost channel is found")

    print("codeplug_port self-test %s (%d failure%s)" % ("PASSED" if not fails else "FAILED", len(fails),
                                                        "" if len(fails) == 1 else "s"))
    return 0 if not fails else 1


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        sys.exit(self_test())
    print(__doc__)
    sys.exit(2)
