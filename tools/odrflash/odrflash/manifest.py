# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Release manifests: one small JSON file per firmware image (docs/dev/release.md "Release artifacts").

    {
      "format": "odr-release/1",
      "board": "dm1701", "board_name": "Baofeng DM-1701",
      "version": "v0.1.0", "git": "5f034440", "git_full": "5f03444...",
      "image": {"file": "odr-dm1701-v0.1.0.bin", "size": 220780, "sha256": "..."},
      "ranges": [{"addr": "0x0800C000", "size": 220780}],
      "sectors": [3, 4, 5, 6],
      "codec": {"included": false, "profile": "dm1701-v0206-csv", "sectors": [7, 8, 10, 11],
                "windows": [{"addr": "0x08074714", "size": 66076}, {"addr": "0x080D19F0", "size": 75856}],
                "keep_supported": true},
      "features": "FM,SUBTONE,DMR,...",
      "toolchain": "arm-none-eabi-gcc 13.3.rel1"
    }

A release image never includes the codec ("included": false); verify() refuses one whose ranges reach into a codec
sector, so "keep the radio's codec" can never be undone by the image itself.
"""
import hashlib
import json

from . import boards

FORMAT = "odr-release/1"


class ManifestError(Exception):
    pass


def _hex(a):
    return "0x%08X" % a


def _int(v):
    return int(v, 0) if isinstance(v, str) else int(v)


def image_name(board_id, version, ext="bin"):
    return "odr-%s-%s.%s" % (board_id, version, ext)


def generate(board, image, version, git_full, features="", toolchain="", file_name=None):
    """The manifest dict for a codec-less raw image linked at board.app_base."""
    lo, hi = board.app_base, board.app_base + len(image)
    if not image:
        raise ManifestError("empty image")
    if hi > board.flash_end:
        raise ManifestError("image ends at 0x%08X, past the end of flash 0x%08X" % (hi, board.flash_end))
    codec_secs = board.codec_sectors()
    hit = [s.index for s in codec_secs if s.overlaps(lo, hi)]
    if hit:
        raise ManifestError("the image (0x%08X..0x%08X) reaches into codec sector(s) %s: the linker script must keep "
                            "the application out of them" % (lo, hi, hit))
    return {
        "format": FORMAT,
        "board": board.id,
        "board_name": board.name,
        "version": version,
        "git": git_full[:8],
        "git_full": git_full,
        "image": {
            "file": file_name or image_name(board.id, version),
            "size": len(image),
            "sha256": hashlib.sha256(image).hexdigest(),
        },
        "ranges": [{"addr": _hex(lo), "size": len(image)}],
        "sectors": [s.index for s in board.sectors_for(lo, hi)],
        "codec": {
            "included": False,
            "profile": board.codec.id,
            "sectors": [s.index for s in codec_secs],
            "windows": [{"addr": _hex(a), "size": n} for a, n in board.codec.windows],
            "keep_supported": board.keep_codec_supported,
        },
        "features": features,
        "toolchain": toolchain,
    }


def dumps(manifest):
    return json.dumps(manifest, indent=2, sort_keys=False) + "\n"


def load(path):
    with open(path, "r", encoding="utf-8") as f:
        try:
            return json.load(f)
        except ValueError as e:
            raise ManifestError("%s is not valid JSON: %s" % (path, e))


def ranges(manifest):
    """[(addr, size), ...] as integers."""
    return [(_int(r["addr"]), _int(r["size"])) for r in manifest.get("ranges", [])]


def segments(manifest, image):
    """Cut the image file into (addr, bytes) segments by the manifest's ranges (the file is the ranges' bytes in
    order)."""
    out, pos = [], 0
    for addr, size in ranges(manifest):
        out.append((addr, image[pos:pos + size]))
        pos += size
    return out


def verify(manifest, image, board_id=None):
    """Check a manifest against its image and (optionally) the board the radio is. Returns the Board. Raises
    ManifestError with a plain explanation."""
    if manifest.get("format") != FORMAT:
        raise ManifestError("unknown manifest format %r (this odrflash reads %s; update odrflash)"
                            % (manifest.get("format"), FORMAT))
    try:
        board = boards.get(manifest.get("board"))
    except KeyError as e:
        raise ManifestError(str(e))
    if board_id is not None and boards.get(board_id).id != board.id:
        raise ManifestError("this image is for the %s, but the radio is a %s: refusing"
                            % (board.name, boards.get(board_id).name))
    img = manifest.get("image") or {}
    if _int(img.get("size", -1)) != len(image):
        raise ManifestError("the image is %d bytes, the manifest says %s" % (len(image), img.get("size")))
    got = hashlib.sha256(image).hexdigest()
    if got != img.get("sha256"):
        raise ManifestError("the image's SHA-256 is %s, the manifest says %s: the download is damaged or the files "
                            "don't belong together" % (got, img.get("sha256")))
    rs = ranges(manifest)
    if not rs or sum(n for _, n in rs) != len(image):
        raise ManifestError("the manifest's ranges don't add up to the image size")
    codec_secs = board.codec_sectors()
    for addr, n in rs:
        if addr < board.app_base or addr + n > board.flash_end:
            raise ManifestError("range 0x%08X+0x%X is outside the application flash" % (addr, n))
        for lo, hi in board.protected:
            if addr < hi and addr + n > lo:
                raise ManifestError("range 0x%08X+0x%X overlaps the bootloader" % (addr, n))
        hit = [s.index for s in codec_secs if s.overlaps(addr, addr + n)]
        if hit and not (manifest.get("codec") or {}).get("included"):
            raise ManifestError("range 0x%08X+0x%X reaches into codec sector(s) %s" % (addr, n, hit))
    codec = manifest.get("codec") or {}
    if codec.get("profile") and codec["profile"] != board.codec.id:
        raise ManifestError("the image expects codec profile %s; this odrflash knows %s for the %s (update odrflash)"
                            % (codec["profile"], board.codec.id, board.name))
    return board


def for_dev_build(board, image, version="dev"):
    """A manifest for a local build without one (odrflash flash build/dm1701/odr_dm1701.bin --board dm1701)."""
    m = generate(board, image, version, "0" * 40)
    m["git"] = "unknown"
    m["dev_build"] = True
    return m


def sha256sums_line(path_name, data):
    return "%s  %s\n" % (hashlib.sha256(data).hexdigest(), path_name)
